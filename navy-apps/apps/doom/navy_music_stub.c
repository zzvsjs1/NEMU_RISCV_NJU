#include "i_sound.h"

/*
 * Navy currently supports Doom sound effects through SDL_mixer, but not Doom's
 * MUS/MIDI music path.  Keep FEATURE_SOUND enabled for SFX while satisfying the
 * music module symbol that the shared Doom sound layer expects.
 *
 * This file intentionally lives outside repo/doomgeneric so the imported Doom
 * sources stay untouched.  The real SDL music backend remains excluded by the
 * app Makefile; enabling it would compile, convert MUS to MIDI, and then ask
 * SDL_mixer for MIDI playback that Navy's small mixer cannot synthesise yet.
 */

static boolean NavyMusic_Init(void)
{
    /*
     * Report successful initialisation so the higher-level sound code can keep
     * running with sound effects enabled.  No device is opened here; the SFX path
     * owns SDL_OpenAudio()/Mix_OpenAudio() through i_sdlsound.c.
     */
    return true;
}

static void NavyMusic_Shutdown(void)
{

}

static void NavyMusic_SetMusicVolume(int volume)
{
    (void)volume;
}

static void NavyMusic_PauseMusic(void)
{

}

static void NavyMusic_ResumeMusic(void)
{

}

static void *NavyMusic_RegisterSong(void *data, int len)
{
    /*
     * Returning NULL tells Doom that there is no playable music handle.  The rest
     * of the music callbacks tolerate NULL handles, which keeps menu and level
     * transitions quiet instead of failing during WAD MUS registration.
     */
    (void)data;
    (void)len;
    return NULL;
}

static void NavyMusic_UnRegisterSong(void *handle)
{
    (void)handle;
}

static void NavyMusic_PlaySong(void *handle, boolean looping)
{
    (void)handle;
    (void)looping;
}

static void NavyMusic_StopSong(void)
{
}

static boolean NavyMusic_MusicIsPlaying(void)
{
    return false;
}

static void NavyMusic_Poll(void)
{
    
}

static snddevice_t navy_music_devices[] = {
    /*
     * Advertise the devices Doom commonly selects for music so the shared sound
     * layer can choose this stub module when FEATURE_SOUND is enabled.  The module
     * itself remains silent regardless of the selected device.
     */
    SNDDEVICE_SB,
    SNDDEVICE_GENMIDI,
};

music_module_t DG_music_module = {
    navy_music_devices,
    arrlen(navy_music_devices),
    NavyMusic_Init,
    NavyMusic_Shutdown,
    NavyMusic_SetMusicVolume,
    NavyMusic_PauseMusic,
    NavyMusic_ResumeMusic,
    NavyMusic_RegisterSong,
    NavyMusic_UnRegisterSong,
    NavyMusic_PlaySong,
    NavyMusic_StopSong,
    NavyMusic_MusicIsPlaying,
    NavyMusic_Poll,
};
