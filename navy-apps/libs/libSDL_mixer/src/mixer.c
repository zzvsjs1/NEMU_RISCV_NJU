#include <SDL_mixer.h>
#include <vorbis.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CHANNELS 8
#define MAX_OUTPUT_CHANNELS 2
#define MUSIC_DECODE_FRAMES 512

struct Mix_Music {
  uint8_t *encoded;
  int encoded_len;
  stb_vorbis *vorbis;
  int frequency;
  int channels;

  int playing;
  int loops_left;
  uint32_t rate_accum;

  int16_t decoded[MUSIC_DECODE_FRAMES * MAX_OUTPUT_CHANNELS];
  int decoded_frames;
  int decoded_pos;
};

typedef struct {
  Mix_Chunk *chunk;
  int playing;
  int paused;
  int loops_left;
  int volume;
  uint32_t frame_pos;
  uint32_t rate_accum;
} MixerChannel;

static SDL_AudioSpec device;
static int audio_opened = 0;
static int device_bytes_per_sample = 0;

static MixerChannel *mix_channels = NULL;
static int mix_channel_count = 0;
static void (*channel_finished_hook)(int channel) = NULL;

static Mix_Music *current_music = NULL;
static int music_volume = MIX_MAX_VOLUME;
static void (*music_finished_hook)(void) = NULL;

static char mixer_error[128] = "";

static void set_error(const char *message)
{
  if (message == NULL) message = "";
  strncpy(mixer_error, message, sizeof(mixer_error) - 1);
  mixer_error[sizeof(mixer_error) - 1] = '\0';
}

static int clamp_volume(int volume)
{
  if (volume < 0) return volume;
  if (volume > MIX_MAX_VOLUME) return MIX_MAX_VOLUME;
  return volume;
}

static int bytes_per_sample(uint16_t format)
{
  if (format == AUDIO_U8) return 1;
  if (format == AUDIO_S16SYS) return 2;
  return 0;
}

