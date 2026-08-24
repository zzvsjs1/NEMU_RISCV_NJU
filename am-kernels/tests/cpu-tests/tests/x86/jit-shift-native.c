#include "trap.h"

static uint8_t shift_byte_mem __attribute__((aligned(4096)));
static uint16_t shift_word_mem __attribute__((aligned(4096)));
static uint32_t shift_mem[3] __attribute__((aligned(4096)));

int main()
{
    uint32_t shl_cl = 0;
    uint32_t shr_cl = 0;
    uint32_t sar_cl = 0;
    uint8_t shl_of = 0;
    uint8_t sar_sf = 0;
    uint8_t byte_cf = 0;
    uint8_t byte_zf = 0;
    uint8_t word_cf = 0;
    uint8_t word_sf = 0;
    uint16_t word_reg_shr = 0;
    uint8_t word_reg_cf = 0;
    uint8_t word_reg_zf = 0;
    uint8_t byte_reg_shr = 0;
    uint8_t byte_reg_cf = 0;
    uint8_t byte_reg_zf = 0;

    shift_byte_mem = 0x88u;
    shift_word_mem = 0x8008u;
    shift_mem[0] = 0x40000000u;
    shift_mem[1] = 0x80000001u;
    shift_mem[2] = 0x80000000u;

    asm volatile("movl $0x40000000, %%eax\n\t"
                 "movl $1, %%ecx\n\t"
                 "shll %%cl, %%eax\n\t"
                 "seto %[shl_of]\n\t"
                 "movl %%eax, %[shl_cl]\n\t"
                 "movl $0x80000000, %%eax\n\t"
                 "movl $4, %%ecx\n\t"
                 "shrl %%cl, %%eax\n\t"
                 "movl %%eax, %[shr_cl]\n\t"
                 "movl $0x80000000, %%eax\n\t"
                 "movl $4, %%ecx\n\t"
                 "sarl %%cl, %%eax\n\t"
                 "sets %[sar_sf]\n\t"
                 "movl %%eax, %[sar_cl]\n\t"
                 "shll $1, %[mem_shl]\n\t"
                 "movl $4, %%ecx\n\t"
                 "shrl %%cl, %[mem_shr]\n\t"
                 "sarl $4, %[mem_sar]"
                 : [shl_cl] "=m"(shl_cl), [shr_cl] "=m"(shr_cl), [sar_cl] "=m"(sar_cl), [shl_of] "=qm"(shl_of), [sar_sf] "=qm"(sar_sf),
                   [mem_shl] "+m"(shift_mem[0]), [mem_shr] "+m"(shift_mem[1]), [mem_sar] "+m"(shift_mem[2])
                 :
                 : "eax", "ecx", "memory", "cc");

    asm volatile("movl $4, %%ecx\n\t"
                 "shrb %%cl, %[byte_shr]\n\t"
                 "setc %[byte_cf]\n\t"
                 "setz %[byte_zf]\n\t"
                 "movl $4, %%ecx\n\t"
                 "sarw %%cl, %[word_sar]\n\t"
                 "setc %[word_cf]\n\t"
                 "sets %[word_sf]"
                 : [byte_shr] "+m"(shift_byte_mem), [word_sar] "+m"(shift_word_mem), [byte_cf] "=qm"(byte_cf), [byte_zf] "=qm"(byte_zf),
                   [word_cf] "=qm"(word_cf), [word_sf] "=qm"(word_sf)
                 :
                 : "ecx", "memory", "cc");

    asm volatile("movw $0x8008, %%ax\n\t"
                 "shrw $4, %%ax\n\t"
                 "setc %[word_cf]\n\t"
                 "setz %[word_zf]\n\t"
                 "movw %%ax, %[word_out]\n\t"
                 "movb $0x88, %%al\n\t"
                 "shrb $4, %%al\n\t"
                 "setc %[byte_cf]\n\t"
                 "setz %[byte_zf]\n\t"
                 "movb %%al, %[byte_out]"
                 : [word_out] "=m"(word_reg_shr), [word_cf] "=qm"(word_reg_cf), [word_zf] "=qm"(word_reg_zf), [byte_out] "=m"(byte_reg_shr),
                   [byte_cf] "=qm"(byte_reg_cf), [byte_zf] "=qm"(byte_reg_zf)
                 :
                 : "eax", "memory", "cc");

    check(shl_cl == 0x80000000u);
    check(shl_of == 1);
    check(shr_cl == 0x08000000u);
    check(sar_cl == 0xf8000000u);
    check(sar_sf == 1);
    check(shift_mem[0] == 0x80000000u);
    check(shift_mem[1] == 0x08000000u);
    check(shift_mem[2] == 0xf8000000u);
    check(shift_byte_mem == 0x08u);
    check(byte_cf == 1);
    check(byte_zf == 0);
    check(shift_word_mem == 0xf800u);
    check(word_cf == 1);
    check(word_sf == 1);
    check(word_reg_shr == 0x0800u);
    check(word_reg_cf == 1);
    check(word_reg_zf == 0);
    check(byte_reg_shr == 0x08u);
    check(byte_reg_cf == 1);
    check(byte_reg_zf == 0);
    return 0;
}
