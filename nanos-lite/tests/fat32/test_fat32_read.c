#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/fs/fat32.h"

void fat32_test_disk_open(const char *path);
void fat32_test_disk_close(void);
void fat32_test_disk_reset_stats(void);
size_t fat32_test_disk_read_calls(void);
size_t fat32_test_disk_read_bytes(void);
void fat32_test_reset_backend(void);
size_t disk_read(void *buf, size_t offset, size_t len);

static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void write_big_file(void)
{
  FILE *file;

  assert(system("rm -rf fat32-read-work && mkdir -p fat32-read-work/fsimg/subdir") == 0);
  file = fopen("fat32-read-work/fsimg/big.bin", "wb");
  assert(file != 0);

  /*
   * The file is larger than one 512-byte cluster, so the generated FAT image
   * must contain a two-cluster chain for BIG.BIN.
   */
  for (int i = 0; i < 900; i++) {
    assert(fputc((uint8_t)(i & 0xff), file) != EOF);
  }

  assert(fclose(file) == 0);

  file = fopen("fat32-read-work/fsimg/large.bin", "wb");
  assert(file != 0);

  /*
   * This file is large enough to expose per-cluster overhead.  The local
   * mkfat32 builder allocates clusters sequentially, while the reader still has
   * to validate the FAT chain before batching those on-disk sectors.
   */
  for (size_t i = 0; i < 128u * 1024u; i++) {
    assert(fputc((uint8_t)(i & 0xffu), file) != EOF);
  }

  assert(fclose(file) == 0);

  file = fopen("fat32-read-work/fsimg/empty.bin", "wb");
  assert(file != 0);
  assert(fclose(file) == 0);
}

static void image_read_at(size_t offset, void *buf, size_t size)
{
  FILE *image = fopen("fat32-read-work/ramdisk.img", "rb");

  assert(image != 0);
  assert(fseek(image, (long)offset, SEEK_SET) == 0);
  assert(fread(buf, 1, size, image) == size);
  assert(fclose(image) == 0);
}

static void image_write_at(size_t offset, const void *buf, size_t size)
{
  FILE *image = fopen("fat32-read-work/ramdisk.img", "rb+");

  assert(image != 0);
  assert(fseek(image, (long)offset, SEEK_SET) == 0);
  assert(fwrite(buf, 1, size, image) == size);
  assert(fclose(image) == 0);
}

static void build_test_image(void)
{
  write_big_file();
  assert(system("python3 ../../../navy-apps/scripts/mkfat32.py "
                "fat32-read-work/fsimg fat32-read-work/ramdisk.img") == 0);
}

static uint32_t find_big_bin_first_cluster(const uint8_t sector[512])
{
  for (size_t offset = 0; offset < 512; offset += 32) {
    const uint8_t *entry = &sector[offset];
    if (entry[0] == 0x00) {
      break;
    }
    if (memcmp(entry, "BIG     BIN", 11) == 0) {
      const uint32_t high = get_le16(&entry[20]);
      const uint32_t low = get_le16(&entry[26]);
      assert(get_le32(&entry[28]) == 900);
      return (high << 16) | low;
    }
  }

  assert(!"BIG.BIN directory entry not found");
  return 0;
}

static size_t find_root_entry_offset(const Fat32Volume *vol, const char short_name[11])
{
  uint8_t sector[512];
  uint32_t root_sector;

  assert(fat32_first_sector_of_cluster(vol, vol->root_cluster, &root_sector) == 0);
  for (uint32_t sector_index = 0; sector_index < vol->sectors_per_cluster; sector_index++) {
    const size_t sector_offset = ((size_t)root_sector + sector_index) * 512u;

    image_read_at(sector_offset, sector, sizeof(sector));
    for (size_t entry_index = 0; entry_index < 512u / 32u; entry_index++) {
      const size_t entry_offset = entry_index * 32u;
      const uint8_t *entry = &sector[entry_offset];

      if (entry[0] == 0x00) {
        break;
      }
      if (memcmp(entry, short_name, 11) == 0) {
        return sector_offset + entry_offset;
      }
    }
  }

  assert(!"root directory entry not found");
  return 0;
}