static uint16_t read_u16le(const uint8_t *p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t read_s16le(const uint8_t *p)
{
  return (int16_t)read_u16le(p);
}

static void write_s16le(uint8_t *p, int16_t value)
{
  uint16_t raw = (uint16_t)value;
  p[0] = (uint8_t)(raw & 0xff);
  p[1] = (uint8_t)(raw >> 8);
}

static int16_t clamp_s16(int value)
{
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return (int16_t)value;
}

static uint8_t clamp_u8_audio(int value)
{
  if (value > 127) value = 127;
  if (value < -128) value = -128;
  return (uint8_t)(value + 128);
}

static void mix_sample(uint8_t *frame, int channel, int16_t sample, int volume)
{
  if (volume <= 0) return;

  int scaled = ((int)sample * volume) / MIX_MAX_VOLUME;

  if (device.format == AUDIO_U8) {
    int mixed = ((int)frame[channel] - 128) + scaled / 256;
    frame[channel] = clamp_u8_audio(mixed);
  } else {
    uint8_t *dst = frame + channel * 2;
    int mixed = (int)read_s16le(dst) + scaled;
    write_s16le(dst, clamp_s16(mixed));
  }
}

static uint32_t chunk_frame_count(const Mix_Chunk *chunk)
{
  int bytes = bytes_per_sample(chunk->format);
  int frame_bytes = bytes * chunk->channels;
  if (frame_bytes <= 0) return 0;
  return chunk->alen / (uint32_t)frame_bytes;
}

static int16_t chunk_source_sample(const Mix_Chunk *chunk, uint32_t frame, int source_channel)
{
  int bytes = bytes_per_sample(chunk->format);
  const uint8_t *src = chunk->abuf + (frame * chunk->channels + source_channel) * bytes;

  if (chunk->format == AUDIO_U8) {
    return (int16_t)(((int)*src - 128) << 8);
  }

  return read_s16le(src);
}

static int16_t chunk_output_sample(const Mix_Chunk *chunk, uint32_t frame, int out_channel)
{
  if (chunk->channels == 1) {
    return chunk_source_sample(chunk, frame, 0);
  }

  if (device.channels == 1) {
    int left = chunk_source_sample(chunk, frame, 0);
    int right = chunk_source_sample(chunk, frame, 1);
    return (int16_t)((left + right) / 2);
  }

  return chunk_source_sample(chunk, frame, out_channel < chunk->channels ? out_channel : 0);
}

static int advance_source_frame(uint32_t *accum, int source_freq)
{
  int steps = 0;

  if (device.freq <= 0 || source_freq <= 0) return 1;

  /*
   * The callback asks for frames at the opened device frequency.  Adding the
   * source rate each output frame and stepping once the accumulator reaches
   * the device rate duplicates low-frequency samples and drops high-frequency
   * samples.  It is nearest-neighbour resampling: basic, deterministic, and
   * enough for ONScripter's short effects and background music.
   */
  *accum += (uint32_t)source_freq;
  while (*accum >= (uint32_t)device.freq) {
    *accum -= (uint32_t)device.freq;
    steps++;
  }

  return steps;
}

static void finish_channel(int channel, int call_hook)
{
  if (channel < 0 || channel >= mix_channel_count) return;

  MixerChannel *slot = &mix_channels[channel];
  if (!slot->playing) return;

  slot->playing = 0;
  slot->paused = 0;
  slot->chunk = NULL;
  slot->frame_pos = 0;
  slot->rate_accum = 0;
  slot->loops_left = 0;

  if (call_hook && channel_finished_hook != NULL) {
    channel_finished_hook(channel);
  }
}

static int refill_music(Mix_Music *music)
{
  int out_channels = device.channels;
  if (out_channels < 1 || out_channels > MAX_OUTPUT_CHANNELS) return 0;

  for (;;) {
    int frames = stb_vorbis_get_samples_short_interleaved(
        music->vorbis, out_channels, music->decoded,
        MUSIC_DECODE_FRAMES * out_channels);

    if (frames > 0) {
      music->decoded_frames = frames;
      music->decoded_pos = 0;
      return 1;
    }

    if (music->loops_left == 0) return 0;
    if (music->loops_left > 0) music->loops_left--;

    if (!stb_vorbis_seek_start(music->vorbis)) {
      return 0;
    }

    music->decoded_frames = 0;
    music->decoded_pos = 0;
    music->rate_accum = 0;
  }
}

static int music_frame(Mix_Music *music, int16_t *samples)
{
  if (music->decoded_pos >= music->decoded_frames && !refill_music(music)) {
    return 0;
  }

  for (int c = 0; c < device.channels; c++) {
    samples[c] = music->decoded[music->decoded_pos * device.channels + c];
  }

  int steps = advance_source_frame(&music->rate_accum, music->frequency);
  music->decoded_pos += steps;
  return 1;
}

static void finish_music(int call_hook)
{
  Mix_Music *music = current_music;
  if (music == NULL || !music->playing) return;

  music->playing = 0;
  current_music = NULL;
  music->decoded_frames = 0;
  music->decoded_pos = 0;
  music->rate_accum = 0;

  if (call_hook && music_finished_hook != NULL) {
    music_finished_hook();
  }
}

static void mix_music_frame(uint8_t *frame)
{
  int16_t samples[MAX_OUTPUT_CHANNELS] = {0};

  if (current_music == NULL || !current_music->playing) return;

  if (!music_frame(current_music, samples)) {
    finish_music(1);
    return;
  }

  for (int c = 0; c < device.channels; c++) {
    mix_sample(frame, c, samples[c], music_volume);
  }
}

static void mix_channel_frame(int channel, uint8_t *frame)
{
  MixerChannel *slot = &mix_channels[channel];
  int16_t samples[MAX_OUTPUT_CHANNELS] = {0};

  if (!slot->playing || slot->paused || slot->chunk == NULL) return;

  Mix_Chunk *chunk = slot->chunk;
  uint32_t total_frames = chunk_frame_count(chunk);

  if (total_frames == 0) {
    finish_channel(channel, 1);
    return;
  }

  while (slot->frame_pos >= total_frames) {
    if (slot->loops_left == 0) {
      finish_channel(channel, 1);
      return;
    }

    if (slot->loops_left > 0) slot->loops_left--;
    slot->frame_pos = 0;
    slot->rate_accum = 0;
  }

  for (int c = 0; c < device.channels; c++) {
    samples[c] = chunk_output_sample(chunk, slot->frame_pos, c);
  }

  int effective_volume = (slot->volume * chunk->volume) / MIX_MAX_VOLUME;
  for (int c = 0; c < device.channels; c++) {
    mix_sample(frame, c, samples[c], effective_volume);
  }

  slot->frame_pos += advance_source_frame(&slot->rate_accum, chunk->frequency);
}

static void mixer_callback(void *userdata, uint8_t *stream, int len)
{
  (void)userdata;

  int frame_bytes = device.channels * device_bytes_per_sample;
  if (!audio_opened || frame_bytes <= 0) {
    memset(stream, 0, len);
    return;
  }

  memset(stream, device.format == AUDIO_U8 ? 128 : 0, len);

  int frames = len / frame_bytes;
  for (int i = 0; i < frames; i++) {
    uint8_t *frame = stream + i * frame_bytes;
    mix_music_frame(frame);

    for (int c = 0; c < mix_channel_count; c++) {
      mix_channel_frame(c, frame);
    }
  }
}

static void stop_all_channels(int call_hook)
{
  for (int i = 0; i < mix_channel_count; i++) {
    finish_channel(i, call_hook);
  }
}

static int allocate_channels_locked(int numchans)
{
  if (numchans < 0) return mix_channel_count;

  MixerChannel *next = NULL;
  if (numchans > 0) {
    next = (MixerChannel *)calloc((size_t)numchans, sizeof(*next));
    if (next == NULL) {
      set_error("Out of memory allocating mixer channels");
      return -1;
    }
  }

  int copy = mix_channel_count < numchans ? mix_channel_count : numchans;
  for (int i = 0; i < copy; i++) {
    next[i] = mix_channels[i];
  }

  for (int i = copy; i < numchans; i++) {
    next[i].volume = MIX_MAX_VOLUME;
  }

  for (int i = numchans; i < mix_channel_count; i++) {
    finish_channel(i, 1);
  }

  free(mix_channels);
  mix_channels = next;
  mix_channel_count = numchans;
  return mix_channel_count;
}

static uint8_t *read_file_data(const char *file, int *out_len)
{
  FILE *fp = fopen(file, "rb");
  if (fp == NULL) {
    set_error("Could not open file");
    return NULL;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    set_error("Could not seek file");
    return NULL;
  }

  long len = ftell(fp);
  if (len <= 0 || len > INT_MAX) {
    fclose(fp);
    set_error("Unsupported file size");
    return NULL;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    set_error("Could not rewind file");
    return NULL;
  }

  uint8_t *data = (uint8_t *)malloc((size_t)len);
  if (data == NULL) {
    fclose(fp);
    set_error("Out of memory reading file");
    return NULL;
  }

  if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
    free(data);
    fclose(fp);
    set_error("Could not read file");
    return NULL;
  }

  fclose(fp);
  *out_len = (int)len;
  return data;
}

