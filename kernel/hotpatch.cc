#include <string.h>
#include "types.h"
#include "amd64.h"
#include "kernel.hh"
#include "bits.hh"
#include "cpuid.hh"
#include "cmdline.hh"
#include "kmeta.hh"

struct patch {
  u64 segment_mask;
  const char* option;
  const char* value;
  u64 start;
  u64 opcode;
  u64 alternative;
  u64 end;
  u64 string_len;
};

#define PATCH_SEGMENT_KTEXT 0x1
#define PATCH_SEGMENT_QTEXT 0x2
#define PATCH_OPCODE_OR_NOPS 4
#define PATCH_OPCODE_OR_CALL 5
#define PATCH_OPCODE_OR_STRING 6

char* qtext, *original_text;
volatile u8 secrets_mapped __attribute__((section (".sflag"))) = 1;
extern u64 __hotpatch_start, __hotpatch_end;

const char* INDIRECT_CALL[] = {
  "\xff\xd0", "\xff\xd1", "\xff\xd2", "\xff\xd3",
  "\xff\xd4", "\xff\xd5", "\xff\xd6", "\xff\xd7",
  "\x41\xff\xd0", "\x41\xff\xd1", "\x41\xff\xd2", "\x41\xff\xd3",
  "\x41\xff\xd4", "\x41\xff\xd5", "\x41\xff\xd6", "\x41\xff\xd7",
};
const int INDIRECT_CALL_LENGTHS[] = {2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3};

const char* INDIRECT_JMP[] = {
  "\xff\xe0", "\xff\xe1", "\xff\xe2", "\xff\xe3",
  "\xff\xe4", "\xff\xe5", "\xff\xe6", "\xff\xe7",
  "\x41\xff\xe0", "\x41\xff\xe1", "\x41\xff\xe2", "\x41\xff\xe3",
  "\x41\xff\xe4", "\x41\xff\xe5", "\x41\xff\xe6", "\x41\xff\xe7",
};
const int INDIRECT_JMP_LENGTHS[] = {2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3};

const char* NOP[] = {
  "",
  "\x90",
  "\x66\x90",
  "\x0f\x1f\x00",
  "\x0f\x1f\x40\x00",
  "\x0f\x1f\x44\x00\x00",
  "\x66\x0f\x1f\x44\x00\x00",
  "\x0f\x1f\x80\x00\x00\x00\x00",
  "\x0f\x1f\x84\x00\x00\x00\x00\x00",
  "\x66\x0f\x1f\x84\x00\x00\x00\x00\x00",
};

// Replace the 5 bytes at location with a call instruction to func.
void insert_call_instruction(char* text_base, u64 location, u64 func) {
  text_base[location-KTEXT] = 0xe8;
  *(u32*)&text_base[location-KTEXT+1] = (u32)((char*)func - (char*)location - 5);
}

// Replace calls to retpolines if patch is true, or restore them if not.
void patch_retpolines(char* text_base, bool patch) {
  const u32* indirect_branches = kmeta::indirect_branches();
  u32 num_indirect_branches = kmeta::num_indirect_branches();
  for(int i = 0; i < num_indirect_branches; i++) {
    u32 v = indirect_branches[i];
    u64 addr = (v & 0xFFFFFF) | KCODE;
    int reg = (v >> 24) & 0xF;
    int is_jmp = (v >> 28) & 0x1;

    if (patch) {
      const char* instr = is_jmp ? INDIRECT_JMP[reg] : INDIRECT_CALL[reg];
      int instr_len = is_jmp ? INDIRECT_JMP_LENGTHS[reg] : INDIRECT_CALL_LENGTHS[reg];

      // The call and jmp instructions we're replacing are always 5 bytes. To
      // avoid issues, we pad the inserted instructions to be the same size by
      // inserting dummy "CS segment override" prefixes. These prefixes are always
      // ignored in 64-bit mode.
      memset(&text_base[addr-KTEXT], 0x2e, 5-instr_len);
      memcpy(&text_base[addr+5-instr_len-KTEXT], instr, instr_len);
    } else {
      memcpy(&text_base[addr-KTEXT], &original_text[addr-KTEXT], 5);
    }
  }
}

