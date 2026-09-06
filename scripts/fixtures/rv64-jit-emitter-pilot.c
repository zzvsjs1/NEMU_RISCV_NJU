/*
 * Host-side byte, metadata and instrumentation regressions for RV64 emitters.
 * Implementation inclusion is confined to this fixture, allowing private
 * emitters to be tested without exporting an emulator testing API.
 */
#include "../../nemu/src/isa/riscv32/jit-rv64-internal.h"
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#undef Assert
#define Assert(condition, ...) assert(condition)

#include "../../nemu/src/isa/riscv32/jit-rv64-emit.c"

CPU_state cpu;
static uint8_t fixture_pmem[16];

/* These stubs supply compile-time inputs only; no native guest code runs here. */
uint8_t *guest_to_host(paddr_t address)
{
    assert(address == CONFIG_MBASE);
    return fixture_pmem;
}

uint32_t rv64_jit_data_tlb_state(int type)
{
    assert(type == MEM_TYPE_READ || type == MEM_TYPE_WRITE);
    return 3;
}

static uint64_t fixture_load(vaddr_t address)
{
    (void)address;
    abort();
}

/* Memory helpers are addresses in these streams, never executed by the fixture. */
uint32_t rv64_jit_store_vaddr_continue(vaddr_t address, uint32_t len, uint64_t data)
{
    (void)address;
    (void)len;
    (void)data;
    abort();
}

uint32_t rv64_jit_store_bare_continue(paddr_t address, uint32_t len, uint64_t data)
{
    return rv64_jit_store_vaddr_continue(address, len, data);
}

uint32_t rv64_jit_store_pmem_continue(paddr_t address, uint32_t len, uint64_t data)
{
    return rv64_jit_store_vaddr_continue(address, len, data);
}

#ifdef CONFIG_DEVICE
/* No device spans are advertised: the native stream retains complete helpers. */
size_t mmio_direct_read_map_count(void)
{
    return 0;
}

const IOMap *mmio_direct_read_map(size_t index)
{
    (void)index;
    abort();
}

size_t mmio_direct_write_region_count(void)
{
    return 0;
}

const IODirectWriteRegion *mmio_direct_write_region(size_t index)
{
    (void)index;
    abort();
}
#endif

#ifdef CONFIG_RISCV_FPU
NEMUState nemu_state;

uint32_t riscv_fpu_jit_exec(uint32_t instr, vaddr_t pc)
{
    (void)instr;
    (void)pc;
    abort();
}
#endif

void rv64_jit_link_pilot_checks(void);

enum
{
    FIXTURE_CODE_CAPACITY = 4096,
    FIXTURE_PREFIX_BYTES = 3,
    FIXTURE_RELOCATION_CAPACITY = 64,
};

typedef struct
{
    size_t offset;
    const char *name;
    uint64_t token;
} fixture_relocation_t;

typedef struct
{
    uint8_t code[FIXTURE_CODE_CAPACITY];
    rv64_jit_writer_t writer;
    rv64_jit_reg_cache_t regs;
    rv64_jit_link_builder_t links;
    rv64_jit_indirect_pic_builder_t pic;
    rv64_jit_indirect_jump_cache_builder_t jump_cache;
    rv64_jit_mmio_route_builder_t mmio;
} emission_fixture_t;