static uint8_t *read_rwops_data(SDL_RWops *src, int *out_len)
{
  if (src == NULL || src->read == NULL || out_len == NULL) {
    set_error("Invalid SDL_RWops");
    return NULL;
  }

  *out_len = 0;

  int64_t start = 0;
  if (src->seek != NULL) {
    start = src->seek(src, 0, RW_SEEK_CUR);
    if (start < 0) start = 0;
  }

  int64_t total = -1;
  if (src->size != NULL) {
    total = src->size(src);
  }

  if (total >= start && total <= INT_MAX) {
    int wanted = (int)(total - start);
    uint8_t *data = (uint8_t *)malloc(wanted > 0 ? (size_t)wanted : 1);
    if (data == NULL) {
      set_error("Out of memory reading SDL_RWops");
      return NULL;
    }

    int got = 0;
    while (got < wanted) {
      size_t n = src->read(src, data + got, 1, (size_t)(wanted - got));
      if (n == 0) break;
      got += (int)n;
    }

    if (got != wanted) {
      free(data);
      set_error("Short read from SDL_RWops");
      return NULL;
    }

    *out_len = wanted;
    return data;
  }

  int cap = 4096;
  int len = 0;
  uint8_t *data = (uint8_t *)malloc((size_t)cap);
  if (data == NULL) {
    set_error("Out of memory reading SDL_RWops");
    return NULL;
  }

  for (;;) {
    if (len == cap) {
      int next_cap = cap * 2;
      uint8_t *next = (uint8_t *)realloc(data, (size_t)next_cap);
      if (next == NULL) {
        free(data);
        set_error("Out of memory growing SDL_RWops buffer");
        return NULL;
      }
      data = next;
      cap = next_cap;
    }

    size_t n = src->read(src, data + len, 1, (size_t)(cap - len));
    if (n == 0) break;
    len += (int)n;
  }

  *out_len = len;
  return data;
}

