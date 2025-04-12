#include <SDL2/SDL.h>
#include <common.h>
#include <debug.h>
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

static uint8_t *sbuf = NULL;
static uint32_t *audio_base = NULL;

// These variables hold the current configuration parameters.
static uint32_t audioFreq = 0;
static uint32_t audioCannels = 0;
static uint32_t audioSamples = 0;

// Ring-buffer pointers for sbuf (in bytes).
// They serve to track the available data (between write and read positions).
static volatile uint32_t sbufRead = 0, sbugWrite = 0;

// Helper functions to compute ring buffer status.
static inline uint32_t available_data() 
{
    if (sbugWrite >= sbufRead) 
    {
        return sbugWrite - sbufRead;
    } 
    else 
    {
        return CONFIG_SB_SIZE - sbufRead + sbugWrite;
    }
}

static inline uint32_t available_space() 
{
    return CONFIG_SB_SIZE - available_data();
}

// This is the callback function that SDL calls to get more audio samples.
// It reads data from sbuf (the ring buffer) and copies it into the stream
// buffer. If data runs out, the rest of the output is filled with zeros.
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) 
{
    int remaining = len;
    while (remaining > 0) 
    {
        const uint32_t data_avail = available_data();
        if (data_avail == 0) 
        {
            // No data available; clear the rest of the stream to prevent garbage
            // noise.
            memset(stream, 0, remaining);
            break;
        }

        // Copy the smaller of what is available or what is needed.
        int chunk = (data_avail > remaining ? remaining : data_avail);

        // It might happen that the ring buffer wraps around; so copy the contiguous
        // part.
        const uint32_t contiguous = CONFIG_SB_SIZE - sbufRead;
        if (contiguous < (uint32_t)chunk) 
        {
            chunk = contiguous;
        }

        memcpy(stream, sbuf + sbufRead, chunk);
        sbufRead = (sbufRead + chunk) % CONFIG_SB_SIZE;
        stream += chunk;
        remaining -= chunk;
    }
}

// Called by the AM layer (e.g., AM_AUDIO_PLAY)
// Copies len bytes from src into the ring buffer, busy-waiting if there isn’t
// enough space.
void audio_play_data(const uint8_t *src, uint32_t len) 
{
    uint32_t written = 0;
    while (written < len) 
    {
        // Wait until there is free space.
        while (available_space() == 0) { }

        uint32_t space = available_space();
        uint32_t chunk = (len - written > space ? space : len - written);
        // Handle possible wrap-around in the ring buffer.
        uint32_t contiguous = CONFIG_SB_SIZE - sbugWrite;

        if (contiguous < chunk) 
        {
            memcpy(sbuf + sbugWrite, src + written, contiguous);
            sbugWrite = 0;
            memcpy(sbuf, src + written + contiguous, chunk - contiguous);
            sbugWrite = chunk - contiguous;
        } 
        else 
        {
            memcpy(sbuf + sbugWrite, src + written, chunk);
            sbugWrite = (sbugWrite + chunk) % CONFIG_SB_SIZE;
        }

        written += chunk;
    }
}

static void audio_io_handler(uint32_t offset, int len, bool is_write) 
{
    int reg = offset / sizeof(uint32_t);
    Assert(reg < reg_freq || reg >= nr_reg,
            "The audio register is out size the bound. It should be %" PRIu32
            " and %" PRIu32 ", but the value is %" PRIu32 ".\n",
            reg_freq, nr_reg, reg);

    if (is_write) 
    {
        // The value has already been written into the mapped memory.
        const uint32_t val = *(uint32_t *)(audio_base + reg);

        switch (reg) 
        {
            case reg_freq: {
                audioFreq = val;
                break;
            }

            case reg_channels: {
                audioCannels = val;
                break;
            }

            case reg_samples: {
                audioSamples = val;
                break;
            }

            // Do init and ignore write value.
            case reg_init: {
                Assert(val == 1, "The write value is not 1 in audio init, please check Abstract Machine.");

                // When the init register is written to, initialize the SDL audio
                // subsystem.
                SDL_AudioSpec spec = {};

                spec.freq = audioFreq;
                spec.format = AUDIO_S16SYS;  // 16-bit signed samples in system byte order
                spec.channels = audioCannels;
                spec.samples = audioSamples;
                spec.callback = sdl_audio_callback;
                spec.userdata = NULL;

                if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) 
                {
                    printf("SDL_InitSubSystem error: %s\n", SDL_GetError());
                    exit(1);
                }

                if (SDL_OpenAudio(&spec, NULL) < 0) 
                {
                    printf("SDL_OpenAudio error: %s\n", SDL_GetError());
                    exit(1);
                }

                // Unpause and start audio playback.
                SDL_PauseAudio(0);  

                // Reset the ring-buffer pointers.
                sbufRead = 0;
                sbugWrite = 0;

                // Write out the stream buffer size to the register.
                *(uint32_t *)(audio_base + reg_sbuf_size) = CONFIG_SB_SIZE;
                break;
            }

            default: {
                break;
            }
        }

        return;
    }

    // Read operations for registers.
    switch (reg) 
    {
        case reg_sbuf_size: {
            *(uint32_t *)(audio_base + reg) = CONFIG_SB_SIZE;
            break;
        }

        case reg_count: {
            const uint32_t count = available_data();
            *(uint32_t *)(audio_base + reg) = count;
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
}
