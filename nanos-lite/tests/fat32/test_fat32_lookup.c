#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/fs/fat32.h"

void fat32_test_disk_open(const char *path);
void fat32_test_disk_close(void);

#define FAT32_SECTOR_SIZE 512u
#define FAT32_DIR_ENTRY_SIZE 32u
#define FAT32_DIR_ENTRIES_PER_SECTOR (FAT32_SECTOR_SIZE / FAT32_DIR_ENTRY_SIZE)
#define LOOKUP_IMAGE "fat32-lookup-work/ramdisk.img"

static void build_image(const char *dir, const char *image)
{
    char command[1024];

    snprintf(command, sizeof(command), "python3 ../../../navy-apps/scripts/mkfat32.py %s %s", dir, image);
    assert(system(command) == 0);
}

static void write_text_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");

    assert(file != 0);
    assert(fwrite(content, 1, strlen(content), file) == strlen(content));
    assert(fclose(file) == 0);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void image_read(uint64_t offset, void *buf, size_t size)
{
    FILE *image = fopen(LOOKUP_IMAGE, "rb");

    assert(image != 0);
    assert(fseek(image, (long)offset, SEEK_SET) == 0);
    assert(fread(buf, 1, size, image) == size);
    assert(fclose(image) == 0);
}

static void image_write(uint64_t offset, const void *buf, size_t size)
{
    FILE *image = fopen(LOOKUP_IMAGE, "rb+");

    assert(image != 0);
    assert(fseek(image, (long)offset, SEEK_SET) == 0);
    assert(fwrite(buf, 1, size, image) == size);
    assert(fclose(image) == 0);
}

static uint64_t cluster_offset(const Fat32Volume *vol, uint32_t cluster)
{
    uint32_t sector;

    assert(fat32_first_sector_of_cluster(vol, cluster, &sector) == 0);
    return (uint64_t)sector * FAT32_SECTOR_SIZE;
}

static void short_alias_to_path_component(const uint8_t short_name[11], char out[13])
{
    size_t pos = 0;
    int base_end = 7;
    int ext_end = 10;

    while (base_end >= 0 && short_name[base_end] == ' ')
    {
        base_end--;
    }
    for (int i = 0; i <= base_end; i++)
    {
        out[pos++] = (char)short_name[i];
    }

    while (ext_end >= 8 && short_name[ext_end] == ' ')
    {
        ext_end--;
    }

    if (ext_end >= 8)
    {
        out[pos++] = '.';
        for (int i = 8; i <= ext_end; i++)
        {
            out[pos++] = (char)short_name[i];
        }
    }
    out[pos] = '\0';
}

static void short_alias_to_share_path(const uint8_t short_name[11], char out[64])
{
    char component[13];

    short_alias_to_path_component(short_name, component);
    snprintf(out, 64, "/share/%s", component);
}

