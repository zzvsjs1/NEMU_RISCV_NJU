#include <cpu/cpu.h>
#include <cpu/exec.h>
#include <cpu/difftest.h>
#include <inttypes.h>
#if !defined(CONFIG_ISA_riscv32) && !defined(CONFIG_ISA_riscv64) && !defined(CONFIG_ISA_x86)
#include <isa-all-instr.h>
#endif
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64) || defined(CONFIG_ISA_x86)
#include <isa-jit.h>
#endif
#include <locale.h>
#include <stdlib.h>

/* The assembly code of instructions executed is only output to the screen
 * when the number of instructions executed is less than this value.
 * This is useful when you use the `si' command.
 * You can modify this value as you want.
 */
#define MAX_INSTR_TO_PRINT 10
/*
 * device_update() itself is wall-clock gated to TIMER_HZ, so checking it after
 * every tiny translated batch mostly pays repeated get_time() and dispatch
 * overhead.  Keep this value a power of two: should_update_device_after() uses a
 * cheap mask to carry the leftover guest-instruction count after each check.
 */
#define DEVICE_UPDATE_CHECK_INTERVAL 65536u

CPU_state cpu = {0};
uint64_t g_nr_guest_instr = 0;
static uint64_t g_timer = 0; // unit: us
static bool g_print_step = false;
const rtlreg_t rzero = 0;
rtlreg_t tmp_reg[6];

void device_update();
#if !defined(CONFIG_ISA_riscv32) && !defined(CONFIG_ISA_riscv64) && !defined(CONFIG_ISA_x86)
void fetch_decode(Decode *s, vaddr_t pc);
#endif

/*
 * Finish one interpreted instruction after cpu.pc has been updated.  Tracing,
 * DiffTest, and watchpoints all need the old pc from Decode plus the committed
 * next pc, so this helper is deliberately called after fetch/decode/execute but
 * before device polling and interrupt delivery.
 */
static void trace_and_difftest(Decode *_this, vaddr_t dnpc)
{
#ifdef CONFIG_IRINGBUF
    /* Record every decoded itrace line so aborts can print recent history. */
    trace_iringbuf_record(_this->logbuf);
#endif

#ifdef CONFIG_ITRACE_COND

    if (ITRACE_COND)
    {
        log_write("%s\n", _this->logbuf);
    }

#endif

    if (g_print_step)
    {
        IFDEF(CONFIG_ITRACE, puts(_this->logbuf));
    }

    IFDEF(CONFIG_DIFFTEST, difftest_step(_this->pc, dnpc));

#ifdef CONFIG_WATCHPOINT
    bool checkEachWpAndPrint();

    if (checkEachWpAndPrint())
    {
        nemu_state.state = NEMU_STOP;
    }
#endif
}

#if !defined(CONFIG_ISA_riscv32) && !defined(CONFIG_ISA_riscv64) && !defined(CONFIG_ISA_x86)
#include <isa-exec.h>

/*
 * Table-interpreter ISAs decode to a small execution id first.  The table keeps
 * the old helper API available for disassembly or external decode users, while
 * exec_decoded_instr() below uses a switch so normal builds do not rely on
 * computed function-pointer calls.
 */
#define FILL_EXEC_TABLE(name) [concat(EXEC_ID_, name)] = concat(exec_, name),
static const void *g_exec_table[TOTAL_INSTR] = {
    MAP(INSTR_LIST, FILL_EXEC_TABLE)};

#define EXEC_DECODED_CASE(name) \
    case concat(EXEC_ID_, name): \
        concat(exec_, name)(s); \
        return;

static inline void exec_decoded_instr(int idx, Decode *s)
{
    /*
     * Execute the helper selected by fetch_decode_idx().  Any unknown id falls
     * back to exec_inv(), which gives the ISA-specific invalid-instruction path
     * rather than silently advancing the PC.
     */
    switch (idx)
    {
        MAP(INSTR_LIST, EXEC_DECODED_CASE)
    default:
        exec_inv(s);
        return;
    }
}