static void overwrite_root_entry_cluster(const Fat32Volume *vol, const char short_name[11],
                                         uint32_t cluster)
{
  uint8_t raw_entry[32];
  const size_t entry_offset = find_root_entry_offset(vol, short_name);

  image_read_at(entry_offset, raw_entry, sizeof(raw_entry));
  put_le16(&raw_entry[20], (uint16_t)(cluster >> 16));
  put_le16(&raw_entry[26], (uint16_t)cluster);
  image_write_at(entry_offset, raw_entry, sizeof(raw_entry));
}

static void overwrite_fat_entry_raw(const Fat32Volume *vol, uint32_t cluster, uint32_t value)
{
  FILE *image = fopen("fat32-read-work/ramdisk.img", "rb+");
  assert(image != 0);
  assert(fseek(image, (long)((size_t)vol->reserved_sector_count * 512u
                             + (size_t)cluster * 4u), SEEK_SET) == 0);
  assert(fputc((uint8_t)value, image) != EOF);
  assert(fputc((uint8_t)(value >> 8), image) != EOF);
  assert(fputc((uint8_t)(value >> 16), image) != EOF);
  assert(fputc((uint8_t)(value >> 24), image) != EOF);
  assert(fclose(image) == 0);
}

static void test_cluster_marker_helpers(void)
{
  assert(fat32_is_end_of_chain(0x0ffffff8u));
  assert(!fat32_is_end_of_chain(0x0ffffff7u));
  assert(!fat32_is_end_of_chain(0xf0000002u));
  assert(fat32_is_bad_cluster(0x0ffffff7u));
  assert(fat32_is_bad_cluster(0xfffffff7u));
}

static void test_reads_fat_chain_from_image(void)
{
  uint8_t boot_sector[512];
  uint8_t root_sector_data[512];
  Fat32Volume vol;
  uint32_t root_sector;
  uint32_t first_file_cluster;
  uint32_t next;

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");

  assert(disk_read(boot_sector, 0, sizeof(boot_sector)) == sizeof(boot_sector));
  assert(fat32_parse_bpb(boot_sector, 512, &vol) == 0);

  assert(fat32_read_fat_entry(&vol, 2, &next) == 0);
  assert(fat32_is_end_of_chain(next));

  assert(fat32_first_sector_of_cluster(&vol, 2, &root_sector) == 0);
  assert(disk_read(root_sector_data, (size_t)root_sector * 512u, sizeof(root_sector_data))
      == sizeof(root_sector_data));

  first_file_cluster = find_big_bin_first_cluster(root_sector_data);
  overwrite_fat_entry_raw(&vol, first_file_cluster, 0xf0000000u | (first_file_cluster + 1u));
  assert(fat32_read_fat_entry(&vol, first_file_cluster, &next) == 0);
  assert(next == first_file_cluster + 1u);
  assert(fat32_read_fat_entry(&vol, next, &next) == 0);
  assert(fat32_is_end_of_chain(next));

  assert(fat32_read_fat_entry(0, 2, &next) == -1);
  assert(fat32_read_fat_entry(&vol, 2, 0) == -1);
  assert(fat32_read_fat_entry(&vol, 0, &next) == -1);
  assert(fat32_read_fat_entry(&vol, 1, &next) == -1);
  assert(fat32_read_fat_entry(&vol, vol.cluster_count + 2u, &next) == -1);

  fat32_test_disk_close();
}

static void test_rejects_fat_byte_offset_overflow_on_32_bit_hosts(void)
{
#if SIZE_MAX <= UINT32_MAX
  Fat32Volume vol;
  uint32_t next;
  const uint32_t cluster = ((uint32_t)(SIZE_MAX / 512u) + 1u) * 128u;

  memset(&vol, 0, sizeof(vol));
  vol.reserved_sector_count = 65535;
  vol.fat_size_sectors = UINT32_MAX;
  vol.cluster_count = UINT32_MAX - 1u;

  assert(fat32_read_fat_entry(&vol, cluster, &next) == -1);
#endif
}