static int ascii_equal_ignore_case(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z')
        {
            ca = (char)(ca - 'A' + 'a');
        }

        if (cb >= 'A' && cb <= 'Z')
        {
            cb = (char)(cb - 'A' + 'a');
        }

        if (ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int find_entry_by_short_name(const Fat32Volume *vol, uint32_t dir_cluster,
                                    const uint8_t short_name[11], uint64_t *entry_offset,
                                    uint8_t entry_out[FAT32_DIR_ENTRY_SIZE])
{
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    for (uint32_t links = 0; links < vol->cluster_count; links++)
    {
        uint64_t base = cluster_offset(vol, cluster);

        for (uint32_t sector_index = 0; sector_index < vol->sectors_per_cluster; sector_index++)
        {
            image_read(base + (uint64_t)sector_index * FAT32_SECTOR_SIZE, sector, sizeof(sector));
            for (size_t i = 0; i < FAT32_DIR_ENTRIES_PER_SECTOR; i++)
            {
                const uint8_t *entry = &sector[i * FAT32_DIR_ENTRY_SIZE];

                if (entry[0] == 0x00)
                {
                    return -1;
                }

                if (memcmp(entry, short_name, 11) == 0)
                {
                    *entry_offset = base + (uint64_t)sector_index * FAT32_SECTOR_SIZE + i * FAT32_DIR_ENTRY_SIZE;
                    memcpy(entry_out, entry, FAT32_DIR_ENTRY_SIZE);
                    return 0;
                }
            }
        }

        uint32_t next;
        assert(fat32_read_fat_entry(vol, cluster, &next) == 0);

        if (fat32_is_end_of_chain(next))
        {
            break;
        }
        cluster = next;
    }

    return -1;
}

static int find_lfn_entry(const Fat32Volume *vol, uint32_t dir_cluster, const char *name,
                          uint64_t *lfn_start, unsigned *lfn_count,
                          uint64_t *short_offset, uint8_t short_entry[FAT32_DIR_ENTRY_SIZE])
{
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;
    char pieces[20][14];
    uint8_t present[20];
    unsigned count = 0;
    uint64_t start = 0;

    memset(present, 0, sizeof(present));
    for (uint32_t links = 0; links < vol->cluster_count; links++)
    {
        uint64_t base = cluster_offset(vol, cluster);

        for (uint32_t sector_index = 0; sector_index < vol->sectors_per_cluster; sector_index++)
        {
            image_read(base + (uint64_t)sector_index * FAT32_SECTOR_SIZE, sector, sizeof(sector));
            for (size_t i = 0; i < FAT32_DIR_ENTRIES_PER_SECTOR; i++)
            {
                const uint8_t *entry = &sector[i * FAT32_DIR_ENTRY_SIZE];
                uint64_t offset = base + (uint64_t)sector_index * FAT32_SECTOR_SIZE + i * FAT32_DIR_ENTRY_SIZE;

                if (entry[0] == 0x00)
                {
                    return -1;
                }

                if (entry[11] == FAT32_ATTR_LONG_NAME)
                {
                    Fat32LfnEntry lfn;
                    unsigned sequence = entry[0] & 0x1fu;

                    memcpy(&lfn, entry, sizeof(lfn));

                    if ((entry[0] & 0x40u) != 0)
                    {
                        count = sequence;
                        start = offset;
                        memset(present, 0, sizeof(present));
                    }

                    if (sequence > 0 && sequence < 20 && fat32_lfn_entry_to_ascii(&lfn, pieces[sequence], sizeof(pieces[sequence])) >= 0)
                    {
                        present[sequence] = 1;
                    }
                    continue;
                }

                if (count > 0)
                {
                    char reconstructed[260];
                    size_t pos = 0;

                    for (unsigned sequence = 1; sequence <= count; sequence++)
                    {
                        assert(present[sequence] != 0);
                        for (const char *p = pieces[sequence]; *p != '\0'; p++)
                        {
                            reconstructed[pos++] = *p;
                        }
                    }
                    reconstructed[pos] = '\0';

                    if (ascii_equal_ignore_case(reconstructed, name))
                    {
                        *lfn_start = start;
                        *lfn_count = count;
                        *short_offset = offset;
                        memcpy(short_entry, entry, FAT32_DIR_ENTRY_SIZE);
                        return 0;
                    }
                }

                count = 0;
                memset(present, 0, sizeof(present));
            }
        }

        uint32_t next;
        assert(fat32_read_fat_entry(vol, cluster, &next) == 0);

        if (fat32_is_end_of_chain(next))
        {
            break;
        }
        cluster = next;
    }

    return -1;
}

static void overwrite_fat_entry(const Fat32Volume *vol, uint32_t cluster, uint32_t value)
{
    uint8_t raw[4];
    uint64_t offset = (uint64_t)vol->reserved_sector_count * FAT32_SECTOR_SIZE + (uint64_t)cluster * 4u;

    put_le32(raw, value);
    image_write(offset, raw, sizeof(raw));
}

static void build_lookup_image(void)
{
    assert(system("rm -rf fat32-lookup-work && mkdir -p "
                  "fat32-lookup-work/fsimg/share/SUBDIR") == 0);
    write_text_file("fat32-lookup-work/fsimg/share/ALPHA.TXT", "alpha");
    write_text_file("fat32-lookup-work/fsimg/share/BETA.TXT", "beta");
    write_text_file("fat32-lookup-work/fsimg/share/GAMMA.TXT", "gamma");
    write_text_file("fat32-lookup-work/fsimg/share/OMEGA.TXT", "omega");
    write_text_file("fat32-lookup-work/fsimg/share/long-name.txt", "abc");
    write_text_file("fat32-lookup-work/fsimg/share/three-part-long-file-name.txt", "triple");
    write_text_file("fat32-lookup-work/fsimg/share/very-long-name.txt", "long");
    write_text_file("fat32-lookup-work/fsimg/share/SIMPLE.TXT", "simple");
    write_text_file("fat32-lookup-work/fsimg/share/SUBDIR/INNER.TXT", "inner");
    build_image("fat32-lookup-work/fsimg", LOOKUP_IMAGE);
}

static void assert_lookup_file(const Fat32Volume *vol, const char *path, uint32_t size)
{
    Fat32DirEntry entry;

    memset(&entry, 0, sizeof(entry));
    assert(fat32_lookup_path(vol, path, &entry) == 0);
    assert((entry.attr & FAT32_ATTR_DIRECTORY) == 0);
    assert(entry.first_cluster >= 2);
    assert(entry.size == size);
}

static void test_lookup_paths(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];

    build_lookup_image();
    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);

    assert_lookup_file(&vol, "/share/long-name.txt", 3);
    assert_lookup_file(&vol, "//share///long-name.txt", 3);
    assert_lookup_file(&vol, "/share/very-long-name.txt", 4);
    assert_lookup_file(&vol, "/share/SIMPLE.TXT", 6);
    assert_lookup_file(&vol, "/./share/long-name.txt", 3);
    assert_lookup_file(&vol, "/../share/long-name.txt", 3);
    assert_lookup_file(&vol, "/share/./long-name.txt", 3);
    assert_lookup_file(&vol, "/share/SUBDIR/../long-name.txt", 3);

    memset(&entry, 0, sizeof(entry));
    assert(fat32_lookup_path(&vol, "/share", &entry) == 0);
    assert((entry.attr & FAT32_ATTR_DIRECTORY) != 0);

    memset(&entry, 0, sizeof(entry));
    assert(fat32_lookup_path(&vol, "/share/SIMPLE.TXT", &entry) == 0);
    assert(entry.dir_entry_offset != 0);
    image_read(entry.dir_entry_offset, raw_entry, sizeof(raw_entry));
    assert(memcmp(raw_entry, "SIMPLE  TXT", 11) == 0);

    assert(fat32_lookup_path(&vol, "/share/long-name.txt/nope", &entry) == -1);
    assert(fat32_lookup_path(&vol, "/share/missing.txt", &entry) == -1);
    assert(fat32_lookup_path(&vol, "/share/"
                                   "this-component-name-is-longer-than-the-fat-lfn-maximum-"
                                   "this-component-name-is-longer-than-the-fat-lfn-maximum-"
                                   "this-component-name-is-longer-than-the-fat-lfn-maximum-"
                                   "this-component-name-is-longer-than-the-fat-lfn-maximum-"
                                   "this-component-name-is-longer-than-the-fat-lfn-maximum.txt",
                             &entry) == -1);

    fat32_test_disk_close();
}

