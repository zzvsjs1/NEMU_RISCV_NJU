#include <common.h>
#include <device/map.h>
#include <SDL2/SDL.h>

enum {
  reg_freq,       // Audio frequency (Hz)
  reg_channels,   // Number of channels (e.g., 1 for mono, 2 for stereo)
  reg_samples,    // Number of samples per callback
  reg_sbuf_size,  // Total size of the stream buffer
  reg_init,       // Write to initialize audio
  reg_count,      // Read to get current used buffer size
  nr_reg
};

static uint8_t *sbuf = NULL;
static uint32_t *audio_base = NULL;

// These variables hold the current configuration parameters.
static uint32_t audio_freq = 0;
static uint32_t audio_channels = 0;
static uint32_t audio_samples = 0;

// Ring-buffer pointers for sbuf (in bytes).
// They serve to track the available data (between write and read positions).
static volatile uint32_t sbuf_read = 0, sbuf_write = 0;

#define CONFIG_SB_SIZE (64 * 1024)  // 64KB stream buffer

// Helper functions to compute ring buffer status.
static inline uint32_t available_data() 
{
  if (sbuf_write >= sbuf_read) return sbuf_write - sbuf_read;
  else return CONFIG_SB_SIZE - sbuf_read + sbuf_write;
}

static inline uint32_t available_space() 
{
  return CONFIG_SB_SIZE - available_data();
}

// This is the callback function that SDL calls to get more audio samples.
// It reads data from sbuf (the ring buffer) and copies it into the stream buffer.
// If data runs out, the rest of the output is filled with zeros.
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  int remaining = len;
  while (remaining > 0) {
    uint32_t data_avail = available_data();
    if (data_avail == 0) {
      // No data available; clear the rest of the stream to prevent garbage noise.
      memset(stream, 0, remaining);
      break;
    }

    // Copy the smaller of what is available or what is needed.
    int chunk = (data_avail > remaining ? remaining : data_avail);
    // It might happen that the ring buffer wraps around; so copy the contiguous part.
    uint32_t contiguous = CONFIG_SB_SIZE - sbuf_read;
    if (contiguous < (uint32_t)chunk) chunk = contiguous;
    
    memcpy(stream, sbuf + sbuf_read, chunk);
    sbuf_read = (sbuf_read + chunk) % CONFIG_SB_SIZE;
    stream += chunk;
    remaining -= chunk;
  }
}

// Called by the AM layer (e.g., AM_AUDIO_PLAY)
// Copies len bytes from src into the ring buffer, busy-waiting if there isn’t enough space.
void audio_play_data(const uint8_t *src, uint32_t len) {
  uint32_t written = 0;
  while (written < len) {
    // Wait until there is free space.
    while (available_space() == 0) { }
    uint32_t space = available_space();
    uint32_t chunk = (len - written > space ? space : len - written);
    // Handle possible wrap-around in the ring buffer.
    uint32_t contiguous = CONFIG_SB_SIZE - sbuf_write;
    if (contiguous < chunk) {
      memcpy(sbuf + sbuf_write, src + written, contiguous);
      sbuf_write = 0;
      memcpy(sbuf, src + written + contiguous, chunk - contiguous);
      sbuf_write = chunk - contiguous;
    } else {
      memcpy(sbuf + sbuf_write, src + written, chunk);
      sbuf_write = (sbuf_write + chunk) % CONFIG_SB_SIZE;
    }
    written += chunk;
  }
}

static void audio_io_handler(uint32_t offset, int len, bool is_write) 
{
  int reg = offset / 4;
  if (reg < 0 || reg >= nr_reg) return;
  
  if (is_write) {
    // The value has already been written into the mapped memory.
    uint32_t val = *(uint32_t *)(audio_base + reg);
    switch (reg) {
      case reg_freq:
        audio_freq = val;
        break;
      case reg_channels:
        audio_channels = val;
        break;
      case reg_samples:
        audio_samples = val;
        break;
      case reg_init: {
          // When the init register is written to, initialize the SDL audio subsystem.
          SDL_AudioSpec spec;
          memset(&spec, 0, sizeof(spec));
          spec.freq = audio_freq;
          spec.format = AUDIO_S16SYS;  // 16-bit signed samples in system byte order
          spec.channels = audio_channels;
          spec.samples = audio_samples;
          spec.callback = sdl_audio_callback;
          spec.userdata = NULL;
          if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("SDL_InitSubSystem error: %s\n", SDL_GetError());
            exit(1);
          }
          if (SDL_OpenAudio(&spec, NULL) < 0) {
            printf("SDL_OpenAudio error: %s\n", SDL_GetError());
            exit(1);
          }
          SDL_PauseAudio(0);  // Unpause and start audio playback
          // Reset the ring-buffer pointers.
          sbuf_read = 0;
          sbuf_write = 0;
          // Write out the stream buffer size to the register.
          *(uint32_t *)(audio_base + reg_sbuf_size) = CONFIG_SB_SIZE;
          break;
      }
      default:
        break;
    }
  } else {  // Read operations for registers.
    switch (reg) {
      case reg_sbuf_size:
        *(uint32_t *)(audio_base + reg) = CONFIG_SB_SIZE;
        break;
      case reg_count: {
          uint32_t count = available_data();
          *(uint32_t *)(audio_base + reg) = count;
          break;
      }
      default:
        break;
    }
  }
}

void init_audio() {
  uint32_t space_size = sizeof(uint32_t) * nr_reg;
  audio_base = (uint32_t *)new_space(space_size);
#ifdef CONFIG_HAS_PORT_IO
  add_pio_map ("audio", CONFIG_AUDIO_CTL_PORT, audio_base, space_size, audio_io_handler);
#else
  add_mmio_map("audio", CONFIG_AUDIO_CTL_MMIO, audio_base, space_size, audio_io_handler);
#endif

  sbuf = (uint8_t *)new_space(CONFIG_SB_SIZE);
  add_mmio_map("audio-sbuf", CONFIG_SB_ADDR, sbuf, CONFIG_SB_SIZE, NULL);
}
