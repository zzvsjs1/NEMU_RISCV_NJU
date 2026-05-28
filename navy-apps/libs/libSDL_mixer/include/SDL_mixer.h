#ifndef __SDL_MIXER_H__
#define __SDL_MIXER_H__

#include <SDL.h>

#define MIX_DEFAULT_FORMAT AUDIO_S16SYS
#define MIX_DEFAULT_CHANNELS 2
#define DEFAULT_AUDIOBUF 4096
#define MIX_MAX_VOLUME 128

typedef struct Mix_Music Mix_Music;

typedef struct
{
    int allocated;
    Uint8 *abuf;
    Uint32 alen;
    Uint8 volume;

    /*
     * SDL_mixer normally converts chunks to the opened device format at load
     * time.  This small Navy implementation keeps the original PCM format and
     * expands it in the audio callback, so the source metadata needs to travel
     * with the chunk.
     */
    int frequency;
    Uint16 format;
    Uint8 channels;
} Mix_Chunk;

#ifdef __cplusplus
extern "C"
{
#endif

    int Mix_OpenAudio(int frequency, uint16_t format, int channels, int chunksize);
    int Mix_QuerySpec(int *frequency, uint16_t *format, int *channels);
    int Mix_AllocateChannels(int numchans);
    void Mix_ChannelFinished(void (*channel_finished)(int channel));
    void Mix_Pause(int channel);
    /*
     * Compatibility subset from SDL_mixer 1.2.  Navy's mixer still has a small
     * implementation, but these calls are part of the source-level contract used by
     * ports such as Doom's SDL sound backend.
     */
    int Mix_Playing(int channel);
    int Mix_HaltChannel(int channel);
    int Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, int ticks);
    int Mix_SetPanning(int channel, uint8_t left, uint8_t right);
    int Mix_UnregisterAllEffects(int channel);
    const SDL_version *Mix_Linked_Version(void);
    int Mix_SetMusicPosition(double position);
    Mix_Music *Mix_LoadMUS(const char *file);
    void Mix_FreeChunk(Mix_Chunk *chunk);
    int Mix_VolumeMusic(int volume);
    int Mix_HaltMusic();
    void Mix_FreeMusic(Mix_Music *music);
    void Mix_HookMusicFinished(void (*music_finished)());
    Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
    char *Mix_GetError();
    void Mix_CloseAudio();
    int Mix_Volume(int channel, int volume);
    int Mix_PlayingMusic();
    int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
    Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src);
    int Mix_PlayMusic(Mix_Music *music, int loops);
    int Mix_SetMusicCMD(const char *command);

#ifdef __cplusplus
}
#endif

#endif
