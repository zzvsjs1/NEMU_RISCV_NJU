#include <NDL.h>
#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static SDL_AudioSpec g_spec;
static int g_paused = 1; // start paused per spec
static int g_bytes_per_frame = 0; // channels * bytes_per_sample
static int g_device_capacity = 0;
static int g_target_queue_bytes = 0;
static int g_audio_lock_depth = 0;

// Reentrancy guard: non-zero while we are inside the audio callback path
volatile int g_in_audio_cb = 0;

#define MAX_PUSH_BYTES (64 * 1024)

/*
 * PAL drives miniSDL audio from places such as SDL_Delay() and event polling,
 * not from a dedicated guest audio thread.  At 800x600 a video frame can take
 * long enough that one audio pump is effectively the only chance to feed the
 * device before the host SDL callback wakes several times.
 *
 * With PAL's common 44.1 kHz, stereo, S16 format, one 1024-sample callback is
 * 1024 * 2 channels * 2 bytes = 4096 bytes.  The pump keeps about eight such
 * chunks queued.  That gives slow video frames headroom, but avoids filling the
 * whole 64 KiB device buffer and adding hundreds of milliseconds of latency.
 */
#define TARGET_QUEUE_CALLBACKS 8
#define MAX_CALLBACKS_PER_PUMP 8

void CallbackHelper(void);

static int readExact(int fd, void *buf, size_t n);

// very small helper for clamp
static inline int16_t clampS16(int x)
{
    if (x > 32767)
        return 32767;

    if (x < -32768)
        return -32768;
    return (int16_t)x;
}

static int alignDownToFrame(int bytes)
{
    if (bytes <= 0 || g_bytes_per_frame <= 0)
        return 0;
    return (bytes / g_bytes_per_frame) * g_bytes_per_frame;
}

static uint16_t rdLE16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rdLE32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int readLE32(int fd, uint32_t *out)
{
    uint8_t b[4];

    if (!readExact(fd, b, sizeof(b)))
        return 0;

    *out = rdLE32(b);
    return 1;
}

static int readS16Native(const uint8_t *p)
{
    int16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void writeS16Native(uint8_t *p, int16_t value)
{
    memcpy(p, &value, sizeof(value));
}

static int readExact(int fd, void *buf, size_t n)
{
    /*
     * WAV chunk headers are tiny but must be complete.  Keep retrying across
     * EINTR so a signal cannot make a valid asset look truncated to the loader.
     */
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(fd, p + got, n - got);

        if (r == 0)
            return 0; // EOF

        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }
        got += (size_t)r;
    }

    return 1;
}

static int skipBytes(int fd, uint32_t n)
{
    // Some targets may not have lseek on /proc-like FS, fallback to read+discard
    uint8_t tmp[512];
    uint32_t left = n;
    while (left)
    {
        uint32_t chunk = left > sizeof(tmp) ? sizeof(tmp) : left;
        ssize_t r = read(fd, tmp, chunk);

        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }

        if (r == 0)
            return 0;
        left -= (uint32_t)r;
    }

    return 1;
}

// Derive bytes_per_sample from SDL_AudioSpec.format for robustness.
static int bytesPerSampleFromFormat(uint16_t fmt)
{
    switch (fmt)
    {
    case AUDIO_S16SYS:
        return 2;
    case AUDIO_U8:
        return 1;
    // case AUDIO_F32SYS: return 4;
    default:
        return 0; // unsupported
    }
}