/* Seed dirty mappings so the slow arm and alignment exit both need stores. */
static void fixture_initialise(emission_fixture_t *fixture, size_t capacity, bool pressure)
{
    memset(fixture, 0, sizeof(*fixture));
    memset(fixture->code, 0xa5, sizeof(fixture->code));
    memset(fixture->code, 0x90, FIXTURE_PREFIX_BYTES);
    fixture->writer = (rv64_jit_writer_t){
        .start = fixture->code,
        .cur = fixture->code + FIXTURE_PREFIX_BYTES,
        .end = fixture->code + FIXTURE_PREFIX_BYTES + capacity,
        .links = &fixture->links,
        .indirect_pic = &fixture->pic,
        .indirect_jump_cache = &fixture->jump_cache,
    };
    rv64_jit_reg_cache_init(&fixture->regs);
    const uint32_t occupied = pressure ? fixture->regs.slot_count : 2;

    for (uint32_t i = 0; i < occupied; i++)
    {
        fixture->regs.slots[i].valid = true;
        fixture->regs.slots[i].loaded = true;
        fixture->regs.slots[i].dirty = true;
        fixture->regs.slots[i].guest_reg = 10 + i;
        fixture->regs.slots[i].age = i + 1;
    }

    fixture->regs.next_age = occupied + 1;
    rv64_jit_reg_cache_set_liveness(&fixture->regs, UINT32_C(1) << 10, UINT32_MAX);
    memset(&rv64_jit_stats, 0, sizeof(rv64_jit_stats));
    rv64_jit_stats.emitted_sites.native_loads = 19;
    rv64_jit_stats.blocks_executed = 23;
    cpu.csr.satp = UINT64_C(0x8000000000000042);
}

static rv64_jit_load_descriptor_t fixture_descriptor(bool alias, bool byte_load)
{
    return (rv64_jit_load_descriptor_t){
        .rd = alias ? 10 : 20,
        .rs1 = 10,
        .funct3 = byte_load ? RV64_FUNCT3_LB : RV64_FUNCT3_LD,
        .imm = -8,
        .len = byte_load ? 1 : 8,
        .paged_helper = (uintptr_t)fixture_load,
        .bare_helper = (uintptr_t)fixture_load,
    };
}

/* Only known pointer operands are normalised; instruction bytes remain exact. */
static size_t fixture_relocations(const uint8_t *code, size_t length, fixture_relocation_t *relocations)
{
    const struct
    {
        uint64_t address;
        const char *name;
    } symbols[] = {
        {(uintptr_t)&cpu, "cpu"},
        {(uintptr_t)fixture_pmem, "pmem"},
        {(uintptr_t)rv64_jit_data_tlb, "data-tlb"},
        {(uintptr_t)fixture_load, "paged-helper"},
        {(uintptr_t)&rv64_jit_stats.data_tlb_hits, "stats.data_tlb_hits"},
        {(uintptr_t)&rv64_jit_stats.inline_paged_load_hits, "stats.inline_paged_load_hits"},
        {(uintptr_t)&rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_LOAD_GUARD], "stats.side_exit_by_reason.load_guard"},
        {(uintptr_t)&rv64_jit_loop_extra, "loop-extra"},
        {(uintptr_t)rv64_jit_store_vaddr_continue, "store-vaddr-helper"},
        {(uintptr_t)rv64_jit_store_bare_continue, "store-bare-helper"},
        {(uintptr_t)rv64_jit_store_pmem_continue, "store-pmem-helper"},
        {(uintptr_t)rv64_jit_source_chunk_refs, "source-chunks"},
        {(uintptr_t)rv64_jit_data_tlb_pt_page_refs, "data-page-table-refs"},
        {(uintptr_t)rv64_jit_ifetch_pt_page_refs, "ifetch-page-table-refs"},
        {(uintptr_t)&rv64_jit_stats.inline_paged_store_hits, "stats.inline_paged_store_hits"},
        {(uintptr_t)&rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_GUARD], "stats.side_exit_by_reason.store_guard"},
        {(uintptr_t)&rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_SOURCE], "stats.side_exit_by_reason.store_source"},
        {(uintptr_t)&rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_HELPER], "stats.side_exit_by_reason.store_helper"},
        {(uintptr_t)&rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER], "stats.side_exit_by_reason.paged_store_helper"},
#ifdef CONFIG_RISCV_FPU
        {(uintptr_t)rv64_jit_exec_fpu, "fp-helper"},
        {(uintptr_t)&rv64_jit_stats.fp_helper_memory_exits, "stats.fp_helper_memory_exits"},
