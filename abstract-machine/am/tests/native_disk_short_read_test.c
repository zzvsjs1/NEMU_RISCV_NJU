#define _GNU_SOURCE
#define ARCH_H "arch/native.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <am.h>

/*
 * Pull in the native disk implementation directly so this one-file regression
 * test can exercise the AM callbacks without needing a full AM image.
 */
#include "../src/native/ioe/disk.c"

static void write_test_image(const char *path)
{
  FILE *fp = fopen(path, "wb");
  assert(fp != NULL);

  uint8_t first_block[512];
  memset(first_block, 0x5a, sizeof(first_block));
  assert(fwrite(first_block, 1, sizeof(first_block), fp) == sizeof(first_block));

  const uint8_t final_byte = 0xa5;
  assert(fwrite(&final_byte, 1, sizeof(final_byte), fp) == sizeof(final_byte));
  assert(fclose(fp) == 0);
}

int main(int argc, char *argv[])
{
  assert(argc == 2);
  write_test_image(argv[1]);

  assert(setenv("diskimg", argv[1], 1) == 0);
  __am_disk_init();

  AM_DISK_CONFIG_T cfg;
  __am_disk_config(&cfg);
  assert(cfg.present);
  assert(cfg.blksz == 512);
  assert(cfg.blkcnt == 2);

  uint8_t block[512];
  memset(block, 0xcc, sizeof(block));

  AM_DISK_BLKIO_T io = {
    .write = false,
    .buf = block,
    .blkno = 1,
    .blkcnt = 1,
  };
  __am_disk_blkio(&io);

  assert(block[0] == 0xa5);
  for (size_t i = 1; i < sizeof(block); i++) {
    assert(block[i] == 0);
  }

  return 0;
}
