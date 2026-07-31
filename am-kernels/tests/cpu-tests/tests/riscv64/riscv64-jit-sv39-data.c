#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define PAGE_SIZE 4096ull
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint64_t))

/* Sv39 PTE flag bits used by this test's hand-built page tables. */
#define PTE_V 0x001ull
#define PTE_R 0x002ull
#define PTE_W 0x004ull
#define PTE_X 0x008ull
#define PTE_A 0x040ull
#define PTE_D 0x080ull

/* satp.MODE=8 selects Sv39 on RV64; MPP=S makes translation active after mret. */
#define SATP_MODE_SV39 (8ull << 60)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MSTATUS_MPP_MPIE_MASK (MSTATUS_MPP_MASK | (1ull << 7))
#define MSTATUS_MPP_S (1ull << 11)
#define MSTATUS_MPRV (1ull << 17)
#define MSTATUS_FS_MASK (3ull << 13)
#define MSTATUS_FS_INITIAL (1ull << 13)
#define MSTATUS_FS_CLEAN (2ull << 13)
#define MSTATUS_FS_DIRTY (3ull << 13)
#define MSTATUS_SD (1ull << 63)

#define IDENTITY_BASE 0x80000000ull
#define IDENTITY_PAGES 32768ull
#define IDENTITY_L1_ENTRIES (IDENTITY_PAGES / 512ull)
#define DATA_ALIAS_VA 0x80400000ull
#define FP_ALIAS_VA (DATA_ALIAS_VA + PAGE_SIZE)
#define PTE_ALIAS_VA (DATA_ALIAS_VA + 2ull * PAGE_SIZE)

