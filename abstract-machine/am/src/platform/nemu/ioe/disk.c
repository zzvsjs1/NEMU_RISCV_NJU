#include <am.h>
#include <nemu.h>

enum
{
    reg_present,
    reg_blksz,
    reg_blkcnt,
    reg_ready,
    reg_write,
    reg_buf,
    reg_blkno,
    reg_io_blkcnt,
    reg_cmd,
};

// The NEMU disk model exposes a compact MMIO register file at DISK_ADDR.
// These enum values are register indices, not byte offsets; DISK_REG() scales
// them to 32-bit addresses so the C side stays tied to the device contract.
#define DISK_REG(offset) (DISK_ADDR + (offset) * sizeof(uint32_t))
#define DISK_CMD_GO 1u

void __am_disk_config(AM_DISK_CONFIG_T *cfg)
{
    // AM reports an absent disk as a zero-sized block device. Callers should use
    // present before trusting blksz or blkcnt, matching the other AM config APIs.
    cfg->present = inl(DISK_REG(reg_present)) != 0;
    cfg->blksz = cfg->present ? (int)inl(DISK_REG(reg_blksz)) : 0;
    cfg->blkcnt = cfg->present ? (int)inl(DISK_REG(reg_blkcnt)) : 0;
}

void __am_disk_status(AM_DISK_STATUS_T *stat)
{
    stat->ready = inl(DISK_REG(reg_ready)) != 0;
}

void __am_disk_blkio(AM_DISK_BLKIO_T *io)
{
    // A non-positive request is treated as a no-op, so higher layers can pass
    // calculated lengths without needing an extra guard at every call site.
    if (io->blkcnt <= 0)
    {
        return;
    }

    // NEMU's disk command is synchronous from the AM point of view: wait for any
    // previous command, publish all request registers, issue GO, then wait until
    // the model has copied data to or from the guest buffer.
    while (inl(DISK_REG(reg_ready)) == 0)
    {
    }

    outl(DISK_REG(reg_write), io->write ? 1 : 0);
    outl(DISK_REG(reg_buf), (uintptr_t)io->buf);
    outl(DISK_REG(reg_blkno), (uint32_t)io->blkno);
    outl(DISK_REG(reg_io_blkcnt), (uint32_t)io->blkcnt);
    /*
   * blkcnt is part of the request, not a loop hint for AM.  Letting NEMU copy
   * several adjacent blocks per command is what makes the Nanos bounce-buffer
   * batching useful for large asset reads.
   */
    outl(DISK_REG(reg_cmd), DISK_CMD_GO);

    while (inl(DISK_REG(reg_ready)) == 0)
    {
    }
}
