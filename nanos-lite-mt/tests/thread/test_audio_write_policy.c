#include <stddef.h>
#include <stdio.h>

#include "../../../nanos-lite/src/audio_policy.h"

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            printf("check failed at line %d: %s\n", __LINE__, #condition);      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/*
 * A change that waits for a full audio ring from inside an MT syscall would
 * monopolise the sole emulated hart. These literal cases require one bounded
 * write decision based only on capacity visible at syscall entry.
 */
static int test_mt_audio_write_is_bounded_by_current_capacity(void)
{
    CHECK(mt_audio_write_count(4096, 4096, 512) == 0);
    CHECK(mt_audio_write_count(4096, 5000, 512) == 0);
    CHECK(mt_audio_write_count(4096, 4000, 512) == 96);
    CHECK(mt_audio_write_count(4096, 1000, 512) == 512);
    CHECK(mt_audio_write_count(4096, 0, 8192) == 4096);
    CHECK(mt_audio_write_count(0, 0, 512) == 0);
    CHECK(mt_audio_write_count(4096, 0, 0) == 0);
    return 0;
}

int main(void)
{
    if (test_mt_audio_write_is_bounded_by_current_capacity() != 0)
    {
        return 1;
    }

    puts("audio write policy tests passed");
    return 0;
}
