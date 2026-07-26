#include "trap.h"

#define X86_CR0_PG 0x80000000u
#define X86_CR4_PSE 0x00000010u
#define X86_PDE_P 0x001u
#define X86_PDE_W 0x002u
#define X86_PDE_PS 0x080u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static volatile uint32_t large_page_sink;

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

static uint32_t read_cr4(void)
{
    uint32_t value;
    asm volatile("movl %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr4(uint32_t value)
{
    asm volatile("movl %0, %%cr4" : : "r"(value) : "memory");
}

static uint32_t expected_work(uint32_t seed)
{
    uint32_t acc = seed;

    for (uint32_t i = 0; i < 64u; i++)
    {
        acc = (acc << 5) ^ (acc >> 2) ^ (0x9e3779b9u + i * 17u);
    }

    return acc;
}

static uint32_t large_page_work(uint32_t seed)
{
    uint32_t acc = seed;

    for (uint32_t i = 0; i < 64u; i++)
    {
        acc = (acc << 5) ^ (acc >> 2) ^ (0x9e3779b9u + i * 17u);
    }

    large_page_sink = acc;
    return acc;
}

int main()
{
    uint32_t old_cr0 = read_cr0();
    uint32_t old_cr3 = read_cr3();
    uint32_t old_cr4 = read_cr4();

    for (int i = 0; i < 1024; i++)
    {
        page_dir[i] = 0;
    }

    /*
     * The AM x86-nemu image is linked at 1 MiB and the test stack is placed near
     * the image, so one identity 4 MiB PDE covers code, data, the page directory,
     * and the current stack.  This deliberately uses PDE.PS so the native JIT DTLB
     * must decline the 4 KiB fast path and let the MMU helper own the translation.
     */
    page_dir[0] = X86_PDE_P | X86_PDE_W | X86_PDE_PS;

    write_cr3((uint32_t)page_dir);
    write_cr4(old_cr4 | X86_CR4_PSE);
    write_cr0(old_cr0 | X86_CR0_PG);

    uint32_t got = large_page_work(0x12345678u);

    write_cr0(old_cr0);
    write_cr4(old_cr4);
    write_cr3(old_cr3);

    check(got == expected_work(0x12345678u));
    check(large_page_sink == got);
    return 0;
}
