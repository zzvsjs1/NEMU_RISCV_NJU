#include "common.h"
#include <difftest-def.h>
#include <sys/prctl.h>
#include <signal.h>

bool gdb_connect_qemu(int);
bool gdb_memcpy_to_qemu(uint32_t, void *, int);
bool gdb_getregs(union isa_gdb_regs *);
bool gdb_setregs(union isa_gdb_regs *);
bool gdb_si();
void gdb_exit();

void init_isa();

#if defined(CONFIG_ISA_x86)
#define X86_EFLAGS_ALWAYS_ON (1u << 1)

typedef struct
{
    union
    {
        uint32_t gpr[8];
        struct
        {
            uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
        };
    };
    uint32_t eflags;
    uint32_t pc;
    uint32_t cs;
} x86_nemu_state;

static void x86_regcpy_to_qemu(union isa_gdb_regs *qemu_r, const x86_nemu_state *dut)
{
    qemu_r->eax = dut->eax;
    qemu_r->ecx = dut->ecx;
    qemu_r->edx = dut->edx;
    qemu_r->ebx = dut->ebx;
    qemu_r->esp = dut->esp;
    qemu_r->ebp = dut->ebp;
    qemu_r->esi = dut->esi;
    qemu_r->edi = dut->edi;
    qemu_r->eip = dut->pc;
    qemu_r->eflags = dut->eflags | X86_EFLAGS_ALWAYS_ON;
    qemu_r->cs = dut->cs;
}

static void x86_regcpy_to_dut(x86_nemu_state *dut, const union isa_gdb_regs *qemu_r)
{
    dut->eax = qemu_r->eax;
    dut->ecx = qemu_r->ecx;
    dut->edx = qemu_r->edx;
    dut->ebx = qemu_r->ebx;
    dut->esp = qemu_r->esp;
    dut->ebp = qemu_r->ebp;
    dut->esi = qemu_r->esi;
    dut->edi = qemu_r->edi;
    dut->pc = qemu_r->eip;
    dut->eflags = qemu_r->eflags | X86_EFLAGS_ALWAYS_ON;
    dut->cs = qemu_r->cs;
}
#endif

#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
static __attribute__((noreturn)) void riscv_qemu_difftest_unsupported(void)
{
    fprintf(stderr,
            "RISC-V qemu-diff does not provide the CSR and privilege state "
            "required by NEMU DiffTest; use spike-diff instead.\n");
    abort();
}
#endif

__EXPORT void difftest_memcpy(paddr_t addr, void *buf, size_t n, bool direction)
{
    assert(direction == DIFFTEST_TO_REF);

    if (direction == DIFFTEST_TO_REF)
    {
        bool ok = gdb_memcpy_to_qemu(addr, buf, n);
        assert(ok == 1);
    }
}

__EXPORT void difftest_regcpy(void *dut, bool direction)
{
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
    riscv_qemu_difftest_unsupported();
#else
    union isa_gdb_regs qemu_r;
    gdb_getregs(&qemu_r);

    if (direction == DIFFTEST_TO_REF)
    {
#if defined(CONFIG_ISA_x86)
        x86_regcpy_to_qemu(&qemu_r, (const x86_nemu_state *)dut);
#else
        memcpy(&qemu_r, dut, DIFFTEST_REG_SIZE);
#endif
        gdb_setregs(&qemu_r);
    }
    else
    {
#if defined(CONFIG_ISA_x86)
        x86_regcpy_to_dut((x86_nemu_state *)dut, &qemu_r);
#else
        memcpy(dut, &qemu_r, DIFFTEST_REG_SIZE);
#endif
    }
#endif
}

__EXPORT void difftest_exec(uint64_t n)
{
    while (n--)
        gdb_si();
}

__EXPORT void difftest_init(int port)
{
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
    riscv_qemu_difftest_unsupported();
#else
    char buf[32];
    sprintf(buf, "tcp::%d", port);

    int ppid_before_fork = getpid();
    int pid = fork();

    if (pid == -1)
    {
        perror("fork");
        assert(0);
    }
    else if (pid == 0)
    {
        // child

        // install a parent death signal in the chlid
        int r = prctl(PR_SET_PDEATHSIG, SIGTERM);

        if (r == -1)
        {
            perror("prctl error");
            assert(0);
        }

        if (getppid() != ppid_before_fork)
        {
            printf("parent has died!\n");
            assert(0);
        }

        close(STDIN_FILENO);
        execlp(ISA_QEMU_BIN, ISA_QEMU_BIN, ISA_QEMU_ARGS "-S", "-gdb", buf, "-nographic",
               "-serial", "none", "-monitor", "none", NULL);
        perror("exec");
        assert(0);
    }
    else
    {
        // father

        gdb_connect_qemu(port);
        printf("Connect to QEMU with %s successfully\n", buf);

        atexit(gdb_exit);

        init_isa();
    }
#endif
}

__EXPORT void difftest_raise_intr(uint64_t NO)
{
    printf("raise_intr is not supported\n");
    assert(0);
}
