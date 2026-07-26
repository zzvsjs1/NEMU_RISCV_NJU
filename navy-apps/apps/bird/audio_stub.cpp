#include "Audio.h"

/* Bird treats audio initialisation failure as fatal. The MIPS32 build therefore
 * reports successful initialisation but supplies no sound data; the remaining
 * no-op functions safely accept those null resources.
 */
int SOUND_OpenAudio(int freq, int channels, int samples)
{
  (void)freq;
  (void)channels;
  (void)samples;
  return 0;
}

void SOUND_CloseAudio()
{
}

void *SOUND_LoadWAV(const char *filename)
{
  (void)filename;
  return nullptr;
}

void SOUND_FreeWAV(void *audio)
{
  (void)audio;
}

void SOUND_PlayWAV(int channel, void *audio)
{
  (void)channel;
  (void)audio;
}
