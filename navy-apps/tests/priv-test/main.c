#include <stdint.h>

#define X86_EFLAGS_CF (1u << 0)
#define X86_EFLAGS_IF (1u << 9)
#define X86_EFLAGS_ZF (1u << 6)

static uint16_t read_cs(void) {
  uint16_t cs;
  asm volatile("movw %%cs, %0" : "=rm"(cs));
  return cs;
}

static uint16_t read_ss(void) {
  uint16_t ss;
  asm volatile("movw %%ss, %0" : "=rm"(ss));
  return ss;
}

static uint32_t read_eflags(void) {
  uint32_t eflags;
  asm volatile("pushfl; popl %0" : "=r"(eflags));
  return eflags;
}

static void write_eflags(uint32_t eflags) {
  asm volatile("pushl %0; popfl" : : "r"(eflags) : "cc", "memory");
}

int main() {
  uint16_t cs = read_cs();
  uint16_t ss = read_ss();

  if ((cs & 0x3) != 0x3) {
    return 1;
  }

  if ((ss & 0x3) != 0x3) {
    return 2;
  }

  uint32_t before = read_eflags();
  if ((before & X86_EFLAGS_IF) == 0) {
    return 3;
  }

  write_eflags(before & ~X86_EFLAGS_IF);

  /*
   * In protected mode, CPL3 code may not change IF when IOPL < CPL.  POPF does
   * not fault in this case; Intel specifies that IF remains unchanged.
   */
  if ((read_eflags() & X86_EFLAGS_IF) == 0) {
    return 4;
  }

  /*
   * Nanos maps ELF text without write permission.  The ADD first reads the text
   * byte and would change arithmetic flags if it completed.  Intel fault
   * semantics require the saved EFLAGS to remain at the pre-instruction value,
   * so the kernel checks that CF and ZF are still set in the #PF frame.
   */
  volatile uint8_t *text = (volatile uint8_t *)(uintptr_t)read_cs;
  asm volatile(
      "xorl %%eax, %%eax\n\t"
      "stc\n\t"
      "addb $1, (%0)"
      :
      : "r"(text)
      : "eax", "cc", "memory");

  return 5;
}
