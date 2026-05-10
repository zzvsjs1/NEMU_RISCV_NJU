#include "fat32.h"

#include <stdint.h>
#include <stddef.h>

size_t disk_read(void *buf, size_t offset, size_t len);

#define FAT32_EOC_MIN 0x0ffffff8u
#define FAT32_BAD_CLUSTER 0x0ffffff7u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_SECTOR_SIZE 512u
#define FAT32_ENTRY_SIZE 4u

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat32_is_end_of_chain(uint32_t value)
{
  return (value & FAT32_ENTRY_MASK) >= FAT32_EOC_MIN;
}

int fat32_is_bad_cluster(uint32_t value)
{
  return (value & FAT32_ENTRY_MASK) == FAT32_BAD_CLUSTER;
}

int fat32_read_fat_entry(const Fat32Volume *vol, uint32_t cluster, uint32_t *next_cluster)
{
  uint8_t sector[FAT32_SECTOR_SIZE];

  if (vol == 0 || next_cluster == 0) {
    return -1;
  }

  /*
   * Data clusters start at 2.  A validated volume has cluster_count usable
   * data clusters, so the highest legal data cluster number is count + 1.
   */
  if (cluster < 2 || (uint64_t)cluster > (uint64_t)vol->cluster_count + 1u) {
    return -1;
  }

  const uint64_t fat_entry_count =
      (uint64_t)vol->fat_size_sectors * FAT32_SECTOR_SIZE / FAT32_ENTRY_SIZE;
  const uint64_t fat_offset = (uint64_t)cluster * FAT32_ENTRY_SIZE;
  if ((uint64_t)cluster >= fat_entry_count) {
    return -1;
  }

  const uint64_t fat_sector_index = fat_offset / FAT32_SECTOR_SIZE;
  const uint64_t sector_number = (uint64_t)vol->reserved_sector_count + fat_sector_index;
  if (sector_number >= (uint64_t)vol->reserved_sector_count + vol->fat_size_sectors) {
    return -1;
  }
  if (sector_number > SIZE_MAX / FAT32_SECTOR_SIZE) {
    return -1;
  }

  const size_t byte_offset = (size_t)sector_number * FAT32_SECTOR_SIZE;
  if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
    return -1;
  }

  *next_cluster = get_le32(&sector[fat_offset % FAT32_SECTOR_SIZE]) & FAT32_ENTRY_MASK;
  return 0;
}
