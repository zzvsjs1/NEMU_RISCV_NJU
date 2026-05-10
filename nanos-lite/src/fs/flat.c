#include "backend.h"

#ifndef SEEK_SET
enum {SEEK_SET, SEEK_CUR, SEEK_END};
#endif

// disk.c
size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

typedef struct {
  const char *name;
  size_t size;
  size_t disk_offset;
} FlatEntry;

/* This is the generated information about all regular files in the ramdisk. */
static const FlatEntry flat_entries[] __attribute__((used)) = {
#include "../files.h"
};

enum { NR_FLAT_ENTRIES = sizeof(flat_entries) / sizeof(flat_entries[0]) };

static size_t min_size(size_t a, size_t b)
{
  return a < b ? a : b;
}

static int flat_init(void)
{
  return 0;
}

static int flat_open(const char *path, FsFile *out)
{
  /*
   * navy-apps/Makefile writes files.h from `find ... | sort`, so every normal
   * ramdisk pathname is lexicographically ordered.  ONScripter opens many
   * PNG/archive paths while changing scenes; binary search cuts that path
   * lookup from about 1,600 string comparisons to about 11 in the current large
   * game image.
   */
  int left = 0;
  int right = NR_FLAT_ENTRIES;

  while (left < right) {
    const int mid = left + (right - left) / 2;
    const int cmp = strcmp(path, flat_entries[mid].name);

    if (cmp == 0) {
      out->size = flat_entries[mid].size;
      out->u.flat.disk_offset = flat_entries[mid].disk_offset;
      return 0;
    }

    if (cmp < 0) {
      right = mid;
    } else {
      left = mid + 1;
    }
  }

  return -1;
}

static size_t flat_read(FsFile *file, size_t offset, void *buf, size_t len)
{
  if (offset >= file->size) {
    return 0;
  }

  const size_t rlen = min_size(len, file->size - offset);
  return disk_read(buf, file->u.flat.disk_offset + offset, rlen);
}

static size_t flat_write(FsFile *file, size_t offset, const void *buf, size_t len)
{
  assert(offset <= file->size);

  const size_t wlen = min_size(len, file->size - offset);
  return disk_write(buf, file->u.flat.disk_offset + offset, wlen);
}

static size_t flat_lseek(FsFile *file, size_t current_offset, size_t offset, int whence)
{
  size_t new_offset = -1;

  switch (whence) {
    case SEEK_SET: {
      new_offset = offset;
      break;
    }
    case SEEK_CUR: {
      new_offset = current_offset + offset;
      break;
    }
    case SEEK_END: {
      new_offset = file->size + offset;
      break;
    }
    default: {
      panic("flat_lseek: invalid whence");
      return (size_t)-1;
    }
  }

  assert(new_offset <= file->size);
  return new_offset;
}

static int flat_close(FsFile *file)
{
  (void)file;
  return 0;
}

const FsBackend regular_fs_backend = {
  .init = flat_init,
  .open = flat_open,
  .read = flat_read,
  .write = flat_write,
  .lseek = flat_lseek,
  .close = flat_close,
};
