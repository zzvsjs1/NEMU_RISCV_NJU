#include "trap.h"

#define X86_CR0_PG 0x80000000u
#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static volatile uint32_t paged_mem[12] __attribute__((aligned(4096)));
static volatile uint32_t result_words[6];
static volatile uint8_t result_bytes[3];
static volatile uint8_t shift_bytes[7];
static volatile uint16_t shift_words[7];
static volatile uint16_t imul16_src[2];
static volatile uint32_t imul16_out[2];
static volatile uint8_t imul16_flags[4];
static volatile uint16_t mul_acc_src[4];
static volatile uint32_t mul_acc_out[4];
static volatile uint8_t mul_acc_flags[8];

static uint32_t read_cr0(void)
{
    uint32_t value;
    asm volatile("movl %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint32_t value)
{
    asm volatile("movl %0, %%cr0" : : "r"(value) : "memory");
}

static uint32_t read_cr3(void)
{
    uint32_t value;
    asm volatile("movl %%cr3, %0" : "=r"(value));
    return value;
}

static void write_cr3(uint32_t value)
{
    asm volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

static uint32_t rol32(uint32_t value, uint32_t count)
{
    count &= 31u;
    return count == 0 ? value : (value << count) | (value >> (32u - count));
}

static uint32_t ror32(uint32_t value, uint32_t count)
{
    count &= 31u;
    return count == 0 ? value : (value >> count) | (value << (32u - count));
}

static void install_identity_4k_paging(void)
{
    for (uint32_t i = 0; i < 1024u; i++)
    {
        page_dir[i] = 0;
        page_table[i] = (i << 12) | X86_PTE_P | X86_PTE_W;
    }

    page_dir[0] = (uint32_t)page_table | X86_PTE_P | X86_PTE_W;
}

static void init_operands(void)
{
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

    for (uint32_t i = 0; i < 6u; i++)
    {
        result_words[i] = 0;
    }

    result_bytes[0] = 0;
    result_bytes[1] = 0;
    result_bytes[2] = 0;
    shift_bytes[0] = 0x81u;
    shift_bytes[1] = 0x03u;
    shift_bytes[2] = 0x7fu;
    shift_bytes[3] = 0x80u;
    shift_bytes[4] = 0x01u;
    shift_bytes[5] = 0x12u;
    shift_bytes[6] = 0x81u;
    shift_words[0] = 0x8001u;
    shift_words[1] = 0x8001u;
    shift_words[2] = 0x0003u;
    shift_words[3] = 0x8001u;
    shift_words[4] = 0x5555u;
    shift_words[5] = 0x1234u;
    shift_words[6] = 0x1234u;
    imul16_src[0] = 7u;
    imul16_src[1] = 4u;

    for (uint32_t i = 0; i < 2u; i++)
    {
        imul16_out[i] = 0;
    }

    for (uint32_t i = 0; i < 4u; i++)
    {
        imul16_flags[i] = 0;
    }

    mul_acc_src[0] = 7u;
    mul_acc_src[1] = 0x0010u;
    mul_acc_src[2] = 7u;
    mul_acc_src[3] = 4u;

    for (uint32_t i = 0; i < 4u; i++)
    {
        mul_acc_out[i] = 0;
    }

    for (uint32_t i = 0; i < 8u; i++)
    {
        mul_acc_flags[i] = 0;
    }
}

static void run_paged_helpers(void)
{
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
        "rolb $1, %[rolb1]\n\t"
        "rorb $1, %[rorb1]\n\t"
        "shlb $1, %[shlb1]\n\t"
        "shrw $1, %[shrw1]\n\t"
        "sarw $1, %[sarw1]\n\t"
        : [rolb1] "+m"(shift_bytes[0]),
          [rorb1] "+m"(shift_bytes[1]),
          [shlb1] "+m"(shift_bytes[2]),
          [shrw1] "+m"(shift_words[0]),
          [sarw1] "+m"(shift_words[1])
        :
        : "cc", "memory");

    asm volatile(
        "movl $1, %%ecx\n\t"
        "shlb %%cl, %[shlbcl1]\n\t"
        "rorb %%cl, %[rorbcl1]\n\t"
        "rolw %%cl, %[rolwcl1]\n\t"
        "sarw %%cl, %[sarwcl1]\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "shrw %%cl, %[shrwcl0]\n\t"
        : [shlbcl1] "+m"(shift_bytes[3]),
          [rorbcl1] "+m"(shift_bytes[4]),
          [rolwcl1] "+m"(shift_words[2]),
          [sarwcl1] "+m"(shift_words[3]),
          [shrwcl0] "+m"(shift_words[4])
        :
        : "ecx", "cc", "memory");

    asm volatile(
        "shlb $4, %[shlb4]\n\t"
        "sarb $3, %[sarb3]\n\t"
        "shrw $3, %[shrw3]\n\t"
        "rolw $4, %[rolw4]\n\t"
        : [shlb4] "+m"(shift_bytes[5]),
          [sarb3] "+m"(shift_bytes[6]),
          [shrw3] "+m"(shift_words[5]),
          [rolw4] "+m"(shift_words[6])
        :
        : "cc", "memory");

    asm volatile(
        "movl $0x12340003, %%eax\n\t"
        "imulw %[src0], %%ax\n\t"
        "setc %[cf0]\n\t"
        "seto %[of0]\n\t"
        "movl %%eax, %[out0]\n\t"
        "movl $0xabcd4000, %%eax\n\t"
        "imulw %[src1], %%ax\n\t"
        "setc %[cf1]\n\t"
        "seto %[of1]\n\t"
        "movl %%eax, %[out1]\n\t"
        : [out0] "=m"(imul16_out[0]),
          [out1] "=m"(imul16_out[1]),
          [cf0] "=m"(imul16_flags[0]),
          [of0] "=m"(imul16_flags[1]),
          [cf1] "=m"(imul16_flags[2]),
          [of1] "=m"(imul16_flags[3])
        : [src0] "m"(imul16_src[0]),
          [src1] "m"(imul16_src[1])
        : "eax", "cc", "memory");

    asm volatile(
        "movl $3, %%eax\n\t"
        "mulb %[umul8_src]\n\t"
        "setc %[umul8_cf]\n\t"
        "seto %[umul8_of]\n\t"
        "movzwl %%ax, %%eax\n\t"
        "movl %%eax, %[umul8_out]\n\t"
        "movl $0x1000, %%eax\n\t"
        "mulw %[umul16_src]\n\t"
        "setc %[umul16_cf]\n\t"
        "seto %[umul16_of]\n\t"
        "movzwl %%ax, %%eax\n\t"
        "movzwl %%dx, %%edx\n\t"
        "shll $16, %%edx\n\t"
        "orl %%edx, %%eax\n\t"
        "movl %%eax, %[umul16_out]\n\t"
        "movl $0xfd, %%eax\n\t"
        "imulb %[imul8_src]\n\t"
        "setc %[imul8_cf]\n\t"
        "seto %[imul8_of]\n\t"
        "movzwl %%ax, %%eax\n\t"
        "movl %%eax, %[imul8_out]\n\t"
        "movl $0x4000, %%eax\n\t"
        "imulw %[imul16_src]\n\t"
        "setc %[imul16_cf]\n\t"
        "seto %[imul16_of]\n\t"
        "movzwl %%ax, %%eax\n\t"
        "movzwl %%dx, %%edx\n\t"
        "shll $16, %%edx\n\t"
        "orl %%edx, %%eax\n\t"
        "movl %%eax, %[imul16_out]\n\t"
        : [umul8_out] "=m"(mul_acc_out[0]),
          [umul16_out] "=m"(mul_acc_out[1]),
          [imul8_out] "=m"(mul_acc_out[2]),
          [imul16_out] "=m"(mul_acc_out[3]),
          [umul8_cf] "=m"(mul_acc_flags[0]),
          [umul8_of] "=m"(mul_acc_flags[1]),
          [umul16_cf] "=m"(mul_acc_flags[2]),
          [umul16_of] "=m"(mul_acc_flags[3]),
          [imul8_cf] "=m"(mul_acc_flags[4]),
          [imul8_of] "=m"(mul_acc_flags[5]),
          [imul16_cf] "=m"(mul_acc_flags[6]),
          [imul16_of] "=m"(mul_acc_flags[7])
        : [umul8_src] "m"(mul_acc_src[0]),
          [umul16_src] "m"(mul_acc_src[1]),
          [imul8_src] "m"(mul_acc_src[2]),
          [imul16_src] "m"(mul_acc_src[3])
        : "eax", "edx", "cc", "memory");

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

int main()
{
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
    check(shift_bytes[0] == 0x03u);
    check(shift_bytes[1] == 0x81u);
    check(shift_bytes[2] == 0xfeu);
    check(shift_bytes[3] == 0x00u);
    check(shift_bytes[4] == 0x80u);
    check(shift_bytes[5] == 0x20u);
    check(shift_bytes[6] == 0xf0u);
    check(shift_words[0] == 0x4000u);
    check(shift_words[1] == 0xc000u);
    check(shift_words[2] == 0x0006u);
    check(shift_words[3] == 0xc000u);
    check(shift_words[4] == 0x5555u);
    check(shift_words[5] == 0x0246u);
    check(shift_words[6] == 0x2341u);
    check(imul16_out[0] == 0x12340015u);
    check(imul16_flags[0] == 0);
    check(imul16_flags[1] == 0);
    check(imul16_out[1] == 0xabcd0000u);
    check(imul16_flags[2] == 1);
    check(imul16_flags[3] == 1);
    check(mul_acc_out[0] == 0x0015u);
    check(mul_acc_flags[0] == 0);
    check(mul_acc_flags[1] == 0);
    check(mul_acc_out[1] == 0x00010000u);
    check(mul_acc_flags[2] == 1);
    check(mul_acc_flags[3] == 1);
    check(mul_acc_out[2] == 0xffebu);
    check(mul_acc_flags[4] == 0);
    check(mul_acc_flags[5] == 0);
    check(mul_acc_out[3] == 0x00010000u);
    check(mul_acc_flags[6] == 1);
    check(mul_acc_flags[7] == 1);
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
