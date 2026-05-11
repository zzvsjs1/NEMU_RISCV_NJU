#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static FILE *disk_file;
/*
 * Number of disk_read() calls since the last reset.  Performance-sensitive
 * tests use this to ensure the FAT32 backend batches contiguous data instead of
 * issuing one host read per 512-byte cluster.
 */
static size_t disk_read_calls;
/*
 * Total bytes returned by disk_read() since the last reset.  This catches
 * hidden over-reading of data sectors while still allowing small FAT-sector
 * reads used to verify the allocation chain.
 */
static size_t disk_read_bytes;

void fat32_test_disk_open(const char *path)
{
  disk_file = fopen(path, "rb+");
  assert(disk_file != 0);
}

void fat32_test_disk_close(void)
{
  assert(disk_file != 0);
  fclose(disk_file);
  disk_file = 0;
}

void fat32_test_disk_reset_stats(void)
{
  disk_read_calls = 0;
  disk_read_bytes = 0;
}

size_t fat32_test_disk_read_calls(void)
{
  return disk_read_calls;
}

size_t fat32_test_disk_read_bytes(void)
{
  return disk_read_bytes;
}

size_t disk_read(void *buf, size_t offset, size_t len)
{
  assert(disk_file != 0);
  assert(fseek(disk_file, (long)offset, SEEK_SET) == 0);
  const size_t ret = fread(buf, 1, len, disk_file);

  disk_read_calls++;
  disk_read_bytes += ret;
  return ret;
}

size_t disk_write(const void *buf, size_t offset, size_t len)
{
  assert(disk_file != 0);
  assert(fseek(disk_file, (long)offset, SEEK_SET) == 0);
  const size_t ret = fwrite(buf, 1, len, disk_file);
  assert(fflush(disk_file) == 0);
  return ret;
}
