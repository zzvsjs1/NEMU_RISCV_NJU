#include "trap.h"

typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t access;
  uint8_t limit_high_flags;
  uint8_t base_high;
} __attribute__((packed)) X86SegDesc;

typedef struct {
  uint16_t limit;
  uintptr_t base;
} __attribute__((packed)) X86DescTable;

typedef struct {
  uint32_t link;
  uint32_t esp0;
  uint32_t ss0;
  uint32_t padding[23];
} __attribute__((packed)) X86Tss32;

static X86SegDesc gdt[6] __attribute__((aligned(8)));
static X86Tss32 tss __attribute__((aligned(8)));

static X86SegDesc flat_desc(uint8_t type, uint8_t dpl) {
  X86SegDesc desc = {
    .limit_low = 0xffff,
    .base_low = 0,
    .base_mid = 0,
    .access = (uint8_t)(0x90u | ((dpl & 0x3u) << 5) | type),
    .limit_high_flags = 0xcf,
    .base_high = 0,
  };
  return desc;
}

static X86SegDesc tss_desc(void *base, uint32_t limit) {
  uintptr_t addr = (uintptr_t)base;
  X86SegDesc desc = {
    .limit_low = (uint16_t)limit,
    .base_low = (uint16_t)addr,
    .base_mid = (uint8_t)(addr >> 16),
    .access = 0x89,
    .limit_high_flags = (uint8_t)(limit >> 16),
    .base_high = (uint8_t)(addr >> 24),
  };
  return desc;
}

int main() {
  X86DescTable gdtr;

  gdt[1] = flat_desc(0x0a, 0);
  gdt[2] = flat_desc(0x02, 0);
  gdt[5] = tss_desc(&tss, sizeof(tss) - 1);
  gdtr.limit = sizeof(gdt) - 1;
  gdtr.base = (uintptr_t)gdt;

  /*
   * LTR consumes an available 32-bit TSS descriptor and marks that descriptor
   * busy in memory.  AM relies on the same selector for ring-3-to-ring-0 stack
   * switching, so the test keeps the GDT flat and only checks the TSS type bit.
   */
  asm volatile("lgdt %0; ltr %%ax" : : "m"(gdtr), "a"((uint16_t)(5u << 3)) : "memory");
  check((gdt[5].access & 0x0fu) == 0x0bu);

  return 0;
}
