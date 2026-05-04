#include <common.h>

size_t ramdisk_read(void *buf, size_t offset, size_t len);
size_t ramdisk_write(const void *buf, size_t offset, size_t len);
size_t get_ramdisk_size(void);

#define DISK_BLOCK_BUF_SIZE 4096

static AM_DISK_CONFIG_T disk_cfg;
static bool disk_present = false;
static uint8_t disk_block_buf[DISK_BLOCK_BUF_SIZE] __attribute__((aligned(4)));

static size_t disk_size(void)
{
  return (size_t)disk_cfg.blksz * (size_t)disk_cfg.blkcnt;
}

static size_t min_size(size_t a, size_t b)
{
  return a < b ? a : b;
}

static void check_disk_range(size_t offset, size_t len)
{
  const size_t size = disk_size();

  assert(offset <= size);
  assert(len <= size - offset);
}

static void disk_transfer_block(bool write, size_t blkno)
{
  /*
   * NEMU receives this pointer as a guest physical address and translates it
   * directly into host PMEM.  Keep it as a kernel static buffer: syscall
   * buffers may be userspace virtual addresses, which the device cannot walk.
   */
  io_write(AM_DISK_BLKIO,
      .write = write,
      .buf = disk_block_buf,
      .blkno = (int)blkno,
      .blkcnt = 1);
}

size_t disk_read(void *buf, size_t offset, size_t len)
{
  if (!disk_present) {
    return ramdisk_read(buf, offset, len);
  }

  check_disk_range(offset, len);

  uint8_t *out = (uint8_t *)buf;
  size_t done = 0;
  const size_t blksz = (size_t)disk_cfg.blksz;

  while (done < len) {
    const size_t cur = offset + done;
    const size_t blkno = cur / blksz;
    const size_t blkoff = cur % blksz;
    const size_t chunk = min_size(len - done, blksz - blkoff);

    disk_transfer_block(false, blkno);
    memcpy(out + done, disk_block_buf + blkoff, chunk);

    done += chunk;
  }

  return len;
}

size_t disk_write(const void *buf, size_t offset, size_t len)
{
  if (!disk_present) {
    return ramdisk_write(buf, offset, len);
  }

  check_disk_range(offset, len);

  const uint8_t *in = (const uint8_t *)buf;
  size_t done = 0;
  const size_t blksz = (size_t)disk_cfg.blksz;

  while (done < len) {
    const size_t cur = offset + done;
    const size_t blkno = cur / blksz;
    const size_t blkoff = cur % blksz;
    const size_t chunk = min_size(len - done, blksz - blkoff);

    if (blkoff != 0 || chunk != blksz) {
      /*
       * Preserve the bytes in this block that are outside the caller's range.
       * Full aligned block writes can skip the read because the whole buffer
       * is overwritten before the write command.
       */
      disk_transfer_block(false, blkno);
    }

    memcpy(disk_block_buf + blkoff, in + done, chunk);
    disk_transfer_block(true, blkno);

    done += chunk;
  }

  return len;
}

void init_disk(void)
{
  disk_cfg = io_read(AM_DISK_CONFIG);
  disk_present = disk_cfg.present && disk_cfg.blksz > 0 && disk_cfg.blkcnt > 0;

  if (!disk_present) {
    Log("disk absent, using embedded ramdisk fallback, size = %zu bytes",
        get_ramdisk_size());
    return;
  }

  assert(disk_cfg.blksz <= DISK_BLOCK_BUF_SIZE);
  Log("disk info: block size = %d bytes, blocks = %d, size = %zu bytes",
      disk_cfg.blksz, disk_cfg.blkcnt, disk_size());
}
