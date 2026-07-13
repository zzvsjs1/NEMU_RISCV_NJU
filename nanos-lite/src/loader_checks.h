#ifndef NANOS_LOADER_CHECKS_H__
#define NANOS_LOADER_CHECKS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keep these calculations free of loader side effects so host tests can verify
 * the same arithmetic used before the kernel looks up or maps any page.
 */
static inline bool nanos_loader_checked_add_uintptr(uintptr_t base,
                                                    uintptr_t increment,
                                                    uintptr_t *result)
{
    if (increment > UINTPTR_MAX - base)
    {
        return false;
    }

    if (result != NULL)
    {
        *result = base + increment;
    }

    return true;
}

/*
 * A mapped image must remain in user memory and leave the fixed initial stack
 * untouched.  An empty segment may end exactly at the stack base; callers
 * normally skip empty PT_LOAD entries before requesting any page mapping.
 */
static inline bool nanos_loader_load_range_fits(uintptr_t user_start,
                                                uintptr_t user_end,
                                                size_t stack_bytes,
                                                uintptr_t segment_start,
                                                uintptr_t segment_bytes,
                                                uintptr_t *segment_end)
{
    uintptr_t end;
    uintptr_t stack_base;

    if (user_end < user_start || stack_bytes > user_end - user_start ||
        !nanos_loader_checked_add_uintptr(segment_start, segment_bytes, &end))
    {
        return false;
    }

    stack_base = user_end - (uintptr_t)stack_bytes;

    if (segment_start < user_start || end > stack_base)
    {
        return false;
    }

    if (segment_end != NULL)
    {
        *segment_end = end;
    }

    return true;
}

/*
 * The initial stack grows down from stack_end.  This checks the complete
 * string area, the alignment gap, and every word in the argc/argv/envp table
 * before the first physical-memory write takes place.
 */
static inline bool nanos_loader_stack_layout_fits(uintptr_t stack_base,
                                                  uintptr_t stack_end,
                                                  size_t string_bytes,
                                                  size_t pointer_words,
                                                  uintptr_t *initial_sp)
{
    uintptr_t string_bottom;
    uintptr_t aligned_bottom;
    size_t pointer_bytes;

    if (stack_end < stack_base || string_bytes > stack_end - stack_base ||
        pointer_words > (size_t)-1 / sizeof(uintptr_t))
    {
        return false;
    }

    string_bottom = stack_end - (uintptr_t)string_bytes;
    aligned_bottom = string_bottom - string_bottom % sizeof(uintptr_t);
    pointer_bytes = pointer_words * sizeof(uintptr_t);

    if (aligned_bottom < stack_base || pointer_bytes > aligned_bottom - stack_base)
    {
        return false;
    }

    if (initial_sp != NULL)
    {
        *initial_sp = aligned_bottom - (uintptr_t)pointer_bytes;
    }

    return true;
}

#endif