#endif
    };

    size_t count = 0;

    for (size_t offset = 0; offset + sizeof(uint64_t) <= length; offset++)
    {
        uint64_t value;
        memcpy(&value, code + offset, sizeof(value));

        for (size_t symbol = 0; symbol < sizeof(symbols) / sizeof(symbols[0]); symbol++)
        {
            if (value == symbols[symbol].address)
            {
                assert(count < FIXTURE_RELOCATION_CAPACITY);
                relocations[count++] = (fixture_relocation_t){offset, symbols[symbol].name, UINT64_C(0xfedcba9800000000) + symbol};
                offset += sizeof(value) - 1;
                break;
            }
        }
    }

    return count;
}

/* Apply the successful stream's relocation positions even to partial operands. */
static void fixture_normalise(uint8_t *code, size_t length, const fixture_relocation_t *relocations, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        for (size_t byte = 0; byte < sizeof(uint64_t) && relocations[i].offset + byte < length; byte++)
        {
            code[relocations[i].offset + byte] = (uint8_t)(relocations[i].token >> (byte * 8));
        }
    }
}

/* A compact, deterministic digest records every retained byte, including fixups. */
static uint64_t fixture_byte_hash(const uint8_t *code, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    for (size_t i = 0; i < length; i++)
    {
        hash = (hash ^ code[i]) * UINT64_C(1099511628211);
    }

    return hash;
}

static void fixture_print_regs(const rv64_jit_reg_cache_t *regs)
{
    printf(" regs=%u,%u,%08x,%08x", regs->slot_count, regs->next_age, regs->current_use_mask, regs->live_after_mask);

    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        const rv64_jit_reg_slot_t *slot = &regs->slots[i];
        printf("/%u,%u,%u,%u,%u,%u", slot->valid, slot->loaded, slot->dirty, slot->guest_reg, slot->age, slot->hreg);
    }
}

