#ifndef __MEMORY_H__
#define __MEMORY_H__

#include <common.h>

#ifndef PGSIZE
// All current VME code assumes Sv32-style 4 KiB pages: Context stacks, page
// tables, ELF segment loading, and brk growth all round to this size.
#define PGSIZE 4096
#endif

#define PG_ALIGN __attribute((aligned(PGSIZE)))

void *new_page(size_t);

#endif