static uint64_t root_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l0[IDENTITY_L1_ENTRIES][512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t data_alias_l0[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t data_page[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t fp_data_page_a[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t fp_data_page_b[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

_Static_assert(((FP_ALIAS_VA >> 12) & 0x1ffull) == 1ull,
               "FP alias must use leaf PTE slot one");
_Static_assert(((PTE_ALIAS_VA >> 12) & 0x1ffull) == 2ull,
               "PTE alias must use leaf PTE slot two");

/*
 * Unexpected traps are test failures.  Encoding mcause into the bad-trap code
 * keeps page-table setup mistakes visible.
 */
asm(".globl rv64_sv39_data_unexpected_trap\n"
    "rv64_sv39_data_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  csrr a1, mepc\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_sv39_data_unexpected_trap(void);

asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"
    ".globl rv64_sv39_fp_memory_loop\n"
    ".type rv64_sv39_fp_memory_loop, @function\n"
    "rv64_sv39_fp_memory_loop:\n"
    "1:\n"
    "  flw f0, 40(a0)\n"
    "  fld f1, 48(a0)\n"
    "  fsw f0, 56(a0)\n"
    "  fsd f1, 64(a0)\n"
    "  addi a1, a1, -1\n"
    "  bne a1, zero, 1b\n"
    "  fmv.x.d a2, f0\n"
    "  fmv.x.d a3, f1\n"
    "  xor a0, a2, a3\n"
    "  ret\n"
    ".size rv64_sv39_fp_memory_loop, "
    ".-rv64_sv39_fp_memory_loop\n"

    /*
     * A cold DTLB probe exits after materialising the uncached a0 base. Six
     * dirty GPRs make that read evict one live mapping; the suffix proves the
     * post-read cache snapshot reaches the side exit without corruption.
     */
    ".globl rv64_sv39_fp_cold_load_eviction\n"
    ".type rv64_sv39_fp_cold_load_eviction, @function\n"
    "rv64_sv39_fp_cold_load_eviction:\n"
    "  addi t2, zero, 0x112\n"
    "  addi t3, zero, 0x223\n"
    "  addi t4, zero, 0x334\n"
    "  addi t5, zero, 0x445\n"
    "  addi t6, zero, 0x556\n"
    "  addi a2, zero, 0x667\n"
    "  flw f7, 0(a0)\n"
    "  sd t2, 0(a1)\n"
    "  sd t3, 8(a1)\n"
    "  sd t4, 16(a1)\n"
    "  sd t5, 24(a1)\n"
    "  sd t6, 32(a1)\n"
    "  sd a2, 40(a1)\n"
    "  fmv.x.d t0, f7\n"
    "  sd t0, 48(a1)\n"
    "  ret\n"
    ".size rv64_sv39_fp_cold_load_eviction, "
    ".-rv64_sv39_fp_cold_load_eviction\n"

    ".globl rv64_sv39_fp_load_state\n"
    ".type rv64_sv39_fp_load_state, @function\n"
    "rv64_sv39_fp_load_state:\n"
    "  flw f8, 0(a0)\n"
    "  fld f9, 8(a0)\n"
    "  fmv.x.d t0, f8\n"
    "  sd t0, 0(a1)\n"
    "  fmv.x.d t0, f9\n"
    "  sd t0, 8(a1)\n"
    "  ret\n"
    ".size rv64_sv39_fp_load_state, .-rv64_sv39_fp_load_state\n"

    ".globl rv64_sv39_fp_seed_stores\n"
    ".type rv64_sv39_fp_seed_stores, @function\n"
    "rv64_sv39_fp_seed_stores:\n"
    "  fmv.d.x f10, a0\n"
    "  fmv.d.x f11, a1\n"
    "  ret\n"
    ".size rv64_sv39_fp_seed_stores, .-rv64_sv39_fp_seed_stores\n"

    ".globl rv64_sv39_fp_store_state\n"
    ".type rv64_sv39_fp_store_state, @function\n"
    "rv64_sv39_fp_store_state:\n"
    "  fsw f10, 16(a0)\n"
    "  fsd f11, 24(a0)\n"
    "  ret\n"
    ".size rv64_sv39_fp_store_state, .-rv64_sv39_fp_store_state\n"

    ".globl rv64_sv39_fp_seed_pte\n"
    ".type rv64_sv39_fp_seed_pte, @function\n"
    "rv64_sv39_fp_seed_pte:\n"
    "  fmv.d.x f12, a0\n"
    "  ret\n"
    ".size rv64_sv39_fp_seed_pte, .-rv64_sv39_fp_seed_pte\n"

    /*
     * PTE_ALIAS_VA maps the active leaf-table page itself. The leading LD via
     * a3 warms its DTLB entry without caching the later a0 store base, so FSD
     * reaches the translated-store dependency guard with the eviction shape
     * intact and exits before the interpreter commits the new PTE.
     */
    ".globl rv64_sv39_fp_patch_pte_eviction\n"
    ".type rv64_sv39_fp_patch_pte_eviction, @function\n"
    "rv64_sv39_fp_patch_pte_eviction:\n"
    "  ld t0, 8(a3)\n"
    "  addi t2, zero, 0x112\n"
    "  addi t3, zero, 0x223\n"
    "  addi t4, zero, 0x334\n"
    "  addi t5, zero, 0x445\n"
    "  addi t6, zero, 0x556\n"
    "  addi a2, zero, 0x667\n"
    "  fsd f12, 8(a0)\n"
    "  sd t2, 0(a1)\n"
    "  sd t3, 8(a1)\n"
    "  sd t4, 16(a1)\n"
    "  sd t5, 24(a1)\n"
    "  sd t6, 32(a1)\n"
    "  sd a2, 40(a1)\n"
    "  ret\n"
    ".size rv64_sv39_fp_patch_pte_eviction, "
    ".-rv64_sv39_fp_patch_pte_eviction\n"
    ".option pop\n");

extern uint64_t rv64_sv39_fp_memory_loop(uint8_t *, uint64_t);
extern void rv64_sv39_fp_cold_load_eviction(
    uint8_t *, uint64_t observed[7]);
extern void rv64_sv39_fp_load_state(uint8_t *, uint64_t observed[2]);
extern void rv64_sv39_fp_seed_stores(uint64_t, uint64_t);
extern void rv64_sv39_fp_store_state(uint8_t *);
extern void rv64_sv39_fp_seed_pte(uint64_t);
extern void rv64_sv39_fp_patch_pte_eviction(
    uint8_t *, uint64_t observed[6], uint64_t, uint8_t *);

/* Return the Sv39 root-table index for a canonical virtual address. */
static uint64_t vpn2(uint64_t va)
{
    return (va >> 30) & 0x1ffull;
}

/* Return the Sv39 middle-table index for a canonical virtual address. */
static uint64_t vpn1(uint64_t va)
{
    return (va >> 21) & 0x1ffull;
}

/* Return the Sv39 leaf-table index for a canonical virtual address. */
static uint64_t vpn0(uint64_t va)
{
    return (va >> 12) & 0x1ffull;
}

/* Encode a 4 KiB physical page pointer and permission bits as an Sv39 PTE. */
static uint64_t pte_for_page(const void *page, uint64_t flags)
{
    const uintptr_t pa = (uintptr_t)page;

    return ((uint64_t)(pa >> 12) << 10) | flags;
}

/* Clear one page-table page before installing the exact entries needed. */
static void clear_page_table(uint64_t *pt)
{
    for (uint32_t i = 0; i < 512u; i++)
    {
        pt[i] = 0;
    }
}

/* Identity-map the normal NEMU PMEM window used by code, stack, and data. */
static void map_identity_window(void)
{
    const uint64_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;

    for (uint64_t l1 = 0; l1 < IDENTITY_L1_ENTRIES; l1++)
    {
        identity_l1[vpn1(IDENTITY_BASE) + l1] =
            pte_for_page(identity_l0[l1], PTE_V);

        for (uint64_t i = 0; i < 512ull; i++)
        {
            const uintptr_t page_index = (uintptr_t)l1 * 512u + (uintptr_t)i;
            const uintptr_t pa = (uintptr_t)IDENTITY_BASE + page_index * PAGE_SIZE;
            identity_l0[l1][i] = ((uint64_t)(pa >> 12) << 10) | leaf_flags;
        }
    }
}

/*
 * Install separate aliases for integer data, FP data, and the active leaf
 * table itself. Keeping FP_ALIAS_VA on its own VPN prevents earlier integer
 * probes from accidentally warming the FP-memory DTLB entry.
 */
static void map_data_aliases(void)
{
    const uint64_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    const uint64_t table_flags = PTE_V;

    for (uint64_t i = 0; i < 512ull; i++)
    {
        const uintptr_t pa =
            (uintptr_t)((vpn1(DATA_ALIAS_VA) * 512ull + i) * PAGE_SIZE +
                        IDENTITY_BASE);
        data_alias_l0[i] = ((uint64_t)(pa >> 12) << 10) | leaf_flags;
    }

    identity_l1[vpn1(DATA_ALIAS_VA)] = pte_for_page(data_alias_l0, table_flags);
    data_alias_l0[vpn0(DATA_ALIAS_VA)] = pte_for_page(data_page, leaf_flags);
    data_alias_l0[vpn0(FP_ALIAS_VA)] =
        pte_for_page(fp_data_page_a, leaf_flags);
    data_alias_l0[vpn0(PTE_ALIAS_VA)] =
        pte_for_page(data_alias_l0, leaf_flags);
}

/* Install a three-level Sv39 tree with identity code and one data alias. */
static void install_page_tables(void)
{
    clear_page_table(root_pt);
    clear_page_table(identity_l1);
    clear_page_table(data_alias_l0);

    for (uint64_t i = 0; i < IDENTITY_L1_ENTRIES; i++)
    {
        clear_page_table(identity_l0[i]);
    }

    map_identity_window();
    map_data_aliases();
    root_pt[vpn2(IDENTITY_BASE)] = pte_for_page(identity_l1, PTE_V);
}

/* Write satp with the root page-table physical page number. */
static void enable_sv39(void)
{
    const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
    const uint64_t satp = SATP_MODE_SV39 | (uint64_t)root_ppn;

    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
}

/* Enable scalar FP state before entering S-mode, where mstatus is not writable. */
static void enable_fp_state(void)
{
    uintptr_t mstatus;

    asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    mstatus = (mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL;
    asm volatile("csrw mstatus, %0" : : "r"(mstatus) : "memory");
}

static uintptr_t read_mstatus(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static void write_mstatus(uintptr_t value)
{
    asm volatile("csrw mstatus, %0" : : "r"(value) : "memory");
}

static uintptr_t read_fflags(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x001" : "=r"(value));
    return value;
}

static void write_fflags(uintptr_t value)
{
    asm volatile("csrw 0x001, %0" : : "r"(value) : "memory");
}

static void set_machine_fp_state(uintptr_t fs)
{
    const uintptr_t next =
        (read_mstatus() & ~MSTATUS_FS_MASK) | fs;

    write_mstatus(next);
}

/*
 * Keep instruction fetch in M-mode while making explicit data accesses use
 * the S-mode Sv39 permissions. This permits precise mstatus FS/SD assertions
 * even though NEMU's deliberately small CSR model does not expose sstatus.
 */
static uintptr_t enable_mprv_supervisor_data(void)
{
    const uintptr_t old = read_mstatus();
    const uintptr_t next =
        (old & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_S | MSTATUS_MPRV;

    write_mstatus(next);
    return old;
}

/* Execute a global SFENCE.VMA; 0x12000073 encodes sfence.vma x0, x0. */
static void sfence_vma_all(void)
{
    asm volatile(".word 0x12000073" : : : "memory");
}

/* Point mtvec at a no-stack failure path before entering translated S-mode. */
static void install_unexpected_trap_handler(void)
{
    asm volatile("csrw mtvec, %0" : : "r"(rv64_sv39_data_unexpected_trap) : "memory");
}

/* Enter S-mode so satp controls explicit loads and stores. */
static void enter_supervisor_mode(void)
{
    uintptr_t mstatus;

    asm volatile(
        "csrr %[mstatus], mstatus\n"
        "li t0, %[mpp_mask]\n"
        "not t0, t0\n"
        "and %[mstatus], %[mstatus], t0\n"
        "li t0, %[mpp_s]\n"
        "or %[mstatus], %[mstatus], t0\n"
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        : [mstatus] "=&r"(mstatus)
        : [mpp_mask] "i"(MSTATUS_MPP_MPIE_MASK),
          [mpp_s] "i"(MSTATUS_MPP_S)
        : "t0", "memory");
}

/* Execute every RV64 integer load width through the Sv39 alias. */
static void sv39_load_widths(uint8_t *alias, uint64_t *out)
{
    uint64_t lb;
    uint64_t lbu;
    uint64_t lh;
    uint64_t lhu;
    uint64_t lw;
    uint64_t lwu;
    uint64_t ld;

    asm volatile(
        "lb %[lb], 0(%[alias])\n"
        "lbu %[lbu], 0(%[alias])\n"
        "lh %[lh], 2(%[alias])\n"
        "lhu %[lhu], 2(%[alias])\n"
        "lw %[lw], 4(%[alias])\n"
        "lwu %[lwu], 4(%[alias])\n"
        "ld %[ld], 16(%[alias])\n"
        : [lb] "=&r"(lb),
          [lbu] "=&r"(lbu),
          [lh] "=&r"(lh),
          [lhu] "=&r"(lhu),
          [lw] "=&r"(lw),
          [lwu] "=&r"(lwu),
          [ld] "=&r"(ld)
        : [alias] "r"(alias)
        : "memory");

    out[0] = lb;
    out[1] = lbu;
    out[2] = lh;
    out[3] = lhu;
    out[4] = lw;
    out[5] = lwu;
    out[6] = ld;
}

/* Execute every RV64 integer store width through the Sv39 alias. */
static void sv39_store_widths(uint8_t *alias)
{
    const uint64_t byte_value = 0xa5ull;
    const uint64_t half_value = 0xb6c7ull;
    const uint64_t word_value = 0xd8e9fa0bull;
    const uint64_t dword_value = 0x1122334455667788ull;

    asm volatile(
        "sb %[byte_value], 24(%[alias])\n"
        "sh %[half_value], 26(%[alias])\n"
        "sw %[word_value], 28(%[alias])\n"
        "sd %[dword_value], 32(%[alias])\n"
        :
        : [alias] "r"(alias),
          [byte_value] "r"(byte_value),
          [half_value] "r"(half_value),
          [word_value] "r"(word_value),
          [dword_value] "r"(dword_value)
        : "memory");
}

/* Force repeated native links between translated S-mode blocks. */
static uint64_t sv39_direct_link_loop(uint8_t *alias)
{
    uint64_t out = 0;
    uint64_t laps = 64;

    asm volatile(
        "1:\n"
        "jal zero, 2f\n"
        "3:\n"
        "addi %[laps], %[laps], -1\n"
        "bnez %[laps], 1b\n"
        "jal zero, 4f\n"
        "2:\n"
        "ld t0, 16(%[alias])\n"
        "add %[out], %[out], t0\n"
        "jal zero, 3b\n"
        "4:\n"
        : [out] "+r"(out), [laps] "+r"(laps)
        : [alias] "r"(alias)
        : "t0", "memory");

    return out;
}

/* Verify that Sv39 data loads and stores use the alias mapping, not VA==PA. */
static void test_sv39_data_memory(void)
{
    uint8_t *raw = (uint8_t *)data_page;
    uint64_t loaded[7];

    for (uint32_t i = 0; i < PAGE_SIZE; i++)
    {
        raw[i] = 0;
    }

    raw[0] = 0x80u;
    raw[2] = 0x00u;
    raw[3] = 0x80u;
    raw[4] = 0x01u;
    raw[5] = 0x00u;
    raw[6] = 0x00u;
    raw[7] = 0x80u;
    data_page[2] = 0x0102030405060708ull;

    sv39_load_widths((uint8_t *)(uintptr_t)DATA_ALIAS_VA, loaded);
    sv39_store_widths((uint8_t *)(uintptr_t)DATA_ALIAS_VA);
    const uint64_t linked_sum =
        sv39_direct_link_loop((uint8_t *)(uintptr_t)DATA_ALIAS_VA);

    check(loaded[0] == 0xffffffffffffff80ull);
    check(loaded[1] == 0x80ull);
    check(loaded[2] == 0xffffffffffff8000ull);
    check(loaded[3] == 0x8000ull);
    check(loaded[4] == 0xffffffff80000001ull);
    check(loaded[5] == 0x80000001ull);
    check(loaded[6] == 0x0102030405060708ull);

    check(raw[24] == 0xa5u);
    check(raw[26] == 0xc7u && raw[27] == 0xb6u);
    check(raw[28] == 0x0bu && raw[29] == 0xfau &&
          raw[30] == 0xe9u && raw[31] == 0xd8u);
    check(data_page[4] == 0x1122334455667788ull);
    check(linked_sum == data_page[2] * 64u);
}

static void clear_fp_page(uint64_t *page)
{
    for (uint32_t i = 0; i < WORDS_PER_PAGE; i++)
    {
        page[i] = 0;
    }
}

static void seed_fp_page(uint64_t *page, uint32_t word,
                         uint64_t doubleword)
{
    uint8_t *const raw = (uint8_t *)page;

    *(uint32_t *)(void *)&raw[0] = word;
    *(uint64_t *)(void *)&raw[8] = doubleword;
    *(uint32_t *)(void *)&raw[40] = word;
    *(uint64_t *)(void *)&raw[48] = doubleword;
}

static void check_eviction_values(const uint64_t *observed)
{
    static const uint64_t expected[6] = {
        UINT64_C(0x112),
        UINT64_C(0x223),
        UINT64_C(0x334),
        UINT64_C(0x445),
        UINT64_C(0x556),
        UINT64_C(0x667),
    };

    for (uint32_t i = 0; i < 6; i++)
    {
        check(observed[i] == expected[i]);
    }
}

static void check_fp_dirty_state(void)
{
    const uintptr_t state = read_mstatus();

    check((state & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((state & MSTATUS_SD) != 0);
    check(read_fflags() == UINT64_C(0x1f));
}

static void check_fp_clean_state(void)
{
    const uintptr_t state = read_mstatus();

    check((state & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((state & MSTATUS_SD) == 0);
    check(read_fflags() == UINT64_C(0x1f));
}

/*
 * Exercise the FP-only cold DTLB probe, a warmed native state transition,
 * raw paged stores, an FP write to an active PTE page, and remapping to a
 * second physical page through the same compiled PCs.
 */
static void test_sv39_fp_memory(void)
{
    const uint32_t word_a = UINT32_C(0x7f800123);
    const uint64_t double_a = UINT64_C(0x7ff0000000000456);
    const uint64_t boxed_word_a = UINT64_C(0xffffffff7f800123);
    const uint32_t word_b = UINT32_C(0xff800321);
    const uint64_t double_b = UINT64_C(0xfff8000000000789);
    const uint64_t boxed_word_b = UINT64_C(0xffffffffff800321);
    const uint64_t malformed_box = UINT64_C(0x012345677f800abc);
    const uint64_t raw_store_double = UINT64_C(0xfff0000000000abc);
    const uint64_t leaf_flags =
        PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    uint8_t *const alias = (uint8_t *)(uintptr_t)FP_ALIAS_VA;
    uint8_t *const pte_alias = (uint8_t *)(uintptr_t)PTE_ALIAS_VA;
    uint8_t *const raw_a = (uint8_t *)fp_data_page_a;
    uint8_t *const raw_b = (uint8_t *)fp_data_page_b;
    uint64_t cold_observed[7] = {0};
    uint64_t load_observed[2] = {0};
    uint64_t patch_observed[6] = {0};

    clear_fp_page(fp_data_page_a);
    clear_fp_page(fp_data_page_b);
    seed_fp_page(fp_data_page_a, word_a, double_a);
    seed_fp_page(fp_data_page_b, word_b, double_b);

    /*
     * No earlier access has touched FP_ALIAS_VA. The generated FLW therefore
     * probes and exits before the instruction; the warmed retry commits the
     * one load, and the suffix exposes every live GPR.
     */
    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_INITIAL);
    rv64_sv39_fp_cold_load_eviction(alias, cold_observed);
    check_eviction_values(cold_observed);
    check(cold_observed[6] == boxed_word_a);
    check_fp_dirty_state();

    /*
     * The cold probe filled this VPN, so both loads now execute through the
     * native paged body. Only successful loads may make FS Dirty and set SD.
     */
    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_INITIAL);
    rv64_sv39_fp_load_state(alias, load_observed);
    check(load_observed[0] == boxed_word_a);
    check(load_observed[1] == double_a);
    check_fp_dirty_state();

    /*
     * Seed a deliberately malformed binary32 box, then restore FS Clean
     * before the stores. FSW must copy its raw low word and neither store may
     * make FS Dirty or change fflags.
     */
    rv64_sv39_fp_seed_stores(malformed_box, raw_store_double);
    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_CLEAN);
    rv64_sv39_fp_store_state(alias);
    check(*(uint32_t *)(void *)&raw_a[16] ==
          (uint32_t)malformed_box);
    check(*(uint64_t *)(void *)&raw_a[24] == raw_store_double);
    check_fp_clean_state();

    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_INITIAL);
    const uint64_t checksum_a =
        rv64_sv39_fp_memory_loop(alias, 64);
    check(checksum_a == (boxed_word_a ^ double_a));
    check(*(uint32_t *)(void *)&raw_a[56] == word_a);
    check(*(uint64_t *)(void *)&raw_a[64] == double_a);
    check_fp_dirty_state();

    /*
     * FSD writes the active FP leaf through a virtual alias of the page-table
     * page. The leading integer LD warms that alias, so the native store must
     * hit its dependency guard, preserve all six dirty GPRs, and let the
     * interpreter commit and invalidate the PTE exactly once.
     */
    const uint64_t pte_b = pte_for_page(fp_data_page_b, leaf_flags);
    rv64_sv39_fp_seed_pte(pte_b);
    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_CLEAN);
    rv64_sv39_fp_patch_pte_eviction(
        pte_alias, patch_observed, 0, pte_alias);
    check_eviction_values(patch_observed);
    check(data_alias_l0[vpn0(FP_ALIAS_VA)] == pte_b);
    check_fp_clean_state();

    sfence_vma_all();

    for (uint32_t i = 0; i < 7; i++)
    {
        cold_observed[i] = 0;
    }

    /*
     * Reusing the compiled cold-probe PC after SFENCE must miss the old entry
     * and observe page B. The probe warms the replacement translation before
     * its retry and the same native loop execute on B.
     */
    write_fflags(UINT64_C(0x1f));
    set_machine_fp_state(MSTATUS_FS_INITIAL);
    rv64_sv39_fp_cold_load_eviction(alias, cold_observed);
    check_eviction_values(cold_observed);
    check(cold_observed[6] == boxed_word_b);
    check_fp_dirty_state();

    const uint64_t checksum_b =
        rv64_sv39_fp_memory_loop(alias, 64);
    check(checksum_b == (boxed_word_b ^ double_b));
    check(*(uint32_t *)(void *)&raw_b[56] == word_b);
    check(*(uint64_t *)(void *)&raw_b[64] == double_b);

    /* Page A must remain unchanged after the remap and second loop. */
    check(*(uint32_t *)(void *)&raw_a[56] == word_a);
    check(*(uint64_t *)(void *)&raw_a[64] == double_a);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    install_page_tables();
    install_unexpected_trap_handler();
    enable_fp_state();
    enable_sv39();
    sfence_vma_all();
    const uintptr_t pre_mprv_mstatus =
        enable_mprv_supervisor_data();
    test_sv39_fp_memory();
    write_mstatus(pre_mprv_mstatus);
    enter_supervisor_mode();
    test_sv39_data_memory();
#endif

    return 0;
}