static void fixture_check_case(const char *name, bool alias, bool byte_load, bool pressure)
{
    emission_fixture_t full;
    fixture_initialise(&full, FIXTURE_CODE_CAPACITY - FIXTURE_PREFIX_BYTES, pressure);
    const rv64_jit_load_descriptor_t descriptor = fixture_descriptor(alias, byte_load);
    assert(emit_paged_load_instr(&full.writer, &full.regs, &descriptor, UINT64_C(0x80001000), 7));
    const size_t length = (size_t)(full.writer.cur - full.code);
    assert(!full.writer.overflowed);

    /* A helper-arm flush must not mark any continuing dirty mapping clean. */
    for (uint32_t i = 0; i < full.regs.slot_count; i++)
    {
        if (full.regs.slots[i].valid)
        {
            assert(full.regs.slots[i].loaded && full.regs.slots[i].dirty);
        }
    }

    const rv64_jit_reg_slot_t *destination = jit_reg_find(&full.regs, descriptor.rd);
    assert(destination != NULL && destination->dirty);
    fixture_relocation_t relocations[FIXTURE_RELOCATION_CAPACITY];
    const size_t relocation_count = fixture_relocations(full.code, length, relocations);
    assert(relocation_count >= 4);
    size_t counter_operands = 0;

    for (size_t i = 0; i < relocation_count; i++)
    {
        counter_operands += strncmp(relocations[i].name, "stats.", 6) == 0;
    }

    /*
     * Byte loads also need the page-fault side exit, despite having no alignment
     * guard. Disabled instrumentation still emits no counter addresses.
     */
    assert(counter_operands == (RV64_JIT_STATS ? 3u : 0u));
    printf("load %s bytes=%zu", name, length);
    fixture_print_regs(&full.regs);
    putchar('\n');

    for (size_t i = 0; i < relocation_count; i++)
    {
        printf("relocation offset=%zu symbol=%s\n", relocations[i].offset, relocations[i].name);
    }

    fixture_normalise(full.code, length, relocations, relocation_count);
    printf("code ");

    for (size_t i = 0; i < length; i++)
    {
        printf("%02x", full.code[i]);
    }

    putchar('\n');

    /* Every byte boundary is a failure site, including inside helper operands. */
    for (size_t capacity = 0; capacity < length - FIXTURE_PREFIX_BYTES; capacity++)
    {
        emission_fixture_t partial;
        fixture_initialise(&partial, capacity, pressure);
        rv64_jit_emitter_checkpoint_t checkpoint;
        rv64_jit_emitter_checkpoint_capture(&checkpoint, &partial.writer, &partial.regs, &partial.mmio);
        assert(!emit_paged_load_instr(&partial.writer, &partial.regs, &descriptor, UINT64_C(0x80001000), 7));
        assert(partial.writer.overflowed && partial.writer.cur == partial.writer.end);
        const size_t retained = (size_t)(partial.writer.cur - partial.code);

        for (size_t i = retained; i < sizeof(partial.code); i++)
        {
            assert(partial.code[i] == 0xa5);
        }

        fixture_normalise(partial.code, retained, relocations, relocation_count);
        printf("failure %s capacity=%zu prefix=%016" PRIx64, name, capacity, fixture_byte_hash(partial.code, retained));
        fixture_print_regs(&partial.regs);
        printf(" sites=%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", rv64_jit_stats.emitted_sites.native_loads,
               rv64_jit_stats.emitted_sites.reg_cache_spills, rv64_jit_stats.emitted_sites.native_paged_loads);

        rv64_jit_emitter_checkpoint_restore(&checkpoint, &partial.writer, &partial.regs, &partial.mmio);
        assert(partial.writer.cur == partial.code + FIXTURE_PREFIX_BYTES && partial.writer.overflowed);
        assert(rv64_jit_reg_cache_matches(&partial.regs, &checkpoint.regs));
        assert(rv64_jit_stats.emitted_sites.native_loads == 19 && rv64_jit_stats.blocks_executed == 23);
        assert(partial.links.state.count == checkpoint.links.count);
        assert(partial.pic.state.fixup_count == checkpoint.indirect_pic.fixup_count);
        assert(partial.jump_cache.state.fixup_count == checkpoint.indirect_jump_cache.fixup_count);
        assert(partial.mmio.state.fixup_count == checkpoint.mmio_routes.fixup_count);

        for (size_t i = 0; i < FIXTURE_PREFIX_BYTES; i++)
        {
            assert(partial.code[i] == 0x90);
        }
    }
}

/* Each rollout case shares the same byte-boundary and checkpoint experiment. */
typedef enum
{
    FIXTURE_BARE_LOAD,
    FIXTURE_PAGED_STORE,
    FIXTURE_BARE_STORE,
    FIXTURE_ALU_IMMEDIATE,
    FIXTURE_ALU_SHIFT,
    FIXTURE_ALU_REGISTER,
    FIXTURE_ALU_WORD,
    FIXTURE_MULTIPLY_WORD,
    FIXTURE_ADD_IMMEDIATE_WORD,
#ifdef CONFIG_RISCV_FPU
    FIXTURE_FP_MEMORY_HELPER,
#endif
} fixture_case_kind_t;

typedef struct
{
    const char *name;
    fixture_case_kind_t kind;
    uint32_t destination;
    bool pressure;
    bool byte_access;
} fixture_case_t;