static inline int fetch_decode_idx(Decode *s, vaddr_t pc)
{
    /*
     * The table path fetches through isa_fetch_decode().  snpc starts at pc and
     * is advanced by each ISA's fetch helper; dnpc defaults to that sequential
     * address until an instruction helper overrides it.
     */
    s->pc = pc;
    s->snpc = pc;
    int idx = isa_fetch_decode(s);
    s->dnpc = s->snpc;
#ifdef CONFIG_ITRACE
    char *p = s->logbuf;
    p += snprintf(p, sizeof(s->logbuf), FMT_WORD ":", s->pc);
    int ilen = s->snpc - s->pc;
    int i;
    uint8_t *instr = (uint8_t *)&s->isa.instr.val;

    for (i = 0; i < ilen; i++)
    {
        p += snprintf(p, 4, " %02x", instr[i]);
    }

    int ilen_max = MUXDEF(CONFIG_ISA_x86, 8, 4);
    int space_len = ilen_max - ilen;

    if (space_len < 0)
    {

        space_len = 0;
    }

    space_len = space_len * 3 + 1;
    memset(p, ' ', space_len);
    p += space_len;

    void disassemble(char *str, int size, uint64_t pc, uint8_t *code, int nbyte);
    disassemble(p, s->logbuf + sizeof(s->logbuf) - p,
                MUXDEF(CONFIG_ISA_x86, s->snpc, s->pc), (uint8_t *)&s->isa.instr.val, ilen);
#endif
    return idx;
}

static inline void fetch_decode_exec_updatepc(Decode *s)
{
    /*
     * Table-interpreter step: decode the current instruction, run the selected
     * helper, then publish dnpc as the architectural pc.
     */
    int idx = fetch_decode_idx(s, cpu.pc);
    exec_decoded_instr(idx, s);
    cpu.pc = s->dnpc;
}
#else
static inline void fetch_decode_exec_updatepc(Decode *s)
{
    /*
     * Direct-interpreter step: isa_exec_once() owns both fetch and
     * execution, so the common CPU loop only copies the committed dnpc into
     * cpu.pc and formats the trace line afterwards. RISC-V uses this for its
     * pattern interpreter; PA x86 also uses it because x86 length decode is
     * interleaved with instruction fetch.
     */
    s->pc = cpu.pc;
    s->snpc = cpu.pc;
    isa_exec_once(s);
    cpu.pc = s->dnpc;
#ifdef CONFIG_ITRACE
    char *p = s->logbuf;
    p += snprintf(p, sizeof(s->logbuf), FMT_WORD ":", s->pc);
    int ilen = s->snpc - s->pc;
    int i;
    uint8_t *inst = (uint8_t *)&s->isa.inst;

#if defined(CONFIG_ISA_riscv64) || defined(CONFIG_ISA_x86)
    /*
     * RV64 and x86 conventionally print fetched bytes in memory order. RV32
     * keeps its older word-order display below for trace compatibility.
     */
    for (i = 0; i < ilen; i++)
    {
        p += snprintf(p, 4, " %02x", inst[i]);
    }
#else
    for (i = ilen - 1; i >= 0; i--)
    {
        p += snprintf(p, 4, " %02x", inst[i]);
    }
#endif

    int ilen_max = MUXDEF(CONFIG_ISA_x86, 8, 4);
    int space_len = ilen_max - ilen;

    if (space_len < 0)
    {
        space_len = 0;
    }

    space_len = space_len * 3 + 1;
    memset(p, ' ', space_len);
    p += space_len;

    void disassemble(char *str, int size, uint64_t pc, uint8_t *code, int nbyte);
    disassemble(p, s->logbuf + sizeof(s->logbuf) - p,
                s->pc, (uint8_t *)&s->isa.inst, ilen);
#endif
}
#endif

/*
 * Print aggregate execution counters when NEMU stops.  The timer is accumulated
 * across cpu_exec() calls, so repeated `si' commands still contribute to the
 * final simulated instruction frequency.
 */
static void statistic()
{
    IFNDEF(CONFIG_TARGET_AM, setlocale(LC_NUMERIC, ""));
#define NUMBERIC_FMT MUXDEF(CONFIG_TARGET_AM, "%ld", "%'ld")
    Log("host time spent = " NUMBERIC_FMT " us", g_timer);
    Log("total guest instructions = " NUMBERIC_FMT, g_nr_guest_instr);

    if (g_timer > 0)
    {
        Log("simulation frequency = " NUMBERIC_FMT " instr/s", g_nr_guest_instr * 1000000 / g_timer);
    }
    else
    {
        Log("Finish running in less than 1 us and can not calculate the simulation frequency");
    }

#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64) || defined(CONFIG_ISA_x86)
    isa_jit_dump_stats();
#endif
}

