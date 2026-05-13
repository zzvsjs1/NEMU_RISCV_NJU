#include <NDL.h>
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "%s:%d: check failed: %s\n",                   \
                    __FILE__, __LINE__, #cond);                             \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

static int fake_open_audio_calls = 0;
static int fake_close_audio_calls = 0;
static int fake_audio_freq = 0;
static int fake_audio_channels = 0;
static int fake_audio_samples = 0;
static int fake_audio_capacity = 0;
static int fake_audio_free = 0;
static int fake_audio_play_calls = 0;
static int fake_audio_played = 0;
static uint32_t fake_ticks = 0;

uint32_t NDL_GetTicks(void)
{
    return fake_ticks;
}

void NDL_OpenAudio(int freq, int channels, int samples)
{
    fake_open_audio_calls++;
    fake_audio_freq = freq;
    fake_audio_channels = channels;
    fake_audio_samples = samples;
    fake_audio_free = fake_audio_capacity;
}

void NDL_CloseAudio(void)
{
    fake_close_audio_calls++;
}

int NDL_PlayAudio(void *buf, int len)
{
    CHECK(buf != NULL);
    CHECK(len >= 0);

    int accepted = len;
    if (accepted > fake_audio_free)
        accepted = fake_audio_free;

    fake_audio_free -= accepted;
    fake_audio_play_calls++;
    fake_audio_played += accepted;
    return accepted;
}

int NDL_QueryAudio(void)
{
    return fake_audio_free;
}

#include "../libs/libminiSDL/src/audio.c"

static int callback_calls = 0;
static int callback_bytes = 0;

static void fill_audio(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    callback_calls++;
    callback_bytes += len;
    memset(stream, 0x5a, (size_t)len);
}

static void reset_fake_audio(int capacity)
{
    SDL_CloseAudio();
    fake_open_audio_calls = 0;
    fake_close_audio_calls = 0;
    fake_audio_freq = 0;
    fake_audio_channels = 0;
    fake_audio_samples = 0;
    fake_audio_capacity = capacity;
    fake_audio_free = capacity;
    fake_audio_play_calls = 0;
    fake_audio_played = 0;
    fake_ticks = 0;
    callback_calls = 0;
    callback_bytes = 0;
}

static SDL_AudioSpec valid_spec(void)
{
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.freq = 44100;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;
    spec.callback = fill_audio;
    return spec;
}

static void test_open_audio_rejects_invalid_runtime_inputs(void)
{
    reset_fake_audio(65536);

    SDL_AudioSpec spec = valid_spec();
    SDL_AudioSpec obtained;
    memset(&obtained, 0, sizeof(obtained));

    spec.format = 0x9999;
    CHECK(SDL_OpenAudio(&spec, &obtained) == -1);
    CHECK(fake_open_audio_calls == 0);

    spec = valid_spec();
    spec.channels = 0;
    CHECK(SDL_OpenAudio(&spec, &obtained) == -1);
    CHECK(fake_open_audio_calls == 0);

    spec = valid_spec();
    spec.samples = 0;
    CHECK(SDL_OpenAudio(&spec, &obtained) == -1);
    CHECK(fake_open_audio_calls == 0);
}

static void test_open_audio_fills_obtained_transfer_size(void)
{
    reset_fake_audio(65536);

    SDL_AudioSpec spec = valid_spec();
    SDL_AudioSpec obtained;
    memset(&obtained, 0, sizeof(obtained));

    CHECK(SDL_OpenAudio(&spec, &obtained) == 0);
    CHECK(fake_open_audio_calls == 1);
    CHECK(fake_audio_freq == 44100);
    CHECK(fake_audio_channels == 2);
    CHECK(fake_audio_samples == 1024);
    CHECK(obtained.size == 4096);
}

static void test_unpause_prefill_uses_watermark_not_whole_device(void)
{
    reset_fake_audio(65536);

    SDL_AudioSpec spec = valid_spec();
    CHECK(SDL_OpenAudio(&spec, NULL) == 0);
    SDL_PauseAudio(0);

    /*
     * A 1024-sample stereo S16 callback is 4096 bytes.  The low-underrun
     * watermark keeps eight callbacks queued, not the full 64 KiB device.
     */
    CHECK(callback_calls == 8);
    CHECK(callback_bytes == 32768);
    CHECK(fake_audio_played == 32768);
    CHECK(fake_audio_free == 32768);

    SDL_PumpAudio();
    CHECK(callback_calls == 8);
    CHECK(fake_audio_played == 32768);
}

static void test_lock_audio_defers_cooperative_callbacks(void)
{
    reset_fake_audio(65536);

    SDL_AudioSpec spec = valid_spec();
    CHECK(SDL_OpenAudio(&spec, NULL) == 0);

    SDL_LockAudio();
    SDL_PauseAudio(0);
    SDL_PumpAudio();
    CHECK(callback_calls == 0);
    CHECK(fake_audio_played == 0);

    SDL_UnlockAudio();
    CHECK(callback_calls == 8);
    CHECK(fake_audio_played == 32768);
}

