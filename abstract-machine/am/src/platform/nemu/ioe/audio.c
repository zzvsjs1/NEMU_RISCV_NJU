#include <am.h>
#include <nemu.h>
#include <stdio.h>

#define AUDIO_FREQ_ADDR       (AUDIO_ADDR + 0x00)
#define AUDIO_CHANNELS_ADDR   (AUDIO_ADDR + 0x04)
#define AUDIO_SAMPLES_ADDR    (AUDIO_ADDR + 0x08)
#define AUDIO_SBUF_SIZE_ADDR  (AUDIO_ADDR + 0x0c)
#define AUDIO_INIT_ADDR       (AUDIO_ADDR + 0x10)
#define AUDIO_COUNT_ADDR      (AUDIO_ADDR + 0x14)

static volatile uint32_t bufferSize = 0;
static volatile uintptr_t curWriteAddress = AUDIO_SBUF_ADDR;

static inline void advance_write_ptr(uint32_t len)
{
    curWriteAddress = AUDIO_SBUF_ADDR
        + ((curWriteAddress - AUDIO_SBUF_ADDR + len) % bufferSize);
}

void __am_audio_init() 
{
    // Do nothing, delay subsystem init in ctrl call.
}

void __am_audio_config(AM_AUDIO_CONFIG_T *cfg) 
{
    cfg->present = true;

    // Get buffer size.
    cfg->bufsize = inl(AUDIO_SBUF_SIZE_ADDR);
    bufferSize = cfg->bufsize;
}

void __am_audio_status(AM_AUDIO_STATUS_T *stat) 
{
    // Read count from register.
    stat->count = inl(AUDIO_COUNT_ADDR);
}

void __am_audio_ctrl(AM_AUDIO_CTRL_T *ctrl) 
{
    outl(AUDIO_FREQ_ADDR, ctrl->freq);
    outl(AUDIO_CHANNELS_ADDR, ctrl->channels);
    outl(AUDIO_SAMPLES_ADDR, ctrl->samples);
    curWriteAddress = AUDIO_SBUF_ADDR;

    // Start init in here, we need to make sure all the necessary data is ready.
    outl(AUDIO_INIT_ADDR, 1);
}

void __am_audio_play(AM_AUDIO_PLAY_T *ctl) 
{
    uint8_t *start = (uint8_t*)ctl->buf.start;
    uint8_t *end = (uint8_t*)ctl->buf.end;
    const uint32_t len = end - start;

    uint32_t bufferBeUsed = inl(AUDIO_COUNT_ADDR);

    // Wait until buffer is free.
    while (bufferSize - bufferBeUsed < len)
    {
        bufferBeUsed = inl(AUDIO_COUNT_ADDR);
    }

    // Write to buffer.
    while (start != end)
    {
        outb(curWriteAddress, *start);

        // Advance inside the device ring buffer and wrap back to the start.
        advance_write_ptr(1);
        ++start;
    }
    
    /*
     * Commit only the number of bytes appended above.  Do not read the count,
     * add len, then write the full value back: SDL's host callback may consume
     * samples between that read and write, so a full-value write can restore a
     * stale count and make the guest believe already-played bytes still exist.
     * NEMU adds this delta to its private counter while holding the SDL audio
     * lock, which keeps producer and consumer updates serialised.
     */
    outl(AUDIO_COUNT_ADDR, len);
}
