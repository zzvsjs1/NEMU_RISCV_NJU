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

/*
 * AM disk geometry reported by NEMU.  The filesystem layer above this file uses
 * byte offsets, but the AM device transfers whole blocks, so every read/write
 * is translated through disk_cfg.blksz and disk_cfg.blkcnt.
 */
static AM_DISK_CONFIG_T disk_cfg;
/*
 * True only when NEMU exposes a usable disk.  If false, the same disk_read()
 * and disk_write() API falls back to the embedded ramdisk symbols, keeping old
 * images and host tests compatible with the disk-backed filesystem path.
 */
static bool disk_present = false;
/*
 * Kernel-owned physical bounce buffer used for all AM disk transfers.  It is
 * aligned for the device contract and deliberately static so user virtual
 * buffers never have to be translated by the simple NEMU disk controller.
 */
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

    /* The public disk_* API accepts byte ranges even though the AM device only
   * transfers whole blocks.  Validate before block arithmetic so the partial
   * first/last-block paths never wrap past the emulated image.
   */
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
    if (!disk_present)
    {
        /* Keep old ramdisk-only images working when NEMU has no external disk
     * device.  The filesystem layer does not need to know which backend won.
     */
        return ramdisk_read(buf, offset, len);
    }

    check_disk_range(offset, len);

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    const size_t blksz = (size_t)disk_cfg.blksz;

    while (done < len)
    {
        const size_t cur = offset + done;
        const size_t blkno = cur / blksz;
        const size_t blkoff = cur % blksz;

        if (blkoff == 0)
        {
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

        /*
     * The first unaligned piece must still read a whole disk block into the
     * bounce buffer, then copy only the requested tail.  This keeps the public
     * byte-addressed filesystem API while the AM/NEMU device remains block-based.
     */
        const size_t chunk = min_size(len - done, blksz - blkoff);

        disk_transfer_blocks(false, blkno, 1);
        memcpy(out + done, disk_block_buf + blkoff, chunk);

        done += chunk;
    }

    return len;
}

size_t disk_write(const void *buf, size_t offset, size_t len)
{
    if (!disk_present)
    {
        /* Writes follow the same fallback as reads so tests using the embedded
     * ramdisk still observe a coherent storage backend.
     */
        return ramdisk_write(buf, offset, len);
    }

    check_disk_range(offset, len);

    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    const size_t blksz = (size_t)disk_cfg.blksz;

    while (done < len)
    {
        const size_t cur = offset + done;
        const size_t blkno = cur / blksz;
        const size_t blkoff = cur % blksz;
        const size_t chunk = min_size(len - done, blksz - blkoff);

        if (blkoff != 0 || chunk != blksz)
        {
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

    if (!disk_present)
    {
        Log("disk absent, using embedded ramdisk fallback, size = %zu bytes",
            get_ramdisk_size());
        return;
    }

    assert(disk_cfg.blksz <= DISK_BLOCK_BUF_SIZE);
    /*
   * The buffer-size assertion is also a performance contract: if a future device
   * uses larger blocks than the bounce buffer, batching would silently collapse
   * back to one partial command or fail to preserve surrounding bytes on writes.
   */
    Log("disk info: block size = %d bytes, blocks = %d, size = %zu bytes",
        disk_cfg.blksz, disk_cfg.blkcnt, disk_size());
}
