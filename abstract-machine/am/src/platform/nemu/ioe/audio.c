#include <am.h>
#include <nemu.h>

#define AUDIO_FREQ_ADDR (AUDIO_ADDR + 0x00)
#define AUDIO_CHANNELS_ADDR (AUDIO_ADDR + 0x04)
#define AUDIO_SAMPLES_ADDR (AUDIO_ADDR + 0x08)
#define AUDIO_SBUF_SIZE_ADDR (AUDIO_ADDR + 0x0c)
#define AUDIO_INIT_ADDR (AUDIO_ADDR + 0x10)
#define AUDIO_COUNT_ADDR (AUDIO_ADDR + 0x14)

void __am_audio_init() 
{
    // Start init.
    outl(AUDIO_INIT_ADDR, 1);
}

void __am_audio_config(AM_AUDIO_CONFIG_T *cfg) 
{
  // Get config in here.
  cfg->present = true;
  const uint32_t bufferSize = inl(AUDIO_SBUF_SIZE_ADDR);
  cfg->bufsize = bufferSize;
}

void __am_audio_status(AM_AUDIO_STATUS_T *stat) 
{ 
    stat->count = 0; 
}

void __am_audio_play(AM_AUDIO_PLAY_T *ctl) 
{
    
}
