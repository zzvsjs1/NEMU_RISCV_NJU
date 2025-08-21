#include <NDL.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

static SDL_AudioSpec g_spec;
static int g_paused = 1;                 // start paused per spec
static uint32_t g_interval_ms = 0;
static uint32_t g_last_cb_ms = 0;
static int g_bytes_per_frame = 0;        // channels * bytes_per_sample

// Reentrancy guard: non-zero while we are inside the audio callback path
volatile int g_in_audio_cb = 0;

#define MAX_PUSH_BYTES   (64 * 1024)
#define BURST_CALLBACKS  4

// very small helper for clamp
static inline int16_t clamp_s16(int x) 
{
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static int read_exact(int fd, void *buf, size_t n) 
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) 
    {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;              // EOF
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        got += (size_t)r;
    }

    return 1;
}

static int skip_bytes(int fd, uint32_t n) 
{
    // Some targets may not have lseek on /proc-like FS, fallback to read+discard
    uint8_t tmp[512];
    uint32_t left = n;
    while (left) 
    {
        uint32_t chunk = left > sizeof(tmp) ? sizeof(tmp) : left;
        ssize_t r = read(fd, tmp, chunk);
        if (r <= 0) return 0;
        left -= (uint32_t)r;
    }

    return 1;
}

// Derive bytes_per_sample from SDL_AudioSpec.format for robustness.
static int bytes_per_sample_from_format(uint16_t fmt) 
{
    switch (fmt) 
    {
        case AUDIO_S16SYS: return 2;
        case AUDIO_U8:     return 1;
        // case AUDIO_F32SYS: return 4;
        default:           return 0;  // unsupported
    }
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) 
{
    assert(desired);
    memset(&g_spec, 0, sizeof(g_spec));
    g_spec = *desired;

    int bytes_per_sample = bytes_per_sample_from_format(desired->format);
    assert(bytes_per_sample != 0 && "Unsupported audio format");
    g_bytes_per_frame = desired->channels * bytes_per_sample;  // interleaved frame size in bytes

    // interval in ms between callbacks
    // protect division by zero
    assert(desired->freq > 0);
    g_interval_ms = (uint32_t)((desired->samples * 1000u) / (uint32_t)desired->freq);
    if (g_interval_ms == 0) g_interval_ms = 1;

    // init device
    NDL_OpenAudio(desired->freq, desired->channels, desired->samples);

    g_paused = 1;             // playback starts paused
    g_last_cb_ms = NDL_GetTicks();

    if (obtained) *obtained = g_spec;
    return 0;
}

void SDL_CloseAudio() 
{
    g_paused = 1;
    NDL_CloseAudio();
    memset(&g_spec, 0, sizeof(g_spec));
    g_interval_ms = 0;
    g_last_cb_ms = 0;
    g_bytes_per_frame = 0;
}

void SDL_PauseAudio(int pause_on) 
{
    g_paused = pause_on ? 1 : 0;
    if (!g_paused) 
    {
        g_last_cb_ms = 0;
        // Prefill a few times so the pipe is warm
        for (int i = 0; i < BURST_CALLBACKS; ++i) 
        {
            if (!g_in_audio_cb) CallbackHelper();
        }
    }
}


// ---------- SDL_MixAudio (support S16 and U8) ----------

void SDL_MixAudio(uint8_t *dst, uint8_t *src, uint32_t len, int volume) 
{
    if (volume <= 0 || len == 0) return;
    if (volume > SDL_MIX_MAXVOLUME) volume = SDL_MIX_MAXVOLUME;

    // Use the current output format, keep it simple for miniSDL
    if (g_spec.format == AUDIO_U8) 
    {
        // Unsigned 8-bit, 0..255 with midpoint at 128
        for (uint32_t i = 0; i < len; i++) 
        {
            int d = (int)dst[i] - 128;
            int s = ((int)src[i] - 128) * volume / SDL_MIX_MAXVOLUME;
            int mixed = d + s;
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (uint8_t)(mixed + 128);
        }
    } 
    else 
    {
        // Default to 16-bit signed little-endian
        uint32_t samples = len / 2;
        int16_t *d = (int16_t *)dst;
        int16_t *s = (int16_t *)src;
        for (uint32_t i = 0; i < samples; i++) {
            int mixed = (int)d[i] + ((int)s[i] * volume) / SDL_MIX_MAXVOLUME;
            if (mixed > 32767) mixed = 32767;
            else if (mixed < -32768) mixed = -32768;
            d[i] = (int16_t)mixed;
        }
    }
}

