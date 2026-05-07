#include "trap.h"
#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * This test builds the smallest Sv32 setup needed to expose stale JIT code:
 * one virtual function address is first mapped to code_page_a, then remapped to
 * code_page_b without changing satp. A cache keyed only by virtual PC and satp
 * would keep executing the native block generated from code_page_a.
 */
#define PAGE_SIZE 4096u

/* Only the Sv32 PTE bits used by this test are named here. */
#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u

#define SATP_MODE_SV32 0x80000000u

/*
 * The test image is linked at CONFIG_MBASE. Keeping an identity-mapped window
 * lets the normal stack, data, and test code continue to run after Sv32 is
 * enabled, while ALIAS_VA is free to point at generated code pages.
 */
#define IDENTITY_BASE 0x80000000u
#define IDENTITY_PAGES 64u
#define ALIAS_VA 0x80400000u

/*
 * Each table or generated-code buffer is page-aligned because Sv32 PTEs store
 * page numbers, not byte addresses. The two code pages intentionally contain
 * different return values so stale native code is observable.
 */
static uint32_t root_pt[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t identity_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t alias_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_a[PAGE_SIZE / sizeof(uint32_t)] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_b[PAGE_SIZE / sizeof(uint32_t)] __attribute__((aligned(PAGE_SIZE)));

static uint32_t pte_for_page(const void *page, uint32_t flags)
{
  const uintptr_t pa = (uintptr_t)page;
  /*
   * Sv32 stores PPN bits starting at PTE bit 10. The page pointer is already
   * aligned, so shifting right by 12 extracts the PPN and shifting left by 10
   * places it in the leaf or table PTE.
   */
  return (uint32_t)((pa >> 12) << 10) | flags;
}

static uint32_t addi_a0_zero_imm(uint32_t imm)
{
  /*
   * Encode "addi a0, zero, imm" by hand so the generated code page is only two
   * instructions: set the return value in a0, then ret through ra.
   */
  return ((imm & 0xfffu) << 20) | (10u << 7) | 0x13u;
}

static void clear_page_table(uint32_t *pt)
{
  /* An all-zero page table means every entry is invalid until explicitly set. */
  for (uint32_t i = 0; i < 1024u; i++)
  {
    pt[i] = 0;
  }
}

static void map_identity_window(void)
{
  const uint32_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X;
  /*
   * Map enough of the low PMEM image for this test: text, rodata, bss, page
   * tables, generated code, and the stack. The window is deliberately small so
   * accidental accesses outside the expected area still fail loudly.
   */
  for (uint32_t i = 0; i < IDENTITY_PAGES; i++)
  {
    const uintptr_t pa = (uintptr_t)IDENTITY_BASE + (uintptr_t)i * PAGE_SIZE;
    identity_l0[i] = (uint32_t)((pa >> 12) << 10) | leaf_flags;
  }
}

static void install_page_tables(void)
{
  /*
   * Build a two-entry root: VPN1 for the normal identity window, and VPN1 for
   * ALIAS_VA. The alias leaf is the entry that will be rewritten later while
   * satp remains unchanged.
   */
  clear_page_table(root_pt);
  clear_page_table(identity_l0);
  clear_page_table(alias_l0);

  map_identity_window();

  const uint32_t table_flags = PTE_V;
  root_pt[IDENTITY_BASE >> 22] = pte_for_page(identity_l0, table_flags);
  root_pt[ALIAS_VA >> 22] = pte_for_page(alias_l0, table_flags);
  alias_l0[(ALIAS_VA >> 12) & 0x3ffu] =
      pte_for_page(code_page_a, PTE_V | PTE_R | PTE_X);
}

static void enable_sv32(void)
{
  const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
  const uint32_t satp = SATP_MODE_SV32 | (uint32_t)root_ppn;
  /*
   * The memory clobber prevents the compiler from moving page-table writes after
   * the CSR update. This test does not need an explicit fence instruction in the
   * emulator because the interpreter/JIT observes memory directly.
   */
  asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
}

static void prepare_generated_code(void)
{
  /*
   * Both pages implement the same function shape but return different values.
   * That makes the expected result depend only on the current page mapping.
   */
  code_page_a[0] = addi_a0_zero_imm(7);
  code_page_a[1] = 0x00008067u;  /* ret */
  code_page_b[0] = addi_a0_zero_imm(9);
  code_page_b[1] = 0x00008067u;  /* ret */
}

int main()
{
  prepare_generated_code();
  install_page_tables();
  enable_sv32();

  generated_fn_t fn = (generated_fn_t)(uintptr_t)ALIAS_VA;

  const int first = fn();
  check(first == 7);

  /*
   * Keep satp unchanged but remap the same virtual function entry to different
   * physical instructions. A stale JIT block would keep returning 7 here because
   * the virtual PC and satp tag are unchanged.
   */
  alias_l0[(ALIAS_VA >> 12) & 0x3ffu] =
      pte_for_page(code_page_b, PTE_V | PTE_R | PTE_X);

  const int second = fn();
  check(second == 9);

  return 0;
}
