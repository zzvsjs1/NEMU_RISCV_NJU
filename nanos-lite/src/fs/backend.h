#ifndef NANOS_LITE_FS_BACKEND_H
#define NANOS_LITE_FS_BACKEND_H

#include <common.h>

typedef struct {
  size_t size;
  union {
    struct {
      size_t disk_offset;
    } flat;
    struct {
      uint32_t first_cluster;
      uint32_t cached_cluster_index;
      uint32_t cached_cluster;
    } fat32;
  } u;
} FsFile;

typedef struct {
  int (*init)(void);
  int (*open)(const char *path, FsFile *out);
  size_t (*read)(FsFile *file, size_t offset, void *buf, size_t len);
  size_t (*write)(FsFile *file, size_t offset, const void *buf, size_t len);
  size_t (*lseek)(FsFile *file, size_t current_offset, size_t offset, int whence);
  int (*close)(FsFile *file);
} FsBackend;

extern const FsBackend regular_fs_backend;

#endif
