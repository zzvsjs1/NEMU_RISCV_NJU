#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/fs/fat32.h"

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void build_valid_bpb(uint8_t sector[512])
{
  memset(sector, 0, 512);
  sector[0] = 0xeb;
  sector[1] = 0x58;
  sector[2] = 0x90;
  memcpy(&sector[3], "NEMUFS  ", 8);
  put16(&sector[11], 512);
  sector[13] = 1;
  put16(&sector[14], 32);
  sector[16] = 2;
  put16(&sector[17], 0);
  put16(&sector[19], 0);
  sector[21] = 0xf8;
  put16(&sector[22], 0);
  put32(&sector[32], 200000);
  put32(&sector[36], 2048);
  put16(&sector[40], 0);
  put16(&sector[42], 0);
  put32(&sector[44], 2);
  put16(&sector[48], 1);
  put16(&sector[50], 6);
  sector[510] = 0x55;
  sector[511] = 0xaa;
}

static void test_valid_fat32_bpb_mounts(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  assert(fat32_parse_bpb(sector, 512, &vol) == 0);
  assert(vol.bytes_per_sector == 512);
  assert(vol.sectors_per_cluster == 1);
  assert(vol.reserved_sector_count == 32);
  assert(vol.fat_count == 2);
  assert(vol.fat_size_sectors == 2048);
  assert(vol.root_cluster == 2);
  assert(vol.first_data_sector == 4128);
}

static void test_rejects_non_fat32_cluster_count(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  put32(&sector[32], 1000);
  assert(fat32_parse_bpb(sector, 512, &vol) == -1);
}

static void test_rejects_non_512_byte_sector(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  put16(&sector[11], 1024);
  assert(fat32_parse_bpb(sector, 512, &vol) == -1);
}

static void test_rejects_fat_too_small_for_cluster_count(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  put32(&sector[36], 128);
  assert(fat32_parse_bpb(sector, 512, &vol) == -1);
}

static void test_rejects_wrapped_fat_layout(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  put32(&sector[32], UINT32_MAX);
  put32(&sector[36], UINT32_MAX);
  assert(fat32_parse_bpb(sector, 512, &vol) == -1);
}

static void test_rejects_root_cluster_beyond_data_range(void)
{
  uint8_t sector[512];
  Fat32Volume vol;

  build_valid_bpb(sector);
  put32(&sector[44], 195874);
  assert(fat32_parse_bpb(sector, 512, &vol) == -1);
}

static void test_checked_first_sector_of_cluster(void)
{
  uint8_t sector[512];
  Fat32Volume vol;
  uint32_t sector_number;

  build_valid_bpb(sector);
  assert(fat32_parse_bpb(sector, 512, &vol) == 0);

  assert(fat32_first_sector_of_cluster(&vol, 0, &sector_number) == -1);
  assert(fat32_first_sector_of_cluster(&vol, 1, &sector_number) == -1);
  assert(fat32_first_sector_of_cluster(&vol, vol.cluster_count + 2, &sector_number) == -1);

  assert(fat32_first_sector_of_cluster(&vol, 2, &sector_number) == 0);
  assert(sector_number == 4128);
  assert(fat32_first_sector_of_cluster(&vol, 3, &sector_number) == 0);
  assert(sector_number == 4129);
}

int main(void)
{
  test_valid_fat32_bpb_mounts();
  test_rejects_non_fat32_cluster_count();
  test_rejects_non_512_byte_sector();
  test_rejects_fat_too_small_for_cluster_count();
  test_rejects_wrapped_fat_layout();
  test_rejects_root_cluster_beyond_data_range();
  test_checked_first_sector_of_cluster();
  puts("fat32_bpb tests passed");
  return 0;
}
