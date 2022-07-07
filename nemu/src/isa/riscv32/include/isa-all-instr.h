#include <cpu/decode.h>
#include "../local-include/rtl.h"

// Step 3. Add your instruction to this list.
#define INSTR_LIST(f) \
    f(lui) \
    f(lb) \
    f(lh)  \
    f(lw) \
    f(lbu) \
    f(lhu) \
    f(sb) \
    f(sh) \
    f(sw)  \
    f(inv) \
    f(addi) \
    f(slli) \
    f(srli) \
    f(srai) \
    f(slti) \
    f(sltiu) \
    f(xori) \
    f(ori) \
    f(andi) \
    f(auipc) \
    f(jal) \
    f(jalr) \
    f(add)  \
    f(sub) \
    f(sll) \
    f(slt) \
    f(sltu) \
    f(xor) \
    f(srl) \
    f(sra) \
    f(or) \
    f(and) \
    f(mul) \
    f(mulh) \
    f(mulhsu) \
    f(mulhu) \
    f(div) \
    f(divu) \
    f(rem) \
    f(remu) \
    f(beq) \
    f(bne) \
    f(blt) \
    f(bge) \
    f(bltu) \
    f(bgeu) \
    f(nemu_trap)

def_all_EXEC_ID();