static bool fixture_emit_case(emission_fixture_t *fixture, const fixture_case_t *test)
{
    rv64_jit_writer_t *w = &fixture->writer;
    rv64_jit_reg_cache_t *regs = &fixture->regs;
    const vaddr_t pc = UINT64_C(0x80001000);
    rv64_jit_load_descriptor_t load = fixture_descriptor(test->destination == 10, test->byte_access);
    const rv64_jit_store_descriptor_t store = {
        .rs1 = 10,
        .rs2 = test->destination,
        .imm = -8,
        .len = test->byte_access ? 1 : 8,
    };

    switch (test->kind)
    {
    case FIXTURE_BARE_LOAD:
        return emit_bare_load_instr(w, regs, &load, pc, 7, &fixture->mmio);
    case FIXTURE_PAGED_STORE:
        return emit_paged_store_instr(w, regs, &store, pc, pc + 4, 7);
    case FIXTURE_BARE_STORE:
        return emit_bare_store_instr(w, regs, &store, pc, pc + 4, 7, &fixture->mmio);
    case FIXTURE_ALU_IMMEDIATE:
        return emit_op_imm_hreg(w, regs, test->destination, 10, HOST_GROUP1_ADD, -17);
    case FIXTURE_ALU_SHIFT:
        return emit_shift_imm_hreg(w, regs, test->destination, 10, HOST_SHIFT_GROUP_SAR, 63);
    case FIXTURE_ALU_REGISTER:
        return emit_op_hreg(w, regs, test->destination, 10, 11, HOST_ALU_ADD, true);
    case FIXTURE_ALU_WORD:
    case FIXTURE_MULTIPLY_WORD:
        return emit_op32_hreg_commutative(w, regs, test->destination, 10, 11, HOST_ALU_ADD, test->kind == FIXTURE_MULTIPLY_WORD);
    case FIXTURE_ADD_IMMEDIATE_WORD:
        return emit_op_imm32(w, regs, (UINT32_C(0xfff) << 20) | (10u << 15) | (test->destination << 7) | RISCV_OPCODE_OP_IMM_32);
#ifdef CONFIG_RISCV_FPU
    case FIXTURE_FP_MEMORY_HELPER:
        return emit_fp_memory_helper_terminal(w, regs, UINT32_C(0x00053007), pc, 7);
#endif
    }

    abort();
}

static void fixture_check_rollout_case(const fixture_case_t *test)
{
    emission_fixture_t full;
    fixture_initialise(&full, FIXTURE_CODE_CAPACITY - FIXTURE_PREFIX_BYTES, test->pressure);
    assert(fixture_emit_case(&full, test));
    assert(!full.writer.overflowed);
    const size_t length = (size_t)(full.writer.cur - full.code);
    fixture_relocation_t relocations[FIXTURE_RELOCATION_CAPACITY];
    const size_t relocation_count = fixture_relocations(full.code, length, relocations);
    printf("rollout %s bytes=%zu", test->name, length);
    fixture_print_regs(&full.regs);
    printf(" sites=%016" PRIx64 "\n", fixture_byte_hash((const uint8_t *)&rv64_jit_stats.emitted_sites, sizeof(rv64_jit_stats.emitted_sites)));

    for (size_t i = 0; i < relocation_count; i++)
    {
        assert(RV64_JIT_STATS || strncmp(relocations[i].name, "stats.", 6) != 0);
        printf("relocation offset=%zu symbol=%s\n", relocations[i].offset, relocations[i].name);
    }

    fixture_normalise(full.code, length, relocations, relocation_count);
    printf("code ");

    for (size_t i = 0; i < length; i++)
    {
        printf("%02x", full.code[i]);
    }

    putchar('\n');

    for (size_t capacity = 0; capacity < length - FIXTURE_PREFIX_BYTES; capacity++)
    {
        emission_fixture_t partial;
        fixture_initialise(&partial, capacity, test->pressure);
        rv64_jit_emitter_checkpoint_t checkpoint;
        rv64_jit_emitter_checkpoint_capture(&checkpoint, &partial.writer, &partial.regs, &partial.mmio);
        assert(!fixture_emit_case(&partial, test));
        assert(partial.writer.overflowed && partial.writer.cur == partial.writer.end);
        const size_t retained = (size_t)(partial.writer.cur - partial.code);

        for (size_t i = retained; i < sizeof(partial.code); i++)
        {
            assert(partial.code[i] == 0xa5);
        }

        fixture_normalise(partial.code, retained, relocations, relocation_count);
        printf("failure %s capacity=%zu prefix=%016" PRIx64, test->name, capacity, fixture_byte_hash(partial.code, retained));
        fixture_print_regs(&partial.regs);
        printf(" sites=%016" PRIx64 "\n", fixture_byte_hash((const uint8_t *)&rv64_jit_stats.emitted_sites, sizeof(rv64_jit_stats.emitted_sites)));

        rv64_jit_emitter_checkpoint_restore(&checkpoint, &partial.writer, &partial.regs, &partial.mmio);
        assert(partial.writer.cur == partial.code + FIXTURE_PREFIX_BYTES && partial.writer.overflowed);
        assert(rv64_jit_reg_cache_matches(&partial.regs, &checkpoint.regs));
#if RV64_JIT_STATS
        assert(memcmp(&rv64_jit_stats.emitted_sites, &checkpoint.emitted_sites, sizeof(checkpoint.emitted_sites)) == 0);
#endif
        assert(rv64_jit_stats.blocks_executed == 23);
        assert(partial.links.state.count == checkpoint.links.count);
        assert(partial.pic.state.fixup_count == checkpoint.indirect_pic.fixup_count);
        assert(partial.jump_cache.state.fixup_count == checkpoint.indirect_jump_cache.fixup_count);
        assert(partial.mmio.state.fixup_count == checkpoint.mmio_routes.fixup_count);
    }
}

