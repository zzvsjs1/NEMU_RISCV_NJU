#include <cpu/cpu.h>
#include <cpu/exec.h>
#include <cpu/difftest.h>
#include <isa-all-instr.h>
#ifdef CONFIG_ISA_riscv32
#include <isa-fast-exec.h>
#include <isa-jit.h>
#endif
#include <locale.h>

/* The assembly code of instructions executed is only output to the screen
 * when the number of instructions executed is less than this value.
 * This is useful when you use the `si' command.
 * You can modify this value as you want.
 */
#define MAX_INSTR_TO_PRINT 10
#define DEVICE_UPDATE_CHECK_INTERVAL 256u

CPU_state cpu = {0};
uint64_t g_nr_guest_instr = 0;
static uint64_t g_timer = 0; // unit: us
static bool g_print_step = false;
const rtlreg_t rzero = 0;
rtlreg_t tmp_reg[6];

void device_update();
void fetch_decode(Decode *s, vaddr_t pc);

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

#include <isa-exec.h>

#define FILL_EXEC_TABLE(name) [concat(EXEC_ID_, name)] = concat(exec_, name),
static const void* g_exec_table[TOTAL_INSTR] = {
    MAP(INSTR_LIST, FILL_EXEC_TABLE)
};

#define EXEC_DECODED_CASE(name) \
    case concat(EXEC_ID_, name): concat(exec_, name)(s); return;

static inline void exec_decoded_instr(int idx, Decode *s)
{
    switch (idx)
    {
        MAP(INSTR_LIST, EXEC_DECODED_CASE)
        default: exec_inv(s); return;
    }
}

static inline int fetch_decode_idx(Decode *s, vaddr_t pc)
{
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

    for (i = 0; i < ilen; i ++) 
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
    int idx = fetch_decode_idx(s, cpu.pc);
    exec_decoded_instr(idx, s);
    cpu.pc = s->dnpc;
}

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

#ifdef CONFIG_ISA_riscv32
    isa_jit_dump_stats();
#endif
}

void assert_fail_msg() 
{
    /* Print the recent instruction window before register/statistic dumps. */
    trace_iringbuf_dump();
    isa_reg_display();
    statistic();
}

void fetch_decode(Decode *s, vaddr_t pc) 
{
    int idx = fetch_decode_idx(s, pc);
    s->EHelper = g_exec_table[idx];
}

static inline word_t query_pending_intr()
{
#ifdef CONFIG_ISA_riscv32
    /*
     * Most fast/JIT batches have no pending interrupt. Avoid the heavier ISA
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

static inline bool can_fast_exec()
{
#if defined(CONFIG_ISA_riscv32) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE)
    /*
     * The fast executor bypasses the normal decode/execute helpers for common
     * RISC-V32 instructions. Keep it disabled when any tracing or checking
     * feature needs the full slow path, because those features depend on the
     * interpreter's normal instrumentation points.
     */
    return !g_print_step;
#else
    return false;
#endif
}

static inline bool can_jit_exec()
{
#if defined(CONFIG_RV32_JIT) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
    /*
     * The JIT bypasses per-instruction Decode objects, so keep it behind the
     * same instrumentation boundary as the fast executor. If exact hooks are
     * needed, the interpreter remains the source of behaviour.
     */
    return !g_print_step && isa_jit_available();
#else
    return false;
#endif
}

/* Simulate how the CPU works. */
void cpu_exec(uint64_t n) 
{
    g_print_step = n < MAX_INSTR_TO_PRINT;

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
    const bool fast_exec = can_fast_exec();
    const bool jit_exec = can_jit_exec();

    while (n > 0)
    {
        uint32_t executed = 0;
        bool jit_done = false;
#ifdef CONFIG_ISA_riscv32
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
            bool fast_done = false;
#ifdef CONFIG_ISA_riscv32
            if (fast_exec)
            {
                fast_done = isa_fast_exec_once();
            }
#endif
            if (!fast_done)
            {
                fetch_decode_exec_updatepc(&s);
            }

            n--;
            executed = 1;
            g_nr_guest_instr++;
            if (!fast_done)
            {
                trace_and_difftest(&s, cpu.pc);
            }
        }
        
        if (nemu_state.state != NEMU_RUNNING)
        {
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
                    (nemu_state.state == NEMU_ABORT ? ANSI_FMT("ABORT", ANSI_FG_RED) :
                     (nemu_state.halt_ret == 0 ? ANSI_FMT("HIT GOOD TRAP", ANSI_FG_GREEN) :
                     ANSI_FMT("HIT BAD TRAP", ANSI_FG_RED))),
                    nemu_state.halt_pc);
            // fall through
        case NEMU_QUIT: 
            statistic();
    }
}
