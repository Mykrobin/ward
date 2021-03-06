#include "types.h"
#include "amd64.h"
#include "mmu.h"
#include "kernel.hh"
#include "spinlock.hh"
#include "condvar.hh"
#include "proc.hh"
#include "cpu.hh"
#include "hpet.hh"
#include "apic.hh"

// Intel 8253/8254/82C54 Programmable Interval Timer (PIT).
// http://en.wikipedia.org/wiki/Intel_8253

#define IO_TIMER1       0x040           // 8253 Timer #1
#define TIMER_FREQ      1193182
#define	TIMER_CNTR      (IO_TIMER1 + 0)	// timer counter port
#define TIMER_MODE      (IO_TIMER1 + 3) // timer mode port
#define TIMER_SEL0      0x00    // select counter 0
#define TIMER_TCOUNT    0x00    // mode 0, terminal count
#define TIMER_16BIT     0x30    // r/w counter 16 bits, LSB first
#define TIMER_STAT      0xe0    // read status mode
#define TIMER_STAT0     (TIMER_STAT | 0x2)  // status mode counter 0

u64 cpuhz = 0;
static u64 ticks __mpalign__;

ilist<pproc,&pproc::cv_sleep> sleepers  __mpalign__;   // XXX one per core?
struct spinlock sleepers_lock;

static void
wakeup(struct pproc *p)
{
  auto it = p->oncv->waiters.iterator_to(p);
  p->oncv->waiters.erase(it);
  p->oncv = 0;
  if (p->get_state() == SLEEPING) {
    addrun(p);
  } else {
    assert(p->get_state() == IDLING);
    p->set_state(RUNNABLE);
    if (p->cpu_halted && p->cpuid != mycpu()->id)
      lapic->send_ipi(&cpus[p->cpuid], T_WAKE_CORE);
  }
}

u64
nsectime(void)
{
  static bool used_ticks;
  if (mycpu()->tsc_period) {
    return rdtsc() * TSC_PERIOD_SCALE / mycpu()->tsc_period;
  }

  if (the_hpet) {
    return the_hpet->read_nsec();
  }
  // XXX Ticks don't happen when interrupts are disabled, which means
  // we lose track of wall-clock time, but if we don't have a HPET,
  // this is the best we can do.
  used_ticks = true;
  u64 msec = ticks*QUANTUM;
  return msec*1000000;
}

void
timerintr(void)
{
  struct condvar *cv;
  int again;
  u64 now;
  
  ticks++;

  now = nsectime();
  do {
    again = 0;
    scoped_acquire l(&sleepers_lock);
    for (auto it = sleepers.begin(); it != sleepers.end(); it++) {
      struct pproc &p = *it;
      if (p.cv_wakeup <= now) {
        if (tryacquire(&p.lock)) {
          if (tryacquire(&p.oncv->lock)) {
            sleepers.erase(it);
            cv = p.oncv;
            p.cv_wakeup = 0;
            wakeup(&p);
            release(&p.lock);
            release(&cv->lock);
            continue;
          } else {
            release(&p.lock);
          }
        }
        again = 1;
      }
    }
  } while (again);
}

void
condvar::sleep_to(struct spinlock *lk, u64 timeout, struct spinlock *lk2)
{
  if(myproc() == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire cv_lock to avoid sleep/wakeup race
  lock.acquire();

  lk->release();
  if (lk2)
    lk2->release();

  myproc()->lock.acquire();

  if(myproc()->oncv)
    panic("condvar::sleep_to oncv");

  waiters.push_front(myproc()->p.get());
  myproc()->oncv = this;
  assert(myproc()->get_state() == RUNNING);
  myproc()->set_state(SLEEPING);

  if (timeout) {
    scoped_acquire l(&sleepers_lock);
    myproc()->cv_wakeup = timeout;
    sleepers.push_back(myproc()->p.get());
 }

  lock.release();
  sched(true);
  // Reacquire original lock.
  lk->acquire();
  if (lk2)
    lk2->acquire();
  if (myproc()->killed) {
    // Callers should use scoped locks to ensure locks are released as the stack
    // is unwinded.  But, callers don't have to check for p->killed to ensure
    // that they don't call wait() again after being killed.
    throw kill_exception();
  }
}

void
condvar::sleep(struct spinlock *lk, struct spinlock *lk2)
{
  sleep_to(lk, 0, lk2);
}

void
condvar::wake_one(pproc *p)
{
  if (p->get_state() != SLEEPING && p->get_state() != IDLING)
    panic("condvar::wake_all: tid %u name %s state %u",
          p->tid, p->p->name, p->get_state());
  if (p->oncv != this)
    panic("condvar::wake_all: tid %u name %s p->cv %p cv %p",
          p->tid, p->p->name, p->oncv, this);
  if (p->cv_wakeup) {
    scoped_acquire s_l(&sleepers_lock);
    auto it = sleepers.iterator_to(p);
    sleepers.erase(it);
    p->cv_wakeup = 0;
  }
  wakeup(p);
}

// Wake up all processes sleeping on this condvar.
void
condvar::wake_all(int yield, pproc *callerproc)
{
  scoped_acquire cv_l(&lock);
  myproc()->yield_ = yield;

  for (auto it = this->waiters.begin(); it != this->waiters.end();
       it++) {
    pproc* p = &(*it);
    if (p == callerproc) {
      wake_one(p);
    } else {
      scoped_acquire p_l(&p->lock);
      wake_one(p);
    }
  }
}

void
microdelay(u64 delay)
{
  assert(cpuhz != 0);
  u64 tscdelay = (cpuhz * delay) / 1000000;
  u64 s = rdtsc();
  while (rdtsc() - s < tscdelay)
    nop_pause();
}

u64
gethzfromPIT(void)
{
  // Setup PIT for terminal count starting from 2^16 - 1
  u64 xticks = 0x000000000000FFFFull;
  outb(TIMER_MODE, TIMER_SEL0 | TIMER_TCOUNT | TIMER_16BIT);  
  outb(IO_TIMER1, xticks % 256);
  outb(IO_TIMER1, xticks / 256);

  // Wait until OUT bit of status byte is set
  u64 s = rdtsc();
  do {
    outb(TIMER_MODE, TIMER_STAT0);
    if (rdtsc() - s > 1ULL<<32) {
      cprintf("inithz: PIT stuck, assuming 2GHz\n");
      return 2 * 1000 * 1000 * 1000;
    }
  } while (!(inb(TIMER_CNTR) & 0x80));
  u64 e = rdtsc();

  return ((e-s)*10000000) / ((xticks*10000000)/TIMER_FREQ);
}

void
inithz()
{
  cpuhz = gethzfromPIT();
}

void
inittsc(void)
{
  if (the_hpet) {
    u64 hpet_start = the_hpet->read_nsec();
    u64 tsc_start = rdtsc();

    u64 hpet_end;
    do {
      nop_pause();
      hpet_end = the_hpet->read_nsec();
    } while(hpet_end < hpet_start + 10000000);
    u64 tsc_end = rdtsc();
    mycpu()->tsc_period = (tsc_end - tsc_start) * TSC_PERIOD_SCALE
      / (hpet_end - hpet_start);
  } else {
    mycpu()->tsc_period = (cpuhz * TSC_PERIOD_SCALE) / 1000000000;
  }
}
