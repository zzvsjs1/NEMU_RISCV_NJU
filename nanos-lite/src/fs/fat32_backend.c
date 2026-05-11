#include "backend.h"
#include "fat32.h"

#ifndef SEEK_SET
enum {SEEK_SET, SEEK_CUR, SEEK_END};
#endif

/*
 * fs.c stores regular-file metadata through the backend-neutral FsFile type,
 * while the FAT32 implementation works with Fat32File.  Convert by value at
 * each call boundary so the generic descriptor table stays independent from
 * FAT32 headers, then copy back any changed size, cluster, or cache fields.
 */
static Fat32File to_fat32_file(const FsFile *file)
{
  Fat32File fat_file;

  fat_file.attr = file->u.fat32.attr;
  fat_file.first_cluster = file->u.fat32.first_cluster;
  fat_file.size = (uint32_t)file->size;
  fat_file.dir_entry_offset = file->u.fat32.dir_entry_offset;
  fat_file.cached_cluster_index = file->u.fat32.cached_cluster_index;
  fat_file.cached_cluster = file->u.fat32.cached_cluster;
  fat_file.contiguous_cluster_count = file->u.fat32.contiguous_cluster_count;
  return fat_file;
}

static void from_fat32_file(FsFile *file, const Fat32File *fat_file)
{
  file->size = fat_file->size;
  file->u.fat32.attr = fat_file->attr;
  file->u.fat32.first_cluster = fat_file->first_cluster;
  file->u.fat32.dir_entry_offset = fat_file->dir_entry_offset;
  file->u.fat32.cached_cluster_index = fat_file->cached_cluster_index;
  file->u.fat32.cached_cluster = fat_file->cached_cluster;
  file->u.fat32.contiguous_cluster_count = fat_file->contiguous_cluster_count;
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
  Fat32File fat_file = to_fat32_file(file);
  const size_t ret = fat32_backend_write(&fat_file, offset, buf, len);

  from_fat32_file(file, &fat_file);
  return ret;
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

  assert(new_offset != (size_t)-1);
  return new_offset;
}

static int fat32_fs_close(FsFile *file)
{
  Fat32File fat_file = to_fat32_file(file);
  const int ret = fat32_backend_close(&fat_file);

  from_fat32_file(file, &fat_file);
  return ret;
}

static int fat32_fs_truncate(FsFile *file, size_t size)
{
  Fat32File fat_file = to_fat32_file(file);
  int ret;

  if (size > UINT32_MAX) {
    return -1;
  }

  ret = fat32_backend_truncate(&fat_file, (uint32_t)size);
  from_fat32_file(file, &fat_file);
  return ret;
}

const FsBackend regular_fs_backend = {
  .init = fat32_fs_init,
  .open = fat32_fs_open,
  .read = fat32_fs_read,
  .write = fat32_fs_write,
  .lseek = fat32_fs_lseek,
  .truncate = fat32_fs_truncate,
  .close = fat32_fs_close,
};
