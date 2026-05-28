#ifndef __SDL_AUDIO_H__
#define __SDL_AUDIO_H__

typedef struct
{
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    /*
     * size is kept for SDL source compatibility.  SDL_OpenAudio() derives the
     * real transfer size from samples, channels, and format when pumping NDL.
     */
    uint32_t size;
    /*
     * The callback is not invoked by a separate audio thread in miniSDL.  It is
     * pumped from SDL_Delay(), SDL_PumpEvents(), and unpause prefill paths.
     */
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef struct
{
    /*
     * Navy does not implement SDL_BuildAudioCVT()/SDL_ConvertAudio(), but some
     * SDL 1.2 code keeps this type in local variables while using its own
     * conversion path.  Keep the common fields available for source compatibility.
     *
     * The fields mirror the names callers normally touch when they do use SDL's
     * converter.  They are deliberately inert in miniSDL: declaring the structure
     * does not promise that automatic format conversion exists, it only lets code
     * that has already chosen another conversion path, such as Doom's sound effect
     * resampler, build against Navy's small SDL surface.
     */
    int needed;
    uint16_t src_format;
    uint16_t dst_format;
    double rate_incr;
    uint8_t *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
} SDL_AudioCVT;

#define AUDIO_U8 8
#define AUDIO_S16 16
#define AUDIO_S16LSB AUDIO_S16
#define AUDIO_S16SYS AUDIO_S16

#define SDL_MIX_MAXVOLUME 128

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio();
void SDL_PauseAudio(int pause_on);
SDL_AudioSpec *SDL_LoadWAV(const char *file, SDL_AudioSpec *spec, uint8_t **audio_buf, uint32_t *audio_len);
void SDL_FreeWAV(uint8_t *audio_buf);
void SDL_MixAudio(uint8_t *dst, const uint8_t *src, uint32_t len, int volume);
void SDL_PumpAudio();
void SDL_LockAudio();
void SDL_UnlockAudio();

#endif
