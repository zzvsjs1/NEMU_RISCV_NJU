#include "fat32.h"

#include <stdbool.h>
#include <string.h>

static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_power_of_two_u8(uint8_t value)
{
  return value != 0 && (value & (uint8_t)(value - 1)) == 0;
}

int fat32_first_sector_of_cluster(const Fat32Volume *vol, uint32_t cluster, uint32_t *out_sector)
{
  if (vol == 0 || out_sector == 0) {
    return -1;
  }

  if (cluster < 2 || (uint64_t)cluster > (uint64_t)vol->cluster_count + 1u) {
    return -1;
  }

  const uint64_t sector = (uint64_t)vol->first_data_sector
      + ((uint64_t)cluster - 2u) * (uint64_t)vol->sectors_per_cluster;
  if (sector > UINT32_MAX) {
    return -1;
  }

  *out_sector = (uint32_t)sector;
  return 0;
}

int fat32_parse_bpb(const uint8_t sector[512], uint32_t disk_block_size, Fat32Volume *out)
{
  if (sector == 0 || out == 0) {
    return -1;
  }

  if (sector[510] != 0x55 || sector[511] != 0xaa) {
    return -1;
  }

  const uint16_t bytes_per_sector = get_le16(&sector[11]);
  const uint8_t sectors_per_cluster = sector[13];
  const uint16_t reserved_sector_count = get_le16(&sector[14]);
  const uint8_t fat_count = sector[16];
  const uint16_t root_entry_count = get_le16(&sector[17]);
  const uint16_t total_sectors_16 = get_le16(&sector[19]);
  const uint16_t fat_size_16 = get_le16(&sector[22]);
  const uint32_t total_sectors_32 = get_le32(&sector[32]);
  const uint32_t fat_size_32 = get_le32(&sector[36]);
  const uint32_t root_cluster = get_le32(&sector[44]);
  const uint16_t fsinfo_sector = get_le16(&sector[48]);
  const uint16_t backup_boot_sector = get_le16(&sector[50]);

  if (bytes_per_sector != 512 || disk_block_size != 512) {
    return -1;
  }

  if (!is_power_of_two_u8(sectors_per_cluster)) {
    return -1;
  }

  if (reserved_sector_count == 0 || fat_count == 0 || fat_size_32 == 0) {
    return -1;
  }

  if (root_entry_count != 0 || total_sectors_16 != 0 || fat_size_16 != 0) {
    return -1;
  }

  if (total_sectors_32 == 0 || root_cluster < 2) {
    return -1;
  }

  const uint64_t first_data_sector = (uint64_t)reserved_sector_count
      + (uint64_t)fat_count * (uint64_t)fat_size_32;
  if ((uint64_t)total_sectors_32 <= first_data_sector || first_data_sector > UINT32_MAX) {
    return -1;
  }

  const uint64_t data_sectors = (uint64_t)total_sectors_32 - first_data_sector;
  const uint64_t cluster_count = data_sectors / (uint64_t)sectors_per_cluster;
  if (data_sectors > UINT32_MAX || cluster_count > UINT32_MAX) {
    return -1;
  }

  if (cluster_count < 65525u) {
    return -1;
  }

  if ((uint64_t)root_cluster > cluster_count + 1u) {
    return -1;
  }

  const uint64_t fat_entries = (uint64_t)fat_size_32 * 512u / 4u;
  if (fat_entries < cluster_count + 2u) {
    return -1;
  }

  memset(out, 0, sizeof(*out));
  out->bytes_per_sector = bytes_per_sector;
  out->sectors_per_cluster = sectors_per_cluster;
  out->reserved_sector_count = reserved_sector_count;
  out->fat_count = fat_count;
  out->total_sectors = total_sectors_32;
  out->fat_size_sectors = fat_size_32;
  out->root_cluster = root_cluster;
  out->fsinfo_sector = fsinfo_sector;
  out->backup_boot_sector = backup_boot_sector;
  out->first_data_sector = (uint32_t)first_data_sector;
  out->data_sectors = (uint32_t)data_sectors;
  out->cluster_count = (uint32_t)cluster_count;
  return 0;
}
