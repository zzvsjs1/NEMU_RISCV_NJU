#include <amtest.h>

#define SIZE 4096

void audio_test() {
  if (!io_read(AM_AUDIO_CONFIG).present) {
    printf("WARNING: %s does not support audio\n", TOSTRING(__ARCH__));
    return;
  }

  io_write(AM_AUDIO_CTRL, 44100, 2, 1024);

  extern uint8_t audio_payload, audio_payload_end;
  uint32_t audio_len = &audio_payload_end - &audio_payload;
  int nplay = 0;
  Area sbuf;
  sbuf.start = &audio_payload;
  while (nplay < audio_len) {
    int len = (audio_len - nplay > SIZE ? SIZE : audio_len - nplay);
    sbuf.end = sbuf.start + len;
    io_write(AM_AUDIO_PLAY, sbuf);
    sbuf.start += len;
    nplay += len;
    // printf("Already play %d/%d bytes of data\n", nplay, audio_len);
  }

  // wait until the audio finishes
  while (io_read(AM_AUDIO_STATUS).count > 0);
}