static Mix_Music *open_music_from_memory(uint8_t *data, int len)
{
  int error = 0;
  stb_vorbis *vorbis = stb_vorbis_open_memory(data, len, &error, NULL);
  if (vorbis == NULL) {
    free(data);
    set_error("Could not decode Ogg Vorbis music");
    return NULL;
  }

  Mix_Music *music = (Mix_Music *)calloc(1, sizeof(*music));
  if (music == NULL) {
    stb_vorbis_close(vorbis);
    free(data);
    set_error("Out of memory allocating music");
    return NULL;
  }

  stb_vorbis_info info = stb_vorbis_get_info(vorbis);
  music->encoded = data;
  music->encoded_len = len;
  music->vorbis = vorbis;
  music->frequency = (int)info.sample_rate;
  music->channels = info.channels;
  return music;
}

static Mix_Chunk *load_wav_from_memory(const uint8_t *data, int len)
{
  if (data == NULL || len < 44 || memcmp(data, "RIFF", 4) != 0 ||
      memcmp(data + 8, "WAVE", 4) != 0) {
    set_error("Invalid WAV file");
    return NULL;
  }

  int have_fmt = 0;
  int have_data = 0;
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t frequency = 0;
  uint16_t bits = 0;
  const uint8_t *pcm = NULL;
  uint32_t pcm_len = 0;
  int pos = 12;

  while (pos + 8 <= len) {
    const uint8_t *chunk = data + pos;
    uint32_t chunk_len = read_u32le(chunk + 4);
    pos += 8;

    if (chunk_len > (uint32_t)(len - pos)) {
      set_error("Truncated WAV chunk");
      return NULL;
    }

    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (chunk_len < 16) {
        set_error("Short WAV format chunk");
        return NULL;
      }

      audio_format = read_u16le(data + pos);
      channels = read_u16le(data + pos + 2);
      frequency = read_u32le(data + pos + 4);
      bits = read_u16le(data + pos + 14);
      have_fmt = 1;
    } else if (memcmp(chunk, "data", 4) == 0) {
      pcm = data + pos;
      pcm_len = chunk_len;
      have_data = 1;
    }

    pos += (int)chunk_len;
    if (chunk_len & 1) pos++;
  }

  if (!have_fmt || !have_data || pcm == NULL) {
    set_error("Incomplete WAV file");
    return NULL;
  }

  if (audio_format != 1 || frequency == 0 || channels == 0 ||
      channels > MAX_OUTPUT_CHANNELS || (bits != 8 && bits != 16)) {
    set_error("Unsupported WAV format");
    return NULL;
  }

  uint16_t format = bits == 8 ? AUDIO_U8 : AUDIO_S16SYS;
  int frame_bytes = channels * bytes_per_sample(format);
  pcm_len = (pcm_len / (uint32_t)frame_bytes) * (uint32_t)frame_bytes;

  Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(*chunk));
  if (chunk == NULL) {
    set_error("Out of memory allocating WAV chunk");
    return NULL;
  }

  chunk->abuf = (Uint8 *)malloc(pcm_len > 0 ? (size_t)pcm_len : 1);
  if (chunk->abuf == NULL) {
    free(chunk);
    set_error("Out of memory copying WAV data");
    return NULL;
  }

  if (pcm_len > 0) memcpy(chunk->abuf, pcm, pcm_len);
  chunk->allocated = 1;
  chunk->alen = pcm_len;
  chunk->volume = MIX_MAX_VOLUME;
  chunk->frequency = (int)frequency;
  chunk->format = format;
  chunk->channels = (Uint8)channels;
  return chunk;
}

// General

