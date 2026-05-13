#include <SDL.h>
#include <SDL_mixer.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    SDL_RWops rw;
    const uint8_t *data;
    int64_t size;
    int64_t pos;
} MemoryRW;

typedef struct
{
    SDL_RWops rw;
    int calls;
} OversizedRW;

static int lock_calls;
static int unlock_calls;
static void *tracked_free_forbidden;
static int tracked_free_forbidden_count;

static void tracked_free(void *ptr)
{
    if (ptr == tracked_free_forbidden)
    {
        tracked_free_forbidden_count++;
        return;
    }

    free(ptr);
}

#define free tracked_free
#include "../libs/libSDL_mixer/src/mixer.c"
#undef free

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #cond);                                       \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (obtained != NULL)
        *obtained = *desired;
    return 0;
}

void SDL_CloseAudio(void) {}
void SDL_PauseAudio(int pause_on) { (void)pause_on; }

void SDL_LockAudio(void)
{
    lock_calls++;
}

void SDL_UnlockAudio(void)
{
    unlock_calls++;
}

static void reset_lock_counts(void)
{
    lock_calls = 0;
    unlock_calls = 0;
}

static int64_t memory_size(SDL_RWops *rw)
{
    MemoryRW *ctx = (MemoryRW *)rw;
    return ctx->size;
}

static int64_t memory_seek(SDL_RWops *rw, int64_t offset, int whence)
{
    MemoryRW *ctx = (MemoryRW *)rw;
    int64_t base = 0;

    if (whence == RW_SEEK_CUR)
    {
        base = ctx->pos;
    }
    else if (whence == RW_SEEK_END)
    {
        base = ctx->size;
    }
    else
    {
        CHECK(whence == RW_SEEK_SET);
    }

    int64_t next = base + offset;

    if (next < 0)
        next = 0;

    if (next > ctx->size)
        next = ctx->size;
    ctx->pos = next;
    return ctx->pos;
}

static size_t memory_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
    MemoryRW *ctx = (MemoryRW *)rw;
    size_t bytes = size * nmemb;
    int64_t left = ctx->size - ctx->pos;

    if (size == 0 || nmemb == 0 || left <= 0)
        return 0;

    if ((int64_t)bytes > left)
        bytes = (size_t)left;

    memcpy(buf, ctx->data + ctx->pos, bytes);
    ctx->pos += (int64_t)bytes;
    return bytes / size;
}

static size_t memory_write(SDL_RWops *rw, const void *buf, size_t size, size_t nmemb)
{
    (void)rw;
    (void)buf;
    (void)size;
    (void)nmemb;
    return 0;
}

static int memory_close(SDL_RWops *rw)
{
    free(rw);
    return 0;
}

static SDL_RWops *open_memory_rw(const uint8_t *data, size_t size)
{
    MemoryRW *ctx = (MemoryRW *)calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);

    ctx->data = data;
    ctx->size = (int64_t)size;
    ctx->rw.size = memory_size;
    ctx->rw.seek = memory_seek;
    ctx->rw.read = memory_read;
    ctx->rw.write = memory_write;
    ctx->rw.close = memory_close;
    ctx->rw.type = RW_TYPE_MEM;
    return &ctx->rw;
}

static size_t oversized_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
    OversizedRW *ctx = (OversizedRW *)rw;
    (void)buf;
    (void)size;
    (void)nmemb;

    if (ctx->calls++ == 0)
        return (size_t)INT_MAX;
    return 0;
}

static SDL_RWops *open_oversized_rw(void)
{
    OversizedRW *ctx = (OversizedRW *)calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);

    ctx->rw.read = oversized_read;
    ctx->rw.write = memory_write;
    ctx->rw.close = memory_close;
    ctx->rw.type = RW_TYPE_MEM;
    return &ctx->rw;
}

static int64_t oversized_known_size(SDL_RWops *rw)
{
    (void)rw;
    return 4;
}

static int64_t oversized_known_seek(SDL_RWops *rw, int64_t offset, int whence)
{
    (void)rw;
    (void)offset;
    CHECK(whence == RW_SEEK_CUR);
    return 0;
}

static size_t oversized_known_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
    OversizedRW *ctx = (OversizedRW *)rw;
    (void)buf;
    (void)size;
    (void)nmemb;

    if (ctx->calls++ == 0)
        return (size_t)INT_MAX + 1u;
    return 0;
}

static SDL_RWops *open_oversized_known_rw(void)
{
    OversizedRW *ctx = (OversizedRW *)calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);

    ctx->rw.size = oversized_known_size;
    ctx->rw.seek = oversized_known_seek;
    ctx->rw.read = oversized_known_read;
    ctx->rw.write = memory_write;
    ctx->rw.close = memory_close;
    ctx->rw.type = RW_TYPE_MEM;
    return &ctx->rw;
}

