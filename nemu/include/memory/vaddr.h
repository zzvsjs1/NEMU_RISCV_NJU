#ifndef __MEMORY_VADDR_H__
#define __MEMORY_VADDR_H__

#include <common.h>

word_t vaddr_ifetch(vaddr_t addr, int len);
word_t vaddr_read(vaddr_t addr, int len);
void vaddr_write(vaddr_t addr, int len, word_t data);

#ifdef CONFIG_RV64
/*
 * Checked architectural accesses leave trap entry to the executing instruction.
 * On failure, read output and memory are unchanged; fault identifies the first
 * failing virtual address, which can differ from addr for a split access.
 */
typedef struct
{
    word_t cause;
    vaddr_t addr;
} vaddr_fault_t;

bool vaddr_try_ifetch(vaddr_t addr, int len, word_t *data, vaddr_fault_t *fault);
bool vaddr_try_read(vaddr_t addr, int len, word_t *data, vaddr_fault_t *fault);
bool vaddr_try_write(vaddr_t addr, int len, word_t data, vaddr_fault_t *fault);
#endif

#define PAGE_SHIFT 12
#define PAGE_SIZE (1ul << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)

#endif
