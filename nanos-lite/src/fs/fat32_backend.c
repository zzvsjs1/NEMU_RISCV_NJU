#include "backend.h"
#include "fat32.h"

#ifndef SEEK_SET
enum {SEEK_SET, SEEK_CUR, SEEK_END};
#endif

static Fat32File to_fat32_file(const FsFile *file)
{
  Fat32File fat_file;

  fat_file.first_cluster = file->u.fat32.first_cluster;
  fat_file.size = (uint32_t)file->size;
  fat_file.cached_cluster_index = file->u.fat32.cached_cluster_index;
  fat_file.cached_cluster = file->u.fat32.cached_cluster;
  return fat_file;
}

static void from_fat32_file(FsFile *file, const Fat32File *fat_file)
{
  file->size = fat_file->size;
  file->u.fat32.first_cluster = fat_file->first_cluster;
  file->u.fat32.cached_cluster_index = fat_file->cached_cluster_index;
  file->u.fat32.cached_cluster = fat_file->cached_cluster;
}

static int fat32_fs_init(void)
{
  return fat32_backend_init();
}

static int fat32_fs_open(const char *path, FsFile *out)
{
  Fat32File fat_file;

  if (fat32_backend_open(path, &fat_file) != 0) {
    return -1;
  }

  from_fat32_file(out, &fat_file);
  return 0;
}

static size_t fat32_fs_read(FsFile *file, size_t offset, void *buf, size_t len)
{
  Fat32File fat_file = to_fat32_file(file);
  const size_t ret = fat32_backend_read(&fat_file, offset, buf, len);

  from_fat32_file(file, &fat_file);
  return ret;
}

static size_t fat32_fs_write(FsFile *file, size_t offset, const void *buf, size_t len)
{
  (void)file;
  (void)offset;
  (void)buf;
  (void)len;

  /* FAT32 support is intentionally read-only.  Return zero so fs_write() leaves
   * the open-file offset unchanged and no disk block is modified.
   */
  return 0;
}

static size_t fat32_fs_lseek(FsFile *file, size_t current_offset, size_t offset, int whence)
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
      panic("fat32_fs_lseek: invalid whence");
      return (size_t)-1;
    }
  }

  assert(new_offset <= file->size);
  return new_offset;
}

static int fat32_fs_close(FsFile *file)
{
  Fat32File fat_file = to_fat32_file(file);
  const int ret = fat32_backend_close(&fat_file);

  from_fat32_file(file, &fat_file);
  return ret;
}

const FsBackend regular_fs_backend = {
  .init = fat32_fs_init,
  .open = fat32_fs_open,
  .read = fat32_fs_read,
  .write = fat32_fs_write,
  .lseek = fat32_fs_lseek,
  .close = fat32_fs_close,
};