/* Execute only native instrumentation, using a writable then executable page. */
static void fixture_check_counter_contract(bool preserve_rax)
{
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *code = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    uint64_t counter = UINT64_MAX;
    uint64_t observed[3] = {0};
    const uint64_t rax_seed = UINT64_C(0x1122334455667788);
    const uint64_t rdi_seed = UINT64_C(0x8877665544332211);
    rv64_jit_writer_t w = {.start = code, .cur = code, .end = code + page_size};

    assert(emit_movabs_rax(&w, rax_seed));
    assert(emit_movabs_rdi(&w, rdi_seed));
    assert(emit_u8(&w, 0x31) && emit_u8(&w, 0xd2)); /* XOR EDX,EDX: known status flags. */
    assert(emit_u8(&w, 0xf9)); /* STC: INC must preserve the existing carry flag. */
    assert(preserve_rax ? emit_inc_jit_stat_counter_preserve_rax(&w, &counter) : emit_inc_jit_stat_counter(&w, &counter));
    assert(emit_movabs_rcx(&w, (uintptr_t)observed));
    assert(emit_u8(&w, 0x48) && emit_u8(&w, 0x89) && emit_u8(&w, 0x01)); /* [RCX] = RAX. */
    assert(emit_u8(&w, 0x48) && emit_u8(&w, 0x89) && emit_u8(&w, 0x79) && emit_u8(&w, 8)); /* [RCX+8] = RDI. */
    assert(emit_u8(&w, 0x9c) && emit_u8(&w, 0x58)); /* PUSHFQ; POP RAX. */
    assert(emit_u8(&w, 0x48) && emit_u8(&w, 0x89) && emit_u8(&w, 0x41) && emit_u8(&w, 16)); /* [RCX+16] = flags. */
    assert(emit_u8(&w, 0xc3));
    assert(mprotect(code, page_size, PROT_READ | PROT_EXEC) == 0);
    ((void (*)(void))(uintptr_t)code)();
    assert(counter == (RV64_JIT_STATS ? 0 : UINT64_MAX));
    assert(observed[0] == (RV64_JIT_STATS && !preserve_rax ? (uintptr_t)&counter : rax_seed));
    assert(observed[1] == (RV64_JIT_STATS && preserve_rax ? (uintptr_t)&counter : rdi_seed));
    assert((observed[2] & UINT64_C(0x8d5)) == (RV64_JIT_STATS ? UINT64_C(0x55) : UINT64_C(0x45)));
    assert(munmap(code, page_size) == 0);
    printf("counter-contract preserve-rax=%u PASS\n", preserve_rax);
}

