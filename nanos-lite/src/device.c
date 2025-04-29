#include <common.h>

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

  int total = sprintf(buff, "WIDTH:%d\nHEIGHT:%d\n\0", gpuConfig.width, gpuConfig.height);
  if (total < 0) 
  {
    total = 0; 
  }

  const size_t avail = total - offset;
  const size_t toCopy = (avail < len ? avail : len);

  memcpy(buf, buff, toCopy);

  return toCopy;
}

size_t fb_write(const void *buf, size_t offset, size_t len) 
{


  return 0;
}

void init_device() 
{
  Log("Initializing devices...");
  ioe_init();
}