int Mix_OpenAudio(int frequency, uint16_t format, int channels, int chunksize)
{
  if (frequency <= 0) frequency = 22050;
  if (chunksize <= 0) chunksize = DEFAULT_AUDIOBUF;

  if (channels < 1 || channels > MAX_OUTPUT_CHANNELS) {
    set_error("Only mono and stereo audio are supported");
    return -1;
  }

  int bps = bytes_per_sample(format);
  if (bps == 0) {
    set_error("Unsupported audio format");
    return -1;
  }

  if (audio_opened) {
    return 0;
  }

  memset(&device, 0, sizeof(device));
  device.freq = frequency;
  device.format = format;
  device.channels = (uint8_t)channels;
  device.samples = (uint16_t)chunksize;
  device.callback = mixer_callback;

  SDL_AudioSpec obtained;
  memset(&obtained, 0, sizeof(obtained));
  if (SDL_OpenAudio(&device, &obtained) != 0) {
    set_error("SDL_OpenAudio failed");
    memset(&device, 0, sizeof(device));
    return -1;
  }

  device = obtained;
  device.callback = mixer_callback;
  device_bytes_per_sample = bytes_per_sample(device.format);
  if (device_bytes_per_sample == 0 || device.channels < 1 ||
      device.channels > MAX_OUTPUT_CHANNELS) {
    SDL_CloseAudio();
    memset(&device, 0, sizeof(device));
    set_error("SDL returned unsupported audio format");
    return -1;
  }

  device.size = device.samples * device.channels * device_bytes_per_sample;
  audio_opened = 1;

  if (mix_channel_count == 0 && allocate_channels_locked(DEFAULT_CHANNELS) < 0) {
    SDL_CloseAudio();
    audio_opened = 0;
    memset(&device, 0, sizeof(device));
    return -1;
  }

  SDL_PauseAudio(0);
  return 0;
}

void Mix_CloseAudio()
{
  if (!audio_opened) return;

  SDL_LockAudio();
  finish_music(0);
  stop_all_channels(0);
  SDL_UnlockAudio();

  SDL_CloseAudio();
  audio_opened = 0;
  device_bytes_per_sample = 0;
  memset(&device, 0, sizeof(device));

  free(mix_channels);
  mix_channels = NULL;
  mix_channel_count = 0;
}

char *Mix_GetError()
{
  return mixer_error;
}

int Mix_QuerySpec(int *frequency, uint16_t *format, int *channels)
{
  if (!audio_opened) return 0;

  if (frequency != NULL) *frequency = device.freq;
  if (format != NULL) *format = device.format;
  if (channels != NULL) *channels = device.channels;
  return 1;
}

// Samples

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
  int len = 0;
  uint8_t *data = read_rwops_data(src, &len);

  if (freesrc && src != NULL && src->close != NULL) {
    src->close(src);
  }

  if (data == NULL) return NULL;

  Mix_Chunk *chunk = load_wav_from_memory(data, len);
  free(data);
  return chunk;
}

void Mix_FreeChunk(Mix_Chunk *chunk)
{
  if (chunk == NULL) return;

  SDL_LockAudio();
  for (int i = 0; i < mix_channel_count; i++) {
    if (mix_channels[i].chunk == chunk) {
      finish_channel(i, 0);
    }
  }
  SDL_UnlockAudio();

  free(chunk->abuf);
  free(chunk);
}


// Channels

int Mix_AllocateChannels(int numchans)
{
  SDL_LockAudio();
  int result = allocate_channels_locked(numchans);
  SDL_UnlockAudio();
  return result;
}

