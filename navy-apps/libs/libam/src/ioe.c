#include <am.h>
#include <NDL.h>
#include <stdio.h>
#include <string.h>

bool ioe_init()
{
    /*
   * Navy already exposes devices through NDL.  This libam layer keeps AM-style
   * applications such as FCEUX usable as normal Navy processes by translating
   * AM register accesses to the same /dev files used by miniSDL.
   */
    NDL_Init(0);
    return true;
}

void __am_timer_init()
{
}

void __am_gpu_init()
{
}

void __am_audio_init()
{
}

void __am_timer_rtc(AM_TIMER_RTC_T *rtc)
{
    /*
   * Nanos-lite provides a monotonic timer through gettimeofday/NDL, but not a
   * real wall clock.  Return a stable placeholder so callers do not read
   * uninitialised fields.
   */
    rtc->year = 1900;
    rtc->month = 1;
    rtc->day = 1;
    rtc->hour = 0;
    rtc->minute = 0;
    rtc->second = 0;
}

void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime)
{
    uptime->us = (uint64_t)NDL_GetTicks() * 1000ull;
}

static void __am_gpu_config(AM_GPU_CONFIG_T *cfg)
{
    static bool canvas_opened = false;
    static int width = 0;
    static int height = 0;

    if (!canvas_opened)
    {
        /*
     * Opening a zero-sized NDL canvas means "use the whole display".  AM GPU
     * coordinates are already physical framebuffer coordinates, so a full-size
     * canvas avoids an extra centring offset inside NDL_DrawRect().
     */
        NDL_OpenCanvas(&width, &height);
        canvas_opened = true;
    }

    cfg->present = true;
    cfg->has_accel = false;
    cfg->width = width;
    cfg->height = height;
    cfg->vmemsz = width * height * (int)sizeof(uint32_t);
}

static void __am_gpu_status(AM_GPU_STATUS_T *status)
{
    status->ready = true;
}

static void __am_gpu_fbdraw(AM_GPU_FBDRAW_T *draw)
{
    /*
   * AM uses a separate sync flag, while NDL flushes through /dev/fb writes.
   * A sync-only request therefore has no extra work here.
   */
    if (draw->pixels == NULL || draw->w <= 0 || draw->h <= 0)
    {
        return;
    }

    NDL_DrawRect((uint32_t *)draw->pixels, draw->x, draw->y, draw->w, draw->h);
}

static const char *key_names[256] = {
    [AM_KEY_NONE] = "NONE",
#define KEY_NAME(key) [AM_KEY_##key] = #key,
    AM_KEYS(KEY_NAME)
#undef KEY_NAME
};

static int lookup_keycode(const char *name)
{
    for (int i = 0; i < (int)(sizeof(key_names) / sizeof(key_names[0])); i++)
    {
        if (key_names[i] != NULL && strcmp(key_names[i], name) == 0)
        {
            return i;
        }
    }

    return AM_KEY_NONE;
}

static int lookup_mouse_button(const char *name)
{
    if (strcmp(name, "LEFT") == 0)
        return AM_MOUSE_BUTTON_LEFT;
    if (strcmp(name, "MIDDLE") == 0)
        return AM_MOUSE_BUTTON_MIDDLE;
    if (strcmp(name, "RIGHT") == 0)
        return AM_MOUSE_BUTTON_RIGHT;
    if (strcmp(name, "WHEELUP") == 0)
        return AM_MOUSE_BUTTON_WHEELUP;
    if (strcmp(name, "WHEELDOWN") == 0)
        return AM_MOUSE_BUTTON_WHEELDOWN;
    return AM_MOUSE_BUTTON_NONE;
}

static void __am_input_keybrd(AM_INPUT_KEYBRD_T *kbd)
{
    char event[64];
    const int n = NDL_PollEvent(event, sizeof(event) - 1);
    if (n <= 0)
    {
        kbd->keydown = false;
        kbd->keycode = AM_KEY_NONE;
        return;
    }

    event[n] = '\0';

    char prefix[3] = {};
    char name[32] = {};
    if (sscanf(event, "%2s %31s", prefix, name) != 2)
    {
        kbd->keydown = false;
        kbd->keycode = AM_KEY_NONE;
        return;
    }

    if (strcmp(prefix, "kd") == 0)
    {
        kbd->keydown = true;
    }
    else if (strcmp(prefix, "ku") == 0)
    {
        kbd->keydown = false;
    }
    else
    {
        /*
     * Mouse records are valid NDL events, but AM_INPUT_KEYBRD reports only one
     * keyboard event per read.  Mouse-aware AM apps should poll AM_INPUT_MOUSE.
     */
        kbd->keydown = false;
        kbd->keycode = AM_KEY_NONE;
        return;
    }

    kbd->keycode = lookup_keycode(name);
}

