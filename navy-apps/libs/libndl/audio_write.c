#include "audio_write.h"

#include <stddef.h>
#include <stdint.h>

int ndl_audio_write_all(int fd, const void *buffer, int length,
                        NdlAudioWriteOnce write_once,
                        NdlAudioYield yield_once)
{
    if (buffer == NULL || length <= 0 || write_once == NULL ||
        yield_once == NULL)
    {
        return 0;
    }

    int written = 0;

    while (written < length)
    {
        const int remaining = length - written;
        int result =
            write_once(fd, (const uint8_t *)buffer + written, remaining);

        if (result < 0)
        {
            return written;
        }

        if (result == 0)
        {
            /*
             * nanos-lite-mt uses a zero-length write as bounded back-pressure
             * when NEMU's audio ring is full.  Yield in user mode so another
             * thread can run and retry only after crossing a normal syscall
             * boundary, rather than spinning inside the kernel trap handler.
             */
            yield_once();
            continue;
        }

        /*
         * A write implementation must not report more bytes than requested.
         * Clamp defensively so a malformed backend cannot move the pointer past
         * the caller's buffer.
         */
        if (result > remaining)
        {
            result = remaining;
        }

        written += result;
    }

    return written;
}