int Mix_Volume(int channel, int volume)
{
  volume = clamp_volume(volume);

  if (channel == -1) {
    if (mix_channel_count == 0) return 0;

    int total = 0;
    for (int i = 0; i < mix_channel_count; i++) {
      total += mix_channels[i].volume;
    }

    int previous = total / mix_channel_count;
    if (volume >= 0) {
      for (int i = 0; i < mix_channel_count; i++) {
        mix_channels[i].volume = volume;
      }
    }
    return previous;
  }

  if (channel < 0 || channel >= mix_channel_count) return -1;

  int previous = mix_channels[channel].volume;
  if (volume >= 0) mix_channels[channel].volume = volume;
  return previous;
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
  if (chunk == NULL) {
    set_error("Cannot play a null chunk");
    return -1;
  }

  if (mix_channel_count == 0 && Mix_AllocateChannels(DEFAULT_CHANNELS) < 0) {
    return -1;
  }

  SDL_LockAudio();

  int chosen = channel;
  if (channel == -1) {
    chosen = -1;
    for (int i = 0; i < mix_channel_count; i++) {
      if (!mix_channels[i].playing) {
        chosen = i;
        break;
      }
    }
  }

  if (chosen < 0 || chosen >= mix_channel_count) {
    SDL_UnlockAudio();
    set_error("No free mixer channel");
    return -1;
  }

  MixerChannel *slot = &mix_channels[chosen];
  slot->chunk = chunk;
  slot->playing = 1;
  slot->paused = 0;
  slot->loops_left = loops;
  slot->frame_pos = 0;
  slot->rate_accum = 0;
  if (slot->volume == 0) slot->volume = MIX_MAX_VOLUME;

  SDL_UnlockAudio();
  return chosen;
}

void Mix_Pause(int channel)
{
  SDL_LockAudio();
  if (channel == -1) {
    for (int i = 0; i < mix_channel_count; i++) {
      if (mix_channels[i].playing) mix_channels[i].paused = 1;
    }
  } else if (channel >= 0 && channel < mix_channel_count) {
    mix_channels[channel].paused = 1;
  }
  SDL_UnlockAudio();
}

void Mix_ChannelFinished(void (*channel_finished)(int channel))
{
  channel_finished_hook = channel_finished;
}

// Music

Mix_Music *Mix_LoadMUS(const char *file)
{
  int len = 0;
  uint8_t *data = read_file_data(file, &len);
  if (data == NULL) return NULL;
  return open_music_from_memory(data, len);
}

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src)
{
  int len = 0;
  uint8_t *data = read_rwops_data(src, &len);
  if (data == NULL) return NULL;
  return open_music_from_memory(data, len);
}

void Mix_FreeMusic(Mix_Music *music)
{
  if (music == NULL) return;

  SDL_LockAudio();
  if (current_music == music) {
    finish_music(0);
  }
  SDL_UnlockAudio();

  if (music->vorbis != NULL) stb_vorbis_close(music->vorbis);
  free(music->encoded);
  free(music);
}

int Mix_PlayMusic(Mix_Music *music, int loops)
{
  if (music == NULL || music->vorbis == NULL) {
    set_error("Cannot play a null music object");
    return -1;
  }

  SDL_LockAudio();
  if (!stb_vorbis_seek_start(music->vorbis)) {
    SDL_UnlockAudio();
    set_error("Could not rewind music");
    return -1;
  }

  current_music = music;
  music->playing = 1;
  music->loops_left = loops;
  music->rate_accum = 0;
  music->decoded_frames = 0;
  music->decoded_pos = 0;
  SDL_UnlockAudio();
  return 0;
}

int Mix_SetMusicPosition(double position)
{
  if (current_music == NULL || current_music->vorbis == NULL || position < 0.0) {
    set_error("No active music to seek");
    return -1;
  }

  unsigned int sample = (unsigned int)(position * current_music->frequency);

  SDL_LockAudio();
  int ok = stb_vorbis_seek(current_music->vorbis, sample);
  current_music->decoded_frames = 0;
  current_music->decoded_pos = 0;
  current_music->rate_accum = 0;
  SDL_UnlockAudio();

  if (!ok) {
    set_error("Could not seek music");
    return -1;
  }

  return 0;
}

int Mix_VolumeMusic(int volume)
{
  int previous = music_volume;
  volume = clamp_volume(volume);
  if (volume >= 0) music_volume = volume;
  return previous;
}

int Mix_SetMusicCMD(const char *command)
{
  if (command == NULL || command[0] == '\0') return 0;
  set_error("External music commands are not supported");
  return -1;
}

int Mix_HaltMusic()
{
  SDL_LockAudio();
  finish_music(1);
  SDL_UnlockAudio();
  return 0;
}

void Mix_HookMusicFinished(void (*music_finished)())
{
  music_finished_hook = music_finished;
}

int Mix_PlayingMusic()
{
  return current_music != NULL && current_music->playing;
}
