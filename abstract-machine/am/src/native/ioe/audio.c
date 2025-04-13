#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <klib.h>
#include <SDL2/SDL.h>

static int rfd = -1, wfd = -1;
static volatile int count = 0;
static SDL_AudioSpec s = {0};
static SDL_AudioSpec sd = {0};

void __am_audio_init() {
  int fds[2];
  int ret = pipe2(fds, O_NONBLOCK);
  assert(ret == 0);
  rfd = fds[0];
  wfd = fds[1];
}

static void audio_play(void *userdata, uint8_t *stream, int len) {
  int nread = len;
  if (count < len) nread = count;
  int b = 0;
  while (b < nread) {
    int n = read(rfd, stream, nread);
    if (n > 0) b += n;
  }

  count -= nread;
  if (len > nread) {
    SDL_memset(stream + nread, sd.silence, len - nread);
  }
}

static void audio_write(uint8_t *buf, int len) {
  int nwrite = 0;
  while (nwrite < len) {
    int n = write(wfd, buf, len);
    if (n == -1) n = 0;
    count += n;
    nwrite += n;
  }
}

void __am_audio_ctrl(AM_AUDIO_CTRL_T *ctrl) {
  s.freq = ctrl->freq;
  s.format = AUDIO_S16SYS;
  s.channels = ctrl->channels;
  s.samples = ctrl->samples;
  s.callback = audio_play;
  s.userdata = NULL;

  count = 0;
  int ret = SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (ret == 0) {
    SDL_OpenAudio(&s, &sd);
    SDL_PauseAudio(0);
  }
}

void __am_audio_status(AM_AUDIO_STATUS_T *stat) {
  stat->count = count;
}

void __am_audio_play(AM_AUDIO_PLAY_T *ctl) {
  int len = ctl->buf.end - ctl->buf.start;
  audio_write(ctl->buf.start, len);
}

void __am_audio_config(AM_AUDIO_CONFIG_T *cfg) {
  cfg->present = true;
  cfg->bufsize = fcntl(rfd, F_GETPIPE_SZ);
}
