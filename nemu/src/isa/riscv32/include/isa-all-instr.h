#include <cpu/decode.h>
#include "../local-include/rtl.h"

// Step 3. Add your instruction to this list.
#define INSTR_LIST(f) \
    f(lui) \
    f(lw)  \
    f(sw)  \
    f(inv) \
    f(addi) \
    f(auipc) \
    f(jal) \
    f(jalr) \
    f(nemu_trap)

def_all_EXEC_ID();
