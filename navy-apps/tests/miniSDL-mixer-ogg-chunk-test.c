#include <SDL.h>
#include <SDL_mixer.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  SDL_RWops rw;
  const uint8_t *data;
  int64_t size;
  int64_t pos;
} MemoryRW;

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
  (void)desired;
  (void)obtained;
  return -1;
}

void SDL_CloseAudio(void) {}
void SDL_PauseAudio(int pause_on) { (void)pause_on; }
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}

static int64_t memory_size(SDL_RWops *rw)
{
  MemoryRW *ctx = (MemoryRW *)rw;
  return ctx->size;
}

static int64_t memory_seek(SDL_RWops *rw, int64_t offset, int whence)
{
  MemoryRW *ctx = (MemoryRW *)rw;
  int64_t base = 0;

  if (whence == RW_SEEK_CUR) {
    base = ctx->pos;
  } else if (whence == RW_SEEK_END) {
    base = ctx->size;
  } else {
    assert(whence == RW_SEEK_SET);
  }

  int64_t next = base + offset;
  if (next < 0) next = 0;
  if (next > ctx->size) next = ctx->size;
  ctx->pos = next;
  return ctx->pos;
}

static size_t memory_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
  MemoryRW *ctx = (MemoryRW *)rw;
  size_t bytes = size * nmemb;
  int64_t left = ctx->size - ctx->pos;

  if (size == 0 || nmemb == 0 || left <= 0) return 0;
  if ((int64_t)bytes > left) bytes = (size_t)left;

  memcpy(buf, ctx->data + ctx->pos, bytes);
  ctx->pos += (int64_t)bytes;
  return bytes / size;
}

static size_t memory_write(SDL_RWops *rw, const void *buf, size_t size, size_t nmemb)
{
  (void)rw;
  (void)buf;
  (void)size;
  (void)nmemb;
  return 0;
}

static int memory_close(SDL_RWops *rw)
{
  free(rw);
  return 0;
}

static SDL_RWops *open_memory_rw(const uint8_t *data, size_t size)
{
  MemoryRW *ctx = (MemoryRW *)calloc(1, sizeof(*ctx));
  assert(ctx != NULL);

  ctx->data = data;
  ctx->size = (int64_t)size;
  ctx->rw.size = memory_size;
  ctx->rw.seek = memory_seek;
  ctx->rw.read = memory_read;
  ctx->rw.write = memory_write;
  ctx->rw.close = memory_close;
  ctx->rw.type = RW_TYPE_MEM;
  return &ctx->rw;
}

static uint8_t *read_whole_file(const char *path, size_t *size)
{
  FILE *fp = fopen(path, "rb");
  assert(fp != NULL);
  assert(fseek(fp, 0, SEEK_END) == 0);
  long end = ftell(fp);
  assert(end > 0);
  assert(fseek(fp, 0, SEEK_SET) == 0);

  uint8_t *data = (uint8_t *)malloc((size_t)end);
  assert(data != NULL);
  assert(fread(data, (size_t)end, 1, fp) == 1);
  fclose(fp);

  *size = (size_t)end;
  return data;
}

#include "../libs/libSDL_mixer/src/mixer.c"

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "navy-apps/fsimg/share/music/rhythm/Do.ogg";
  size_t ogg_size = 0;
  uint8_t *ogg = read_whole_file(path, &ogg_size);

  SDL_RWops *rw = open_memory_rw(ogg, ogg_size);
  Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1);

  assert(chunk != NULL);
  assert(chunk->format == AUDIO_S16SYS);
  assert(chunk->channels >= 1 && chunk->channels <= 2);
  assert(chunk->frequency > 0);
  assert(chunk->alen > 0);

  Mix_FreeChunk(chunk);
  free(ogg);
  return 0;
}
