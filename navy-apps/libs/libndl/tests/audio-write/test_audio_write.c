#include "audio_write.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__,          \
                    #condition);                                                 \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const int scripted_results[] = {3, 0, 2, 3};
static int script_index;
static int yield_count;
static unsigned char observed[8];
static int observed_count;

static int scripted_write(int fd, const void *buffer, int length)
{
    (void)fd;
    int result = scripted_results[script_index++];

    if (result > length)
    {
        result = length;
    }

    if (result > 0)
    {
        memcpy(observed + observed_count, buffer, (size_t)result);
        observed_count += result;
    }

    return result;
}

static void record_yield(void)
{
    yield_count++;
}

static int failing_write(int fd, const void *buffer, int length)
{
    (void)fd;
    (void)buffer;
    (void)length;
    return -1;
}

int main(void)
{
    static const unsigned char payload[] = "12345678";

    CHECK(ndl_audio_write_all(7, payload, 8, scripted_write,
                              record_yield) == 8);
    CHECK(script_index == 4);
    CHECK(yield_count == 1);
    CHECK(observed_count == 8);
    CHECK(memcmp(observed, payload, 8) == 0);

    CHECK(ndl_audio_write_all(7, payload, 8, failing_write,
                              record_yield) == 0);

    puts("NDL audio retry tests passed");
    return 0;
}
