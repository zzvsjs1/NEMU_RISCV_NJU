#include "trap.h"

#define X86_CR0_PG 0x80000000u
#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static volatile uint32_t paged_mem[12] __attribute__((aligned(4096)));
static volatile uint32_t result_words[6];
static volatile uint8_t result_bytes[3];

static uint32_t read_cr0(void) {
  uint32_t value;
  asm volatile("movl %%cr0, %0" : "=r"(value));
  return value;
}

static void write_cr0(uint32_t value) {
  asm volatile("movl %0, %%cr0" : : "r"(value) : "memory");
}

static uint32_t read_cr3(void) {
  uint32_t value;
  asm volatile("movl %%cr3, %0" : "=r"(value));
  return value;
}

static void write_cr3(uint32_t value) {
  asm volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

static uint32_t rol32(uint32_t value, uint32_t count) {
  count &= 31u;
  return count == 0 ? value : (value << count) | (value >> (32u - count));
}

static uint32_t ror32(uint32_t value, uint32_t count) {
  count &= 31u;
  return count == 0 ? value : (value >> count) | (value << (32u - count));
}

static void install_identity_4k_paging(void) {
  for (uint32_t i = 0; i < 1024u; i++) {
    page_dir[i] = 0;
    page_table[i] = (i << 12) | X86_PTE_P | X86_PTE_W;
  }

  page_dir[0] = (uint32_t)page_table | X86_PTE_P | X86_PTE_W;
}

static void init_operands(void) {
  paged_mem[0] = 0xc0000001u;
  paged_mem[1] = 0xf0000008u;
  paged_mem[2] = 0x80000000u;
  paged_mem[3] = 0x10203040u;
  paged_mem[4] = 0x10203040u;
  paged_mem[5] = 0x55aa00ffu;
  paged_mem[6] = 0x00001000u;
  paged_mem[7] = 0xdeadbeefu;
  paged_mem[8] = 0xfffffff7u;
  paged_mem[9] = 0x00010001u;
  paged_mem[10] = 37u;
  paged_mem[11] = (uint32_t)(0u - 37u);
  for (uint32_t i = 0; i < 6u; i++) {
    result_words[i] = 0;
  }
  result_bytes[0] = 0;
  result_bytes[1] = 0;
  result_bytes[2] = 0;
}

static void run_paged_helpers(void) {
  asm volatile(
      "shll $1, %[shl1]\n\t"
      "setc %[shl_cf]\n\t"
      "seto %[shl_of]\n\t"
      "shrl $3, %[shr3]\n\t"
      "movl $3, %%ecx\n\t"
      "sarl %%cl, %[sarcl]\n\t"
      "roll $5, %[rol5]\n\t"
      "movl $9, %%ecx\n\t"
      "rorl %%cl, %[rorcl]\n\t"
      "notl %[notv]\n\t"
      "negl %[negv]\n\t"
      "cmpl $0xdeadbeef, %[cmpv]\n\t"
      "sete %[eq]\n\t"
      "movl %[imul_lhs], %%eax\n\t"
      "imull %[imul_src], %%eax\n\t"
      "movl %%eax, %[imul_out]\n\t"
      "movl %[mul_lhs], %%eax\n\t"
      "mull %[mul_src]\n\t"
      "xorl %%edx, %%eax\n\t"
      "movl %%eax, %[mul_out]\n\t"
      "xorl %%edx, %%edx\n\t"
      "movl %[div_lhs], %%eax\n\t"
      "divl %[div_src]\n\t"
      "movl %%eax, %[div_quot]\n\t"
      "movl %%edx, %[div_rem]\n\t"
      : [shl1] "+m"(paged_mem[0]),
        [shr3] "+m"(paged_mem[1]),
        [sarcl] "+m"(paged_mem[2]),
        [rol5] "+m"(paged_mem[3]),
        [rorcl] "+m"(paged_mem[4]),
        [notv] "+m"(paged_mem[5]),
        [negv] "+m"(paged_mem[6]),
        [shl_cf] "=m"(result_bytes[0]),
        [shl_of] "=m"(result_bytes[1]),
        [eq] "=m"(result_bytes[2]),
        [imul_out] "=m"(result_words[0]),
        [mul_out] "=m"(result_words[1]),
        [div_quot] "=m"(result_words[2]),
        [div_rem] "=m"(result_words[3])
      : [cmpv] "m"(paged_mem[7]),
        [imul_lhs] "r"(0xfffffff3u),
        [imul_src] "m"(paged_mem[8]),
        [mul_lhs] "r"(0x00012345u),
        [mul_src] "m"(paged_mem[9]),
        [div_lhs] "r"(100000u),
        [div_src] "m"(paged_mem[10])
      : "eax", "ecx", "edx", "cc", "memory");

  asm volatile(
      "movl %[idiv_lhs], %%eax\n\t"
      "cdq\n\t"
      "idivl %[idiv_src]\n\t"
      "movl %%eax, %[idiv_quot]\n\t"
      "movl %%edx, %[idiv_rem]\n\t"
      : [idiv_quot] "=m"(result_words[4]),
        [idiv_rem] "=m"(result_words[5])
      : [idiv_lhs] "r"((uint32_t)(0u - 100000u)),
        [idiv_src] "m"(paged_mem[11])
      : "eax", "ecx", "edx", "cc", "memory");
}

int main() {
  uint32_t old_cr0 = read_cr0();
  uint32_t old_cr3 = read_cr3();

  install_identity_4k_paging();
  init_operands();

  write_cr3((uint32_t)page_dir);
  write_cr0(old_cr0 | X86_CR0_PG);
  run_paged_helpers();
  write_cr0(old_cr0);
  write_cr3(old_cr3);

  check(paged_mem[0] == 0x80000002u);
  check(result_bytes[0] == 1);
  check(result_bytes[1] == 0);
  check(paged_mem[1] == 0x1e000001u);
  check(paged_mem[2] == 0xf0000000u);
  check(paged_mem[3] == rol32(0x10203040u, 5));
  check(paged_mem[4] == ror32(0x10203040u, 9));
  check(paged_mem[5] == ~0x55aa00ffu);
  check(paged_mem[6] == (uint32_t)(0u - 0x1000u));
  check(result_bytes[2] == 1);
  check(result_words[0] == 117u);

  uint64_t product = (uint64_t)0x00012345u * (uint64_t)0x00010001u;
  uint32_t product_fold = (uint32_t)product ^ (uint32_t)(product >> 32);
  check(result_words[1] == product_fold);
  check(result_words[2] == 100000u / 37u);
  check(result_words[3] == 100000u % 37u);
  check((int32_t)result_words[4] == -100000 / -37);
  check((int32_t)result_words[5] == -100000 % -37);
  return 0;
}
