#include <isa.h>
#include "local-include/reg.h"
#include <stdio.h> // printf

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
    {0x180, "satp"},
    {0x300, "mstatus"},
    {0x305, "mtvec"},
    {0x340, "mscratch"},
    {0x341, "mepc"},
    {0x342, "mcause"},
    {0x343, "mtval"},
};

static size_t csr_list_len(void)
{
    return sizeof(csr_list) / sizeof(csr_list[0]);
}

static bool str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

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

    for (int i = 0; i < ARRLEN(regs); i++)
    {
        if (str_eq(regs[i], name))
        {
            return i;
        }
    }

    /*
     * Accept architectural xN aliases as a small RV64 quality-of-life path.
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
            if (idx >= ARRLEN(regs))
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

word_t getCSRValue(const word_t address)
{
    return (word_t)(*getCSRAddress(address));
}

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

void isa_reg_display()
{
    printf("\n");

    for (int i = 0; i < ARRLEN(regs); i++)
    {
        const word_t v = gpr(i);
        printf(REG_FMT, reg_name(i, 8), v, " ", v, " ", (sword_t)v);
    }

    {
        const word_t pc = ((riscv64_CPU_state *)&cpu)->pc;
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
        return ((riscv64_CPU_state *)&cpu)->pc;
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

void isa_set_reg_val(const char *name, const word_t val)
{
    if (name == NULL)
    {
        PRI_ERR_E("Failed to set value, register name is NULL.\n");
        return;
    }

    if (str_eq("pc", name))
    {
        ((riscv64_CPU_state *)&cpu)->pc = val;
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
        *getCSRAddress(csr_addr) = (csr_addr == 0x300) ? riscv64_mstatus_normalise(val) : val;
        return;
    }

    PRI_ERR("Failed to set value, unknown register or CSR %s.\n", name);
}
