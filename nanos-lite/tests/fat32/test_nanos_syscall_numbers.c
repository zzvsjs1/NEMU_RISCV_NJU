#include <stdio.h>

#include "syscall.h"

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "CHECK failed: %s at %s:%d\n", #condition, \
                    __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    /*
     * These hand-written values protect the guest/kernel ABI.  New MT calls
     * must be appended so an existing binary still gives every old syscall the
     * meaning it had before multithreading was introduced.
     */
    CHECK(SYS_exit == 0);
    CHECK(SYS_clock_gettime == 27);
    CHECK(SYS_thread_create == 28);
    CHECK(SYS_thread_exit == 29);
    CHECK(SYS_thread_join == 30);
    CHECK(SYS_thread_self == 31);
    CHECK(SYS_thread_kill == 32);
    CHECK(SYS_mutex_lock == 33);
    CHECK(SYS_mutex_unlock == 34);

    puts("nanos syscall number tests passed");
    return 0;
}
