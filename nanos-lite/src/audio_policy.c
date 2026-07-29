#include "audio_policy.h"

size_t mt_audio_write_count(size_t capacity, size_t used, size_t requested)
{
    if (capacity == 0 || requested == 0 || used >= capacity)
    {
        return 0;
    }

    const size_t available = capacity - used;
    return requested < available ? requested : available;
}
