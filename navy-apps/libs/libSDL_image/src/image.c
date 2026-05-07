#define SDL_malloc  malloc
#define SDL_free    free
#define SDL_realloc realloc

#include <limits.h>

#define SDL_STBIMAGE_IMPLEMENTATION
#include "SDL_stbimage.h"

SDL_Surface* IMG_Load_RW(SDL_RWops *src, int freesrc) 
{
  if (src == NULL) return NULL;

  int64_t size = SDL_RWsize(src);
  if (size <= 0) {
    if (freesrc) SDL_RWclose(src);
    return NULL;
  }

  if (src->type == RW_TYPE_MEM) {
    /*
     * Memory RWops already expose a contiguous byte range, so decode directly
     * from the current pointer instead of copying.  Seek to EOF afterwards to
     * match the "stream has been consumed" behaviour of the file-backed path.
     */
    int64_t pos = SDL_RWtell(src);
    if (pos < 0 || pos > size || size - pos > INT_MAX) {
      if (freesrc) SDL_RWclose(src);
      return NULL;
    }

    int len = (int)(size - pos);
    unsigned char *base = src->mem.base + pos;
    SDL_Surface *surface = STBIMG_LoadFromMemory(base, len);
    SDL_RWseek(src, size, RW_SEEK_SET);
    if (freesrc) SDL_RWclose(src);
    return surface;
  }

  uint8_t *buf = malloc((size_t)size);
  if (buf == NULL) {
    if (freesrc) SDL_RWclose(src);
    return NULL;
  }

  /*
   * Decode from the current stream position, matching SDL_image's RWops
   * contract. ONScripter leaves memory streams at offset zero before loading.
   */
  size_t got = SDL_RWread(src, buf, 1, (size_t)size);
  SDL_Surface *surface = NULL;
  if (got == (size_t)size) {
    surface = STBIMG_LoadFromMemory(buf, (int)size);
  }

  free(buf);
  if (freesrc) SDL_RWclose(src);
  return surface;
}

SDL_Surface* IMG_Load(const char *filename) 
{
  if (!filename) 
  {
    printf("IMG_Load: filename is NULL");
    return NULL;
  }

  /*
   * IMG_Load is just a file convenience wrapper around STB's memory decoder.
   * Reading the whole file first keeps all image format handling inside
   * SDL_stbimage.h and avoids maintaining separate PNG/JPEG file paths here.
   */
  // 1. Open the file in binary mode
  FILE *fp = fopen(filename, "rb");
  if (!fp) 
  {
      printf("IMG_Load: could not open '%s'\n", filename);
      return NULL;
  }

  // 2. Determine file size
  if (fseek(fp, 0, SEEK_END) != 0) 
  {
      printf("IMG_Load: fseek failed");
      fclose(fp);
      return NULL;
  }

  long size = ftell(fp);
  if (size < 0) 
  {
    printf("IMG_Load: ftell failed");
    fclose(fp);
    return NULL;
  }

  rewind(fp);

  // 3. Allocate temporary buffer
  unsigned char *buf = (unsigned char*)malloc((size_t)size);
  if (!buf) 
  {
    printf("IMG_Load: out of memory allocating %ld bytes", size);
    fclose(fp);
    return NULL;
  }

  // 4. Read file into buffer
  size_t read_bytes = fread(buf, 1, (size_t)size, fp);
  if (read_bytes != (size_t)size) 
  {
      printf("IMG_Load: read error (%zu of %ld bytes)", read_bytes, size);
      free(buf);
      fclose(fp);
      return NULL;
  }

  // 5. Decode image from memory
  //    STBIMG_LoadFromMemory takes ownership of buf on success,
  //    or you must free it yourself if it fails.
  SDL_Surface *surface = STBIMG_LoadFromMemory(buf, (size_t)size);
  if (!surface) 
  {
    // STBIMG_LoadFromMemory should call SDL_SetError on failure
    free(buf);
    return NULL;
  }

  // 6. Free the temporary buffer if STBIMG didn't consume it
  free(buf);

  // 7. Return the decoded surface
  return surface;
}



int IMG_isPNG(SDL_RWops *src) 
{
  if (src == NULL) return 0;

  /*
   * Format probes must not consume bytes.  Save and restore the RWops position
   * so a later IMG_Load_RW() sees the same stream contents.
   */
  static const uint8_t png_magic[8] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
  };
  uint8_t buf[8];
  int64_t pos = SDL_RWtell(src);
  if (pos < 0) return 0;

  size_t got = SDL_RWread(src, buf, 1, sizeof(buf));
  SDL_RWseek(src, pos, RW_SEEK_SET);
  return got == sizeof(buf) && memcmp(buf, png_magic, sizeof(buf)) == 0;
}

SDL_Surface* IMG_LoadJPG_RW(SDL_RWops *src) 
{
  return IMG_Load_RW(src, 0);
}

char *IMG_GetError() 
{
  return "Navy does not support IMG_GetError()";
}
