#include "fat32.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

#define FAT32_EOC_MIN 0x0ffffff8u
#define FAT32_EOC_VALUE 0x0fffffffu
#define FAT32_BAD_CLUSTER 0x0ffffff7u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_SECTOR_SIZE 512u
/*
 * FAT32 stores each FAT entry in a 32-bit slot, but only the low 28 bits carry
 * the cluster-chain value.  The high four bits are reserved and must be
 * preserved when writing an entry.
 */
#define FAT32_ENTRY_SIZE 4u
#define FAT32_UNKNOWN_COUNT 0xffffffffu
/*
 * FSInfo signatures and unknown-count sentinel from the FAT32 specification.
 * Free-count and next-free values are performance hints, not proof that a
 * cluster is usable.
 */
#define FAT32_FSINFO_LEAD_SIG 0x41615252u
#define FAT32_FSINFO_STRUCT_SIG 0x61417272u
#define FAT32_FSINFO_TRAIL_SIG 0xaa550000u

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static int checked_fat_entry_location(const Fat32Volume *vol, uint32_t cluster,
                                      unsigned fat_index, size_t *byte_offset,
                                      size_t *entry_offset)
{
  if (vol == 0 || byte_offset == 0 || entry_offset == 0) {
    return -1;
  }

  /*
   * Data clusters start at 2.  A validated volume has cluster_count usable
   * data clusters, so the highest legal data cluster number is count + 1.
   */
  if (cluster < 2 || (uint64_t)cluster > (uint64_t)vol->cluster_count + 1u) {
    return -1;
  }
  if (fat_index >= vol->fat_count) {
    return -1;
  }

  const uint64_t fat_entry_count =
      (uint64_t)vol->fat_size_sectors * FAT32_SECTOR_SIZE / FAT32_ENTRY_SIZE;
  const uint64_t fat_offset = (uint64_t)cluster * FAT32_ENTRY_SIZE;
  if ((uint64_t)cluster >= fat_entry_count) {
    return -1;
  }

  const uint64_t fat_sector_index = fat_offset / FAT32_SECTOR_SIZE;
  const uint64_t sector_number = (uint64_t)vol->reserved_sector_count
      + (uint64_t)fat_index * (uint64_t)vol->fat_size_sectors + fat_sector_index;
  const uint64_t fat_end = (uint64_t)vol->reserved_sector_count
      + (uint64_t)(fat_index + 1u) * (uint64_t)vol->fat_size_sectors;
  if (sector_number >= fat_end || sector_number > SIZE_MAX / FAT32_SECTOR_SIZE) {
    return -1;
  }

  *byte_offset = (size_t)sector_number * FAT32_SECTOR_SIZE;
  *entry_offset = (size_t)(fat_offset % FAT32_SECTOR_SIZE);
  return 0;
}

static int is_data_cluster(const Fat32Volume *vol, uint32_t cluster)
{
  return vol != 0 && cluster >= 2 && (uint64_t)cluster <= (uint64_t)vol->cluster_count + 1u;
}

static int checked_sector_byte_offset(uint64_t sector_number, size_t *out)
{
  if (out == 0 || sector_number > SIZE_MAX / FAT32_SECTOR_SIZE) {
    return -1;
  }

  *out = (size_t)sector_number * FAT32_SECTOR_SIZE;
  return 0;
}

static int zero_cluster(const Fat32Volume *vol, uint32_t cluster)
{
  uint8_t zero[FAT32_SECTOR_SIZE];
  uint32_t first_sector;

  if (fat32_first_sector_of_cluster(vol, cluster, &first_sector) != 0) {
    return -1;
  }

  memset(zero, 0, sizeof(zero));
  for (uint32_t sector_index = 0; sector_index < vol->sectors_per_cluster; sector_index++) {
    size_t byte_offset;

    if (checked_sector_byte_offset((uint64_t)first_sector + sector_index, &byte_offset) != 0) {
      return -1;
    }
    if (disk_write(zero, byte_offset, sizeof(zero)) != sizeof(zero)) {
      return -1;
    }
  }

  return 0;
}

static void mark_cluster_allocated(Fat32Volume *vol, uint32_t cluster)
{
  if (vol->free_cluster_count != FAT32_UNKNOWN_COUNT && vol->free_cluster_count > 0) {
    vol->free_cluster_count--;
  }

  const uint32_t next = cluster + 1u;
  vol->next_free_cluster = is_data_cluster(vol, next) ? next : FAT32_UNKNOWN_COUNT;
}

