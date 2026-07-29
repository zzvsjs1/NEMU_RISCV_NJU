#ifndef NANOS_LITE_MT_PROC_H
#define NANOS_LITE_MT_PROC_H

#include <common.h>
#include <memory.h>

#define STACK_SIZE (8 * PGSIZE)

enum
{
    MAX_NR_PROC = 4,
    NR_FOREGROUND_PROC = 3,
    HELLO_PROC = 3,
};

typedef union
{
    /*
     * Keep the original Nanos-lite PCB layout exactly.  The shared loader and
     * memory manager place a process's initial Context at the top of this
     * kernel stack and keep address-space metadata at its low end.
     */
    uint8_t stack[STACK_SIZE] PG_ALIGN;

    struct
    {
        Context *cp;
        AddrSpace as;
        uintptr_t max_brk;
    };
} PCB;

extern PCB *current;

bool switch_fg_pcb(int index);
int current_pcb_index(void);
int foreground_pcb_index(void);

/*
 * These hooks are compiled only by nanos-lite-mt.  syscall.c remains shared
 * with the single-thread kernel and calls them behind NANOS_LITE_MT guards.
 */
bool mt_handle_syscall(Context *context, uintptr_t number, uintptr_t argument1,
                       uintptr_t argument2, uintptr_t argument3);
void mt_process_context_replaced(Context *replacement);

#endif