static uint32_t mount_and_find_share(Fat32Volume *vol)
{
    Fat32DirEntry share;

    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, vol) == 0);
    assert(fat32_lookup_path(vol, "/share", &share) == 0);
    assert((share.attr & FAT32_ATTR_DIRECTORY) != 0);
    fat32_test_disk_close();
    return share.first_cluster;
}

static void test_malformed_lfn_checksum_falls_back_to_short_alias(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    uint32_t share_cluster;
    uint64_t lfn_start;
    uint64_t short_offset;
    unsigned lfn_count;
    uint8_t short_entry[FAT32_DIR_ENTRY_SIZE];
    uint8_t bad_checksum;
    char alias_path[64];

    build_lookup_image();
    share_cluster = mount_and_find_share(&vol);
    assert(find_lfn_entry(&vol, share_cluster, "long-name.txt", &lfn_start, &lfn_count,
                          &short_offset, short_entry) == 0);
    assert(lfn_count > 0);

    image_read(lfn_start + 13u, &bad_checksum, sizeof(bad_checksum));
    bad_checksum ^= 0x55u;
    image_write(lfn_start + 13u, &bad_checksum, sizeof(bad_checksum));
    short_alias_to_share_path(short_entry, alias_path);

    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/share/long-name.txt", &entry) == -1);
    assert(fat32_lookup_path(&vol, alias_path, &entry) == 0);
    assert(entry.size == 3);
    fat32_test_disk_close();
}

