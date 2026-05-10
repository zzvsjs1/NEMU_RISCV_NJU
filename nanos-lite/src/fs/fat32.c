#include "fat32.h"

#include <stdint.h>
#include <string.h>

size_t disk_read(void *buf, size_t offset, size_t len);

#define FAT32_SECTOR_SIZE 512u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_RESERVED_CLUSTER_MIN 0x0ffffff0u
#define FAT32_RESERVED_CLUSTER_MAX 0x0ffffff6u

static Fat32Volume mounted_volume;
static int mounted;

static size_t min_size(size_t a, size_t b)
{
  return a < b ? a : b;
}

static int cluster_size_bytes(const Fat32Volume *vol, size_t *out)
{
  const uint64_t size = (uint64_t)vol->sectors_per_cluster * FAT32_SECTOR_SIZE;

  if (size == 0 || size > SIZE_MAX) {
    return -1;
  }

  *out = (size_t)size;
  return 0;
}

static int is_data_cluster(const Fat32Volume *vol, uint32_t cluster)
{
  return cluster >= 2 && (uint64_t)cluster <= (uint64_t)vol->cluster_count + 1u;
}

static int is_valid_file_data_cluster(const Fat32Volume *vol, uint32_t cluster)
{
  /*
   * Non-empty files must start at a real data cluster.  FAT values in the
   * reserved, bad, or end-of-chain ranges are not usable cluster numbers even
   * if a malformed BPB claims a very large cluster count.
   */
  if (!is_data_cluster(vol, cluster)) {
    return 0;
  }
  if (cluster >= FAT32_RESERVED_CLUSTER_MIN || fat32_is_bad_cluster(cluster)
      || fat32_is_end_of_chain(cluster)) {
    return 0;
  }

  return 1;
}

static int is_unusable_chain_value(const Fat32Volume *vol, uint32_t cluster)
{
  const uint32_t value = cluster & FAT32_ENTRY_MASK;

  if (value == 0 || fat32_is_bad_cluster(value) || fat32_is_end_of_chain(value)) {
    return 1;
  }
  if (value >= FAT32_RESERVED_CLUSTER_MIN && value <= FAT32_RESERVED_CLUSTER_MAX) {
    return 1;
  }

  return !is_data_cluster(vol, value);
}

static int checked_sector_byte_offset(uint64_t sector_number, size_t *out)
{
  if (sector_number > SIZE_MAX / FAT32_SECTOR_SIZE) {
    return -1;
  }

  *out = (size_t)sector_number * FAT32_SECTOR_SIZE;
  return 0;
}

static int seek_cluster(Fat32File *file, uint32_t cluster_index, uint32_t *out_cluster)
{
  uint32_t cluster = file->first_cluster;
  uint32_t index = 0;

  if (!is_data_cluster(&mounted_volume, cluster)) {
    return -1;
  }

  /*
   * Reads commonly advance forwards.  Reusing the cached point keeps repeated
   * small reads from restarting at the first cluster every time.
   */
  if (file->cached_cluster != 0 && file->cached_cluster_index <= cluster_index
      && is_data_cluster(&mounted_volume, file->cached_cluster)) {
    cluster = file->cached_cluster;
    index = file->cached_cluster_index;
  }

  while (index < cluster_index) {
    uint32_t next_cluster;

    if (fat32_read_fat_entry(&mounted_volume, cluster, &next_cluster) != 0) {
      return -1;
    }
    if (is_unusable_chain_value(&mounted_volume, next_cluster)) {
      return -1;
    }

    cluster = next_cluster & FAT32_ENTRY_MASK;
    index++;
  }

  file->cached_cluster_index = index;
  file->cached_cluster = cluster;
  *out_cluster = cluster;
  return 0;
}

int fat32_backend_init(void)
{
  if (mounted) {
    return 0;
  }

  if (fat32_mount_from_disk(FAT32_SECTOR_SIZE, &mounted_volume) != 0) {
    return -1;
  }

  mounted = 1;
  return 0;
}

int fat32_backend_open(const char *path, Fat32File *out)
{
  Fat32DirEntry entry;

  if (path == 0 || out == 0) {
    return -1;
  }
  if (fat32_backend_init() != 0) {
    return -1;
  }
  if (fat32_lookup_path(&mounted_volume, path, &entry) != 0) {
    return -1;
  }
  if ((entry.attr & FAT32_ATTR_DIRECTORY) != 0) {
    return -1;
  }

  memset(out, 0, sizeof(*out));
  out->first_cluster = entry.first_cluster;
  out->size = entry.size;
  if (entry.size != 0) {
    if (!is_valid_file_data_cluster(&mounted_volume, entry.first_cluster)) {
      return -1;
    }
    out->cached_cluster = entry.first_cluster;
  }
  return 0;
}

size_t fat32_backend_read(Fat32File *file, size_t offset, void *buf, size_t len)
{
  uint8_t *dst = (uint8_t *)buf;
  size_t cluster_size;
  size_t remaining;
  size_t copied = 0;

  if (file == 0 || buf == 0 || len == 0) {
    return 0;
  }

  if (file->size == 0 || offset >= file->size) {
    return 0;
  }

  if (cluster_size_bytes(&mounted_volume, &cluster_size) != 0) {
    return 0;
  }

  remaining = min_size(len, (size_t)file->size - offset);
  while (remaining > 0) {
    const uint32_t cluster_index = (uint32_t)(offset / cluster_size);
    const size_t offset_in_cluster = offset % cluster_size;
    const uint32_t first_sector_index = (uint32_t)(offset_in_cluster / FAT32_SECTOR_SIZE);
    size_t offset_in_sector = offset_in_cluster % FAT32_SECTOR_SIZE;
    size_t cluster_left = cluster_size - offset_in_cluster;
    uint32_t cluster;
    uint32_t first_sector;

    if (cluster_index != offset / cluster_size) {
      break;
    }
    if (seek_cluster(file, cluster_index, &cluster) != 0) {
      break;
    }
    if (fat32_first_sector_of_cluster(&mounted_volume, cluster, &first_sector) != 0) {
      break;
    }

    for (uint32_t sector_index = first_sector_index;
         sector_index < mounted_volume.sectors_per_cluster && remaining > 0 && cluster_left > 0;
         sector_index++) {
      uint8_t sector[FAT32_SECTOR_SIZE];
      size_t byte_offset;
      size_t chunk;
      const uint64_t sector_number = (uint64_t)first_sector + sector_index;

      if (checked_sector_byte_offset(sector_number, &byte_offset) != 0) {
        return copied;
      }
      if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector)) {
        return copied;
      }

      chunk = min_size(FAT32_SECTOR_SIZE - offset_in_sector, remaining);
      chunk = min_size(chunk, cluster_left);
      memcpy(&dst[copied], &sector[offset_in_sector], chunk);

      copied += chunk;
      offset += chunk;
      remaining -= chunk;
      cluster_left -= chunk;
      offset_in_sector = 0;
    }
  }

  return copied;
}

size_t fat32_backend_size(const Fat32File *file)
{
  if (file == 0) {
    return 0;
  }

  return file->size;
}

int fat32_backend_close(Fat32File *file)
{
  if (file == 0) {
    return -1;
  }

  memset(file, 0, sizeof(*file));
  return 0;
}

#ifdef FAT32_HOST_TEST
void fat32_test_reset_backend(void)
{
  memset(&mounted_volume, 0, sizeof(mounted_volume));
  mounted = 0;
}
#endif
