#include <SDL2/SDL.h>
#include <common.h>
#include <debug.h>
#include <stdbool.h>
#include <math.h>
#include <device/map.h>
#include <isa.h>
#include <memory/paddr.h>
#include <utils.h>

enum
{
    reg_freq,      // Audio frequency (Hz)
    reg_channels,  // Number of channels (e.g., 1 for mono, 2 for stereo)
    reg_samples,   // Number of samples per callback
    reg_sbuf_size, // Total size of the stream buffer
    reg_init,      // Write to initialise audio
    reg_count,     // Read used bytes; write the number of newly appended bytes
    reg_bulk_src,  // Guest pointer for bulk stream-buffer append
    reg_bulk_len,  // Number of bytes to append from reg_bulk_src
    reg_bulk_cmd,  // Write AUDIO_BULK_CMD_APPEND to commit the append
    nr_reg
};

#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#define AUDIO_PAGE_SIZE 4096u
#define AUDIO_PAGE_MASK (AUDIO_PAGE_SIZE - 1u)
#define AUDIO_BULK_CMD_APPEND 1u
#define AUDIO_STATS_INTERVAL_US 5000000ull

#ifndef CONFIG_AUDIO_DUMMY
static SDL_AudioSpec spec = {0};
static bool audio_opened = false;
#endif

static uint8_t *sbuf = NULL;
static uint32_t *audio_base = NULL;

static uint32_t audio_count = 0;
static uint32_t sbufReadIndex = 0;
static bool audio_stats_enabled = false;
static uint64_t audio_stats_last_us = 0;
static uint64_t audio_stats_appends = 0;
static uint64_t audio_stats_append_bytes = 0;
static uint64_t audio_stats_callbacks = 0;
static uint64_t audio_stats_played_bytes = 0;
static uint64_t audio_stats_underrun_callbacks = 0;
static uint64_t audio_stats_underrun_bytes = 0;
static uint32_t audio_stats_max_count = 0;

