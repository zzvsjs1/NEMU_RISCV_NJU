#include <am.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define BLKSZ 512

static int disk_blkcnt = 0;
static FILE *fp = NULL;

void __am_disk_init() {
  const char *diskimg = getenv("diskimg");
  if (diskimg) {
    fp = fopen(diskimg, "r+b");
    if (fp) {
      assert(fseek(fp, 0, SEEK_END) == 0);
      long size = ftell(fp);
      assert(size >= 0);
      disk_blkcnt = (size + BLKSZ - 1) / BLKSZ;
      rewind(fp);
    }
  }
}

void __am_disk_config(AM_DISK_CONFIG_T *cfg) {
  cfg->present = (fp != NULL);
  cfg->blksz = BLKSZ;
  cfg->blkcnt = disk_blkcnt;
}

void __am_disk_status(AM_DISK_STATUS_T *stat) {
  stat->ready = 1;
}

void __am_disk_blkio(AM_DISK_BLKIO_T *io) {
  if (fp) {
    assert(io->blkno >= 0);
    assert(io->blkcnt >= 0);
    assert(io->blkno <= disk_blkcnt && io->blkcnt <= disk_blkcnt - io->blkno);

    const size_t bytes = (size_t)io->blkcnt * BLKSZ;
    assert(fseek(fp, (long)io->blkno * BLKSZ, SEEK_SET) == 0);

    if (io->write) {
      size_t ret = fwrite(io->buf, 1, bytes, fp);
      assert(ret == bytes);
      assert(fflush(fp) == 0);
    } else {
      /*
       * A disk image is a byte file, but AM exposes it as whole blocks.  If the
       * final block is short, return zeroes for the padding bytes rather than
       * leaking the caller's old buffer contents.
       */
      memset(io->buf, 0, bytes);
      size_t ret = fread(io->buf, 1, bytes, fp);
      assert(ret == bytes || feof(fp));
      assert(ferror(fp) == 0);
      clearerr(fp);
    }
  }
}