/* Optional emission-only measurement, deliberately separate from guest runtime. */
static void fixture_benchmark(unsigned long iterations)
{
    emission_fixture_t fixture;
    struct timespec start;
    struct timespec finish;
    uint64_t total_bytes = 0;
    fixture_initialise(&fixture, FIXTURE_CODE_CAPACITY - FIXTURE_PREFIX_BYTES, true);
    const rv64_jit_reg_cache_t initial_regs = fixture.regs;
    const rv64_jit_load_descriptor_t descriptor = fixture_descriptor(false, false);
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) == 0);

    for (unsigned long i = 0; i < iterations; i++)
    {
        fixture.writer.cur = fixture.code + FIXTURE_PREFIX_BYTES;
        fixture.regs = initial_regs;
        assert(emit_paged_load_instr(&fixture.writer, &fixture.regs, &descriptor, UINT64_C(0x80001000), 7));
        total_bytes += (uint64_t)(fixture.writer.cur - fixture.code);
    }

    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &finish) == 0);
    const int64_t elapsed_ns = (int64_t)(finish.tv_sec - start.tv_sec) * INT64_C(1000000000) + finish.tv_nsec - start.tv_nsec;
    printf("emission iterations=%lu bytes=%" PRIu64 " cpu_ns=%" PRId64 "\n", iterations, total_bytes, elapsed_ns);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--benchmark") == 0)
    {
        char *end;
        const unsigned long iterations = strtoul(argv[2], &end, 10);
        assert(*end == '\0' && iterations > 0);
        fixture_benchmark(iterations);
        return 0;
    }

    assert(argc == 1);
    printf("rv64-jit-pilot stats=%u\n", RV64_JIT_STATS);
    rv64_jit_link_pilot_checks();
    fixture_check_case("ld-alias", true, false, false);
    fixture_check_case("ld-pressure", false, false, true);
    fixture_check_case("lb-pressure", false, true, true);
    const fixture_case_t rollout_cases[] = {
        {"bare-ld-alias", FIXTURE_BARE_LOAD, 10, false, false},
        {"bare-ld-pressure", FIXTURE_BARE_LOAD, 20, true, false},
        {"bare-lb-pressure", FIXTURE_BARE_LOAD, 20, true, true},
        {"paged-sd-alias", FIXTURE_PAGED_STORE, 10, false, false},
        {"paged-sd-pressure", FIXTURE_PAGED_STORE, 20, true, false},
        {"paged-sb-zero", FIXTURE_PAGED_STORE, 0, true, true},
        {"bare-sd-alias", FIXTURE_BARE_STORE, 10, false, false},
        {"bare-sd-pressure", FIXTURE_BARE_STORE, 20, true, false},
        {"bare-sb-zero", FIXTURE_BARE_STORE, 0, true, true},
        {"addi-alias", FIXTURE_ALU_IMMEDIATE, 10, false, false},
        {"addi-pressure", FIXTURE_ALU_IMMEDIATE, 20, true, false},
        {"shift-pressure", FIXTURE_ALU_SHIFT, 20, true, false},
        {"add-rhs-alias", FIXTURE_ALU_REGISTER, 11, true, false},
        {"add-pressure", FIXTURE_ALU_REGISTER, 20, true, false},
        {"addw-pressure", FIXTURE_ALU_WORD, 20, true, false},
        {"mulw-rhs-alias", FIXTURE_MULTIPLY_WORD, 11, true, false},
        {"addiw-pressure", FIXTURE_ADD_IMMEDIATE_WORD, 20, true, false},
#ifdef CONFIG_RISCV_FPU
        {"fp-memory-helper", FIXTURE_FP_MEMORY_HELPER, 0, true, false},
#endif
    };

    for (size_t i = 0; i < sizeof(rollout_cases) / sizeof(rollout_cases[0]); i++)
    {
        fixture_check_rollout_case(&rollout_cases[i]);
    }

    fixture_check_counter_contract(false);
    fixture_check_counter_contract(true);
    puts("RV64 JIT readability pilot PASS");
    return 0;
}