static bool audio_env_flag_enabled(const char *name)
{
    const char *env = getenv(name);
    return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static void audio_note_count(void)
{
    if (audio_count > audio_stats_max_count)
    {
        audio_stats_max_count = audio_count;
    }
}

static void audio_stats_maybe_print(void)
{
    if (!audio_stats_enabled)
    {
        return;
    }

    const uint64_t now = get_time();

    if (audio_stats_last_us == 0)
    {
        audio_stats_last_us = now;
        return;
    }

    const uint64_t elapsed = now - audio_stats_last_us;

    if (elapsed < AUDIO_STATS_INTERVAL_US)
    {
        return;
    }

    const double seconds = (double)elapsed / 1000000.0;
    const double append_kib = (double)audio_stats_append_bytes / 1024.0;
    const double played_kib = (double)audio_stats_played_bytes / 1024.0;
    const double underrun_kib = (double)audio_stats_underrun_bytes / 1024.0;
    printf("[audio] elapsed=%.3f s appends=%" PRIu64 " append=%.1f KiB "
           "callbacks=%" PRIu64 " played=%.1f KiB underruns=%" PRIu64
           " underrun=%.1f KiB count=%u max_count=%u\n",
           seconds, audio_stats_appends, append_kib,
           audio_stats_callbacks, played_kib,
           audio_stats_underrun_callbacks, underrun_kib,
           audio_count, audio_stats_max_count);
    fflush(stdout);

    audio_stats_last_us = now;
    audio_stats_appends = 0;
    audio_stats_append_bytes = 0;
    audio_stats_callbacks = 0;
    audio_stats_played_bytes = 0;
    audio_stats_underrun_callbacks = 0;
    audio_stats_underrun_bytes = 0;
    audio_stats_max_count = audio_count;
}

static void publish_audio_count(void)
{
    audio_base[reg_count] = audio_count;
}

static void lock_audio_counter(void)
{
#ifndef CONFIG_AUDIO_DUMMY
    /*
     * The SDL callback and the guest MMIO handler are the two writers of the
     * occupancy counter.  Lock only after the host device is open: before that,
     * SDL has no callback thread and SDL_LockAudio() would only add host-side
     * work to the init path.
     */

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
    audio_note_count();
    publish_audio_count();
#ifndef CONFIG_AUDIO_DUMMY
    memset(&spec, 0, sizeof(spec));
#endif
}

#ifndef CONFIG_AUDIO_DUMMY
// This is the callback function that SDL calls to get more audio samples.
// It reads data from sbuf (the ring buffer) and copies it into the stream
// buffer. If data runs out, the rest of the output is filled with silence's value.
static void sdlAudioCallback(void *userdata, Uint8 *stream, int len)
{
    // Fill the stream buffer with silence.
    SDL_memset(stream, spec.silence, len);

    // Determine how many bytes we can copy from the audio buffer.
    const uint32_t currentSizeOfData = audio_count;
    const uint32_t filledBytes = MIN((uint32_t)len, currentSizeOfData);
    const uint32_t missingBytes = (uint32_t)len - filledBytes;

    /*
     * Copy at most to the physical end of the ring before wrapping.  The old
     * endIndex comparison could not distinguish "no wrap" from "wrapped
     * exactly back to the same index"; with a full-buffer callback and a
     * non-zero read index, that would try to copy past the end of sbuf.
     */
    const uint32_t firstHalfLen = MIN(filledBytes, CONFIG_SB_SIZE - sbufReadIndex);
    const uint32_t secondHalfLen = filledBytes - firstHalfLen;
    const uint32_t endIndex = (sbufReadIndex + filledBytes) % CONFIG_SB_SIZE;

    // printf("Len%d datasize: %d\n", len, (int)filledBytes);

    Assert(sbufReadIndex < CONFIG_SB_SIZE && endIndex < CONFIG_SB_SIZE, "Index error");

    // Copy data from the circular buffer into the stream.
    SDL_memcpy(stream, sbuf + sbufReadIndex, firstHalfLen);
    SDL_memcpy(stream + firstHalfLen, sbuf, secondHalfLen);

    // Update the read index and the count of audio data.
    sbufReadIndex = endIndex;
    audio_count -= filledBytes;
    audio_stats_callbacks++;
    audio_stats_played_bytes += filledBytes;

    if (missingBytes > 0)
    {
        audio_stats_underrun_callbacks++;
        audio_stats_underrun_bytes += missingBytes;
    }
    audio_note_count();
    publish_audio_count();
}
#endif

static bool audio_guest_read_chunk(vaddr_t addr, size_t wanted, uint8_t **host,
                                   size_t *len)
{
    if (wanted == 0)
    {
        return false;
    }

    paddr_t paddr = 0;
    const int mmu = isa_mmu_check(addr, 1, MEM_TYPE_READ);

    if (mmu == MMU_DIRECT)
    {
        paddr = (paddr_t)addr;
    }
    else if (mmu == MMU_TRANSLATE)
    {
        const paddr_t ret = isa_mmu_translate(addr, 1, MEM_TYPE_READ);

        if ((ret & (paddr_t)AUDIO_PAGE_MASK) != MEM_RET_OK)
        {
            return false;
        }
        
        paddr = (ret & ~(paddr_t)AUDIO_PAGE_MASK) | (paddr_t)(addr & AUDIO_PAGE_MASK);
    }
    else
    {
        return false;
    }

    if (!in_pmem(paddr))
    {
        return false;
    }

    size_t chunk = AUDIO_PAGE_SIZE - (size_t)(addr & AUDIO_PAGE_MASK);
    const paddr_t pmem_end = (paddr_t)CONFIG_MBASE + (paddr_t)CONFIG_MSIZE;

    if ((paddr_t)(paddr + chunk) > pmem_end)
    {
        chunk = (size_t)(pmem_end - paddr);
    }

    if (chunk > wanted)
    {
        chunk = wanted;
    }

    *host = guest_to_host(paddr);
    *len = chunk;
    return chunk > 0;
}

static void append_audio_bytes(vaddr_t src, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

#ifdef CONFIG_AUDIO_DUMMY
    Assert(len <= CONFIG_SB_SIZE,
           "Dummy audio append is larger than the stream buffer: append=%u size=%u",
           len, CONFIG_SB_SIZE);
    audio_stats_appends++;
    audio_stats_append_bytes += len;
    audio_count = 0;
    publish_audio_count();
#else
    /*
     * The count register is a delta-commit interface.  NEMU, not AM, owns the
     * live count because the host callback can consume bytes between two guest
     * instructions.  Copy the whole guest chunk and publish the new count while
     * the SDL audio lock is held, so producer and consumer never resurrect a
     * stale occupancy value.
     */
    lock_audio_counter();
    Assert(audio_count <= CONFIG_SB_SIZE,
           "Audio stream buffer count is invalid: count=%u size=%u",
           audio_count, CONFIG_SB_SIZE);
    Assert(len <= CONFIG_SB_SIZE - audio_count,
           "Audio stream buffer overflow: count=%u append=%u size=%u",
           audio_count, len, CONFIG_SB_SIZE);

    uint32_t writeIndex = (sbufReadIndex + audio_count) % CONFIG_SB_SIZE;
    size_t done = 0;
    while (done < len)
    {
        uint8_t *host = NULL;
        size_t src_chunk = 0;
        Assert(audio_guest_read_chunk(src + (vaddr_t)done, (size_t)len - done,
                                      &host, &src_chunk),
               "audio: cannot translate bulk source vaddr=0x%08x",
               src + (vaddr_t)done);

        size_t dst_chunk = CONFIG_SB_SIZE - writeIndex;

        if (dst_chunk > src_chunk)
        {
            dst_chunk = src_chunk;
        }
        memcpy(sbuf + writeIndex, host, dst_chunk);
        writeIndex = (writeIndex + dst_chunk) % CONFIG_SB_SIZE;
        done += dst_chunk;
    }

    audio_count += len;
    audio_stats_appends++;
    audio_stats_append_bytes += len;
    audio_note_count();
    publish_audio_count();
    unlock_audio_counter();
#endif

    audio_stats_maybe_print();
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
            case reg_freq:
            {
                break;
            }

            case reg_channels:
            {
                break;
            }

            case reg_samples:
            {
                break;
            }

            case reg_bulk_src:
            case reg_bulk_len:
            {
                break;
            }

            case reg_count:
            {
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

            case reg_bulk_cmd:
            {
                /*
                    * Bulk append keeps the stream-buffer protocol but avoids the
                    * historical byte-at-a-time MMIO path.  Large PAL/ONScripter
                    * callbacks can otherwise spend so long crossing MMIO that the
                    * host audio callback drains the queue faster than the guest can
                    * refill it.
                    */
                Assert(val == AUDIO_BULK_CMD_APPEND,
                    "Unsupported audio bulk command: %u", val);
                append_audio_bytes((vaddr_t)audio_base[reg_bulk_src],
                                audio_base[reg_bulk_len]);
                audio_base[reg_bulk_cmd] = 0;
                break;
            }

            // Do init and ignore write value.
            case reg_init:
            {
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

                /*
                * NEMU stores guest PCM bytes exactly as they were written to
                * /dev/sb.  There is no conversion layer between the guest ring
                * buffer and this callback, so the callback stream must stay in
                * the same S16SYS/frequency/channel layout requested by the
                * guest.  Passing a non-NULL obtained spec lets SDL change the
                * callback format on some hosts; copying guest S16 bytes into
                * that changed format can make PAL audio sound warped or
                * distorted.  With obtained == NULL, SDL keeps the callback
                * format as requested and converts to the real hardware format
                * internally when needed.
                */
                if (SDL_OpenAudio(&spec, NULL) < 0)
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

            default:
            {
                Assert(false, "Write to a wrong audio register, the value is %" PRIu32 ".\n", offset);
                break;
            }
            }

            return;
        }

        // Read operations for registers.
        switch (reg)
        {
        case reg_sbuf_size:
        {
            break;
        }

        case reg_count:
        {
            lock_audio_counter();
            publish_audio_count();
            unlock_audio_counter();
            break;
        }

        default:
        {
            break;
        }
    }
}

void init_audio()
{
    audio_stats_enabled = audio_env_flag_enabled("NEMU_AUDIO_STATS");
    audio_stats_last_us = get_time();

    if (audio_stats_enabled)
    {
        Log("audio: host stats enabled, print interval = %" PRIu64 " us",
            (uint64_t)AUDIO_STATS_INTERVAL_US);
    }

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
