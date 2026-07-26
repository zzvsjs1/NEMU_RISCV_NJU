#include <isa.h>
#include "local-include/reg.h"
#include <stdio.h> // printf
#include <string.h>

#define REG_FMT ("%-8s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {
    "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

typedef struct
{
    word_t addr;      // CSR address.
    const char *name; // Display and debugger name.
} csr_disp_t;

static const csr_disp_t csr_list[] = {
#ifdef CONFIG_RV64_FPU
    {0x001, "fflags"},
    {0x002, "frm"},
    {0x003, "fcsr"},
#endif
    {0x180, "satp"},
    {0x300, "mstatus"},
#ifdef CONFIG_RV64_FPU
    {0x301, "misa"},
#endif
    {0x305, "mtvec"},
    {0x340, "mscratch"},
    {0x341, "mepc"},
    {0x342, "mcause"},
    {0x343, "mtval"},
};

/* Keep the CSR table length derived from the table itself to avoid drift. */
static size_t csr_list_len(void)
{
    return sizeof(csr_list) / sizeof(csr_list[0]);
}

/* Small equality wrapper keeps NULL handling explicit at the call sites. */
static bool str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/*
 * Convert a monitor/debugger register name to a GPR index.  ABI names are
 * accepted first because they are what isa_reg_display() prints; xN names are
 * accepted afterwards for users who think in architectural register numbers.
 */
static int reg_name_to_index(const char *name)
{
    if (name == NULL)
    {
        return -1;
    }

    if (str_eq(name, "0"))
    {
        return 0;
    }

    for (int i = 0; i < RISCV_GPR_NUM; i++)
    {
        if (str_eq(regs[i], name))
        {
            return i;
        }
    }

    /*
     * Accept architectural xN aliases as a small RISC-V quality-of-life path.
     * ABI aliases above remain the primary display names, matching the RV32
     * monitor output.
     */
    if (name[0] == 'x' && name[1] >= '0' && name[1] <= '9')
    {
        int idx = 0;

        for (const char *p = name + 1; *p != '\0'; p++)
        {
            if (*p < '0' || *p > '9')
            {
                return -1;
            }

            idx = idx * 10 + (*p - '0');

            if (idx >= RISCV_GPR_NUM)
            {
                return -1;
            }
        }

        if (idx >= 0)
        {
            return idx;
        }
    }

    return -1;
}

/*
 * Convert a supported CSR display name to its numeric address.  Only CSRs that
 * are modelled in CPU_state are accepted, so the monitor never fabricates state
 * for unimplemented registers.
 */
static bool csr_name_to_address(const char *name, word_t *addr)
{
    if (name == NULL || addr == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < csr_list_len(); i++)
    {
        if (str_eq(csr_list[i].name, name))
        {
            *addr = csr_list[i].addr;
            return true;
        }
    }

    return false;
}

/*
 * Read a CSR by architectural value.  FP control CSRs are overlapping views of
 * one fcsr slot, so a pointer API cannot represent them without duplicated
 * state.  Ordinary CSRs continue to use their direct backing storage below.
 */
word_t getCSRValue(const word_t address)
{
#ifdef CONFIG_RV64_FPU
    switch (address)
    {
    case 0x001:
        return cpu.fcsr & RISCV_FFLAGS_MASK;
    case 0x002:
        return (cpu.fcsr & RISCV_FRM_MASK) >> 5;
    case 0x003:
        return cpu.fcsr & RISCV_FCSR_MASK;
    case 0x301:
        /*
         * RV64 MXL=2 plus the implemented base, multiply/divide, FP, and
         * privilege-mode extension bits. Zicsr/Zifencei are not represented in
         * misa's single-letter bitmap.
         */
        return ((word_t)2u << 62) |
               ((word_t)1u << ('I' - 'A')) |
               ((word_t)1u << ('M' - 'A')) |
               ((word_t)1u << ('F' - 'A')) |
               ((word_t)1u << ('D' - 'A')) |
               ((word_t)1u << ('S' - 'A')) |
               ((word_t)1u << ('U' - 'A'));
    default:
        break;
    }
#endif

    return (word_t)(*getCSRAddress(address));
}

/*
 * Write one implemented CSR after the instruction path has checked privilege,
 * read-only status, and FS permission.  FP alias writes update only their
 * visible field and mark the shared floating-point state Dirty.
 */
void setCSRValue(const word_t address, word_t value)
{
#ifdef CONFIG_RV64_FPU
    switch (address)
    {
    case 0x001:
        cpu.fcsr = (cpu.fcsr & ~RISCV_FFLAGS_MASK) |
                   ((uint32_t)value & RISCV_FFLAGS_MASK);
        cpu.csr.mstatus =
            riscv_mstatus_mark_fp_dirty(cpu.csr.mstatus);
        return;
    case 0x002:
        cpu.fcsr = (cpu.fcsr & ~RISCV_FRM_MASK) |
                   (((uint32_t)value & 0x7u) << 5);
        cpu.csr.mstatus =
            riscv_mstatus_mark_fp_dirty(cpu.csr.mstatus);
        return;
    case 0x003:
        cpu.fcsr = (uint32_t)value & RISCV_FCSR_MASK;
        cpu.csr.mstatus =
            riscv_mstatus_mark_fp_dirty(cpu.csr.mstatus);
        return;
    case 0x301:
        /*
         * This implementation exposes a fixed maximal ISA set.  Treat writes
         * as WARL attempts that leave the supported F/D dependency intact.
         */
        return;
    default:
        break;
    }
#endif

    rtlreg_t *csr = getCSRAddress(address);
    *csr = address == 0x300 ? riscv_mstatus_normalise(value) : value;
}

