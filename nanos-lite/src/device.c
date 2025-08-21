#include "debug.h"
#include <common.h>

#define NAME(key) \
  [AM_KEY_##key] = #key,

static const char *keyname[256] __attribute__((used)) = {
  [AM_KEY_NONE] = "NONE",
  AM_KEYS(NAME)
};

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

  return len;
}

size_t sb_write(const void *buf, size_t offset, size_t len) 
{
  if (len == 0) return 0;

  // Wait until the stream buffer has enough free space to hold `len` bytes
  // This makes write() to /dev/sb blocking as required.
  while (1) 
  {
    AM_AUDIO_STATUS_T st = io_read(AM_AUDIO_STATUS);
    AM_AUDIO_CONFIG_T cfg = io_read(AM_AUDIO_CONFIG);
    size_t free_bytes = (size_t)cfg.bufsize - (size_t)st.count;
    if (free_bytes >= len) break;
  }

  // Now push the whole chunk
  Area sbuf;
  sbuf.start = (void *)buf;
  sbuf.end   = (void *)((uint8_t *)buf + len);
  io_write(AM_AUDIO_PLAY, sbuf);
  return len;
}

size_t sbctl_write(const void *buf, size_t offset, size_t len)
{
  if (len != 3 * sizeof(uint32_t)) 
  {
    return 0;  // not supported
  }

  const uint32_t *p = (const uint32_t *)buf;
  uint32_t freq = p[0];
  uint32_t channels = p[1];
  uint32_t samples = p[2];

  Log("sbctl_write");

  // Program the audio device
  io_write(AM_AUDIO_CTRL, .freq = freq, .channels = channels, .samples = samples);
  return len;
}

size_t sbctl_read(void *buf, size_t offset, size_t len)
{
  AM_AUDIO_STATUS_T st = io_read(AM_AUDIO_STATUS);
  AM_AUDIO_CONFIG_T cfg = io_read(AM_AUDIO_CONFIG);
  int32_t free_bytes = (int32_t)cfg.bufsize - (int32_t)st.count;

  // Return one int, truncated to caller's `len`
  size_t to_copy = len < sizeof(int32_t) ? len : sizeof(int32_t);
  memcpy(buf, &free_bytes, to_copy);
  return to_copy;
}

void init_device() 
{
  Log("Initializing devices...");
  ioe_init();
}