static uint8_t *read_whole_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    CHECK(fseek(fp, 0, SEEK_END) == 0);
    long end = ftell(fp);
    CHECK(end > 0);
    CHECK(fseek(fp, 0, SEEK_SET) == 0);

    uint8_t *data = (uint8_t *)malloc((size_t)end);
    CHECK(data != NULL);
    CHECK(fread(data, (size_t)end, 1, fp) == 1);
    fclose(fp);

    *size = (size_t)end;
    return data;
}

static void put_u16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
    p[2] = (uint8_t)((value >> 16) & 0xff);
    p[3] = (uint8_t)(value >> 24);
}

static uint8_t *make_wav_with_block_align(uint16_t block_align, size_t *size)
{
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t sample_rate = 22050;
    const uint32_t data_bytes = 2;
    uint8_t *wav = (uint8_t *)calloc(1, 44 + data_bytes);
    CHECK(wav != NULL);

    memcpy(wav, "RIFF", 4);
    put_u32le(wav + 4, 36 + data_bytes);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_u32le(wav + 16, 16);
    put_u16le(wav + 20, 1);
    put_u16le(wav + 22, channels);
    put_u32le(wav + 24, sample_rate);
    put_u32le(wav + 28, sample_rate * channels * (bits / 8));
    put_u16le(wav + 32, block_align);
    put_u16le(wav + 34, bits);
    memcpy(wav + 36, "data", 4);
    put_u32le(wav + 40, data_bytes);
    put_u16le(wav + 44, 0);

    *size = 44 + data_bytes;
    return wav;
}

static Mix_Chunk *make_test_chunk(int allocated)
{
    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(*chunk));
    CHECK(chunk != NULL);

    chunk->abuf = (Uint8 *)malloc(4);
    CHECK(chunk->abuf != NULL);
    memset(chunk->abuf, 0, 4);
    chunk->allocated = allocated;
    chunk->alen = 4;
    chunk->volume = MIX_MAX_VOLUME;
    chunk->frequency = 22050;
    chunk->format = AUDIO_S16SYS;
    chunk->channels = 1;
    return chunk;
}

static int finished_count;
static int finished_channel;

static void channel_finished(int channel)
{
    finished_count++;
    finished_channel = channel;
}

static void test_muted_channel_stays_muted_on_play(void)
{
    Mix_Chunk *chunk = make_test_chunk(1);

    CHECK(Mix_OpenAudio(22050, AUDIO_S16SYS, 2, 64) == 0);
    CHECK(Mix_AllocateChannels(1) == 1);
    CHECK(Mix_Volume(0, 0) == MIX_MAX_VOLUME);
    CHECK(Mix_PlayChannel(0, chunk, 0) == 0);
    CHECK(mix_channels[0].volume == 0);

    Mix_CloseAudio();
    Mix_FreeChunk(chunk);
}

static void test_specific_channel_replacement_finishes_old_channel(void)
{
    Mix_Chunk *first = make_test_chunk(1);
    Mix_Chunk *second = make_test_chunk(1);

    CHECK(Mix_OpenAudio(22050, AUDIO_S16SYS, 2, 64) == 0);
    CHECK(Mix_AllocateChannels(1) == 1);
    finished_count = 0;
    finished_channel = -1;
    Mix_ChannelFinished(channel_finished);

    CHECK(Mix_PlayChannel(0, first, 0) == 0);
    CHECK(Mix_PlayChannel(0, second, 0) == 0);
    CHECK(finished_count == 1);
    CHECK(finished_channel == 0);
    CHECK(mix_channels[0].chunk == second);

    Mix_CloseAudio();
    Mix_FreeChunk(first);
    Mix_FreeChunk(second);
}

static void test_free_chunk_respects_allocated_and_reports_finished_channel(void)
{
    Mix_Chunk *chunk = make_test_chunk(0);
    void *caller_buffer = chunk->abuf;

    CHECK(Mix_OpenAudio(22050, AUDIO_S16SYS, 2, 64) == 0);
    CHECK(Mix_AllocateChannels(1) == 1);
    finished_count = 0;
    finished_channel = -1;
    Mix_ChannelFinished(channel_finished);
    tracked_free_forbidden = caller_buffer;
    tracked_free_forbidden_count = 0;

    CHECK(Mix_PlayChannel(0, chunk, 0) == 0);
    Mix_FreeChunk(chunk);
    CHECK(tracked_free_forbidden_count == 0);
    CHECK(finished_count == 1);
    CHECK(finished_channel == 0);

    tracked_free_forbidden = NULL;
    free(caller_buffer);
    Mix_CloseAudio();
}

static void test_music_replacement_clears_previous_music(void)
{
    Mix_Music *first = Mix_LoadMUS("navy-apps/fsimg/share/music/rhythm/empty.ogg");
    Mix_Music *second = Mix_LoadMUS("navy-apps/fsimg/share/music/rhythm/Do.ogg");
    CHECK(first != NULL);
    CHECK(second != NULL);

    CHECK(Mix_PlayMusic(first, 0) == 0);
    CHECK(first->playing == 1);
    CHECK(Mix_PlayMusic(second, 0) == 0);
    CHECK(first->playing == 0);
    CHECK(second->playing == 1);
    CHECK(current_music == second);

    Mix_FreeMusic(first);
    Mix_FreeMusic(second);
}

