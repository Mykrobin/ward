#include "mmu.h"
#include "types.h"
#include "lib.h"

struct ipcmsg;

// console.c
void            consoleintr(int(*)(void));

// kbd.c
void            kbdintr(void);

// swtch.S
struct context;
struct contextptr;
void            swtch(struct contextptr*, struct context*);
void            swtch_and_barrier(struct contextptr*, struct context*);
void            switch_to_kstack();

// trap.c
extern struct segdesc bootgdt[NSEGS];

// other exported/imported functions
void cmain(u64 mbmagic, u64 mbaddr);
void mpboot(void);
void trapret(void);
void threadstub(void);
void threadhelper(void (*fn)(void *), void *arg);

struct trapframe;
void sysentry(void);
u64 sysentry_c(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 num);
