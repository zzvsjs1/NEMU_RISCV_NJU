#include <sdl-file.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int64_t file_size(SDL_RWops *rw)
{
    long old = ftell(rw->fp);

    if (old < 0)
        return -1;

    if (fseek(rw->fp, 0, SEEK_END) != 0)
        return -1;
    long end = ftell(rw->fp);

    if (end < 0)
        return -1;

    if (fseek(rw->fp, old, SEEK_SET) != 0)
        return -1;
    return end;
}

static int64_t file_seek(SDL_RWops *rw, int64_t offset, int whence)
{
    if (fseek(rw->fp, (long)offset, whence) != 0)
        return -1;
    return ftell(rw->fp);
}

static size_t file_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
    return fread(buf, size, nmemb, rw->fp);
}

static size_t file_write(SDL_RWops *rw, const void *buf, size_t size, size_t nmemb)
{
    return fwrite(buf, size, nmemb, rw->fp);
}

static int file_close(SDL_RWops *rw)
{
    int ret = 0;

    if (rw->fp != NULL)
        ret = fclose(rw->fp);
    free(rw);
    return ret;
}

static int64_t mem_size(SDL_RWops *rw)
{
    return rw->mem.size;
}

static int64_t mem_seek(SDL_RWops *rw, int64_t offset, int whence)
{
    int64_t next = 0;

    switch (whence)
    {
    case RW_SEEK_SET:
        next = offset;
        break;
    case RW_SEEK_CUR:
        next = rw->mem.offset + offset;
        break;
    case RW_SEEK_END:
        next = rw->mem.size + offset;
        break;
    default:
        return -1;
    }

    if (next < 0 || next > rw->mem.size)
        return -1;
    rw->mem.offset = next;
    return rw->mem.offset;
}

static size_t mem_read(SDL_RWops *rw, void *buf, size_t size, size_t nmemb)
{
    if (size == 0 || nmemb == 0)
        return 0;

    const size_t want = size * nmemb;
    const size_t left = (size_t)(rw->mem.size - rw->mem.offset);
    const size_t full_objects = (left < want ? left : want) / size;
    const size_t bytes = full_objects * size;

    /*
   * SDL_RWread(), like fread(), reports complete objects.  Copy only the
   * complete object bytes so callers never observe a partial element as a
   * successful read.
   */

    if (bytes > 0)
    {
        memcpy(buf, rw->mem.base + rw->mem.offset, bytes);
        rw->mem.offset += bytes;
    }

    return full_objects;
}

static size_t mem_write(SDL_RWops *rw, const void *buf, size_t size, size_t nmemb)
{
    if (size == 0 || nmemb == 0)
        return 0;

    const size_t want = size * nmemb;
    const size_t left = (size_t)(rw->mem.size - rw->mem.offset);
    const size_t full_objects = (left < want ? left : want) / size;
    const size_t bytes = full_objects * size;

    if (bytes > 0)
    {
        memcpy(rw->mem.base + rw->mem.offset, buf, bytes);
        rw->mem.offset += bytes;
    }

    return full_objects;
}

static int mem_close(SDL_RWops *rw)
{
    free(rw);
    return 0;
}

SDL_RWops *SDL_RWFromFile(const char *filename, const char *mode)
{
    FILE *fp = fopen(filename, mode);

    if (fp == NULL)
        return NULL;

    SDL_RWops *rw = calloc(1, sizeof(*rw));
    assert(rw != NULL);

    rw->size = file_size;
    rw->seek = file_seek;
    rw->read = file_read;
    rw->write = file_write;
    rw->close = file_close;
    rw->type = RW_TYPE_FILE;
    rw->fp = fp;
    return rw;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    if (mem == NULL || size < 0)
        return NULL;

    SDL_RWops *rw = calloc(1, sizeof(*rw));
    assert(rw != NULL);

    rw->size = mem_size;
    rw->seek = mem_seek;
    rw->read = mem_read;
    rw->write = mem_write;
    rw->close = mem_close;
    rw->type = RW_TYPE_MEM;
    rw->mem.base = mem;
    rw->mem.size = size;
    rw->mem.offset = 0;
    return rw;
}
