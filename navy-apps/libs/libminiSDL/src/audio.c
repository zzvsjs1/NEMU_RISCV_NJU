#include <NDL.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static SDL_AudioSpec g_spec;
static int g_paused = 1;                 // start paused per spec
static uint32_t g_interval_ms = 0;
static uint32_t g_last_cb_ms = 0;
static int g_bytes_per_frame = 0;        // channels * bytes_per_sample

// very small helper for clamp
static inline int16_t clamp_s16(int x) 
{
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) 
{
  assert(desired);
  memset(&g_spec, 0, sizeof(g_spec));
  g_spec = *desired;

  printf("Here");

  // assume 16-bit signed
  int bytes_per_sample = 2;
  g_bytes_per_frame = desired->channels * bytes_per_sample;

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
  // on unpause, reset tick to trigger quickly
  if (!g_paused) g_last_cb_ms = 0;
}


void SDL_MixAudio(uint8_t *dst, uint8_t *src, uint32_t len, int volume) 
{
  // volume in [0, 128], 128 is max
  if (volume <= 0) return;
  if (volume > 128) volume = 128;

  // Assume S16 samples
  uint32_t samples = len / 2;
  int16_t *d = (int16_t *)dst;
  int16_t *s = (int16_t *)src;
  for (uint32_t i = 0; i < samples; i++) 
  {
    int mixed = (int)d[i] + ((int)s[i] * volume) / 128;
    d[i] = clamp_s16(mixed);
  }
}

SDL_AudioSpec *SDL_LoadWAV(const char *file, SDL_AudioSpec *spec, uint8_t **audio_buf, uint32_t *audio_len) 
{
  return NULL;
}

void SDL_FreeWAV(uint8_t *audio_buf) 
{

}

void SDL_LockAudio() 
{

}

void SDL_UnlockAudio() 
{
  
}

// Call this very frequently from event or delay APIs
void CallbackHelper(void) 
{
  if (g_paused) return;
  if (!g_spec.callback) return;

  uint32_t now = NDL_GetTicks();
  if (now - g_last_cb_ms < g_interval_ms) return;

  // Ask device for free space, avoid blocking writes
  int free_bytes = NDL_QueryAudio();
  if (free_bytes <= 0) {
    // Even if no space, we still advance the tick to avoid busy-calling
    g_last_cb_ms = now;
    return;
  }

  // Target bytes for one callback worth of audio
  int target = g_spec.samples * g_bytes_per_frame;
  int chunk = free_bytes < target ? free_bytes : target;

  // Align to whole frames
  if (g_bytes_per_frame > 0) {
    chunk = (chunk / g_bytes_per_frame) * g_bytes_per_frame;
  }
  if (chunk <= 0) {
    g_last_cb_ms = now;
    return;
  }

  // Use a reasonably sized stack buffer, NPlayer's default 1024 samples, 2ch, s16 → 4096 bytes
  // To be safer, keep 64KB max per push
  static uint8_t buf[64 * 1024];
  if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);

  // App fills PCM into `buf`
  g_spec.callback(g_spec.userdata, buf, chunk);

  // Push to device
  int wrote = NDL_PlayAudio(buf, chunk);
  (void)wrote;

  // mark time
  g_last_cb_ms = now;
}