/*
 * Assertion handling wants the same diagnostic order as an explicit abort:
 * first recent instruction history, then register state, then aggregate counts.
 */
void assert_fail_msg()
{
    /* Print the recent instruction window before register/statistic dumps. */
    trace_iringbuf_dump();
    isa_reg_display();
    statistic();
}

/* Only dump registers for failures; a good trap has already reported success. */
static bool should_dump_failure_registers()
{
    return nemu_state.state == NEMU_ABORT ||
           (nemu_state.state == NEMU_END && nemu_state.halt_ret != 0);
}

static uint64_t diagnostic_instr_limit(void)
{
    static bool parsed = false;
    static uint64_t limit = 0;

    if (!parsed)
    {
        const char *value = getenv("NEMU_EXIT_AFTER_INSTR");
        if (value != NULL && value[0] != '\0')
        {
            char *end = NULL;
            limit = strtoull(value, &end, 0);
            if (end == value || *end != '\0')
            {
                limit = 0;
            }
        }
        parsed = true;
    }

    return limit;
}

#if !defined(CONFIG_ISA_riscv32) && !defined(CONFIG_ISA_riscv64) && !defined(CONFIG_ISA_x86)
void fetch_decode(Decode *s, vaddr_t pc)
{
    /*
     * External users can request a decode without executing it.  Store the
     * helper pointer selected by the table so callers can inspect or run it
     * later under their own control.
     */
    int idx = fetch_decode_idx(s, pc);
    s->EHelper = g_exec_table[idx];
}
#endif

/*
 * Ask the ISA layer for a pending interrupt.  RISC-V keeps a cheap latched flag
 * in CPU_state so the hot JIT path avoids inspecting CSRs when no interrupt is
 * pending.
 */
static inline word_t query_pending_intr()
{
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
    /*
     * Most JIT batches have no pending interrupt. Avoid the heavier ISA
     * query unless the latched CPU flag says there is real work to inspect.
     */

    if (likely(!cpu.INTR))
    {
        return INTR_EMPTY;
    }
#endif
    return isa_query_intr();
}

#ifdef CONFIG_DEVICE
static inline bool should_update_device_after(uint32_t *counter, uint32_t executed)
{
    /*
     * JIT blocks may retire more than one guest instruction per loop iteration.
     * Accounting in batches keeps device polling close to interpreter timing
     * without forcing a return after every translated instruction.
     */
    *counter += executed;

    if (*counter >= DEVICE_UPDATE_CHECK_INTERVAL || g_print_step)
    {
        *counter &= (DEVICE_UPDATE_CHECK_INTERVAL - 1u);
        return true;
    }

    return false;
}
#endif

/*
 * Decide whether the fast JIT path is allowed for this cpu_exec() call.
 * Any feature that needs exact per-instruction hooks forces the interpreter so
 * trace logs, watchpoints, memory traces, and DiffTest keep precise behaviour.
 */
static inline bool can_jit_exec()
{
#if (defined(CONFIG_RV32_JIT) || defined(CONFIG_RV64_JIT) || defined(CONFIG_X86_JIT)) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
    /*
     * The JIT bypasses per-instruction Decode objects, so keep it behind the
     * instrumentation boundary. If exact hooks are needed, the interpreter
     * remains the source of behaviour.
     */
    return !g_print_step && isa_jit_available();
#else
    return false;
#endif
}

/*
 * Simulate how the CPU works for at most n guest instructions.  Each loop
 * iteration either retires one interpreter instruction or a JIT batch, then
 * performs the common post-instruction duties: accounting, tracing/DiffTest,
 * device polling, and interrupt delivery.
 */
void cpu_exec(uint64_t n)
{
    g_print_step = n < MAX_INSTR_TO_PRINT;
    const uint64_t instr_limit = diagnostic_instr_limit();

    switch (nemu_state.state)
    {
    case NEMU_END:
    case NEMU_ABORT:
    case NEMU_QUIT:
        printf("Program execution has ended. To restart the program, exit NEMU and run again.\n");
        return;
    default:
        nemu_state.state = NEMU_RUNNING;
    }

    uint64_t timer_start = get_time();

    Decode s;
#ifdef CONFIG_DEVICE
    uint32_t device_update_counter = 0;
#endif
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64) || defined(CONFIG_ISA_x86)
    const bool jit_exec = can_jit_exec();