static void mark_cluster_freed(Fat32Volume *vol, uint32_t cluster)
{
  if (vol->free_cluster_count != FAT32_UNKNOWN_COUNT
      && vol->free_cluster_count < vol->cluster_count) {
    vol->free_cluster_count++;
  }

  if (vol->next_free_cluster == FAT32_UNKNOWN_COUNT || cluster < vol->next_free_cluster) {
    vol->next_free_cluster = cluster;
  }
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
  size_t byte_offset;
  size_t entry_offset;

  if (vol == 0 || next_cluster == 0) {
    return -1;
  }

  if (checked_fat_entry_location(vol, cluster, vol->active_fat, &byte_offset, &entry_offset) != 0) {
    return -1;
  }

  /*
   * Sequential FAT walks touch many entries from the same 512-byte FAT sector.
   * Keep that sector in the mounted volume so reading a large contiguous file
   * costs one disk command per 128 FAT32 entries instead of one per entry.
   */
  Fat32Volume *cache = (Fat32Volume *)vol;
  if (!cache->fat_sector_cache_valid
      || cache->fat_sector_cache_fat_index != vol->active_fat
      || cache->fat_sector_cache_offset != (uint64_t)byte_offset) {
    if (disk_read(cache->fat_sector_cache, byte_offset, sizeof(cache->fat_sector_cache))
        != sizeof(cache->fat_sector_cache)) {
      return -1;
    }

    cache->fat_sector_cache_valid = 1;
    cache->fat_sector_cache_fat_index = vol->active_fat;
    cache->fat_sector_cache_offset = (uint64_t)byte_offset;
  }

  *next_cluster = get_le32(&cache->fat_sector_cache[entry_offset]) & FAT32_ENTRY_MASK;
  return 0;
}

int fat32_write_fat_entry(const Fat32Volume *vol, uint32_t cluster, uint32_t value)
{
  if (vol == 0) {
    return -1;
  }

  const unsigned start = vol->mirror_fats == 0 ? vol->active_fat : 0u;
  const unsigned end = vol->mirror_fats == 0 ? start + 1u : vol->fat_count;

  for (unsigned fat_index = start; fat_index < end; fat_index++) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    size_t byte_offset;
    size_t entry_offset;

    if (checked_fat_entry_location(vol, cluster, fat_index, &byte_offset, &entry_offset) != 0) {
      return -1;
    }
    if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
      return -1;
    }

    /*
     * FAT32 stores reserved flags in the high nibble.  Preserve the high bits
     * from each FAT copy separately, because mirrored copies are allowed to
     * carry different reserved-bit values.
     */
    const uint32_t old_value = get_le32(&sector[entry_offset]);
    const uint32_t new_value = (old_value & 0xf0000000u) | (value & FAT32_ENTRY_MASK);
    put_le32(&sector[entry_offset], new_value);

    if (disk_write(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
      return -1;
    }

    Fat32Volume *cache = (Fat32Volume *)vol;
    if (cache->fat_sector_cache_valid
        && cache->fat_sector_cache_fat_index == fat_index
        && cache->fat_sector_cache_offset == (uint64_t)byte_offset) {
      memcpy(cache->fat_sector_cache, sector, sizeof(cache->fat_sector_cache));
    }
  }

  return 0;
}

int fat32_load_fsinfo(Fat32Volume *vol)
{
  uint8_t sector[FAT32_SECTOR_SIZE];
  size_t byte_offset;

  if (vol == 0) {
    return -1;
  }

  vol->fsinfo_valid = 0;
  vol->free_cluster_count = FAT32_UNKNOWN_COUNT;
  vol->next_free_cluster = FAT32_UNKNOWN_COUNT;

  if (vol->fsinfo_sector == 0 || vol->fsinfo_sector >= vol->reserved_sector_count) {
    return 0;
  }
  if (checked_sector_byte_offset(vol->fsinfo_sector, &byte_offset) != 0) {
    return -1;
  }
  if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
    return -1;
  }

  if (get_le32(&sector[0]) != FAT32_FSINFO_LEAD_SIG
      || get_le32(&sector[484]) != FAT32_FSINFO_STRUCT_SIG
      || get_le32(&sector[508]) != FAT32_FSINFO_TRAIL_SIG) {
    return 0;
  }

  vol->fsinfo_valid = 1;
  vol->free_cluster_count = get_le32(&sector[488]);
  vol->next_free_cluster = get_le32(&sector[492]);

  if (vol->free_cluster_count != FAT32_UNKNOWN_COUNT
      && vol->free_cluster_count > vol->cluster_count) {
    vol->free_cluster_count = FAT32_UNKNOWN_COUNT;
  }
  if (vol->next_free_cluster != FAT32_UNKNOWN_COUNT
      && !is_data_cluster(vol, vol->next_free_cluster)) {
    vol->next_free_cluster = FAT32_UNKNOWN_COUNT;
  }

  return 0;
}

