#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <assert.h>

static int evtdev = -1;
static int fbdev = -1;
static int screen_w = 0, screen_h = 0;
static int canvas_x = 0, canvas_y = 0;


// For keyboard.
static int eventsFd = -1;

/* initTick holds the millisecond timestamp taken at NDL_Init() */
static uint32_t initTick = 0;

uint32_t NDL_GetTicks() 
{
  struct timeval tv;
  assert(gettimeofday(&tv, NULL) == 0);

  // NowMs is milliseconds since the Unix epoch.
  const uint32_t nowMs = (uint32_t)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);

  // Subtract the base time recorded in initTick.
  return nowMs - initTick;
}

int NDL_PollEvent(char *buf, int len) 
{
  // Open keyborad file.
  if (eventsFd == -1)
  {
    eventsFd = open("/dev/events", 0);
  }

  if (eventsFd < 0)
  {
    return 0;
  }

  const int readLen = read(eventsFd, buf, len);
  return readLen > 0 ? readLen : 0;
}

void NDL_OpenCanvas(int *w, int *h) {
  if (getenv("NWM_APP")) {
    int fbctl = 4;
    fbdev = 5;
    screen_w = *w; screen_h = *h;
    char buf[64];
    int len = sprintf(buf, "%d %d", screen_w, screen_h);
    // let NWM resize the window and create the frame buffer
    write(fbctl, buf, len);
    while (1) {
      // 3 = evtdev
      int nread = read(3, buf, sizeof(buf) - 1);
      if (nread <= 0) continue;
      buf[nread] = '\0';
      if (strcmp(buf, "mmap ok") == 0) break;
    }
    close(fbctl);
  }
}

void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h) 
{

}

void NDL_OpenAudio(int freq, int channels, int samples) 
{

}

void NDL_CloseAudio() 
{

}

int NDL_PlayAudio(void *buf, int len) {
  return 0;
}

int NDL_QueryAudio() {
  return 0;
}

int NDL_Init(uint32_t flags) 
{
  if (getenv("NWM_APP")) {
    evtdev = 3;
  }

  // ecord the “zero” point in milliseconds.
  struct timeval tv;
  assert(gettimeofday(&tv, NULL) == 0);
  initTick = (uint32_t)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);

  return 0;
}

void NDL_Quit() 
{
  // Close event fd.
  assert(close(eventsFd) == 0);


}
