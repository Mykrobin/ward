#include "cpuid.hh"
#include <string.h>

cpuid cpuid::instance_;

cpuid::cpuid() : basic_{}, extended_{}
{
  // Leaf 0 tells us how many leafs there are
  basic_[(int)leafid::basic] = read_leaf((int)leafid::basic);

  // Cache all basic leafs
  for (int i = 1; i < MAX_BASIC && i <= basic_[0].a; ++i)
    basic_[i] = read_leaf(i);

  // Leaf 0x80000000 tells us how many extended leafs there are
  extended_[0] = read_leaf((uint32_t)leafid::extended_info);

  // Cache extended leafs
  for (int i = 1; i < MAX_EXTENDED &&
         i <= extended_[0].a - (uint32_t)leafid::extended_info; ++i)
    extended_[i] = read_leaf((uint32_t)leafid::extended_info + i);

  // Decode vendor_
  leaf l = get_leaf(leafid::basic);
  uint32_t name[3] = {l.b, l.d, l.c};
  memmove(vendor_, name, sizeof name);
  vendor_[12] = 0;

  // Decode features
  l = get_leaf(leafid::features);
  features_.mwait = l.c & (1<<3);
  features_.pdcm = l.c & (1<<15);
  features_.pcid = l.c & (1<<17);
  features_.x2apic = l.c & (1<<21);
  features_.hypervisor = l.c & (1<<31);

  features_.apic = l.d & (1<<9);
  features_.ds = l.d & (1<<21);

  l = get_leaf(leafid::ext_features);
  features_.fsgsbase = l.b & (1<<0);
  features_.intel_pt = l.b & (1<<25);
  features_.md_clear = l.d & (1<<10);
  features_.spec_ctrl = l.d & (1<<26);

  if(features_.hypervisor) {
    // We can't use get_leaf because the hypervisor leaf will be rejected by the
    // maximum leaf check.
    l = read_leaf((uint32_t)leafid::hypervisor);
    *(uint32_t*)features_.hypervisor_id = l.b;
    *(uint32_t*)(features_.hypervisor_id+4) = l.c;
    *(uint32_t*)(features_.hypervisor_id+8) = l.d;
    features_.hypervisor_id[12] = '\0';
  } else {
    features_.hypervisor_id[0] = '\0';
  }

  l = get_leaf(leafid::extended_features);
  features_.page1GB = l.d & (1<<26);
}
