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

static X86SegDesc gdt[4] __attribute__((aligned(8)));

static X86SegDesc seg_desc(uint32_t base, uint32_t limit, uint8_t type, uint8_t dpl) {
  X86SegDesc desc = {
    .limit_low = (uint16_t)limit,
    .base_low = (uint16_t)base,
    .base_mid = (uint8_t)(base >> 16),
    .access = (uint8_t)(0x90u | ((dpl & 0x3u) << 5) | type),
    .limit_high_flags = (uint8_t)(0x40u | ((limit >> 16) & 0x0fu)),
    .base_high = (uint8_t)(base >> 24),
  };
  return desc;
}

static uint32_t read_with_cs_override(uint32_t *ptr) {
  uint32_t out;
  asm volatile(".byte 0x2e, 0x8b, 0x00"
               : "=a"(out)
               : "a"(ptr)
               : "memory");
  return out;
}

static uint32_t read_with_addr16(uint16_t offset) {
  uint32_t out;
  asm volatile("movw %1, %%bx; .byte 0x67, 0x8b, 0x07"
               : "=a"(out)
               : "rm"(offset)
               : "ebx", "memory");
  return out;
}

static uint32_t read_with_nonflat_ds(uint16_t selector) {
  uint32_t out;
  asm volatile(
      "movw %%ds, %%dx\n\t"
      "movw %1, %%ax\n\t"
      "movw %%ax, %%ds\n\t"
      "movl 0x20, %%ecx\n\t"
      "movw %%dx, %%ds\n\t"
      "movl %%ecx, %%eax\n\t"
      : "=a"(out)
      : "rm"(selector)
      : "ecx", "edx", "memory");
  return out;
}

static uint32_t read_moffs_with_cs_override(uint16_t selector) {
  uint32_t out;
  asm volatile(
      "movw %%ds, %%dx\n\t"
      "movw %1, %%ax\n\t"
      "movw %%ax, %%ds\n\t"
      ".byte 0x2e, 0xa1\n\t"
      ".long 0x9000\n\t"
      "movw %%dx, %%ds\n\t"
      : "=a"(out)
      : "r"(selector)
      : "edx", "memory");
  return out;
}

int main() {
  uint32_t value = 0x13579bdfu;
  X86DescTable gdtr;

  *(volatile uint32_t *)0x9000 = 0x2468ace0u;
  *(volatile uint32_t *)0xa020 = 0xfeedc0deu;
  *(volatile uint32_t *)0x13000 = 0x10203040u;

  check(read_with_cs_override(&value) == 0x13579bdfu);
  check(read_with_addr16(0x9000u) == 0x2468ace0u);

  gdt[1] = seg_desc(0, 0xfffffu, 0x0a, 0);
  gdt[2] = seg_desc(0, 0xfffffu, 0x02, 0);
  gdt[3] = seg_desc(0xa000u, 0x0fffu, 0x02, 0);
  gdtr.limit = sizeof(gdt) - 1;
  gdtr.base = (uintptr_t)gdt;
  asm volatile("lgdt %0" : : "m"(gdtr) : "memory");

  check(read_with_nonflat_ds((uint16_t)(3u << 3)) == 0xfeedc0deu);
  check(read_moffs_with_cs_override((uint16_t)(3u << 3)) == 0x2468ace0u);
  return 0;
}