#endif

    while (n > 0)
    {
        if (instr_limit != 0)
        {
            if (g_nr_guest_instr >= instr_limit)
            {
                nemu_state.state = NEMU_QUIT;
                break;
            }

            const uint64_t remaining_to_limit = instr_limit - g_nr_guest_instr;
            if (n > remaining_to_limit)
            {
                n = remaining_to_limit;
            }
        }

        uint32_t executed = 0;
        bool jit_done = false;
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64) || defined(CONFIG_ISA_x86)
        if (jit_exec)
        {
            /*
             * device_budget is the remaining instruction count before the next
             * mandatory device update. The JIT must not run past it, otherwise
             * timers and DMA-visible device state could lag behind the
             * interpreter's observable schedule. The JIT has its own smaller
             * block and batch caps, but this outer budget is the one tied to
             * NEMU's device polling contract.
             */
            uint32_t device_budget = UINT32_MAX;
#ifdef CONFIG_DEVICE
            device_budget = DEVICE_UPDATE_CHECK_INTERVAL - device_update_counter;
#endif
            jit_done = isa_jit_exec(n, device_budget, &executed);
        }
#endif

        if (jit_done)
        {
            /*
             * A successful JIT call has already updated cpu.pc inside generated
             * code or helper exits. cpu_exec() only updates global accounting
             * here, then performs the same device and interrupt checks used by
             * the interpreter path. Keeping this ownership in cpu_exec() is why
             * translated blocks return an instruction count instead of polling
             * devices or raising interrupts themselves.
             */
            Assert(executed > 0, "JIT reported success without executing instructions");
            n -= executed;
            g_nr_guest_instr += executed;
        }
        else
        {
#if defined(CONFIG_ISA_x86)
            vaddr_t fault_pc = cpu.pc;
            x86_exception_env_valid = true;
            if (setjmp(x86_exception_env) == 0)
            {
                fetch_decode_exec_updatepc(&s);
                x86_exception_env_valid = false;
                executed = 1;
                n -= executed;
                g_nr_guest_instr += executed;
                trace_and_difftest(&s, cpu.pc);
            }
            else
            {
                x86_mmu_clear_cpl_override();
                x86_exception_env_valid = false;
                cpu.pc = x86_exception_target;
                executed = 1;
                n -= executed;
                g_nr_guest_instr += executed;
                difftest_skip_ref();
                difftest_step(fault_pc, cpu.pc);
            }
#else
            fetch_decode_exec_updatepc(&s);
            executed = 1;
            n -= executed;
            g_nr_guest_instr += executed;
            trace_and_difftest(&s, cpu.pc);
#endif
        }

        if (nemu_state.state != NEMU_RUNNING)
        {
            break;
        }

        if (instr_limit != 0 && g_nr_guest_instr >= instr_limit)
        {
            Log("diagnostic instruction limit reached at %" PRIu64 " guest instructions",
                g_nr_guest_instr);
            nemu_state.state = NEMU_QUIT;
            break;
        }

#ifdef CONFIG_DEVICE
        if (should_update_device_after(&device_update_counter, executed))
        {
            device_update();
        }
#endif

        word_t intr = query_pending_intr();

        if (intr != INTR_EMPTY)
        {
            cpu.pc = isa_raise_intr(intr, cpu.pc);
        }
    }

    uint64_t timer_end = get_time();
    g_timer += timer_end - timer_start;

    switch (nemu_state.state)
    {
    case NEMU_RUNNING:
        nemu_state.state = NEMU_STOP;
        break;
    case NEMU_END:
    case NEMU_ABORT:
        /* NEMU_ABORT may not go through Assert(), so dump the ring here too. */
        IFDEF(CONFIG_IRINGBUF, trace_iringbuf_dump());
        Log("nemu: %s at pc = " FMT_WORD,
            (nemu_state.state == NEMU_ABORT ? ANSI_FMT("ABORT", ANSI_FG_RED) : (nemu_state.halt_ret == 0 ? ANSI_FMT("HIT GOOD TRAP", ANSI_FG_GREEN) : ANSI_FMT("HIT BAD TRAP", ANSI_FG_RED))),
            nemu_state.halt_pc);
        if (should_dump_failure_registers())
        {
            isa_reg_display();
        }
        // fall through
    case NEMU_QUIT:
        statistic();
    }
}
