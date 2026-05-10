#ifndef NANOS_LITE_FS_FAT32_H
#define NANOS_LITE_FS_FAT32_H

#include <stddef.h>
#include <stdint.h>

#define FAT32_ATTR_READ_ONLY 0x01u
#define FAT32_ATTR_HIDDEN    0x02u
#define FAT32_ATTR_SYSTEM    0x04u
#define FAT32_ATTR_VOLUME_ID 0x08u
#define FAT32_ATTR_DIRECTORY 0x10u
#define FAT32_ATTR_ARCHIVE   0x20u
#define FAT32_ATTR_LONG_NAME (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

typedef struct __attribute__((packed)) {
  uint8_t order;
  uint16_t name1[5];
  uint8_t attr;
  uint8_t type;
  uint8_t checksum;
  uint16_t name2[6];
  uint16_t first_cluster_low;
  uint16_t name3[2];
} Fat32LfnEntry;

typedef struct {
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sector_count;
  uint8_t fat_count;
  uint32_t total_sectors;
  uint32_t fat_size_sectors;
  uint32_t root_cluster;
  uint16_t fsinfo_sector;
  uint16_t backup_boot_sector;
  uint32_t first_data_sector;
  uint32_t data_sectors;
  uint32_t cluster_count;
} Fat32Volume;

typedef struct {
  uint8_t attr;
  uint32_t first_cluster;
  uint32_t size;
} Fat32DirEntry;

typedef struct {
  uint32_t first_cluster;
  uint32_t size;
  uint32_t cached_cluster_index;
  uint32_t cached_cluster;
} Fat32File;

uint8_t fat32_lfn_checksum(const uint8_t short_name[11]);
int fat32_lfn_entry_to_ascii(const Fat32LfnEntry *entry, char *out, size_t out_size);
int fat32_parse_bpb(const uint8_t sector[512], uint32_t disk_block_size, Fat32Volume *out);
int fat32_mount_from_disk(uint32_t disk_block_size, Fat32Volume *out);
int fat32_first_sector_of_cluster(const Fat32Volume *vol, uint32_t cluster, uint32_t *out_sector);
int fat32_is_end_of_chain(uint32_t value);
int fat32_is_bad_cluster(uint32_t value);
int fat32_read_fat_entry(const Fat32Volume *vol, uint32_t cluster, uint32_t *next_cluster);
int fat32_lookup_path(const Fat32Volume *vol, const char *path, Fat32DirEntry *out);
int fat32_backend_init(void);
int fat32_backend_open(const char *path, Fat32File *out);
size_t fat32_backend_read(Fat32File *file, size_t offset, void *buf, size_t len);
size_t fat32_backend_size(const Fat32File *file);
int fat32_backend_close(Fat32File *file);

#ifdef FAT32_HOST_TEST
void fat32_test_fill_lfn_entry(Fat32LfnEntry *entry, const uint16_t chars[13]);
void fat32_test_reset_backend(void);
#endif

#endif