static void test_out_of_order_lfn_falls_back_to_short_alias(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    uint32_t share_cluster;
    uint64_t lfn_start;
    uint64_t short_offset;
    unsigned lfn_count;
    uint8_t short_entry[FAT32_DIR_ENTRY_SIZE];
    uint8_t second[FAT32_DIR_ENTRY_SIZE];
    uint8_t third[FAT32_DIR_ENTRY_SIZE];
    char alias_path[64];

    build_lookup_image();
    share_cluster = mount_and_find_share(&vol);
    assert(find_lfn_entry(&vol, share_cluster, "three-part-long-file-name.txt", &lfn_start,
                          &lfn_count, &short_offset, short_entry) == 0);
    assert(lfn_count == 3);

    image_read(lfn_start + FAT32_DIR_ENTRY_SIZE, second, sizeof(second));
    image_read(lfn_start + 2u * FAT32_DIR_ENTRY_SIZE, third, sizeof(third));
    image_write(lfn_start + FAT32_DIR_ENTRY_SIZE, third, sizeof(third));
    image_write(lfn_start + 2u * FAT32_DIR_ENTRY_SIZE, second, sizeof(second));
    short_alias_to_share_path(short_entry, alias_path);

    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/share/three-part-long-file-name.txt", &entry) == -1);
    assert(fat32_lookup_path(&vol, alias_path, &entry) == 0);
    assert(entry.size == 6);
    fat32_test_disk_close();
}

static void test_deleted_and_volume_label_entries_do_not_hide_valid_files(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    uint32_t share_cluster;
    uint64_t simple_offset;
    uint8_t simple_entry[FAT32_DIR_ENTRY_SIZE];
    uint8_t deleted_entry[FAT32_DIR_ENTRY_SIZE];
    uint8_t label_entry[FAT32_DIR_ENTRY_SIZE];

    build_lookup_image();
    share_cluster = mount_and_find_share(&vol);
    assert(find_entry_by_short_name(&vol, share_cluster, (const uint8_t *)"SIMPLE  TXT",
                                    &simple_offset, simple_entry) == 0);

    memset(deleted_entry, 0, sizeof(deleted_entry));
    memset(label_entry, 0, sizeof(label_entry));
    deleted_entry[0] = 0xe5;
    memcpy(label_entry, "FAKE LABEL ", 11);
    label_entry[11] = FAT32_ATTR_VOLUME_ID;

    image_write(simple_offset, deleted_entry, sizeof(deleted_entry));
    image_write(simple_offset + FAT32_DIR_ENTRY_SIZE, label_entry, sizeof(label_entry));
    image_write(simple_offset + 2u * FAT32_DIR_ENTRY_SIZE, simple_entry, sizeof(simple_entry));

    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/share/SIMPLE.TXT", &entry) == 0);
    assert(entry.size == 6);
    fat32_test_disk_close();
}

static void test_directory_chain_cycle_returns_error(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    uint32_t share_cluster;

    build_lookup_image();
    share_cluster = mount_and_find_share(&vol);
    overwrite_fat_entry(&vol, share_cluster, share_cluster);

    fat32_test_disk_open(LOOKUP_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    vol.cluster_count = share_cluster;
    assert(fat32_lookup_path(&vol, "/share/not-present-after-cycle.txt", &entry) == -1);
    fat32_test_disk_close();
}

int main(void)
{
    test_lookup_paths();
    test_malformed_lfn_checksum_falls_back_to_short_alias();
    test_out_of_order_lfn_falls_back_to_short_alias();
    test_deleted_and_volume_label_entries_do_not_hide_valid_files();
    test_directory_chain_cycle_returns_error();
    puts("fat32_lookup tests passed");
    return 0;
}