static void test_volume_functions_lock_audio_state(void)
{
    CHECK(Mix_OpenAudio(22050, AUDIO_S16SYS, 2, 64) == 0);
    CHECK(Mix_AllocateChannels(1) == 1);

    reset_lock_counts();
    CHECK(Mix_Volume(0, 64) == MIX_MAX_VOLUME);
    CHECK(lock_calls == 1);
    CHECK(unlock_calls == 1);

    reset_lock_counts();
    CHECK(Mix_VolumeMusic(64) == MIX_MAX_VOLUME);
    CHECK(lock_calls == 1);
    CHECK(unlock_calls == 1);

    Mix_CloseAudio();
}

static void test_set_music_position_locks_null_current_music(void)
{
    current_music = NULL;
    reset_lock_counts();
    CHECK(Mix_SetMusicPosition(0.0) == -1);
    CHECK(lock_calls == 1);
    CHECK(unlock_calls == 1);
}

static void test_set_music_position_rejects_nan(void)
{
    Mix_Music *music = Mix_LoadMUS("navy-apps/fsimg/share/music/rhythm/empty.ogg");
    volatile double zero = 0.0;
    double nan_position = zero / zero;

    CHECK(music != NULL);
    CHECK(Mix_PlayMusic(music, 0) == 0);
    CHECK(Mix_SetMusicPosition(nan_position) == -1);

    Mix_FreeMusic(music);
}

static void test_rwops_unknown_size_rejects_oversized_read(void)
{
    SDL_RWops *rw = open_oversized_rw();
    int len = 0;
    uint8_t *data = read_rwops_data(rw, &len);

    CHECK(data == NULL);
    CHECK(len == 0);
    CHECK(SDL_RWclose(rw) == 0);
}

static void test_rwops_known_size_rejects_oversized_read_count(void)
{
    SDL_RWops *rw = open_oversized_known_rw();
    int len = 0;
    uint8_t *data = read_rwops_data(rw, &len);

    CHECK(data == NULL);
    CHECK(len == 0);
    CHECK(strcmp(Mix_GetError(), "SDL_RWops stream is too large") == 0);
    CHECK(SDL_RWclose(rw) == 0);
}

static void test_wav_loader_rejects_invalid_block_align(void)
{
    size_t wav_size = 0;
    uint8_t *wav = make_wav_with_block_align(1, &wav_size);
    SDL_RWops *rw = open_memory_rw(wav, wav_size);
    Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1);

    CHECK(chunk == NULL);
    free(wav);
}

static void test_mono_output_decodes_stereo_ogg_music_safely(void)
{
    size_t ogg_size = 0;
    uint8_t *ogg = read_whole_file("navy-apps/fsimg/share/music/rhythm/Do.ogg", &ogg_size);
    SDL_RWops *rw = open_memory_rw(ogg, ogg_size);
    Mix_Music *music = Mix_LoadMUS_RW(rw);
    uint8_t stream[256];
    int non_silent = 0;

    CHECK(music != NULL);
    CHECK(SDL_RWclose(rw) == 0);
    CHECK(Mix_OpenAudio(22050, AUDIO_S16SYS, 1, 64) == 0);
    CHECK(Mix_PlayMusic(music, 0) == 0);

    mixer_callback(NULL, stream, sizeof(stream));
    for (size_t i = 0; i < sizeof(stream); i++)
    {
        if (stream[i] != 0)
        {
            non_silent = 1;
            break;
        }
    }
    CHECK(non_silent == 1);

    Mix_CloseAudio();
    Mix_FreeMusic(music);
    free(ogg);
}

typedef void (*TestFn)(void);

typedef struct
{
    const char *name;
    TestFn fn;
} TestCase;

static const TestCase tests[] = {
    {"muted-channel", test_muted_channel_stays_muted_on_play},
    {"replace-channel", test_specific_channel_replacement_finishes_old_channel},
    {"free-chunk", test_free_chunk_respects_allocated_and_reports_finished_channel},
    {"music-replace", test_music_replacement_clears_previous_music},
    {"volume-locks", test_volume_functions_lock_audio_state},
    {"seek-lock-null", test_set_music_position_locks_null_current_music},
    {"seek-nan", test_set_music_position_rejects_nan},
    {"rwops-overflow", test_rwops_unknown_size_rejects_oversized_read},
    {"rwops-known-overflow", test_rwops_known_size_rejects_oversized_read_count},
    {"wav-block-align", test_wav_loader_rejects_invalid_block_align},
    {"mono-ogg-music", test_mono_output_decodes_stereo_ogg_music_safely},
};

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        {
            if (strcmp(argv[1], tests[i].name) == 0)
            {
                tests[i].fn();
                printf("%s PASS\n", tests[i].name);
                return 0;
            }
        }

        fprintf(stderr, "Unknown test: %s\n", argv[1]);
        return 2;
    }

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        tests[i].fn();
        printf("%s PASS\n", tests[i].name);
    }

    return 0;
}
