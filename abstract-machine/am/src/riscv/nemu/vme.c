#include <am.h>
#include <nemu.h>
#include <klib.h>

static AddrSpace kas = {};
static void* (*pgalloc_usr)(int) = NULL;
static void (*pgfree_usr)(void*) = NULL;
static int vme_enable = 0;

static Area segments[] = {      // Kernel memory mappings
  NEMU_PADDR_SPACE
};

#define USER_SPACE RANGE(0x40000000, 0x80000000)

static inline void set_satp(void *pdir) {
  uintptr_t mode = 1ul << (__riscv_xlen - 1);
  asm volatile("csrw satp, %0" : : "r"(mode | ((uintptr_t)pdir >> 12)));
}

static inline uintptr_t get_satp() {
  uintptr_t satp;
  asm volatile("csrr %0, satp" : "=r"(satp));
  return satp << 12;
}

bool vme_init(void* (*pgalloc_f)(int), void (*pgfree_f)(void*)) {
  pgalloc_usr = pgalloc_f;
  pgfree_usr = pgfree_f;

  kas.ptr = pgalloc_f(PGSIZE);

  int i;
  for (i = 0; i < LENGTH(segments); i ++) {
    void *va = segments[i].start;
    for (; va < segments[i].end; va += PGSIZE) {
      map(&kas, va, va, 0);
    }
  }

  set_satp(kas.ptr);
  vme_enable = 1;

  return true;
}

void protect(AddrSpace *as) {
  PTE *updir = (PTE*)(pgalloc_usr(PGSIZE));
  as->ptr = updir;
  as->area = USER_SPACE;
  as->pgsize = PGSIZE;
  // map kernel space
  memcpy(updir, kas.ptr, PGSIZE);
}

void unprotect(AddrSpace *as) {
}

void __am_get_cur_as(Context *c) {
  c->pdir = (vme_enable ? (void *)get_satp() : NULL);
}

void __am_switch(Context *c) {
  if (vme_enable && c->pdir != NULL) {
    set_satp(c->pdir);
  }
}

void map(AddrSpace *as, void *va, void *pa, int prot) {
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry) 
{
  // Allocate the Context at the top of the kernel stack.
  uintptr_t sp = (uintptr_t)kstack.end;
  // 16-byte alignment is a good ABI habit
  sp &= ~((uintptr_t)0xF);

  Context *c = (Context *)(sp - sizeof(Context));

  // Clean zero.
  memset(c, 0, sizeof(Context));

  // Start executing from 'entry' after mret.
  c->mepc = (uintptr_t)entry;

  // For a user process, return to U-mode after mret.
  // RISC-V mstatus.MPP is at bits [12:11].
  // Set MPP=00 (U-mode) by clearing those bits.
  // Also set MPIE (bit 7) so interrupt enable state is sane after mret.
  c->mstatus = 0;
  c->mstatus |= (1 << 7);             // MPIE = 1
  c->mstatus &= ~((uintptr_t)0x1800); // MPP = 00 (U)

  return c;
}
