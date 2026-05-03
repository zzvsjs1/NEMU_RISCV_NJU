#include "am.h"
#include "debug.h"
#include <common.h>
#include <memory.h>
#include <proc.h>

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

// Mirror the current foreground app's one-row /dev/fb write into its shadow.
static void update_current_fb_shadow(const void *buf, int row, int col, int w_pixels)
{
  if (!fb_shadow_ready || buf == NULL || w_pixels <= 0)
  {
    return;
  }

  const int owner = current_pcb_index();
  if (owner < 0 || owner >= NR_FOREGROUND_PROC)
  {
    return;
  }

  if (row < 0 || row >= fb_screen_h || col >= fb_screen_w)
  {
    return;
  }

  int src_skip = 0;
  if (col < 0)
  {
    src_skip = -col;
    w_pixels -= src_skip;
    col = 0;
  }

  if (w_pixels <= 0)
  {
    return;
  }

  if (col + w_pixels > fb_screen_w)
  {
    w_pixels = fb_screen_w - col;
  }

  if (w_pixels <= 0)
  {
    return;
  }

  uint32_t *dst = fb_shadow[owner] + (size_t)row * (size_t)fb_screen_w + col;
  const uint32_t *src = (const uint32_t *)buf + src_skip;
  memcpy(dst, src, (size_t)w_pixels * sizeof(uint32_t));
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

static bool handle_foreground_hotkey(AM_INPUT_KEYBRD_T *keyboard)
{
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
  // Get keyborad ouput.
  AM_INPUT_KEYBRD_T keyboard = io_read(AM_INPUT_KEYBRD);

  // Ignore NONE key.
  if (keyboard.keycode == AM_KEY_NONE)
  {
    return 0;
  }

  if (handle_foreground_hotkey(&keyboard))
  {
    return 0;
  }

  // Determine the event type string: "kd" for key down, "ku" for key up.
  const char *prefix = keyboard.keydown ? "kd" : "ku";

  // Look up the key name from the pre-defined keyname array.
  const char *name = keyname[keyboard.keycode];

  // 5. Format the event into a temporary buffer, e.g. "kd A\n".
  char event[32];
  const int formatLen = sprintf(event, "%s %s\n", prefix, name);
  if (formatLen < 0) 
  {
    Log("events_read fail with formatLen=%d\n", formatLen);
    // sprintf failed for some reason
    return 0;
  }

  // Only write up to len bytes into the user’s buffer.
  const size_t writeLen = (formatLen > (int)len) ? len : (size_t)formatLen;
  memcpy(buf, event, writeLen);

  // Return the actual number of bytes written
  return writeLen;
}

size_t dispinfo_read(void *buf, size_t offset, size_t len) 
{
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
  const AM_GPU_CONFIG_T cfg = io_read(AM_GPU_CONFIG);
  assert(cfg.present);

  const int screenW = cfg.width;

  // Compute necessary variables.
  const size_t byteOffset = offset;             // offset in bytes
  const size_t pixelOffset = byteOffset / sizeof(uint32_t);    // now in pixels
  
  int row = (int)(pixelOffset / screenW);        // integer division → row
  int col = (int)(pixelOffset % screenW);        // remainder → column
  
  int wPixels = len / sizeof(uint32_t);                 // len is bytes → divide to get pixel count

  // printf("offset=%d row=%d col=%d wPixels=%d\n", (int)offset, row, col, wPixels);

  io_write(AM_GPU_FBDRAW, col, row, (void*)buf, wPixels, 1, true);
  update_current_fb_shadow(buf, row, col, wPixels);

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
  io_write(AM_AUDIO_CTRL, .freq = freq, .channels = channels, .samples = samples);
  return len;
}

size_t sbctl_read(void *buf, size_t offset, size_t len)
{
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
