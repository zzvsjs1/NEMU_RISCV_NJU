#include "trap.h"

#define X86_CR0_PG 0x80000000u
#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static uint32_t pop_stack[16] __attribute__((aligned(64)));
static volatile uint32_t memory_dest;
static volatile uint32_t after_mem_pop;
static volatile uint32_t after_esp_pop;
static volatile uint32_t after_reg_pop;
static volatile uint32_t reg_dest;

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

static void install_identity_4k_paging(void)
{
    for (uint32_t i = 0; i < 1024u; i++)
    {
        page_dir[i] = 0;
        page_table[i] = (i << 12) | X86_PTE_P | X86_PTE_W;
    }

    page_dir[0] = (uint32_t)page_table | X86_PTE_P | X86_PTE_W;
}

__attribute__((noinline)) static void run_pop_rm_cases(void)
{
    pop_stack[3] = 0x11223344u;
    pop_stack[5] = 0x55667788u;
    pop_stack[6] = 0xa5a5a5a5u;
    pop_stack[9] = 0x99aabbccu;
    memory_dest = 0;
    reg_dest = 0;

    asm volatile(
        "movl %%esp, %%edi\n\t"
        "movl %[mem_top], %%esp\n\t"
        "popl (%[mem_dst])\n\t"
        "movl %%esp, %[after_mem]\n\t"
        "movl %[esp_top], %%esp\n\t"
        "popl (%%esp)\n\t"
        "movl %%esp, %[after_esp]\n\t"
        "movl %[reg_top], %%esp\n\t"
        ".byte 0x8f, 0xc0\n\t"
        "movl %%eax, %[reg_out]\n\t"
        "movl %%esp, %[after_reg]\n\t"
        "movl %%edi, %%esp"
        : [after_mem] "=m"(after_mem_pop),
          [after_esp] "=m"(after_esp_pop),
          [after_reg] "=m"(after_reg_pop),
          [reg_out] "=m"(reg_dest)
        : [mem_top] "r"(&pop_stack[3]),
          [esp_top] "r"(&pop_stack[5]),
          [reg_top] "r"(&pop_stack[9]),
          [mem_dst] "r"(&memory_dest)
        : "eax", "edi", "cc", "memory");
}

int main()
{
    uint32_t old_cr0 = read_cr0();
    uint32_t old_cr3 = read_cr3();

    install_identity_4k_paging();

    write_cr3((uint32_t)page_dir);
    write_cr0(old_cr0 | X86_CR0_PG);
    run_pop_rm_cases();
    write_cr0(old_cr0);
    write_cr3(old_cr3);

    check(memory_dest == 0x11223344u);
    check(after_mem_pop == (uint32_t)&pop_stack[4]);
    check(pop_stack[6] == 0x55667788u);
    check(after_esp_pop == (uint32_t)&pop_stack[6]);
    check(reg_dest == 0x99aabbccu);
    check(after_reg_pop == (uint32_t)&pop_stack[10]);
    return 0;
}
