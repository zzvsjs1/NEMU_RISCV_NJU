#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static FILE *disk_file;

void fat32_test_disk_open(const char *path)
{
  disk_file = fopen(path, "rb");
  assert(disk_file != 0);
}

void fat32_test_disk_close(void)
{
  assert(disk_file != 0);
  fclose(disk_file);
  disk_file = 0;
}

size_t disk_read(void *buf, size_t offset, size_t len)
{
  assert(disk_file != 0);
  assert(fseek(disk_file, (long)offset, SEEK_SET) == 0);
  return fread(buf, 1, len, disk_file);
}
