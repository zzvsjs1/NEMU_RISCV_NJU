#include <amtest.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define SIZE 4096
#define RAW_PCM_RATE 44100
#define RAW_PCM_CH   2
#define RAW_PCM_BUFSZ 1024

typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t bits_per_sample;
  uint16_t block_align;
  const uint8_t *data;
  uint32_t data_size;
} WavInfo;

static inline uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16)
       | ((uint32_t)p[3] << 24);
}

static inline bool tag_is(const uint8_t *p, char a, char b, char c, char d) {
  return p[0] == (uint8_t)a && p[1] == (uint8_t)b && p[2] == (uint8_t)c && p[3] == (uint8_t)d;
}

// No libc calls, parses only 16-bit PCM WAV
static bool parse_wav(const uint8_t *buf, uint32_t len, WavInfo *out) {
  if (len < 12) return false;
  if (!tag_is(buf + 0, 'R','I','F','F')) return false;
  if (!tag_is(buf + 8, 'W','A','V','E')) return false;

  uint32_t off = 12;
  bool got_fmt = false, got_data = false;

  uint16_t audio_format = 0, channels = 0, bits_per_sample = 0, block_align = 0;
  uint32_t sample_rate = 0;
  const uint8_t *data_ptr = 0;
  uint32_t data_size = 0;

  while (off + 8 <= len) {
    const uint8_t *ch = buf + off;
    uint32_t chunk_size = rd_le32(ch + 4);
    if (off + 8 + chunk_size > len) break;  // malformed

    if (tag_is(ch + 0, 'f','m','t',' ')) {
      if (chunk_size < 16) return false;
      const uint8_t *fp = ch + 8;
      audio_format    = rd_le16(fp + 0);
      channels        = rd_le16(fp + 2);
      sample_rate     = rd_le32(fp + 4);
      /* byte_rate   = rd_le32(fp + 8); */  // not needed
      block_align     = rd_le16(fp + 12);
      bits_per_sample = rd_le16(fp + 14);
      got_fmt = true;
    } else if (tag_is(ch + 0, 'd','a','t','a')) {
      data_ptr  = ch + 8;
      data_size = chunk_size;
      got_data  = true;
    }
    // chunks are padded to even size
    off += 8 + chunk_size + (chunk_size & 1);
  }

  if (!got_fmt || !got_data) return false;
  if (audio_format != 1) return false;        // PCM only
  if (bits_per_sample != 16) return false;    // 16-bit only
  if (channels == 0 || block_align == 0 || sample_rate == 0) return false;

  out->sample_rate     = sample_rate;
  out->channels        = channels;
  out->bits_per_sample = bits_per_sample;
  out->block_align     = block_align;
  out->data            = data_ptr;
  out->data_size       = data_size;
  return true;
}

#define SIZE_ALIGNED(sz, align)  (((sz) / (align)) * (align))

void audio_test() {
  if (!io_read(AM_AUDIO_CONFIG).present) {
    printf("WARNING: %s does not support audio\n", TOSTRING(__ARCH__));
    return;
  }

  extern const uint8_t audio_payload[];
  extern const uint8_t audio_payload_end[];
  const uint8_t *blob   = audio_payload;
  uint32_t blob_len     = (uint32_t)(audio_payload_end - audio_payload);

  // Try to parse WAV, otherwise treat as raw 16-bit LE PCM
  WavInfo wi;
  const uint8_t *play_ptr;
  uint32_t play_len;
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t frame_bytes;
  uint32_t dev_bufsz = RAW_PCM_BUFSZ;

  if (parse_wav(blob, blob_len, &wi)) {
    sample_rate = wi.sample_rate;
    channels    = wi.channels;
    play_ptr    = wi.data;
    play_len    = wi.data_size;
    frame_bytes = wi.block_align;
    printf("Find WAV file\n");
  } else {
    sample_rate = RAW_PCM_RATE;
    channels    = RAW_PCM_CH;
    play_ptr    = blob;
    play_len    = blob_len;
    frame_bytes = 2 * channels;
    printf("Find PCM data\n");
  }

  io_write(AM_AUDIO_CTRL, sample_rate, channels, dev_bufsz);

  uint32_t nplay = 0;
  Area sbuf;
  sbuf.start = (uint8_t *)play_ptr;

  while (nplay < play_len) {
    uint32_t remain = play_len - nplay;
    uint32_t len = remain > SIZE ? SIZE : remain;

    // keep writes aligned to complete sample frames, avoids pops
    if (frame_bytes) {
      uint32_t aligned = SIZE_ALIGNED(len, frame_bytes);
      if (aligned == 0 && remain >= (uint32_t)frame_bytes) aligned = frame_bytes;
      if (aligned) len = aligned;
    }

    sbuf.end = sbuf.start + len;
    io_write(AM_AUDIO_PLAY, sbuf);

    sbuf.start += len;
    nplay      += len;
    printf("Already play %u/%u bytes of data\n", nplay, play_len);
  }

  while (io_read(AM_AUDIO_STATUS).count > 0);
}