SDL_AudioSpec *SDL_LoadWAV(
    const char *file,
    SDL_AudioSpec *spec,
    uint8_t **audio_buf,
    uint32_t *audio_len
) {
    if (!file || !spec || !audio_buf || !audio_len) return NULL;
    *audio_buf = NULL;
    *audio_len = 0;

    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;

    // RIFF header: "RIFF" <size> "WAVE"
    char riff[4], wave[4];
    uint32_t riff_size;
    if (!read_exact(fd, riff, 4) ||
            !read_exact(fd, &riff_size, 4) ||
            !read_exact(fd, wave, 4) ||
            memcmp(riff, "RIFF", 4) != 0 ||
            memcmp(wave, "WAVE", 4) != 0) {
        close(fd);
        return NULL;
    }

    // Parse chunks until we find "fmt " and "data"
    int have_fmt = 0, have_data = 0;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate  = 0;
    uint16_t bitsPerSample = 0;

    uint32_t data_size = 0;
    uint8_t *data_ptr = NULL;

    while (1) {
        char id[4];
        uint32_t sz;

        ssize_t r = read(fd, id, 4);
        if (r == 0) break;              // EOF
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (!read_exact(fd, &sz, 4)) break;

        if (memcmp(id, "fmt ", 4) == 0) {
            // Expect at least PCM WAVEFORMAT header (16 bytes)
            uint8_t fmtbuf[32] = {0};
            if (sz < 16 || sz > sizeof(fmtbuf)) {
                // read what we can, skip the rest
                uint32_t toread = sz > sizeof(fmtbuf) ? sizeof(fmtbuf) : sz;
                if (!read_exact(fd, fmtbuf, toread)) { break; }
                if (sz > toread && !skip_bytes(fd, sz - toread)) { break; }
            } else {
                if (!read_exact(fd, fmtbuf, sz)) { break; }
            }

            audioFormat   = (uint16_t)(fmtbuf[0] | (fmtbuf[1] << 8));
            numChannels   = (uint16_t)(fmtbuf[2] | (fmtbuf[3] << 8));
            sampleRate    = (uint32_t)(fmtbuf[4] | (fmtbuf[5] << 8) | (fmtbuf[6] << 16) | (fmtbuf[7] << 24));
            // byteRate at [8..11], blockAlign at [12..13]
            bitsPerSample = (uint16_t)(fmtbuf[14] | (fmtbuf[15] << 8));

            if (audioFormat != 1 /* PCM */) {
                // compressed formats are not supported in PA
                break;
            }
            if (numChannels == 0 || sampleRate == 0 || (bitsPerSample != 8 && bitsPerSample != 16)) {
                break;
            }
            have_fmt = 1;

            // If chunk size is odd, one padding byte follows
            if (sz & 1) { if (!skip_bytes(fd, 1)) break; }

        } else if (memcmp(id, "data", 4) == 0) {
            // Allocate and read the whole PCM payload
            data_size = sz;
            data_ptr = (uint8_t *)malloc(data_size ? data_size : 1);
            if (!data_ptr) break;
            if (data_size && !read_exact(fd, data_ptr, data_size)) { free(data_ptr); data_ptr = NULL; break; }
            have_data = 1;

            // Pad byte if needed
            if (sz & 1) { if (!skip_bytes(fd, 1)) break; }

        } else {
            // Unknown chunk, skip its payload plus pad if odd
            if (!skip_bytes(fd, sz)) { break; }
            if (sz & 1) { if (!skip_bytes(fd, 1)) break; }
        }

        if (have_fmt && have_data) break;
    }

    close(fd);

    if (!have_fmt || !have_data || !data_ptr) {
        if (data_ptr) free(data_ptr);
        return NULL;
    }

    // Fill spec from fmt
    memset(spec, 0, sizeof(*spec));
    spec->freq     = (int)sampleRate;
    spec->channels = (uint8_t)numChannels;
    spec->format   = (bitsPerSample == 8) ? AUDIO_U8 : AUDIO_S16SYS;
    // Choose a reasonable default buffer size for callback-driven playback
    spec->samples  = 1024;

    *audio_buf = data_ptr;
    *audio_len = data_size;
    return spec;
}

void SDL_FreeWAV(uint8_t *audio_buf) 
{
    if (audio_buf) free(audio_buf);
}

void SDL_LockAudio() 
{

}

void SDL_UnlockAudio() 
{
    
}

// Call this very frequently from event or delay APIs.
// Also call it a few times right after unpausing for prefill.
void CallbackHelper(void) {
    // Fast exits for common conditions
    if (g_paused) return;
    if (!g_spec.callback) return;
    if (g_bytes_per_frame <= 0) return;   // misconfigured format

    // Prevent recursive entry if callback calls APIs that also call CallbackHelper
    if (g_in_audio_cb) return;

    // Query free space first, skip if none
    int free_bytes = NDL_QueryAudio();
    if (free_bytes <= 0) return;

    uint32_t now = NDL_GetTicks();

    // Respect the nominal schedule unless we can feed at least one frame
    if ((now - g_last_cb_ms) < g_interval_ms && free_bytes < g_bytes_per_frame) {
        return;
    }

    const int target_per_cb = g_spec.samples * g_bytes_per_frame;

    static uint8_t buf[MAX_PUSH_BYTES];
    int bursts = 0;
    int fed_any = 0;

    // Enter reentrancy-protected region
    g_in_audio_cb = 1;

    // Feed several chunks back to back, until space drops, or we hit limits
    while (free_bytes >= g_bytes_per_frame && bursts < BURST_CALLBACKS) {
        int chunk = (free_bytes < target_per_cb) ? free_bytes : target_per_cb;
        if (chunk > MAX_PUSH_BYTES) chunk = MAX_PUSH_BYTES;

        // Align to whole frames to keep channels in sync
        chunk = (chunk / g_bytes_per_frame) * g_bytes_per_frame;
        if (chunk <= 0) break;

        // App fills PCM into buf
        g_spec.callback(g_spec.userdata, buf, chunk);

        // Push to device, /dev/sb is blocking per write, but we cap size
        NDL_PlayAudio(buf, chunk);

        fed_any = 1;
        bursts += 1;

        // Re-query to minimize races with hardware consumption
        free_bytes = NDL_QueryAudio();
    }

    if (fed_any) g_last_cb_ms = now;

    // Leave protected region
    g_in_audio_cb = 0;
}