// Replace the range [start, end) with NOPs.
void remove_range(char* text_base, u64 start, u64 end) {
  u64 current = start;
  while (current != end) {
    u64 len = end - current;
    if (len >= 9) {
      memcpy((char*)&text_base[current-KTEXT], NOP[9], 9);
      current += 9;
    } else {
      memcpy((char*)&text_base[current-KTEXT], NOP[len], len);
      current += len;
    }
  }
}

bool patch_needed(patch* p, bool ktext) {
  bool value;

  if(strcmp(p->value, "yes") == 0) {
    value = true;
  } else if(strcmp(p->value, "no") == 0) {
    value = false;
  } else {
    return false;
  }

  bool cmdline_value = false;
  if(strcmp(p->option, "lazy_barrier") == 0) {
    cmdline_value = cmdline_params.lazy_barrier;
  } else if(strcmp(p->option, "mds") == 0) {
    cmdline_value = cmdline_params.mds;
  } else if(strcmp(p->option, "fsgsbase") == 0) {
    cmdline_value = cpuid::features().fsgsbase;
  } else if(strcmp(p->option, "spectre_v2") == 0) {
    cmdline_value = cmdline_params.spectre_v2;
  } else if(strcmp(p->option, "retpolines") == 0) {
    cmdline_value = ktext ? cmdline_params.spectre_v2 : cmdline_params.keep_retpolines;
#if ENABLE_PARAVIRT
  } else if(strcmp(p->option, "kvm_paravirt") == 0) {
    cmdline_value = (strcmp(cpuid::features().hypervisor_id, "KVMKVMKVM") == 0);
#endif
  } else if(strcmp(p->option, "kpti") == 0) {
    cmdline_value = cmdline_params.kpti;
  } else {
    return false;
  }

  return cmdline_value != value;
}

void apply_hotpatches()
{
  // Hotpatching involves modifying the (normally) read only text
  // segment. Thus we temporarily disable write protection for kernel
  // pages. We'll re-enable it again at the end of this function.
  lcr0(rcr0() & ~CR0_WP);

  char* text_bases[2] = {(char*)KTEXT, qtext};

  patch_retpolines(text_bases[0], !cmdline_params.keep_retpolines && !cmdline_params.spectre_v2);
  patch_retpolines(text_bases[1], !cmdline_params.keep_retpolines);

  for (patch* p = (patch*)&__hotpatch_start; p < (patch*)&__hotpatch_end; p++) {
    assert(p->segment_mask == 1 || p->segment_mask == 2 || p->segment_mask == 3);

    for (int i = 0; i < 2; i++) {
      if(!(p->segment_mask & (1<<i)) || !p->start)
        continue;

      if(patch_needed(p, (1<<i) == PATCH_SEGMENT_KTEXT)) {
        switch(p->opcode) {
        case PATCH_OPCODE_OR_NOPS:
          remove_range(text_bases[i], p->start, p->end);
          break;
        case PATCH_OPCODE_OR_CALL:
          assert(p->end - p->start >= 5);
          insert_call_instruction(text_bases[i], p->start, p->alternative);
          remove_range(text_bases[i], p->start+5, p->end);
          break;
        case PATCH_OPCODE_OR_STRING:
          assert(p->string_len > 0);
          assert(p->string_len <= p->end - p->start);
          memcpy(&text_bases[i][p->start - KTEXT],
                 (char*)p->alternative,
                 p->string_len);
          remove_range(text_bases[i], p->start + p->string_len, p->end);
          break;
        default:
          panic("hotpatch: bad opcode");
        }
      } else {
        memcpy(&text_bases[i][p->start - KTEXT],
               &original_text[p->start - KTEXT],
               p->end - p->start);
      }
    }
  }

  *(&secrets_mapped - KTEXT + (u64)qtext) = 0;

  lcr0(rcr0() | CR0_WP);
}

void inithotpatch()
{
  original_text = kalloc("original_text", 0x200000);
  memmove(original_text, (void*)KTEXT, 0x200000);

  qtext = kalloc("qtext", 0x200000);
  memmove(qtext, (void*)KTEXT, 0x200000);

  apply_hotpatches();
}
