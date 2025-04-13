#include <SDL2/SDL.h>
#include <common.h>
#include <debug.h>
#include <stdbool.h>
#include <math.h>
#include <device/map.h>

enum {
  reg_freq,       // Audio frequency (Hz)
  reg_channels,   // Number of channels (e.g., 1 for mono, 2 for stereo)
  reg_samples,    // Number of samples per callback
  reg_sbuf_size,  // Total size of the stream buffer
  reg_init,       // Write to initialize audio
  reg_count,      // Read to get current used buffer size
  nr_reg
};

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

static SDL_AudioSpec spec = {0};
static SDL_AudioSpec specOut = {0};

static uint8_t *sbuf = NULL;
static uint32_t *audio_base = NULL;

static volatile int sbufReadIndex  = 0;

// This is the callback function that SDL calls to get more audio samples.
// It reads data from sbuf (the ring buffer) and copies it into the stream
// buffer. If data runs out, the rest of the output is filled with silence's value.
static void sdlAudioCallback(void *userdata, Uint8 *stream, int len) 
{
    // Fill the stream buffer with silence.
    SDL_memset(stream, specOut.silence, len);

    // Determine how many bytes we can copy from the audio buffer.
    const uint32_t currentSizeOfData = audio_base[reg_count];
    const uint32_t filledBytes = MIN((uint32_t)len, currentSizeOfData);

    // Calculate new index with wrap-around logic.
    const int endIndex = (sbufReadIndex + filledBytes) % CONFIG_SB_SIZE;

    // Determine how many bytes to copy before and after the buffer wrap.
    const int firstHalfLen = (endIndex < sbufReadIndex) ? (CONFIG_SB_SIZE - sbufReadIndex) : filledBytes;
    const int secondHalfLen = filledBytes - firstHalfLen;

    Assert(sbufReadIndex < CONFIG_SB_SIZE && endIndex < CONFIG_SB_SIZE, "Index error");

    // Copy data from the circular buffer into the stream.
    SDL_memcpy(stream, sbuf + sbufReadIndex, firstHalfLen);
    SDL_memcpy(stream + firstHalfLen, sbuf, secondHalfLen);

    // Update the read index and the count of audio data.
    sbufReadIndex = endIndex;
    audio_base[reg_count] -= filledBytes;
}

static void audio_io_handler(uint32_t offset, int len, bool is_write) 
{
    const int reg = offset / sizeof(uint32_t);
    Assert(reg >= reg_freq && reg < nr_reg,
            "The audio register is out size the bound. It should be %" PRIu32
            " and %" PRIu32 ", but the value is %" PRIu32 ".\n",
            reg_freq, nr_reg, reg);

    if (is_write) 
    {
        // The value has already been written into the mapped memory.
        const uint32_t val = audio_base[reg];

        switch (reg) 
        {
            case reg_freq: {
                break;
            }

            case reg_channels: {
                break;
            }

            case reg_samples: {
                break;
            }

            case reg_count: {
                SDL_LockAudio();
                SDL_UnlockAudio();
                break;
            }

            // Do init and ignore write value.
            case reg_init: {
                Assert(val == 1, "The write value is not 1 in audio init, please check Abstract Machine.");

                spec.freq = audio_base[reg_freq];
                spec.format = AUDIO_S16SYS;
                spec.channels = audio_base[reg_channels];
                spec.samples = audio_base[reg_samples];
                spec.callback = sdlAudioCallback;
                spec.userdata = NULL;

                if (SDL_OpenAudio(&spec, &specOut) < 0) 
                {
                    printf("SDL_OpenAudio error: %s\n", SDL_GetError());
                    exit(1);
                }

                // Unpause and start audio playback.
                SDL_PauseAudio(0);

                break;
            }

            default: {
                Assert(false, "Write to a wrong audio register, the value is %" PRIu32 ".\n", offset);
                break;
            }
        }

        return;
    }

    // Read operations for registers.
    switch (reg) 
    {
        case reg_sbuf_size: {
            break;
        }

        case reg_count: {
            SDL_LockAudio();
            SDL_UnlockAudio();
            break;
        }

        default: {
            break;
        }
    }
}

void init_audio() 
{
  uint32_t space_size = sizeof(uint32_t) * nr_reg;
  audio_base = (uint32_t *)new_space(space_size);

#ifdef CONFIG_HAS_PORT_IO
  add_pio_map("audio", CONFIG_AUDIO_CTL_PORT, audio_base, space_size,
              audio_io_handler);
#else
  add_mmio_map("audio", CONFIG_AUDIO_CTL_MMIO, audio_base, space_size,
               audio_io_handler);
#endif

  sbuf = (uint8_t *)new_space(CONFIG_SB_SIZE);
  add_mmio_map("audio-sbuf", CONFIG_SB_ADDR, sbuf, CONFIG_SB_SIZE, NULL);

  // Write out the stream buffer size to the register.
  // In AM, it will run as: init -> config -> ctrl.
  audio_base[reg_sbuf_size] = CONFIG_SB_SIZE;

  // Init subsystem in here before open device.
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) 
  {
      printf("SDL_InitSubSystem error: %s\n", SDL_GetError());
      exit(1);
  }
}