int fat32_flush_fsinfo(const Fat32Volume *vol)
{
  if (vol == 0) {
    return -1;
  }
  if (!vol->fsinfo_valid) {
    return 0;
  }

  const uint32_t sectors[] = {
      vol->fsinfo_sector,
      vol->backup_boot_sector != 0 ? (uint32_t)vol->backup_boot_sector + vol->fsinfo_sector : 0,
  };

  for (size_t i = 0; i < sizeof(sectors) / sizeof(sectors[0]); i++) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    size_t byte_offset;

    if (sectors[i] == 0 || sectors[i] >= vol->reserved_sector_count) {
      continue;
    }
    if (checked_sector_byte_offset(sectors[i], &byte_offset) != 0) {
      return -1;
    }
    if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
      return -1;
    }
    if (get_le32(&sector[0]) != FAT32_FSINFO_LEAD_SIG
        || get_le32(&sector[484]) != FAT32_FSINFO_STRUCT_SIG
        || get_le32(&sector[508]) != FAT32_FSINFO_TRAIL_SIG) {
      continue;
    }

    put_le32(&sector[488], vol->free_cluster_count);
    put_le32(&sector[492], vol->next_free_cluster);
    if (disk_write(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
      return -1;
    }
  }

  return 0;
}

int fat32_alloc_cluster(Fat32Volume *vol, uint32_t preferred_after, uint32_t *out_cluster)
{
  uint32_t start;

  if (vol == 0 || out_cluster == 0) {
    return -1;
  }

  if (is_data_cluster(vol, preferred_after) && is_data_cluster(vol, preferred_after + 1u)) {
    start = preferred_after + 1u;
  } else if (is_data_cluster(vol, vol->next_free_cluster)) {
    start = vol->next_free_cluster;
  } else {
    start = 2;
  }

  for (uint32_t scanned = 0; scanned < vol->cluster_count; scanned++) {
    const uint32_t cluster = 2u + ((start - 2u + scanned) % vol->cluster_count);
    uint32_t value;

    if (fat32_read_fat_entry(vol, cluster, &value) != 0) {
      return -1;
    }
    if (value != 0) {
      continue;
    }

    if (fat32_write_fat_entry(vol, cluster, FAT32_EOC_VALUE) != 0) {
      return -1;
    }
    if (zero_cluster(vol, cluster) != 0) {
      (void)fat32_write_fat_entry(vol, cluster, 0);
      return -1;
    }

    mark_cluster_allocated(vol, cluster);
    if (fat32_flush_fsinfo(vol) != 0) {
      return -1;
    }

    *out_cluster = cluster;
    return 0;
  }

  return -1;
}

int fat32_free_chain(Fat32Volume *vol, uint32_t first_cluster)
{
  uint32_t cluster = first_cluster;

  if (vol == 0) {
    return -1;
  }
  if (first_cluster == 0) {
    return 0;
  }
  if (!is_data_cluster(vol, first_cluster)) {
    return -1;
  }

  for (uint32_t links = 0; links < vol->cluster_count; links++) {
    uint32_t next;

    if (fat32_read_fat_entry(vol, cluster, &next) != 0) {
      return -1;
    }
    if (fat32_write_fat_entry(vol, cluster, 0) != 0) {
      return -1;
    }
    mark_cluster_freed(vol, cluster);

    if (fat32_is_end_of_chain(next)) {
      return fat32_flush_fsinfo(vol);
    }
    if (next == 0 || fat32_is_bad_cluster(next) || !is_data_cluster(vol, next)) {
      return -1;
    }
    cluster = next & FAT32_ENTRY_MASK;
  }

  return -1;
}