static void expect_pattern(const uint8_t *buf, size_t start, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    assert(buf[i] == (uint8_t)((start + i) & 0xffu));
  }
}

static void test_backend_reads_whole_file_across_clusters(void)
{
  Fat32File file;
  uint8_t buf[900];

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);
  assert(fat32_backend_open("/big.bin", &file) == 0);
  assert(fat32_backend_size(&file) == 900);

  memset(buf, 0xa5, sizeof(buf));
  assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
  assert(buf[0] == 0);
  assert(buf[511] == 255);
  assert(buf[512] == 0);
  assert(buf[899] == 131);
  expect_pattern(buf, 0, sizeof(buf));

  assert(fat32_backend_close(&file) == 0);
  fat32_test_disk_close();
}

static void test_backend_reads_partial_range_and_eof(void)
{
  Fat32File file;
  uint8_t buf[32];

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);
  assert(fat32_backend_open("/big.bin", &file) == 0);

  memset(buf, 0, sizeof(buf));
  assert(fat32_backend_read(&file, 500, buf, sizeof(buf)) == sizeof(buf));
  expect_pattern(buf, 500, sizeof(buf));

  memset(buf, 0, sizeof(buf));
  assert(fat32_backend_read(&file, 900, buf, sizeof(buf)) == 0);
  assert(fat32_backend_read(&file, 899, buf, 8) == 1);
  assert(buf[0] == (uint8_t)(899 & 0xffu));

  assert(fat32_backend_close(&file) == 0);
  fat32_test_disk_close();
}

static void test_backend_batches_contiguous_large_reads(void)
{
  Fat32File file;
  uint8_t buf[128u * 1024u];

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);
  assert(fat32_backend_open("/large.bin", &file) == 0);
  assert(fat32_backend_size(&file) == sizeof(buf));

  memset(buf, 0xa5, sizeof(buf));
  fat32_test_disk_reset_stats();
  assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
  expect_pattern(buf, 0, sizeof(buf));

  /*
   * A 128 KiB read spans 256 one-sector clusters.  The performance contract is
   * that the FAT reader verifies the chain in FAT-sector-sized groups and hands
   * contiguous data runs to disk_read(), instead of issuing one disk_read() per
   * 512-byte data sector.
   */
  assert(fat32_test_disk_read_calls() <= 4);
  assert(fat32_test_disk_read_bytes() < sizeof(buf) + 4u * 512u);

  assert(fat32_backend_close(&file) == 0);
  fat32_test_disk_close();
}

static void test_backend_uses_contiguous_cache_for_random_large_reads(void)
{
  Fat32File file;
  uint8_t buf[4096];
  const size_t offset = 96u * 1024u;

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);
  assert(fat32_backend_open("/large.bin", &file) == 0);

  memset(buf, 0xa5, sizeof(buf));
  fat32_test_disk_reset_stats();
  assert(fat32_backend_read(&file, offset, buf, sizeof(buf)) == sizeof(buf));
  expect_pattern(buf, offset, sizeof(buf));

  /*
   * ONScripter repeatedly seeks inside large archive files and reads one member.
   * For the local FAT32 image, the opened file's verified contiguous chain
   * should make that lookup direct rather than scanning from the first cluster.
   */
  assert(fat32_test_disk_read_calls() == 1);
  assert(fat32_test_disk_read_bytes() == sizeof(buf));

  assert(fat32_backend_close(&file) == 0);
  fat32_test_disk_close();
}

static void test_backend_handles_empty_files_and_rejects_directories(void)
{
  Fat32File file;
  uint8_t byte = 0xa5;

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);

  assert(fat32_backend_open("/empty.bin", &file) == 0);
  assert(fat32_backend_size(&file) == 0);
  assert(file.first_cluster == 0);
  assert(fat32_backend_read(&file, 0, &byte, 1) == 0);
  assert(byte == 0xa5);
  assert(fat32_backend_close(&file) == 0);

  assert(fat32_backend_open("/", &file) == -1);
  assert(fat32_backend_open("/subdir", &file) == -1);

  fat32_test_disk_close();
}

