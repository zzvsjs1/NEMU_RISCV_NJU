#include <fs.h>
#include "fs/backend.h"

/*
 * Device-file read callback.  The offset is the descriptor's current special
 * file offset, not a backend regular-file offset.  Poll-like devices can ignore
 * it, while synthetic files such as /proc/dispinfo may use it if they later
 * grow seek-style behaviour.
 */
typedef size_t (*ReadFn) (void *buf, size_t offset, size_t len);
/*
 * Device-file write callback.  Stream devices may ignore offset; /dev/fb uses
 * it as a byte position inside the linear framebuffer.
 */
typedef size_t (*WriteFn) (const void *buf, size_t offset, size_t len);

// device.c
size_t serial_write(const void *buf, size_t offset, size_t len);
size_t events_read(void *buf, size_t offset, size_t len);
size_t dispinfo_read(void *buf, size_t offset, size_t len);
size_t fb_write(const void *buf, size_t offset, size_t len); 
size_t sb_write(const void *buf, size_t offset, size_t len);
size_t sbctl_write(const void *buf, size_t offset, size_t len);
size_t sbctl_read(void *buf, size_t offset, size_t len);


typedef struct {
  /*
   * Kernel-visible pathname.  Special files bypass the regular backend, so
   * these names are matched before any FAT32 or flat-image lookup.
   */
  const char *name;
  /*
   * Logical size for seek-bounded special files.  Stream-like devices keep this
   * as zero because their callbacks define the valid operation range.
   */
  size_t size;
  /* Callback used by fs_read() for this special descriptor. */
  ReadFn read;
  /* Callback used by fs_write() for this special descriptor. */
  WriteFn write;
} SpecialFile;

typedef struct {
  /*
   * Slot ownership flag.  Regular descriptors are allocated from open_files[];
   * special descriptors are stable enum values and do not use this table.
   */
  int used;
  /*
   * Current logical file offset for read/write/lseek.  fs.c advances this after
   * successful backend reads or writes, mirroring POSIX descriptor semantics.
   */
  size_t offset;
  /*
   * Backend-specific regular-file state.  For FAT32 this includes directory
   * metadata and cluster caches; for flat mode it is just size and disk offset.
   */
  FsFile file;
} OpenFile;

enum {
  FD_STDIN, 
  FD_STDOUT, 
  FD_STDERR, 
  FD_FB, 
  FD_EVENTS, 
  FD_DISPINFO, 
  FD_SB,
  FD_SBCTL
};

enum {
  FIRST_REGULAR_FD = FD_SBCTL + 1,
  MAX_OPEN_FILES = 128,
  MAX_REGULAR_OPEN_FILES = MAX_OPEN_FILES - FIRST_REGULAR_FD,
};

enum {
  FS_O_TRUNC = 0x0400,
};

size_t invalid_read(void *buf, size_t offset, size_t len) {
  panic("should not reach here");
  return 0;
}

size_t invalid_write(const void *buf, size_t offset, size_t len) {
  panic("should not reach here");
  return 0;
}

static SpecialFile special_files[] = {
  [FD_STDIN]  = {"stdin", 0, invalid_read, invalid_write},
  [FD_STDOUT] = {"stdout", 0, invalid_read, serial_write},
  [FD_STDERR] = {"stderr", 0, invalid_read, serial_write},
  [FD_FB] = {"/dev/fb", 0, invalid_read, fb_write},
  [FD_EVENTS] = {"/dev/events", 0, events_read, invalid_write},
  [FD_DISPINFO] = {"/proc/dispinfo", 0, dispinfo_read, invalid_write},
  [FD_SB] = {"/dev/sb", 0, invalid_read, sb_write},
  [FD_SBCTL] = {"/dev/sbctl", 0, sbctl_read, sbctl_write},
};

enum { NR_SPECIAL_FILES = sizeof(special_files) / sizeof(special_files[0]) };

static size_t special_offsets[NR_SPECIAL_FILES];
static OpenFile open_files[MAX_REGULAR_OPEN_FILES];

static int find_special_file(const char *pathname)
{
  for (int i = 0; i < NR_SPECIAL_FILES; i++) {
    if (strcmp(pathname, special_files[i].name) == 0) {
      return i;
    }
  }

  return -1;
}

static OpenFile *regular_file(int fd)
{
  assert(fd >= FIRST_REGULAR_FD && fd < MAX_OPEN_FILES);

  OpenFile *open = &open_files[fd - FIRST_REGULAR_FD];
  assert(open->used);
  return open;
}

void init_fs() 
{
  // Initialise the size of /dev/fb
  // The AM display contract reports this as a byte count for the linear framebuffer.
  const AM_GPU_CONFIG_T gpuConfig = io_read(AM_GPU_CONFIG);
  // Must present
  assert(gpuConfig.present);

  // Set file size.
  const int vmemsz = gpuConfig.vmemsz;
  special_files[FD_FB].size = (size_t)vmemsz;

  assert(regular_fs_backend.init() == 0);
}