static void __am_input_mouse(AM_INPUT_MOUSE_T *mouse)
{
    char event[64];
    const int n = NDL_PollEvent(event, sizeof(event) - 1);
    if (n <= 0)
    {
        mouse->type = AM_MOUSE_NONE;
        return;
    }

    event[n] = '\0';

    char prefix[3] = {};
    if (sscanf(event, "%2s", prefix) != 1)
    {
        mouse->type = AM_MOUSE_NONE;
        return;
    }

    int x = 0;
    int y = 0;
    int buttons = 0;
    int wheel_x = 0;
    int wheel_y = 0;
    char button_name[16] = {};

    if (strcmp(prefix, "mm") == 0 &&
        sscanf(event, "%*s %d %d %d", &x, &y, &buttons) == 3)
    {
        mouse->type = AM_MOUSE_MOVE;
        mouse->x = x;
        mouse->y = y;
        mouse->button = AM_MOUSE_BUTTON_NONE;
        mouse->buttons = buttons;
        mouse->wheel_x = 0;
        mouse->wheel_y = 0;
        return;
    }

    if ((strcmp(prefix, "md") == 0 || strcmp(prefix, "mu") == 0) &&
        sscanf(event, "%*s %15s %d %d %d", button_name, &x, &y, &buttons) == 4)
    {
        mouse->type = strcmp(prefix, "md") == 0
                          ? AM_MOUSE_BUTTON_DOWN
                          : AM_MOUSE_BUTTON_UP;
        mouse->x = x;
        mouse->y = y;
        mouse->button = lookup_mouse_button(button_name);
        mouse->buttons = buttons;
        mouse->wheel_x = 0;
        mouse->wheel_y = 0;
        return;
    }

    if (strcmp(prefix, "mw") == 0 &&
        sscanf(event, "%*s %d %d %d %d %d", &wheel_x, &wheel_y, &x, &y, &buttons) == 5)
    {
        mouse->type = AM_MOUSE_WHEEL;
        mouse->x = x;
        mouse->y = y;
        mouse->button = wheel_y > 0 ? AM_MOUSE_BUTTON_WHEELUP : AM_MOUSE_BUTTON_WHEELDOWN;
        mouse->buttons = buttons;
        mouse->wheel_x = wheel_x;
        mouse->wheel_y = wheel_y;
        return;
    }

    mouse->type = AM_MOUSE_NONE;
}

enum
{
    NAVY_AM_AUDIO_BUFSIZE = 0x10000,
};

static bool audio_opened = false;

static void __am_audio_config(AM_AUDIO_CONFIG_T *cfg)
{
    cfg->present = true;
    cfg->bufsize = NAVY_AM_AUDIO_BUFSIZE;
}

static void __am_audio_ctrl(AM_AUDIO_CTRL_T *ctrl)
{
    NDL_OpenAudio(ctrl->freq, ctrl->channels, ctrl->samples);
    audio_opened = true;
}

static void __am_audio_status(AM_AUDIO_STATUS_T *status)
{
    if (!audio_opened)
    {
        status->count = 0;
        return;
    }

    const int free_bytes = NDL_QueryAudio();
    status->count = NAVY_AM_AUDIO_BUFSIZE - free_bytes;
    if (status->count < 0)
        status->count = 0;
    if (status->count > NAVY_AM_AUDIO_BUFSIZE)
        status->count = NAVY_AM_AUDIO_BUFSIZE;
}

static void __am_audio_play(AM_AUDIO_PLAY_T *play)
{
    uint8_t *start = (uint8_t *)play->buf.start;
    uint8_t *end = (uint8_t *)play->buf.end;
    if (start == NULL || end <= start)
    {
        return;
    }

    NDL_PlayAudio(start, (int)(end - start));
}

static void __am_disk_config(AM_DISK_CONFIG_T *cfg)
{
    /*
   * Navy applications should use libc files.  There is no raw block-device ABI
   * above nanos-lite that matches AM_DISK_BLKIO, so report disk as absent.
   */
    cfg->present = false;
    cfg->blksz = 0;
    cfg->blkcnt = 0;
}

static void __am_disk_status(AM_DISK_STATUS_T *stat)
{
    stat->ready = true;
}

static void __am_disk_blkio(AM_DISK_BLKIO_T *io)
{
    (void)io;
}

static void __am_timer_config(AM_TIMER_CONFIG_T *cfg)
{
    cfg->present = true;
    cfg->has_rtc = true;
}
static void __am_input_config(AM_INPUT_CONFIG_T *cfg) { cfg->present = true; }
static void __am_uart_config(AM_UART_CONFIG_T *cfg) { cfg->present = false; }
static void __am_net_config(AM_NET_CONFIG_T *cfg) { cfg->present = false; }

typedef void (*handler_t)(void *buf);
/*
 * AM register numbers are dispatched through a small lookup table, matching
 * the common AbstractMachine interface while allowing Navy to provide only the
 * services that are meaningful above nanos-lite.
 */
static void *lut[128] = {
    [AM_TIMER_CONFIG] = __am_timer_config,
    [AM_TIMER_RTC] = __am_timer_rtc,
    [AM_TIMER_UPTIME] = __am_timer_uptime,
    [AM_INPUT_CONFIG] = __am_input_config,
    [AM_INPUT_KEYBRD] = __am_input_keybrd,
    [AM_INPUT_MOUSE] = __am_input_mouse,
    [AM_GPU_CONFIG] = __am_gpu_config,
    [AM_GPU_FBDRAW] = __am_gpu_fbdraw,
    [AM_GPU_STATUS] = __am_gpu_status,
    [AM_UART_CONFIG] = __am_uart_config,
    [AM_AUDIO_CONFIG] = __am_audio_config,
    [AM_AUDIO_CTRL] = __am_audio_ctrl,
    [AM_AUDIO_STATUS] = __am_audio_status,
    [AM_AUDIO_PLAY] = __am_audio_play,
    [AM_DISK_CONFIG] = __am_disk_config,
    [AM_DISK_STATUS] = __am_disk_status,
    [AM_DISK_BLKIO] = __am_disk_blkio,
    [AM_NET_CONFIG] = __am_net_config,
};

void ioe_read(int reg, void *buf)
{
    handler_t handler = (handler_t)lut[reg];
    if (handler != NULL)
    {
        handler(buf);
    }
}

void ioe_write(int reg, void *buf)
{
    handler_t handler = (handler_t)lut[reg];
    if (handler != NULL)
    {
        handler(buf);
    }
}