static void test_backend_returns_partial_read_when_chain_ends_early(void)
{
  uint8_t root_sector_data[512];
  Fat32Volume vol;
  Fat32File file;
  uint8_t buf[900];
  uint32_t root_sector;
  uint32_t first_file_cluster;

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  assert(fat32_mount_from_disk(512, &vol) == 0);
  assert(fat32_first_sector_of_cluster(&vol, vol.root_cluster, &root_sector) == 0);
  assert(disk_read(root_sector_data, (size_t)root_sector * 512u, sizeof(root_sector_data))
      == sizeof(root_sector_data));
  first_file_cluster = find_big_bin_first_cluster(root_sector_data);
  overwrite_fat_entry_raw(&vol, first_file_cluster, 0x0fffffffu);

  fat32_test_reset_backend();
  assert(fat32_backend_init() == 0);
  assert(fat32_backend_open("/big.bin", &file) == 0);
  memset(buf, 0xa5, sizeof(buf));
  assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == 512);
  expect_pattern(buf, 0, 512);
  assert(buf[512] == 0xa5);
  assert(fat32_backend_close(&file) == 0);

  fat32_test_disk_close();
}

static void test_backend_rejects_non_empty_files_with_invalid_first_cluster(void)
{
  Fat32Volume vol;
  Fat32File file;
  const uint32_t invalid_clusters[] = {
      0,
      1,
      0x0ffffff0u,
      0x0ffffff7u,
      0,
  };

  for (size_t i = 0; i < sizeof(invalid_clusters) / sizeof(invalid_clusters[0]); i++) {
    build_test_image();
    fat32_test_disk_open("fat32-read-work/ramdisk.img");
    assert(fat32_mount_from_disk(512, &vol) == 0);

    if (i == sizeof(invalid_clusters) / sizeof(invalid_clusters[0]) - 1u) {
      overwrite_root_entry_cluster(&vol, "BIG     BIN", vol.cluster_count + 2u);
    } else {
      overwrite_root_entry_cluster(&vol, "BIG     BIN", invalid_clusters[i]);
    }

    fat32_test_reset_backend();
    assert(fat32_backend_init() == 0);
    assert(fat32_backend_open("/big.bin", &file) == -1);
    fat32_test_disk_close();
  }
}

static void test_cluster_zero_subdirectory_does_not_alias_root(void)
{
  Fat32Volume vol;
  Fat32DirEntry entry;

  build_test_image();
  fat32_test_disk_open("fat32-read-work/ramdisk.img");
  assert(fat32_mount_from_disk(512, &vol) == 0);
  overwrite_root_entry_cluster(&vol, "SUBDIR     ", 0);

  assert(fat32_lookup_path(&vol, "/subdir", &entry) == 0);
  assert((entry.attr & FAT32_ATTR_DIRECTORY) != 0);
  assert(entry.first_cluster == 0);
  assert(fat32_lookup_path(&vol, "/subdir/big.bin", &entry) == -1);

  fat32_test_disk_close();
}

int main(void)
{
  test_cluster_marker_helpers();
  test_reads_fat_chain_from_image();
  test_rejects_fat_byte_offset_overflow_on_32_bit_hosts();
  test_backend_reads_whole_file_across_clusters();
  test_backend_reads_partial_range_and_eof();
  test_backend_batches_contiguous_large_reads();
  test_backend_uses_contiguous_cache_for_random_large_reads();
  test_backend_handles_empty_files_and_rejects_directories();
  test_backend_returns_partial_read_when_chain_ends_early();
  test_backend_rejects_non_empty_files_with_invalid_first_cluster();
  test_cluster_zero_subdirectory_does_not_alias_root();
  puts("fat32_read tests passed");
  return 0;
}
