#ifndef __ISA_H__
#define __ISA_H__

// Located at src/isa/$(GUEST_ISA)/include/isa-def.h
#include <isa-def.h>
#if defined(CONFIG_ISA_x86) || defined(CONFIG_ISA_mips32)
#include <setjmp.h>
#endif

// The macro `__GUEST_ISA__` is defined in $(CFLAGS).
// It will be expanded as "x86" or "mips32" ...
typedef concat(__GUEST_ISA__, _CPU_state) CPU_state;
typedef concat(__GUEST_ISA__, _ISADecodeInfo) ISADecodeInfo;

// monitor
extern char isa_logo[];
void init_isa();

// reg
extern CPU_state cpu;
void isa_reg_display();
word_t isa_reg_str2val(const char *name, bool *success);
void isa_set_reg_val(const char *name, const word_t val);

// exec
struct Decode;
int isa_exec_once(struct Decode *s);
int isa_fetch_decode(struct Decode *s);

// memory
enum
{
    MMU_DIRECT,
    MMU_TRANSLATE,
    MMU_FAIL,
    MMU_DYNAMIC
};

enum
{
    MEM_TYPE_IFETCH,
    MEM_TYPE_READ,
    MEM_TYPE_WRITE
};

enum
{
    MEM_RET_OK,
    MEM_RET_FAIL,
    MEM_RET_CROSS_PAGE
};

#ifndef isa_mmu_check
int isa_mmu_check(vaddr_t vaddr, int len, int type);
#endif
paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type);

// interrupt/exception
vaddr_t isa_raise_intr(word_t NO, vaddr_t epc);

#if defined(CONFIG_ISA_x86)
vaddr_t isa_raise_intr_sw(word_t NO, vaddr_t epc);
vaddr_t isa_raise_intr_err(word_t NO, vaddr_t epc, word_t errcode);
extern jmp_buf x86_exception_env;
extern bool x86_exception_env_valid;
extern vaddr_t x86_exception_target;
void x86_mmu_clear_cpl_override(void);
void x86_raise_page_fault(void) __attribute__((noreturn));
void x86_seg_set_flat(int idx, uint16_t selector);
void x86_seg_load_from_descriptor(int idx, uint16_t selector, uint32_t lo, uint32_t hi);
#endif

#if defined(CONFIG_ISA_mips32)
extern jmp_buf mips32_exception_env;
extern bool mips32_exception_env_valid;
extern vaddr_t mips32_exception_target;
void mips32_raise_tlb_exception(void) __attribute__((noreturn));
#endif

#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
vaddr_t isa_raise_intr_tval(word_t NO, vaddr_t epc, word_t tval);
#endif

#define INTR_EMPTY ((word_t) - 1)
word_t isa_query_intr();

// difftest
// for dut
bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc);
void isa_difftest_attach();

// for ref
void isa_difftest_regcpy(void *dut, bool direction);
void isa_difftest_raise_intr(word_t NO);

#endif
