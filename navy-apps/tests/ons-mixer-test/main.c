#include <sys/types.h>
#include <SDL.h>
#include <SDL_mixer.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            fprintf(stderr, "CHECK failed: %s at %s:%d\n", #cond, __FILE__, \
                    __LINE__); \
            exit(1); \
        } \
    } while (0)

typedef struct
{
    SDL_RWops rw;
    const uint8_t *data;
    int64_t size;
    int64_t pos;
} MemoryRW;

/*
 * This test drives SDL_mixer without a real ONScripter process. It checks the
 * callback and RWops contracts that script sound effects and music playback
 * depend on inside Navy.
 */
static int music_finished_count = 0;
static int channel_finished_index = -1;

static void music_finished(void)
{
    music_finished_count++;
}

static void channel_finished(int channel)
{
    channel_finished_index = channel;
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
        assert(whence == RW_SEEK_SET);
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

    /*
   * SDL_RWread returns the number of complete objects, not the number of
   * bytes.  Returning partial objects would make callers believe more whole
   * data was available than we actually copied.
   */
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
    assert(ctx != NULL);

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

static uint8_t *read_whole_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long end = ftell(fp);
    assert(end > 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);

    uint8_t *data = (uint8_t *)malloc((size_t)end);
    assert(data != NULL);
    assert(fread(data, (size_t)end, 1, fp) == 1);
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

static uint8_t *make_mono_wav(size_t *size)
{
    /*
   * Generate a tiny valid PCM WAV in memory so the mixer test does not depend
   * solely on external OGG assets. Little-endian fields are written by helper
   * functions to match the file format exactly.
   */
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t rate = 11025;
    const uint32_t samples = 64;
    const uint32_t data_bytes = samples * channels * (bits / 8);
    uint8_t *wav = (uint8_t *)calloc(1, 44 + data_bytes);
    assert(wav != NULL);

    memcpy(wav + 0, "RIFF", 4);
    put_u32le(wav + 4, 36 + data_bytes);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_u32le(wav + 16, 16);
    put_u16le(wav + 20, 1);
    put_u16le(wav + 22, channels);
    put_u32le(wav + 24, rate);
    put_u32le(wav + 28, rate * channels * (bits / 8));
    put_u16le(wav + 32, channels * (bits / 8));
    put_u16le(wav + 34, bits);
    memcpy(wav + 36, "data", 4);
    put_u32le(wav + 40, data_bytes);

    for (uint32_t i = 0; i < samples; i++)
    {
        /*
     * A tiny ramp is enough to prove the loader accepts real PCM data.  The
     * channel-finished callback later proves the mixer consumes it through
     * the normal audio callback path.
     */
        int16_t sample = (int16_t)((int)i * 256 - 8192);
        put_u16le(wav + 44 + i * 2, (uint16_t)sample);
    }

    *size = 44 + data_bytes;
    return wav;
}

int main(void)
{
    SDL_Init(0);

    CHECK(Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, 512) == 0);

    /*
   * The query immediately after opening locks down the audio format that the
   * rest of the test assumes. A mismatch here would make later channel and
   * callback checks hard to interpret.
   */
    int frequency = 0;
    uint16_t format = 0;
    int channels = 0;
    CHECK(Mix_QuerySpec(&frequency, &format, &channels) == 1);
    CHECK(frequency == 22050);
    CHECK(format == MIX_DEFAULT_FORMAT);
    CHECK(channels == 2);

    CHECK(Mix_VolumeMusic(-1) == MIX_MAX_VOLUME);
    CHECK(Mix_VolumeMusic(64) == MIX_MAX_VOLUME);
    CHECK(Mix_VolumeMusic(-1) == 64);

    size_t ogg_size = 0;
    uint8_t *ogg = read_whole_file("/share/music/rhythm/empty.ogg", &ogg_size);
    SDL_RWops *ogg_rw = open_memory_rw(ogg, ogg_size);
    /*
   * The empty music file reaches end-of-stream quickly but still has to trigger
   * the normal music-finished callback.  This catches scene-transition cases
   * where ONS stops or swaps BGM while the mixer is being pumped by SDL_Delay().
   */
    Mix_Music *music = Mix_LoadMUS_RW(ogg_rw);
    CHECK(music != NULL);
    CHECK(SDL_RWclose(ogg_rw) == 0);

    Mix_HookMusicFinished(music_finished);
    CHECK(Mix_PlayMusic(music, 0) == 0);
    CHECK(Mix_SetMusicPosition(0.0) == 0);

    for (int i = 0; i < 600 && Mix_PlayingMusic(); i++)
    {
        SDL_Delay(5);
    }

    CHECK(Mix_PlayingMusic() == 0);
    CHECK(music_finished_count == 1);
    Mix_FreeMusic(music);
    free(ogg);

    CHECK(Mix_AllocateChannels(2) == 2);
    Mix_ChannelFinished(channel_finished);

    ogg = read_whole_file("/share/music/rhythm/Do.ogg", &ogg_size);
    ogg_rw = open_memory_rw(ogg, ogg_size);
    /*
   * ONS uses short OGG clips as sound effects, not only as streamed music.  The
   * chunk path must therefore decode through Mix_LoadWAV_RW, report a concrete
   * PCM format, and still drive the channel-finished callback.
   */
    Mix_Chunk *ogg_chunk = Mix_LoadWAV_RW(ogg_rw, 1);
    CHECK(ogg_chunk != NULL);
    CHECK(ogg_chunk->format == AUDIO_S16SYS);
    CHECK(ogg_chunk->channels >= 1 && ogg_chunk->channels <= 2);
    CHECK(ogg_chunk->frequency > 0);
    CHECK(ogg_chunk->alen > 0);

    channel_finished_index = -1;
    int ogg_channel = Mix_PlayChannel(-1, ogg_chunk, 0);
    CHECK(ogg_channel >= 0);

    for (int i = 0; i < 200 && channel_finished_index < 0; i++)
    {
        SDL_Delay(5);
    }

    CHECK(channel_finished_index == ogg_channel);
    Mix_FreeChunk(ogg_chunk);
    free(ogg);

    size_t wav_size = 0;
    uint8_t *wav = make_mono_wav(&wav_size);
    SDL_RWops *wav_rw = open_memory_rw(wav, wav_size);
    Mix_Chunk *chunk = Mix_LoadWAV_RW(wav_rw, 1);
    CHECK(chunk != NULL);
    free(wav);

    CHECK(Mix_Volume(-1, 32) >= 0);
    int played_channel = Mix_PlayChannel(-1, chunk, 0);
    CHECK(played_channel >= 0);

    for (int i = 0; i < 100 && channel_finished_index < 0; i++)
    {
        SDL_Delay(5);
    }

    CHECK(channel_finished_index == played_channel);
    Mix_Pause(-1);
    Mix_FreeChunk(chunk);

    Mix_HaltMusic();
    Mix_CloseAudio();
    SDL_Quit();

    printf("ons-mixer-test PASS\n");
    exit(0);
}