static void configureQueueWatermark(void)
{
    int capacity = NDL_QueryAudio();

    if (capacity < 0)
        capacity = 0;

    g_device_capacity = alignDownToFrame(capacity);
    g_target_queue_bytes = 0;

    if (g_device_capacity <= 0 || g_bytes_per_frame <= 0)
        return;

    const int target_per_cb = alignDownToFrame((int)g_spec.size);
    uint64_t target = (uint64_t)target_per_cb * TARGET_QUEUE_CALLBACKS;

    if (target > (uint64_t)g_device_capacity)
        target = (uint64_t)g_device_capacity;

    g_target_queue_bytes = alignDownToFrame((int)target);

    if (g_target_queue_bytes <= 0)
        g_target_queue_bytes = g_bytes_per_frame;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (!desired)
        return -1;

    const int bytes_per_sample = bytesPerSampleFromFormat(desired->format);

    if (bytes_per_sample == 0 ||
        desired->freq <= 0 ||
        desired->channels == 0 ||
        desired->samples == 0)
    {
        return -1;
    }

    if ((int)desired->channels > INT_MAX / bytes_per_sample)
        return -1;

    const int bytes_per_frame = (int)desired->channels * bytes_per_sample;

    if ((int)desired->samples > INT_MAX / bytes_per_frame)
        return -1;

    /*
     * miniSDL accepts the caller's requested format as the obtained format.
     * The NDL sound device only receives frequency, channel count, and buffer
     * size, so format conversion is deliberately handled above this layer.
     */
    memset(&g_spec, 0, sizeof(g_spec));
    g_spec = *desired;
    g_spec.silence = (desired->format == AUDIO_U8) ? 0x80 : 0x00;
    g_spec.padding = 0;
    g_spec.size = (uint32_t)((int)desired->samples * bytes_per_frame);

    g_bytes_per_frame = bytes_per_frame; // interleaved frame size in bytes
    g_device_capacity = 0;
    g_target_queue_bytes = 0;
    g_audio_lock_depth = 0;
    g_in_audio_cb = 0;

    // init device
    NDL_OpenAudio(desired->freq, desired->channels, desired->samples);
    configureQueueWatermark();

    g_paused = 1; // playback starts paused

    if (obtained)
        *obtained = g_spec;
    return 0;
}

void SDL_CloseAudio()
{
    g_paused = 1;
    NDL_CloseAudio();
    memset(&g_spec, 0, sizeof(g_spec));
    g_bytes_per_frame = 0;
    g_device_capacity = 0;
    g_target_queue_bytes = 0;
    g_audio_lock_depth = 0;
    g_in_audio_cb = 0;
}

void SDL_PauseAudio(int pause_on)
{
    g_paused = pause_on ? 1 : 0;

    if (!g_paused)
    {
        /*
         * Prefill synchronously on unpause.  There is no background audio thread
         * in miniSDL, so waiting for the next event/timer poll can leave NEMU's
         * host callback with silence immediately after SDL_PauseAudio(0).  The
         * helper itself applies the queue watermark, so this cannot fill the
         * whole emulated device buffer in one unpause call.
         */
        CallbackHelper();
    }
}

// SDL_MixAudio (support S16 and U8)
void SDL_MixAudio(uint8_t *dst, const uint8_t *src, uint32_t len, int volume)
{
    if (!dst || !src || volume <= 0 || len == 0)
        return;

    if (volume > SDL_MIX_MAXVOLUME)
        volume = SDL_MIX_MAXVOLUME;

    switch (g_spec.format)
    {
    case AUDIO_U8:
    {
        // Unsigned 8-bit, 0..255 with midpoint at 128
        for (uint32_t i = 0; i < len; i++)
        {
            int d = (int)dst[i] - 128;
            int s = ((int)src[i] - 128) * volume / SDL_MIX_MAXVOLUME;
            int mixed = d + s;

            if (mixed > 127)
                mixed = 127;

            if (mixed < -128)
                mixed = -128;
            dst[i] = (uint8_t)(mixed + 128);
        }
        break;
    }

    case AUDIO_S16SYS:
    {
        // Native-endian S16SYS, using byte copies so unaligned buffers work.
        uint32_t samples = len / 2;
        for (uint32_t i = 0; i < samples; i++)
        {
            uint8_t *d = dst + i * 2;
            const uint8_t *s = src + i * 2;
            int mixed = readS16Native(d) +
                        (readS16Native(s) * volume) / SDL_MIX_MAXVOLUME;
            writeS16Native(d, clampS16(mixed));
        }
        break;
    }

    default:
        break;
    }
}

