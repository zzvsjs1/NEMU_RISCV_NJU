#include "am.h"
#include "debug.h"
#include <common.h>
#include <memory.h>
#include <proc.h>
#include <stdint.h>

#if defined(MULTIPROGRAM) && !defined(TIME_SHARING)
# define MULTIPROGRAM_YIELD() yield()
#else
# define MULTIPROGRAM_YIELD()
#endif

#define NAME(key) \
  [AM_KEY_##key] = #key,

static const char *keyname[256] __attribute__((used)) = {
  [AM_KEY_NONE] = "NONE",
  AM_KEYS(NAME)
};

/*
 * All foreground apps share one physical /dev/fb. If a smaller app such as
 * Bird is centered on the screen, then a wider app such as NSlider can paint
 * outside Bird's canvas. When we later switch back to Bird, Bird may repaint
 * only its own canvas, leaving stale pixels around it.
 *
 * Treat the framebuffer like foreground-owned device state: keep one full
 * screen shadow for each switchable foreground PCB, update the running app's
 * shadow on every /dev/fb write, and blit the selected app's shadow during
 * F1/F2/F3 switching. This preserves each app's last visible screen without
 * requiring event-driven apps to repaint just because they became foreground.
 */
static int fb_screen_w = 0;
static int fb_screen_h = 0;
static uint32_t *fb_shadow[NR_FOREGROUND_PROC] = {};
static bool fb_shadow_ready = false;

/*
 * Audio is also a foreground-owned physical device. PAL and Bird keep their
 * own userspace SDL state after a hotkey switch, but NEMU has only one host
 * SDL audio device. If Bird opens 44100 Hz audio and we later switch back to
 * PAL, PAL will not call SDL_OpenAudio() again because its process still thinks
 * the device is open. Without restoring PAL's saved hardware format, NEMU keeps
 * playing one app's samples through another app's host configuration, which
 * makes the music sound sped up, slowed down, or distorted.
 */
typedef struct {
  bool configured;
  uint32_t freq;
  uint32_t channels;
  uint32_t samples;
} ForegroundAudioState;

static ForegroundAudioState audio_state[NR_FOREGROUND_PROC] = {};
static bool audio_restore_pending = false;

static size_t round_up_to_page(size_t size)
{
  return (size + PGSIZE - 1) & ~(size_t)(PGSIZE - 1);
}

static void init_fb_shadow(void)
{
  const AM_GPU_CONFIG_T cfg = io_read(AM_GPU_CONFIG);
  assert(cfg.present);

  fb_screen_w = cfg.width;
  fb_screen_h = cfg.height;

  const size_t fb_size = (size_t)fb_screen_w * (size_t)fb_screen_h * sizeof(uint32_t);
  const size_t alloc_size = round_up_to_page(fb_size);
  const size_t nr_page = alloc_size / PGSIZE;

  for (int i = 0; i < NR_FOREGROUND_PROC; i++)
  {
    fb_shadow[i] = new_page(nr_page);
    memset(fb_shadow[i], 0, alloc_size);
  }

  fb_shadow_ready = true;
}

// Mirror the current foreground app's linear /dev/fb write into its shadow.
static void update_current_fb_shadow(const void *buf, size_t offset, size_t len)
{
  /*
   * Mirror the exact byte range that userspace wrote, rather than reconstructing
   * rows from width/height.  Later performance work batches full-width images as
   * multi-row writes, and this byte-range mirror keeps the foreground restore
   * path independent of how the write was grouped.
   */
  if (!fb_shadow_ready || buf == NULL || len == 0)
  {
    return;
  }

  const int owner = current_pcb_index();
  if (owner < 0 || owner >= NR_FOREGROUND_PROC)
  {
    return;
  }

  const size_t fb_size = (size_t)fb_screen_w * (size_t)fb_screen_h * sizeof(uint32_t);
  if (offset >= fb_size)
  {
    return;
  }

  const size_t copy_len = len < fb_size - offset ? len : fb_size - offset;
  memcpy((uint8_t *)fb_shadow[owner] + offset, buf, copy_len);
}

// Restore the selected foreground app's full-screen shadow to the real display.
static void fb_restore_foreground(void)
{
  if (!fb_shadow_ready)
  {
    return;
  }

  const int owner = foreground_pcb_index();
  if (owner < 0 || owner >= NR_FOREGROUND_PROC)
  {
    return;
  }

  io_write(AM_GPU_FBDRAW, 0, 0, fb_shadow[owner], fb_screen_w, fb_screen_h, true);
}

static void audio_program(uint32_t freq, uint32_t channels, uint32_t samples)
{
  io_write(AM_AUDIO_CTRL, .freq = freq, .channels = channels, .samples = samples);
}

static void audio_remember_current(uint32_t freq, uint32_t channels, uint32_t samples)
{
  /*
   * Userspace SDL state survives a foreground switch, but the single physical
   * NEMU audio backend does not.  Remember only the hardware format here; queued
   * sample bytes are intentionally discarded when a foreground owner is restored.
   */
  const int owner = current_pcb_index();
  if (owner < 0 || owner >= NR_FOREGROUND_PROC)
  {
    return;
  }

  audio_state[owner] = (ForegroundAudioState) {
    .configured = true,
    .freq = freq,
    .channels = channels,
    .samples = samples,
  };
}

static void audio_restore_foreground(void)
{
  const int owner = foreground_pcb_index();
  if (owner < 0 || owner >= NR_FOREGROUND_PROC)
  {
    return;
  }

  const ForegroundAudioState *state = &audio_state[owner];
  if (!state->configured)
  {
    return;
  }

  /*
   * Reprogramming AM_AUDIO_CTRL resets both the AM stream-buffer write pointer
   * and NEMU's host SDL audio stream. Do it when the selected app is about to
   * run, not inside the old app's key-event syscall, because miniSDL calls its
   * audio callback once after polling events. Restoring here prevents the old
   * app from writing one last callback through the new app's audio format.
   */
  audio_program(state->freq, state->channels, state->samples);
}

void device_note_foreground_switch(void)
{
  audio_restore_pending = true;
}

void device_restore_foreground_on_schedule(void)
{
  if (!audio_restore_pending)
  {
    return;
  }

  audio_restore_pending = false;
  audio_restore_foreground();
}

static bool handle_foreground_hotkey(AM_INPUT_KEYBRD_T *keyboard)
{
  /* F1-F3 are kernel-owned hotkeys.  They switch the visible foreground app and
   * are not forwarded to userspace, which prevents games from treating a window
   * switch as normal input.
   */
  switch (keyboard->keycode) {
    case AM_KEY_F1:
      if (keyboard->keydown)
      {
        switch_fg_pcb(0);
        fb_restore_foreground();
      }
      return true;
    case AM_KEY_F2:
      if (keyboard->keydown)
      {
        switch_fg_pcb(1);
        fb_restore_foreground();
      }
      return true;
    case AM_KEY_F3:
      if (keyboard->keydown)
      {
        switch_fg_pcb(2);
        fb_restore_foreground();
      }
      return true;
    default:
      return false;
  }
}

static const char *mouse_button_name(int button)
{
  switch (button)
  {
    case AM_MOUSE_BUTTON_LEFT: return "LEFT";
    case AM_MOUSE_BUTTON_MIDDLE: return "MIDDLE";
    case AM_MOUSE_BUTTON_RIGHT: return "RIGHT";
    case AM_MOUSE_BUTTON_WHEELUP: return "WHEELUP";
    case AM_MOUSE_BUTTON_WHEELDOWN: return "WHEELDOWN";
    default: return "NONE";
  }
}

static size_t format_mouse_event(char *event, size_t event_size, AM_INPUT_MOUSE_T mouse)
{
  int format_len = 0;

  switch (mouse.type)
  {
    case AM_MOUSE_MOVE:
      format_len = snprintf(event, event_size, "mm %d %d %d\n",
          mouse.x, mouse.y, mouse.buttons);
      break;
    case AM_MOUSE_BUTTON_DOWN:
      format_len = snprintf(event, event_size, "md %s %d %d %d\n",
          mouse_button_name(mouse.button), mouse.x, mouse.y, mouse.buttons);
      break;
    case AM_MOUSE_BUTTON_UP:
      format_len = snprintf(event, event_size, "mu %s %d %d %d\n",
          mouse_button_name(mouse.button), mouse.x, mouse.y, mouse.buttons);
      break;
    case AM_MOUSE_WHEEL:
      format_len = snprintf(event, event_size, "mw %d %d %d %d %d\n",
          mouse.wheel_x, mouse.wheel_y, mouse.x, mouse.y, mouse.buttons);
      break;
    default:
      return 0;
  }

  if (format_len < 0 || (size_t)format_len >= event_size)
  {
    Log("format_mouse_event failed, len=%d", format_len);
    return 0;
  }

  return (size_t)format_len;
}

static size_t copy_event_record(void *buf, size_t len, const char *event, size_t event_len)
{
  /* /dev/events is a byte stream in navy apps.  A short userspace buffer may
   * receive a truncated record; the caller decides when to poll again.
   */
  const size_t write_len = event_len > len ? len : event_len;
  memcpy(buf, event, write_len);
  return write_len;
}

static size_t read_keyboard_event(void *buf, size_t len)
{
  AM_INPUT_KEYBRD_T keyboard = io_read(AM_INPUT_KEYBRD);
  if (keyboard.keycode == AM_KEY_NONE)
  {
    return 0;
  }

  if (handle_foreground_hotkey(&keyboard))
  {
    return 0;
  }

  const char *prefix = keyboard.keydown ? "kd" : "ku";
  const char *name = keyname[keyboard.keycode];
  char event[32];
  const int format_len = snprintf(event, sizeof(event), "%s %s\n", prefix, name);
  if (format_len < 0 || format_len >= (int)sizeof(event))
  {
    Log("read_keyboard_event format failed, len=%d", format_len);
    return 0;
  }

  return copy_event_record(buf, len, event, (size_t)format_len);
}

static size_t read_mouse_event(void *buf, size_t len)
{
  AM_INPUT_MOUSE_T mouse = io_read(AM_INPUT_MOUSE);
  if (mouse.type == AM_MOUSE_NONE)
  {
    return 0;
  }

  char event[64];
  const size_t event_len = format_mouse_event(event, sizeof(event), mouse);
  if (event_len == 0)
  {
    return 0;
  }

  return copy_event_record(buf, len, event, event_len);
}

size_t serial_write(const void *buf, size_t offset, size_t len)
{
  const char* buff = (const char*)buf;
  for (size_t i = 0; i < len; i++)
  {
    putch(buff[i]);
  }

  return len;
}

size_t events_read(void *buf, size_t offset, size_t len)
{
  static bool prefer_mouse = false;
  (void)offset;

  /*
   * Keyboard hotkeys and mouse movement share one text device.  Alternating the
   * first source is a small fairness rule: rapid mouse motion should not delay
   * F1/F2/F3 foreground switches, and a held key should not hide cursor updates.
   */
  size_t n = 0;
  if (prefer_mouse)
  {
    n = read_mouse_event(buf, len);
    if (n == 0) n = read_keyboard_event(buf, len);
  }
  else
  {
    n = read_keyboard_event(buf, len);
    if (n == 0) n = read_mouse_event(buf, len);
  }

  prefer_mouse = !prefer_mouse;
  return n;
}

size_t dispinfo_read(void *buf, size_t offset, size_t len) 
{
  /* /proc/dispinfo is regenerated on every read from the current AM GPU
   * config, so it stays correct even if the display size is supplied by NEMU.
   * The existing simple procfs model ignores offset for this synthetic file.
   */
  const AM_GPU_CONFIG_T gpuConfig = io_read(AM_GPU_CONFIG);
  assert(gpuConfig.present);

  char buff[64];

  int total = sprintf(buff, "WIDTH:%d\nHEIGHT:%d\n", gpuConfig.width, gpuConfig.height);
  if (total < 0 || total > 64) 
  {
    Log("dispinfo_read: total < 0 || total > 64  total=%d", total);
    assert(0);

    total = 0; 
  }

  const size_t avail = total;
  const size_t toCopy = avail < len ? avail : len;

  // Copy data to user program.
  memcpy(buf, buff, toCopy);

  return toCopy;
}

size_t fb_write(const void *buf, size_t offset, size_t len) 
{
  assert(fb_shadow_ready);
  assert(len % sizeof(uint32_t) == 0);

  const int screenW = fb_screen_w;

  // Compute necessary variables.
  const size_t byteOffset = offset;             // offset in bytes
  const size_t pixelOffset = byteOffset / sizeof(uint32_t);    // now in pixels
  
  int row = (int)(pixelOffset / screenW);        // integer division → row
  int col = (int)(pixelOffset % screenW);        // remainder → column
  
  size_t pixelCount = len / sizeof(uint32_t);            // len is bytes → divide to get pixel count

  if (pixelCount == 0)
  {
    return len;
  }

  // printf("offset=%d row=%d col=%d wPixels=%d\n", (int)offset, row, col, wPixels);

  /*
   * Full-width writes are common when an app repaints a whole image.  Send
   * those as one multi-row draw so the VGA sync flag is raised once instead of
   * once per row.  Other linear spans keep the old one-span behaviour.
   */
  if (col == 0 && pixelCount >= (size_t)screenW
      && pixelCount % (size_t)screenW == 0)
  {
    const int hPixels = (int)(pixelCount / (size_t)screenW);
    io_write(AM_GPU_FBDRAW, col, row, (void *)buf, screenW, hPixels, true);
  }
  else
  {
    /* Userspace framebuffer writes are expected to describe one linear span.
     * Non-full-width spans cannot wrap across rows here, so preserving the old
     * one-row draw keeps MiniSDL's simple /dev/fb contract intact.
     */
    assert(pixelCount <= (size_t)INT32_MAX);
    io_write(AM_GPU_FBDRAW, col, row, (void *)buf, (int)pixelCount, 1, true);
  }

  update_current_fb_shadow(buf, offset, len);

  return len;
}

// Semantics: write `len` bytes starting at (uint8_t*)buf + offset, block until all bytes are queued.
// Note: offset applies to the user buffer, not the device, since a stream device has no seek position.
size_t sb_write(const void *buf, size_t offset, size_t len) 
{
  if (buf == NULL || len == 0) return 0;

  const uint8_t *p = (const uint8_t *)buf + offset;
  size_t remain = len;

  while (remain > 0) {
    size_t free_bytes = 0;

    // Wait until the device ring buffer exposes some free space
    while (1) 
    {
      AM_AUDIO_STATUS_T st = io_read(AM_AUDIO_STATUS);
      AM_AUDIO_CONFIG_T cfg = io_read(AM_AUDIO_CONFIG);
      size_t used = (size_t)st.count;
      size_t cap  = (size_t)cfg.bufsize;
      free_bytes = (used >= cap) ? 0 : (cap - used);
      if (free_bytes > 0) break;
      MULTIPROGRAM_YIELD();  // be cooperative if a scheduler exists
    }

    // Write at most the available free space, and no more than what remains
    size_t n = free_bytes;
    if (n > remain) n = remain;

    // Optional, enforce sample frame alignment if the device requires it
    // size_t frame_bytes = 1;  // e.g., stereo 16-bit PCM is 4 bytes per frame
    // n -= n % frame_bytes;
    // if (n == 0) { MULTIPROGRAM_YIELD(); continue; }

    // Push this chunk
    Area sbuf;
    sbuf.start = (void *)p;
    sbuf.end   = (void *)(p + n);
    io_write(AM_AUDIO_PLAY, sbuf);

    // Advance source pointer
    p      += n;
    remain -= n;
  }

  // All requested bytes have been queued to the device
  return len;
}


size_t sbctl_write(const void *buf, size_t offset, size_t len)
{
  /* MiniSDL writes exactly freq/channels/samples as three 32-bit words.  Any
   * other length is rejected because a partial audio format would leave NEMU's
   * host stream in a state no app can reason about.
   */
  if (len != 3 * sizeof(uint32_t)) 
  {
    // Not supported
    return 0;  
  }

  const uint32_t *p = (const uint32_t *)buf;
  uint32_t freq = p[0];
  uint32_t channels = p[1];
  uint32_t samples = p[2];

  // Program the audio device.
  audio_remember_current(freq, channels, samples);
  audio_program(freq, channels, samples);
  return len;
}

size_t sbctl_read(void *buf, size_t offset, size_t len)
{
  /* /dev/sbctl exposes writable space, not used space, matching MiniSDL's
   * expectation that a positive value means another /dev/sb write can proceed.
   */
  AM_AUDIO_STATUS_T st = io_read(AM_AUDIO_STATUS);
  AM_AUDIO_CONFIG_T cfg = io_read(AM_AUDIO_CONFIG);
  int32_t free_bytes = (int32_t)cfg.bufsize - (int32_t)st.count;

  // Return one int, truncated to caller's len.
  size_t to_copy = len < sizeof(int32_t) ? len : sizeof(int32_t);
  memcpy(buf, &free_bytes, to_copy);
  return to_copy;
}

void init_device() 
{
  Log("Initializing devices...");
  ioe_init();
  init_fb_shadow();
}