// Open a file by pathname, returning a stable special fd or a reusable regular fd.
int fs_open(const char *pathname, int flags, int mode) 
{
  int fd = find_special_file(pathname);
  if (fd >= 0) {
    special_offsets[fd] = 0;
    return fd;
  }

  FsFile file;
  if (regular_fs_backend.open(pathname, &file) == 0) {
    if ((flags & FS_O_TRUNC) != 0 && regular_fs_backend.truncate(&file, 0) != 0) {
      regular_fs_backend.close(&file);
      return -1;
    }

    for (int i = 0; i < MAX_REGULAR_OPEN_FILES; i++) {
      if (!open_files[i].used) {
        open_files[i].used = 1;
        open_files[i].offset = 0;
        open_files[i].file = file;
        return FIRST_REGULAR_FD + i;
      }
    }

    regular_fs_backend.close(&file);
    return -1;
  }

  /*
   * Many applications probe optional files as normal control flow.  Logging
   * every miss is very expensive on NEMU because each character goes through
   * the emulated serial device.  Enable this macro only when debugging file
   * table generation or path translation.
   */
#ifdef CONFIG_TRACE_FS_OPEN_MISS
  Log("fs_open: Invalid pathname: %s", pathname);
#endif
  return -1;
}

// Read up to len bytes from file descriptor into buf
size_t fs_read(int fd, void *buf, size_t len) 
{
  assert(fd >= 0 && fd < MAX_OPEN_FILES);

  if (fd < FIRST_REGULAR_FD) {
    SpecialFile *f = &special_files[fd];
    /* Device files own their offset semantics.  For example /dev/events is
     * poll-like and /proc/dispinfo is synthetic, so the special descriptor
     * offset is intentionally not advanced on this path.
     */
    return f->read(buf, special_offsets[fd], len);
  }

  OpenFile *open = regular_file(fd);
  const size_t ret = regular_fs_backend.read(&open->file, open->offset, buf, len);
  open->offset += ret;
  return ret;
}

// Write up to len bytes from buf to file descriptor
size_t fs_write(int fd, const void *buf, size_t len) 
{
  assert(fd >= 0 && fd < MAX_OPEN_FILES);

  if (fd < FIRST_REGULAR_FD) {
    SpecialFile *f = &special_files[fd];
    /* Device writers receive the current open offset so stream-like devices can
     * ignore it while memory-mapped-style devices such as /dev/fb can translate
     * it into a screen position.  The offset is not advanced here because some
     * devices, especially /dev/sb, define their own stream semantics.
     */
    return f->write(buf, special_offsets[fd], len);
  }

  OpenFile *open = regular_file(fd);
  const size_t ret = regular_fs_backend.write(&open->file, open->offset, buf, len);
  open->offset += ret;
  return ret;
}

// Adjust the file offset based on whence (SEEK_SET, SEEK_CUR, SEEK_END)
size_t fs_lseek(int fd, size_t offset, int whence) 
{
  assert(fd >= 0 && fd < MAX_OPEN_FILES);

  if (fd >= FIRST_REGULAR_FD) {
    OpenFile *open = regular_file(fd);
    const size_t newOffset = regular_fs_backend.lseek(&open->file, open->offset, offset, whence);
    open->offset = newOffset;
    return newOffset;
  }

  const size_t old_offset = special_offsets[fd];
  const size_t file_size = special_files[fd].size;
  size_t newOffset = -1;

  switch (whence) {
    case SEEK_SET: {
      newOffset = offset;
      break;
    }
    case SEEK_CUR: {
      newOffset = old_offset + offset;
      break;
    }
    case SEEK_END: {
      newOffset = file_size + offset;
      break;
    }
    default: {
      panic("fs_lseek: invalid whence");
      return (size_t)-1;
    }
  }

  // Ensure the new offset is within bounds
  assert(newOffset <= file_size);

  if (fd < FIRST_REGULAR_FD) {
    special_offsets[fd] = newOffset;
  } else {
    regular_file(fd)->offset = newOffset;
  }
  return newOffset;
}

// Close a regular file descriptor and make the slot reusable.
int fs_close(int fd) 
{
  assert(fd >= 0 && fd < MAX_OPEN_FILES);

  if (fd < FIRST_REGULAR_FD) {
    return 0;
  }

  OpenFile *open = &open_files[fd - FIRST_REGULAR_FD];
  if (!open->used) {
    return 0;
  }

  const int ret = regular_fs_backend.close(&open->file);
  open->used = 0;
  open->offset = 0;
  return ret;
}
