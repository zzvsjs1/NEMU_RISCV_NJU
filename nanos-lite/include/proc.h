#ifndef __PROC_H__
#define __PROC_H__

#include <common.h>
#include <memory.h>

#define STACK_SIZE (8 * PGSIZE)

enum {
  MAX_NR_PROC = 4,
  NR_FOREGROUND_PROC = 3,
  HELLO_PROC = 3,
};

typedef union {
  uint8_t stack[STACK_SIZE] PG_ALIGN;
  struct {
    Context *cp;
    AddrSpace as;
    // we do not free memory, so use `max_brk' to determine when to call _map()
    uintptr_t max_brk;
  };
} PCB;

extern PCB *current;

void switch_fg_pcb(int index);

/*
 * Device code sometimes needs to attribute a shared device operation to the
 * running user process. Keep PCB storage private to proc.c and expose only
 * stable slot indexes: 0..NR_FOREGROUND_PROC-1 are switchable foreground apps,
 * HELLO_PROC is the background hello task, and -1 means boot/unknown.
 */
int current_pcb_index(void);
int foreground_pcb_index(void);

#endif
