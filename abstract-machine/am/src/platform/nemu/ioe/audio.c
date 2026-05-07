#include <am.h>
#include <nemu.h>
#include <stdio.h>

#define AUDIO_FREQ_ADDR       (AUDIO_ADDR + 0x00)
#define AUDIO_CHANNELS_ADDR   (AUDIO_ADDR + 0x04)
#define AUDIO_SAMPLES_ADDR    (AUDIO_ADDR + 0x08)
#define AUDIO_SBUF_SIZE_ADDR  (AUDIO_ADDR + 0x0c)
#define AUDIO_INIT_ADDR       (AUDIO_ADDR + 0x10)
#define AUDIO_COUNT_ADDR      (AUDIO_ADDR + 0x14)
#define AUDIO_BULK_SRC_ADDR   (AUDIO_ADDR + 0x18)
#define AUDIO_BULK_LEN_ADDR   (AUDIO_ADDR + 0x1c)
#define AUDIO_BULK_CMD_ADDR   (AUDIO_ADDR + 0x20)
#define AUDIO_BULK_CMD_APPEND 1u

/* Cached after AM_AUDIO_CONFIG so AM_AUDIO_PLAY can check ring-buffer capacity
 * without rereading the mostly-static configuration register on every poll.
 * NEMU owns the live occupancy count; AM only keeps the immutable capacity.
 */
static volatile uint32_t bufferSize = 0;

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
    /* The init register is the commit point: NEMU samples the three format
     * registers when this is written, then recreates the host audio stream.
     */
    outl(AUDIO_FREQ_ADDR, ctrl->freq);
    outl(AUDIO_CHANNELS_ADDR, ctrl->channels);
    outl(AUDIO_SAMPLES_ADDR, ctrl->samples);

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
        /*
         * This poll reads NEMU's published occupancy only.  AM deliberately does
         * not compute a new absolute count after copying data; the host callback
         * may drain bytes while the guest is spinning, so NEMU must be the sole
         * owner of the live count.
         */
        bufferBeUsed = inl(AUDIO_COUNT_ADDR);
    }

    /*
     * NEMU provides a private bulk append command for the stream buffer.  The
     * older byte-at-a-time path used one MMIO write for every sample byte, which
     * can starve the audio producer when PAL is also painting frames.  The bulk
     * command keeps the same ring-buffer occupancy protocol but lets NEMU copy
     * the whole chunk while holding its audio lock.
     */
    outl(AUDIO_BULK_SRC_ADDR, (uintptr_t)start);
    outl(AUDIO_BULK_LEN_ADDR, len);
    outl(AUDIO_BULK_CMD_ADDR, AUDIO_BULK_CMD_APPEND);
}
