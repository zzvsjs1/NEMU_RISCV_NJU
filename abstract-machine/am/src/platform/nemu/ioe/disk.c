#include <am.h>
#include <nemu.h>

enum {
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

#define DISK_REG(offset) (DISK_ADDR + (offset) * sizeof(uint32_t))
#define DISK_CMD_GO 1u

void __am_disk_config(AM_DISK_CONFIG_T *cfg) {
  cfg->present = inl(DISK_REG(reg_present)) != 0;
  cfg->blksz = cfg->present ? (int)inl(DISK_REG(reg_blksz)) : 0;
  cfg->blkcnt = cfg->present ? (int)inl(DISK_REG(reg_blkcnt)) : 0;
}

void __am_disk_status(AM_DISK_STATUS_T *stat) {
  stat->ready = inl(DISK_REG(reg_ready)) != 0;
}

void __am_disk_blkio(AM_DISK_BLKIO_T *io) {
  if (io->blkcnt <= 0) {
    return;
  }

  while (inl(DISK_REG(reg_ready)) == 0) {
  }

  outl(DISK_REG(reg_write), io->write ? 1 : 0);
  outl(DISK_REG(reg_buf), (uintptr_t)io->buf);
  outl(DISK_REG(reg_blkno), (uint32_t)io->blkno);
  outl(DISK_REG(reg_io_blkcnt), (uint32_t)io->blkcnt);
  outl(DISK_REG(reg_cmd), DISK_CMD_GO);

  while (inl(DISK_REG(reg_ready)) == 0) {
  }
}
