#ifndef __CPU_EXEC_H__
#define __CPU_EXEC_H__

#include <cpu/decode.h>

/*
 * Define a table-interpreter execution helper.  Each helper receives the Decode
 * object for the current instruction and is responsible for updating operands,
 * memory, and s->dnpc according to that ISA's semantics.
 */
#define def_EHelper(name) static inline void concat(exec_, name)(Decode * s)

#endif