static void write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0)
    {
        ssize_t written = write(fd, p, len);
        CHECK(written > 0);
        p += written;
        len -= (size_t)written;
    }
}

static void write_u16le(int fd, uint16_t value)
{
    uint8_t b[2];
    b[0] = (uint8_t)(value & 0xff);
    b[1] = (uint8_t)(value >> 8);
    write_all(fd, b, sizeof(b));
}

static void write_u32le(int fd, uint32_t value)
{
    uint8_t b[4];
    b[0] = (uint8_t)(value & 0xff);
    b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16);
    b[3] = (uint8_t)(value >> 24);
    write_all(fd, b, sizeof(b));
}

static char *make_pcm_wav(uint16_t channels, uint16_t bits_per_sample,
                          uint16_t block_align, uint32_t data_size,
                          int include_pad)
{
    char template[] = "/tmp/minisdl-audio-XXXXXX";
    int fd = mkstemp(template);
    CHECK(fd >= 0);

    const uint32_t fmt_size = 16;
    const uint32_t riff_size = 4 + 8 + fmt_size + 8 + data_size;
    const uint32_t sample_rate = 44100;
    const uint32_t byte_rate = sample_rate * block_align;

    write_all(fd, "RIFF", 4);
    write_u32le(fd, riff_size);
    write_all(fd, "WAVE", 4);
    write_all(fd, "fmt ", 4);
    write_u32le(fd, fmt_size);
    write_u16le(fd, 1);
    write_u16le(fd, channels);
    write_u32le(fd, sample_rate);
    write_u32le(fd, byte_rate);
    write_u16le(fd, block_align);
    write_u16le(fd, bits_per_sample);
    write_all(fd, "data", 4);
    write_u32le(fd, data_size);

    for (uint32_t i = 0; i < data_size; i++)
    {
        uint8_t byte = (uint8_t)i;
        write_all(fd, &byte, 1);
    }

    if ((data_size & 1) && include_pad)
    {
        uint8_t pad = 0;
        write_all(fd, &pad, 1);
    }

    CHECK(close(fd) == 0);
    return strdup(template);
}

static void test_load_wav_rejects_bad_block_align(void)
{
    char *path = make_pcm_wav(2, 16, 2, 8, 1);
    SDL_AudioSpec spec;
    uint8_t *audio = NULL;
    uint32_t len = 0;

    CHECK(SDL_LoadWAV(path, &spec, &audio, &len) == NULL);
    CHECK(audio == NULL);
    CHECK(len == 0);

    unlink(path);
    free(path);
}

static void test_load_wav_truncates_partial_frame_data(void)
{
    char *path = make_pcm_wav(2, 16, 4, 5, 1);
    SDL_AudioSpec spec;
    uint8_t *audio = NULL;
    uint32_t len = 0;

    CHECK(SDL_LoadWAV(path, &spec, &audio, &len) == &spec);
    CHECK(audio != NULL);
    CHECK(len == 4);
    CHECK(spec.freq == 44100);
    CHECK(spec.channels == 2);
    CHECK(spec.format == AUDIO_S16SYS);

    SDL_FreeWAV(audio);
    unlink(path);
    free(path);
}

static void test_load_wav_rejects_missing_odd_chunk_pad(void)
{
    char *path = make_pcm_wav(2, 16, 4, 5, 0);
    SDL_AudioSpec spec;
    uint8_t *audio = NULL;
    uint32_t len = 0;

    CHECK(SDL_LoadWAV(path, &spec, &audio, &len) == NULL);
    CHECK(audio == NULL);
    CHECK(len == 0);

    unlink(path);
    free(path);
}

static void test_mix_audio_ignores_unknown_format(void)
{
    uint8_t dst[4] = {1, 2, 3, 4};
    uint8_t src[4] = {100, 100, 100, 100};
    uint8_t before[4];
    memcpy(before, dst, sizeof(before));

    g_spec.format = 0x9999;
    SDL_MixAudio(dst, src, sizeof(dst), SDL_MIX_MAXVOLUME);
    CHECK(memcmp(dst, before, sizeof(dst)) == 0);
}

int main(void)
{
    test_open_audio_rejects_invalid_runtime_inputs();
    test_open_audio_fills_obtained_transfer_size();
    test_unpause_prefill_uses_watermark_not_whole_device();
    test_lock_audio_defers_cooperative_callbacks();
    test_load_wav_rejects_bad_block_align();
    test_load_wav_truncates_partial_frame_data();
    test_load_wav_rejects_missing_odd_chunk_pad();
    test_mix_audio_ignores_unknown_format();
    puts("miniSDL audio unit tests passed");
    return 0;
}
