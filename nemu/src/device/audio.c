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
  reg_init,       // Write to initialise audio
  reg_count,      // Read used bytes; write the number of newly appended bytes
  nr_reg
};

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

#ifndef CONFIG_AUDIO_DUMMY
static SDL_AudioSpec spec = {0};
static SDL_AudioSpec specOut = {0};
static bool audio_opened = false;
#endif

static uint8_t *sbuf = NULL;
static uint32_t *audio_base = NULL;

static uint32_t audio_count = 0;
static uint32_t sbufReadIndex = 0;

static void publish_audio_count(void)
{
    audio_base[reg_count] = audio_count;
}

static void lock_audio_counter(void)
{
#ifndef CONFIG_AUDIO_DUMMY
    if (audio_opened)
    {
        SDL_LockAudio();
    }
#endif
}

static void unlock_audio_counter(void)
{
#ifndef CONFIG_AUDIO_DUMMY
    if (audio_opened)
    {
        SDL_UnlockAudio();
    }
#endif
}

/*
 * NEMU exposes one host SDL audio device, but under Nanos-lite multiple user
 * programs can be loaded at once and each may call SDL_OpenAudio() when it
 * first becomes foreground. SDL rejects a second open on the same process with
 * "Audio device is already opened", so each guest audio init request is treated
 * as ownership transfer: pause and close the previous host stream, clear the
 * emulated ring-buffer state, then open the new guest's requested format.
 */
static void close_audio_if_open(void)
{
#ifndef CONFIG_AUDIO_DUMMY
    if (!audio_opened)
    {
        return;
    }

    SDL_PauseAudio(1);
    SDL_CloseAudio();
    audio_opened = false;
#endif
}

static void reset_audio_stream(void)
{
    sbufReadIndex = 0;
    audio_count = 0;
    publish_audio_count();
#ifndef CONFIG_AUDIO_DUMMY
    memset(&spec, 0, sizeof(spec));
    memset(&specOut, 0, sizeof(specOut));
#endif
}

#ifndef CONFIG_AUDIO_DUMMY
// This is the callback function that SDL calls to get more audio samples.
// It reads data from sbuf (the ring buffer) and copies it into the stream
// buffer. If data runs out, the rest of the output is filled with silence's value.
static void sdlAudioCallback(void *userdata, Uint8 *stream, int len) 
{
    // Fill the stream buffer with silence.
    SDL_memset(stream, specOut.silence, len);

    // Determine how many bytes we can copy from the audio buffer.
    const uint32_t currentSizeOfData = audio_count;
    const uint32_t filledBytes = MIN((uint32_t)len, currentSizeOfData);

    // Calculate new index with wrap-around logic.
    const uint32_t endIndex = (sbufReadIndex + filledBytes) % CONFIG_SB_SIZE;

    // Determine how many bytes to copy before and after the buffer wrap.
    const uint32_t firstHalfLen = (endIndex < sbufReadIndex) ? (CONFIG_SB_SIZE - sbufReadIndex) : filledBytes;
    const uint32_t secondHalfLen = filledBytes - firstHalfLen;

    // printf("Len%d datasize: %d\n", len, (int)filledBytes);

    Assert(sbufReadIndex < CONFIG_SB_SIZE && endIndex < CONFIG_SB_SIZE, "Index error");

    // Copy data from the circular buffer into the stream.
    SDL_memcpy(stream, sbuf + sbufReadIndex, firstHalfLen);
    SDL_memcpy(stream + firstHalfLen, sbuf, secondHalfLen);

    // Update the read index and the count of audio data.
    sbufReadIndex = endIndex;
    audio_count -= filledBytes;
    publish_audio_count();
}
#endif

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
#ifdef CONFIG_AUDIO_DUMMY
                /*
                 * The dummy backend accepts the guest's MMIO protocol but has
                 * no SDL callback thread to consume samples later.  Treat each
                 * commit as immediately drained; otherwise a headless run would
                 * fill the emulated stream buffer and block forever.
                 */
                Assert(val <= CONFIG_SB_SIZE,
                        "Dummy audio append is larger than the stream buffer: append=%u size=%u",
                        val, CONFIG_SB_SIZE);
                audio_count = 0;
                publish_audio_count();
#else
                /*
                 * AM writes a delta: the number of bytes just copied into the
                 * stream buffer.  Keeping the real occupancy in audio_count
                 * avoids a race where the SDL callback drains bytes between a
                 * guest count read and a later guest count write.  The mapped
                 * register cell is only the public view returned to reads.
                 */
                lock_audio_counter();
                Assert(audio_count <= CONFIG_SB_SIZE,
                        "Audio stream buffer count is invalid: count=%u size=%u",
                        audio_count, CONFIG_SB_SIZE);
                Assert(val <= CONFIG_SB_SIZE - audio_count,
                        "Audio stream buffer overflow: count=%u append=%u size=%u",
                        audio_count, val, CONFIG_SB_SIZE);
                audio_count += val;
                publish_audio_count();
                unlock_audio_counter();
#endif
                break;
            }

            // Do init and ignore write value.
            case reg_init: {
                Assert(val == 1, "The write value is not 1 in audio init, please check Abstract Machine.");

                close_audio_if_open();
                reset_audio_stream();

#ifndef CONFIG_AUDIO_DUMMY
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
                audio_opened = true;
                SDL_PauseAudio(0);
#endif

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
            lock_audio_counter();
            publish_audio_count();
            unlock_audio_counter();
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
  reset_audio_stream();

#ifndef CONFIG_AUDIO_DUMMY
  // Init subsystem in here before open device.
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) 
  {
      printf("SDL_InitSubSystem error: %s\n", SDL_GetError());
      exit(1);
  }
#endif
}