/*
 * Return the storage location for an implemented CSR.  Instruction execution
 * should use value-based get/set helpers.  This legacy accessor remains for
 * ordinary storage-backed CSRs only; the Assert catches attempts to represent
 * an aliased FP CSR as a standalone pointer.
 */
rtlreg_t *getCSRAddress(const word_t address)
{
    switch (address)
    {
    case 0x180:
        return &cpu.csr.satp;
    case 0x300:
        return &cpu.csr.mstatus;
    case 0x305:
        return &cpu.csr.mtvec;
    case 0x340:
        return &cpu.csr.mscratch;
    case 0x341:
        return &cpu.csr.mepc;
    case 0x342:
        return &cpu.csr.mcause;
    case 0x343:
        return &cpu.csr.mtval;
    default:
        Assert(false, "Invalid csr address: " FMT_WORD "\n", address);
        return NULL; // Keep the compiler happy after Assert().
    }
}

/* Check whether the CSR exists in this simplified RISC-V model. */
bool isCSRImplemented(const word_t address)
{
    for (size_t i = 0; i < csr_list_len(); i++)
    {
        if (csr_list[i].addr == address)
        {
            return true;
        }
    }

    return false;
}

bool isCSRWriteable(const word_t csrAddr)
{
    /*
     * CSR bits [11:10] encode read/write capability.
     * 0b11 marks read-only CSRs; privilege checks are handled separately.
     */
    const uint32_t access_type = (csrAddr >> 10) & 0x3u;
    return access_type != 0x3u;
}

/*
 * Print GPRs, pc, and modelled CSRs in hexadecimal plus signed/unsigned decimal.
 * This is diagnostic output only; it must not normalise x0 or mutate CSR state.
 */
void isa_reg_display()
{
    printf("\n");

    for (int i = 0; i < RISCV_GPR_NUM; i++)
    {
        const word_t v = gpr(i);
        printf(REG_FMT, reg_name(i, 8), v, " ", v, " ", (sword_t)v);
    }

    {
        const word_t pc = ((riscv_CPU_state *)&cpu)->pc;
        printf(REG_FMT, "pc", pc, " ", pc, " ", (sword_t)pc);
    }

    printf("\n\n");

    for (size_t i = 0; i < csr_list_len(); i++)
    {
        const word_t v = getCSRValue(csr_list[i].addr);
        printf(REG_FMT, csr_list[i].name, v, " ", v, " ", (sword_t)v);
    }

    printf("\n\n");
}

/*
 * Resolve a monitor expression token such as "$a0", "pc", or "mstatus" to the
 * current value.  Unknown names report failure through the success flag and keep
 * the sentinel return value for legacy callers.
 */
word_t isa_reg_str2val(const char *s, bool *success)
{
    if (success == NULL)
    {
        return (word_t)-1;
    }

    if (s == NULL)
    {
        *success = false;
        PRI_ERR_E("Unknown register or CSR: NULL.\n");
        return (word_t)-1;
    }

    if (str_eq(s, "pc"))
    {
        *success = true;
        return ((riscv_CPU_state *)&cpu)->pc;
    }

    const int reg_idx = reg_name_to_index(s);

    if (reg_idx >= 0)
    {
        *success = true;
        return gpr(reg_idx);
    }

    word_t csr_addr = 0;

    if (csr_name_to_address(s, &csr_addr))
    {
        *success = true;
        return getCSRValue(csr_addr);
    }

    *success = false;
    PRI_ERR("Unknown register or CSR %s.\n", s);
    return (word_t)-1;
}

/*
 * Debugger-side register write.  It accepts the same names as reads, preserves
 * x0 as the hard-wired zero register, and applies mstatus normalisation for CSR
 * writes so monitor changes obey the same WARL rule as guest instructions.
 */
void isa_set_reg_val(const char *name, const word_t val)
{
    if (name == NULL)
    {
        PRI_ERR_E("Failed to set value, register name is NULL.\n");
        return;
    }

    if (str_eq("pc", name))
    {
        ((riscv_CPU_state *)&cpu)->pc = val;
        return;
    }

    const int reg_idx = reg_name_to_index(name);

    if (reg_idx >= 0)
    {
        // RISC-V x0 is hard-wired to zero, so debugger writes are ignored.
        if (reg_idx == 0)
        {
            return;
        }

        gpr(reg_idx) = val;
        return;
    }

    word_t csr_addr = 0;

    if (csr_name_to_address(name, &csr_addr))
    {
        setCSRValue(csr_addr, val);
        return;
    }

    PRI_ERR("Failed to set value, unknown register or CSR %s.\n", name);
}