SDL_AudioSpec *SDL_LoadWAV(
    const char *file,
    SDL_AudioSpec *spec,
    uint8_t **audio_buf,
    uint32_t *audio_len)
{
    if (!file || !spec || !audio_buf || !audio_len)
        return NULL;
    *audio_buf = NULL;
    *audio_len = 0;

    int fd = open(file, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return NULL;

    // RIFF header: "RIFF" <size> "WAVE"
    char riff[4], wave[4];
    uint32_t riff_size;

    if (!readExact(fd, riff, 4) ||
        !readLE32(fd, &riff_size) ||
        !readExact(fd, wave, 4) ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(wave, "WAVE", 4) != 0)
    {
        close(fd);
        return NULL;
    }
    (void)riff_size;

    // Parse chunks until we find "fmt " and "data"
    int have_fmt = 0, have_data = 0;
    int parse_ok = 1;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;

    uint32_t data_size = 0;
    uint8_t *data_ptr = NULL;

    while (1)
    {
        char id[4];
        uint32_t sz;

        /*
         * RIFF chunks may appear in either order and can include metadata that
         * SDL applications do not care about.  Walk the chunk list instead of
         * assuming a fixed 44-byte header layout.
         */
        if (!readExact(fd, id, 4))
            break;

        if (!readLE32(fd, &sz))
        {
            parse_ok = 0;
            break;
        }

        if (memcmp(id, "fmt ", 4) == 0)
        {
            // Expect at least PCM WAVEFORMAT header (16 bytes)
            uint8_t fmtbuf[32] = {0};

            if (sz < 16 || sz > sizeof(fmtbuf))
            {
                // read what we can, skip the rest
                uint32_t toread = sz > sizeof(fmtbuf) ? sizeof(fmtbuf) : sz;

                if (!readExact(fd, fmtbuf, toread))
                {
                    parse_ok = 0;
                    break;
                }

                if (sz > toread && !skipBytes(fd, sz - toread))
                {
                    parse_ok = 0;
                    break;
                }
            }
            else
            {
                if (!readExact(fd, fmtbuf, sz))
                {
                    parse_ok = 0;
                    break;
                }
            }

            audioFormat = rdLE16(&fmtbuf[0]);
            numChannels = rdLE16(&fmtbuf[2]);
            sampleRate = rdLE32(&fmtbuf[4]);
            // byteRate at [8..11]
            blockAlign = rdLE16(&fmtbuf[12]);
            bitsPerSample = rdLE16(&fmtbuf[14]);

            if (audioFormat != 1 /* PCM */)
            {
                // compressed formats are not supported in PA
                parse_ok = 0;
                break;
            }

            if (numChannels == 0 || numChannels > UINT8_MAX ||
                sampleRate == 0 || sampleRate > INT_MAX ||
                (bitsPerSample != 8 && bitsPerSample != 16))
            {
                parse_ok = 0;
                break;
            }

            const uint32_t expectedAlign =
                (uint32_t)numChannels * (uint32_t)(bitsPerSample / 8);

            if (blockAlign == 0 || blockAlign != expectedAlign)
            {
                parse_ok = 0;
                break;
            }

            // If chunk size is odd, one padding byte follows

            if (sz & 1)
            {
                if (!skipBytes(fd, 1))
                {
                    parse_ok = 0;
                    break;
                }
            }

            have_fmt = 1;
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            if (have_data)
            {
                if (!skipBytes(fd, sz))
                {
                    parse_ok = 0;
                    break;
                }

                if ((sz & 1) && !skipBytes(fd, 1))
                {
                    parse_ok = 0;
                    break;
                }
                continue;
            }

            // Allocate and read the whole PCM payload
            data_size = sz;
            data_ptr = (uint8_t *)malloc(data_size ? data_size : 1);

            if (!data_ptr)
                break;

            if (data_size && !readExact(fd, data_ptr, data_size))
            {
                free(data_ptr);
                data_ptr = NULL;
                parse_ok = 0;
                break;
            }

            // Pad byte if needed

            if (sz & 1)
            {
                if (!skipBytes(fd, 1))
                {
                    parse_ok = 0;
                    break;
                }
            }

            have_data = 1;
        }
        else
        {
            // Unknown chunk, skip its payload plus pad if odd

            if (!skipBytes(fd, sz))
            {
                parse_ok = 0;
                break;
            }

            if (sz & 1)
            {
                if (!skipBytes(fd, 1))
                {
                    parse_ok = 0;
                    break;
                }
            }
        }

        if (have_fmt && have_data)
            break;
    }

    close(fd);

    if (!parse_ok || !have_fmt || !have_data || !data_ptr)
    {
        if (data_ptr)
            free(data_ptr);
        return NULL;
    }

    if (blockAlign > 0)
        data_size -= data_size % blockAlign;

    // Fill spec from fmt
    memset(spec, 0, sizeof(*spec));
    spec->freq = (int)sampleRate;
    spec->channels = (uint8_t)numChannels;
    spec->format = (bitsPerSample == 8) ? AUDIO_U8 : AUDIO_S16SYS;
    spec->silence = (spec->format == AUDIO_U8) ? 0x80 : 0x00;
    // Choose a reasonable default buffer size for callback-driven playback
    spec->samples = 1024;
    spec->size = (uint32_t)spec->samples * spec->channels *
                 (uint32_t)(bitsPerSample / 8);

    *audio_buf = data_ptr;
    *audio_len = data_size;
    return spec;
}

void SDL_FreeWAV(uint8_t *audio_buf)
{
    if (audio_buf)
        free(audio_buf);
}

void SDL_LockAudio()
{
    /*
     * Navy currently pumps audio synchronously from SDL APIs instead of using
     * a pre-emptive callback thread.  The lock still matters for cooperative
     * re-entry: code may update mixer state, call SDL_PollEvent() or
     * SDL_Delay(), and only then finish the update.  Deferring the pump while
     * locked keeps the callback from observing half-written state.
     */
    if (g_audio_lock_depth < INT_MAX)
        g_audio_lock_depth++;
}

void SDL_UnlockAudio()
{
    if (g_audio_lock_depth > 0)
        g_audio_lock_depth--;

    if (g_audio_lock_depth == 0 && !g_in_audio_cb)
        CallbackHelper();
}

// Call this very frequently from event or delay APIs.
// SDL_PauseAudio(0) also calls it once for bounded prefill.
void CallbackHelper(void)
{
    // Fast exits for common conditions

    if (g_paused)
        return;

    if (!g_spec.callback)
        return;

    if (g_bytes_per_frame <= 0)
        return; // misconfigured format

    if (g_audio_lock_depth > 0)
        return;

    // Prevent recursive entry if callback calls APIs that also call CallbackHelper

    if (g_in_audio_cb)
        return;

    // Query free space first, skip if none
    /*
     * NDL_QueryAudio() returns free bytes in the device queue, not queued
     * bytes.  Only ask the application for samples that can be accepted soon;
     * this keeps the blocking /dev/sb write bounded and prevents latency from
     * growing beyond the emulated device buffer.
     */
    int free_bytes = NDL_QueryAudio();

    if (free_bytes <= 0)
        return;

    free_bytes = alignDownToFrame(free_bytes);

    if (free_bytes <= 0)
        return;

    if (g_device_capacity <= 0 || free_bytes > g_device_capacity)
    {
        g_device_capacity = alignDownToFrame(free_bytes);
        configureQueueWatermark();
    }

    if (g_target_queue_bytes <= 0)
        return;

    int queued_bytes = g_device_capacity - free_bytes;

    if (queued_bytes < 0)
        queued_bytes = 0;

    /*
     * Hysteresis avoids tiny callback lengths.  Refill when the queue has
     * dropped by at least one normal callback chunk, then top it back up to the
     * target watermark.
     */
    const int target_per_cb = alignDownToFrame((int)g_spec.size);
    const int low_watermark =
        g_target_queue_bytes > target_per_cb ? g_target_queue_bytes - target_per_cb : 0;

    if (queued_bytes > low_watermark)
        return;

    int fill_budget = g_target_queue_bytes - queued_bytes;

    if (fill_budget <= 0)
        return;

    static uint8_t buf[MAX_PUSH_BYTES];
    int bursts = 0;
    int produced = 0;

    // Enter reentrancy-protected region
    /*
     * Some game audio callbacks call back into SDL APIs that may pump events or
     * timers.  The guard keeps that from nesting another CallbackHelper() and
     * overproducing samples against the same free-space snapshot.
     */
    g_in_audio_cb = 1;

    // Feed several chunks back to back, until the watermark is restored.
    while (free_bytes >= g_bytes_per_frame &&
           produced < fill_budget &&
           bursts < MAX_CALLBACKS_PER_PUMP)
    {
        int chunk = target_per_cb;

        if (chunk > fill_budget - produced)
            chunk = fill_budget - produced;

        if (chunk > free_bytes)
            chunk = free_bytes;

        if (chunk > MAX_PUSH_BYTES)
            chunk = MAX_PUSH_BYTES;

        // Align to whole frames to keep channels in sync
        chunk = (chunk / g_bytes_per_frame) * g_bytes_per_frame;

        if (chunk <= 0)
            break;

        // App fills PCM into buf
        g_spec.callback(g_spec.userdata, buf, chunk);

        // Push to device, /dev/sb is blocking per write, but we cap size
        /*
         * /dev/sb accepts a byte stream.  The frame alignment above preserves
         * interleaved channel boundaries, so a partial refill cannot swap left
         * and right samples on the next callback.
         */
        int played = NDL_PlayAudio(buf, chunk);

        if (played <= 0)
            break;

        produced += alignDownToFrame(played);
        bursts += 1;

        if (played != chunk)
            break;

        // Re-query to minimize races with hardware consumption
        free_bytes = NDL_QueryAudio();

        if (free_bytes <= 0)
            break;

        free_bytes = alignDownToFrame(free_bytes);
    }

    // Leave protected region
    g_in_audio_cb = 0;
}

void SDL_PumpAudio(void)
{
    if (!g_in_audio_cb)
    {
        CallbackHelper();
    }
}
