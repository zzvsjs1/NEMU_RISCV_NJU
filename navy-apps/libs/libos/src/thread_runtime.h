#ifndef NAVY_THREAD_RUNTIME_H
#define NAVY_THREAD_RUNTIME_H

#include <stdbool.h>

#include <reent.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Activate the MT-only libc path.  Activation is intentionally one-way:
 * ordinary applications retain the old no-op locking behaviour unless a
 * threading library explicitly opts in before creating its first worker.
 */
int nanos_thread_runtime_enable(void);

int nanos_thread_create(void (*entry)(void *), void *argument, void *stack_top);
void nanos_thread_exit(int status) __attribute__((noreturn));
int nanos_thread_join(int tid, int *status);
int nanos_thread_self(void);
int nanos_thread_kill(int tid);

/*
 * The kernel identifies a mutex by its stable user-space address.  Keeping the
 * recursive and try-only attributes separate avoids losing SDL/newlib locking
 * semantics at the syscall boundary.
 */
int nanos_lock_acquire(const void *key, bool recursive, bool try_only);
int nanos_lock_release(const void *key);

/*
 * This is newlib's Navy-specific dynamic-reentrancy provider.  Before runtime
 * activation, and for the original main thread afterwards, it returns the
 * existing global record.
 */
struct _reent *__nanos_getreent(void);

#ifdef __cplusplus
}
#endif

#endif
