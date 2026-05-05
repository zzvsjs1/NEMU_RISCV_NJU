#include <common.h>

size_t ramdisk_read(void *buf, size_t offset, size_t len);
size_t ramdisk_write(const void *buf, size_t offset, size_t len);
size_t get_ramdisk_size(void);

/*
 * Keep one kernel-owned DMA bounce buffer for disk I/O.  User buffers can be
 * virtual addresses that NEMU's simple disk device cannot translate, so Nanos
 * copies through this physical buffer.  A larger buffer is worthwhile for
 * ONScripter: switching images often reads hundreds of KiB from the ramdisk,
 * and each extra chunk costs several MMIO register writes plus one host-side
 * disk command.
 */
#define DISK_BLOCK_BUF_SIZE (128 * 1024)

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

static void disk_transfer_blocks(bool write, size_t blkno, size_t blkcnt)
{
  assert(blkcnt > 0);
  assert(blkcnt * (size_t)disk_cfg.blksz <= DISK_BLOCK_BUF_SIZE);

  /*
   * NEMU receives this pointer as a guest physical address and translates it
   * directly into host PMEM.  Keep it as a kernel static buffer: syscall
   * buffers may be userspace virtual addresses, which the device cannot walk.
   */
  io_write(AM_DISK_BLKIO,
      .write = write,
      .buf = disk_block_buf,
      .blkno = (int)blkno,
      .blkcnt = (int)blkcnt);
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

    if (blkoff == 0) {
      /*
       * Read as many consecutive whole blocks as the bounce buffer can hold.
       * The AM/NEMU disk interface already supports multi-block transfers, so
       * this cuts MMIO traffic and host fseek/fread calls for large assets.
       */
      const size_t max_blocks = DISK_BLOCK_BUF_SIZE / blksz;
      const size_t needed_blocks = (len - done + blksz - 1) / blksz;
      const size_t disk_blocks_left = (size_t)disk_cfg.blkcnt - blkno;
      const size_t blkcnt = min_size(min_size(needed_blocks, max_blocks), disk_blocks_left);
      const size_t chunk = min_size(len - done, blkcnt * blksz);

      disk_transfer_blocks(false, blkno, blkcnt);
      memcpy(out + done, disk_block_buf, chunk);
      done += chunk;
      continue;
    }

    const size_t chunk = min_size(len - done, blksz - blkoff);

    disk_transfer_blocks(false, blkno, 1);
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
      disk_transfer_blocks(false, blkno, 1);
    }

    memcpy(disk_block_buf + blkoff, in + done, chunk);
    disk_transfer_blocks(true, blkno, 1);

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
