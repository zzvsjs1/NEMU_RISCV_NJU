#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/fs/fat32.h"

void fat32_test_disk_open(const char *path);
void fat32_test_disk_close(void);
void fat32_test_reset_backend(void);
size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

#define WRITE_IMAGE "fat32-write-work/ramdisk.img"
#define FAT32_SECTOR_SIZE 512u
#define FAT32_ENTRY_MASK 0x0fffffffu

typedef struct
{
    /*
   * FSI_Free_Count copied from the FSInfo sector.  The spec treats this as
   * advisory, but the writer updates it so tests can detect allocation leaks.
   */
    uint32_t free_count;
    /*
   * FSI_Nxt_Free copied from the FSInfo sector.  The allocator uses it only as
   * a starting hint, then validates candidate clusters through the FAT.
   */
    uint32_t next_free;
} FsInfoSnapshot;

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void image_read(size_t offset, void *buf, size_t size)
{
    FILE *image = fopen(WRITE_IMAGE, "rb");

    assert(image != 0);
    assert(fseek(image, (long)offset, SEEK_SET) == 0);
    assert(fread(buf, 1, size, image) == size);
    assert(fclose(image) == 0);
}

static void image_write(size_t offset, const void *buf, size_t size)
{
    FILE *image = fopen(WRITE_IMAGE, "rb+");

    assert(image != 0);
    assert(fseek(image, (long)offset, SEEK_SET) == 0);
    assert(fwrite(buf, 1, size, image) == size);
    assert(fclose(image) == 0);
}

static void write_text_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");

    assert(file != 0);
    assert(fwrite(content, 1, strlen(content), file) == strlen(content));
    assert(fclose(file) == 0);
}

static void write_pattern_file(const char *path, size_t size)
{
    FILE *file = fopen(path, "wb");

    assert(file != 0);
    for (size_t i = 0; i < size; i++)
    {
        assert(fputc((int)(i & 0xffu), file) != EOF);
    }
    assert(fclose(file) == 0);
}

static void build_write_image(void)
{
    assert(system("rm -rf fat32-write-work && mkdir -p fat32-write-work/fsimg") == 0);
    write_text_file("fat32-write-work/fsimg/SMALL.TXT", "small");
    write_text_file("fat32-write-work/fsimg/EMPTY.TXT", "");
    write_pattern_file("fat32-write-work/fsimg/BIG.BIN", 900);
    assert(system("python3 ../../../navy-apps/scripts/mkfat32.py "
                  "fat32-write-work/fsimg " WRITE_IMAGE) == 0);
}

static size_t fat_entry_offset(const Fat32Volume *vol, unsigned fat_index, uint32_t cluster)
{
    return ((size_t)vol->reserved_sector_count + (size_t)fat_index * (size_t)vol->fat_size_sectors) * FAT32_SECTOR_SIZE + (size_t)cluster * 4u;
}

static uint32_t read_raw_fat_entry(const Fat32Volume *vol, unsigned fat_index, uint32_t cluster)
{
    uint8_t raw[4];

    image_read(fat_entry_offset(vol, fat_index, cluster), raw, sizeof(raw));
    return get_le32(raw);
}

static void write_raw_fat_entry(const Fat32Volume *vol, unsigned fat_index, uint32_t cluster,
                                uint32_t value)
{
    uint8_t raw[4];

    put_le32(raw, value);
    image_write(fat_entry_offset(vol, fat_index, cluster), raw, sizeof(raw));
}

static void overwrite_bpb_ext_flags(uint16_t ext_flags)
{
    uint8_t boot[FAT32_SECTOR_SIZE];

    image_read(0, boot, sizeof(boot));
    put_le16(&boot[40], ext_flags);
    image_write(0, boot, sizeof(boot));
}

static void set_entry_attr(const Fat32Volume *vol, const char *path, uint8_t attr)
{
    Fat32DirEntry entry;
    uint8_t raw[32];

    assert(fat32_lookup_path(vol, path, &entry) == 0);
    image_read((size_t)entry.dir_entry_offset, raw, sizeof(raw));
    raw[11] = attr;
    image_write((size_t)entry.dir_entry_offset, raw, sizeof(raw));
}

static size_t cluster_byte_offset(const Fat32Volume *vol, uint32_t cluster)
{
    uint32_t sector;

    assert(fat32_first_sector_of_cluster(vol, cluster, &sector) == 0);
    return (size_t)sector * FAT32_SECTOR_SIZE;
}

static FsInfoSnapshot read_fsinfo_sector(uint32_t sector)
{
    uint8_t raw[FAT32_SECTOR_SIZE];
    FsInfoSnapshot info;

    image_read((size_t)sector * FAT32_SECTOR_SIZE, raw, sizeof(raw));
    assert(get_le32(&raw[0]) == 0x41615252u);
    assert(get_le32(&raw[484]) == 0x61417272u);
    assert(get_le32(&raw[508]) == 0xaa550000u);

    info.free_count = get_le32(&raw[488]);
    info.next_free = get_le32(&raw[492]);
    return info;
}

static void assert_range_is_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        assert(buf[i] == 0);
    }
}

static void expect_pattern_with_patch(const uint8_t *buf, size_t size, size_t patch_offset,
                                      const uint8_t *patch, size_t patch_size)
{
    for (size_t i = 0; i < size; i++)
    {
        if (i >= patch_offset && i < patch_offset + patch_size)
        {
            assert(buf[i] == patch[i - patch_offset]);
        }
        else
        {
            assert(buf[i] == (uint8_t)(i & 0xffu));
        }
    }
}

static uint32_t count_chain(const Fat32Volume *vol, uint32_t first_cluster)
{
    uint32_t count = 0;
    uint32_t cluster = first_cluster;

    if (first_cluster == 0)
    {
        return 0;
    }

    for (uint32_t links = 0; links < vol->cluster_count; links++)
    {
        uint32_t next;

        count++;
        assert(fat32_read_fat_entry(vol, cluster, &next) == 0);
        if (fat32_is_end_of_chain(next))
        {
            return count;
        }
        cluster = next;
    }

    assert(!"FAT chain cycle");
    return 0;
}

static void reopen_backend_image(void)
{
    fat32_test_disk_open(WRITE_IMAGE);
    fat32_test_reset_backend();
    assert(fat32_backend_init() == 0);
}

static void test_fat_entry_write_updates_all_mirrored_fats(void)
{
    Fat32Volume vol;

    build_write_image();
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);

    assert(fat32_write_fat_entry(&vol, 3, 0x0fffffffu) == 0);
    fat32_test_disk_close();

    assert((read_raw_fat_entry(&vol, 0, 3) & FAT32_ENTRY_MASK) == 0x0fffffffu);
    assert((read_raw_fat_entry(&vol, 1, 3) & FAT32_ENTRY_MASK) == 0x0fffffffu);
}

static void test_fat_entry_write_preserves_high_nibble_per_copy(void)
{
    Fat32Volume vol;

    build_write_image();
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    fat32_test_disk_close();

    write_raw_fat_entry(&vol, 0, 3, 0xa0000000u);
    write_raw_fat_entry(&vol, 1, 3, 0xb0000000u);

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_write_fat_entry(&vol, 3, 0x00000004u) == 0);
    fat32_test_disk_close();

    assert(read_raw_fat_entry(&vol, 0, 3) == 0xa0000004u);
    assert(read_raw_fat_entry(&vol, 1, 3) == 0xb0000004u);
}

static void test_active_fat_mode_writes_and_reads_only_active_copy(void)
{
    Fat32Volume vol;
    uint32_t next;
    uint32_t fat0_before;

    build_write_image();
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    fat32_test_disk_close();

    fat0_before = read_raw_fat_entry(&vol, 0, 3);
    write_raw_fat_entry(&vol, 1, 3, 0x00000005u);
    overwrite_bpb_ext_flags(0x0081u);

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(vol.mirror_fats == 0);
    assert(vol.active_fat == 1);
    assert(fat32_read_fat_entry(&vol, 3, &next) == 0);
    assert(next == 5);
    assert(fat32_write_fat_entry(&vol, 3, 0x0fffffffu) == 0);
    fat32_test_disk_close();

    assert(read_raw_fat_entry(&vol, 0, 3) == fat0_before);
    assert((read_raw_fat_entry(&vol, 1, 3) & FAT32_ENTRY_MASK) == 0x0fffffffu);
}

static void test_alloc_cluster_marks_fat_zero_fills_and_updates_fsinfo(void)
{
    Fat32Volume vol;
    FsInfoSnapshot before;
    FsInfoSnapshot after_primary;
    FsInfoSnapshot after_backup;
    uint32_t cluster;
    uint8_t data[FAT32_SECTOR_SIZE];

    build_write_image();
    before = read_fsinfo_sector(1);
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(vol.fsinfo_valid == 1);
    assert(vol.free_cluster_count == before.free_count);

    assert(fat32_alloc_cluster(&vol, 0, &cluster) == 0);
    fat32_test_disk_close();

    assert(cluster >= 2);
    assert((read_raw_fat_entry(&vol, 0, cluster) & FAT32_ENTRY_MASK) == 0x0fffffffu);
    assert((read_raw_fat_entry(&vol, 1, cluster) & FAT32_ENTRY_MASK) == 0x0fffffffu);
    image_read(cluster_byte_offset(&vol, cluster), data, sizeof(data));
    assert_range_is_zero(data, sizeof(data));

    after_primary = read_fsinfo_sector(1);
    after_backup = read_fsinfo_sector(vol.backup_boot_sector + vol.fsinfo_sector);
    assert(after_primary.free_count == before.free_count - 1u);
    assert(after_backup.free_count == after_primary.free_count);
    assert(after_backup.next_free == after_primary.next_free);
}

static void test_alloc_cluster_rolls_back_fat_when_zeroing_fails(void)
{
    Fat32Volume vol;
    uint32_t expected_cluster;
    uint32_t allocated = 0;

    build_write_image();
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    expected_cluster = vol.next_free_cluster;
    assert(expected_cluster >= 2);

    /*
   * Corrupt only the in-memory data-region position.  FAT reads/writes still
   * address the real FAT, but zeroing the chosen cluster must fail before the
   * cluster is exposed as allocated.
   */
    vol.first_data_sector = UINT32_MAX;
    assert(fat32_alloc_cluster(&vol, 0, &allocated) == -1);
    fat32_test_disk_close();

    assert((read_raw_fat_entry(&vol, 0, expected_cluster) & FAT32_ENTRY_MASK) == 0);
    assert((read_raw_fat_entry(&vol, 1, expected_cluster) & FAT32_ENTRY_MASK) == 0);
}

static void test_free_chain_clears_fat_entries_and_restores_fsinfo(void)
{
    Fat32Volume vol;
    FsInfoSnapshot before;
    FsInfoSnapshot after_alloc;
    FsInfoSnapshot after_free;
    uint32_t first;
    uint32_t second;

    build_write_image();
    before = read_fsinfo_sector(1);
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_alloc_cluster(&vol, 0, &first) == 0);
    assert(fat32_alloc_cluster(&vol, first, &second) == 0);
    assert(fat32_write_fat_entry(&vol, first, second) == 0);
    fat32_test_disk_close();

    after_alloc = read_fsinfo_sector(1);
    assert(after_alloc.free_count == before.free_count - 2u);

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_free_chain(&vol, first) == 0);
    fat32_test_disk_close();

    assert((read_raw_fat_entry(&vol, 0, first) & FAT32_ENTRY_MASK) == 0);
    assert((read_raw_fat_entry(&vol, 0, second) & FAT32_ENTRY_MASK) == 0);
    assert((read_raw_fat_entry(&vol, 1, first) & FAT32_ENTRY_MASK) == 0);
    assert((read_raw_fat_entry(&vol, 1, second) & FAT32_ENTRY_MASK) == 0);
    after_free = read_fsinfo_sector(1);
    assert(after_free.free_count == before.free_count);
    assert(after_free.next_free <= first);
}

static void test_backend_overwrites_existing_file_without_changing_size(void)
{
    Fat32File file;
    uint8_t buf[900];
    const uint8_t patch[] = {
        0xde,
        0xad,
        0xbe,
        0xef,
        0xca,
        0xfe,
        0x11,
        0x22,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xaa,
    };

    build_write_image();
    reopen_backend_image();
    assert(fat32_backend_open("/BIG.BIN", &file) == 0);
    assert(fat32_backend_write(&file, 508, patch, sizeof(patch)) == sizeof(patch));
    assert(fat32_backend_size(&file) == 900);

    memset(buf, 0, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    expect_pattern_with_patch(buf, sizeof(buf), 508, patch, sizeof(patch));
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

static void test_backend_appends_inside_existing_cluster_capacity(void)
{
    Fat32Volume vol;
    Fat32File file;
    FsInfoSnapshot before;
    FsInfoSnapshot after;
    uint8_t data[100];

    memset(data, 0x5a, sizeof(data));
    build_write_image();
    before = read_fsinfo_sector(1);
    reopen_backend_image();
    assert(fat32_backend_open("/BIG.BIN", &file) == 0);
    assert(fat32_backend_write(&file, 900, data, sizeof(data)) == sizeof(data));
    assert(fat32_backend_size(&file) == 1000);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/BIG.BIN", &(Fat32DirEntry){0}) == 0);
    fat32_test_disk_close();

    after = read_fsinfo_sector(1);
    assert(after.free_count == before.free_count);
    assert(after.next_free == before.next_free);
}

static void test_backend_appends_and_allocates_new_cluster(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    Fat32File file;
    FsInfoSnapshot before;
    FsInfoSnapshot after;
    uint8_t data[300];

    memset(data, 0x6b, sizeof(data));
    build_write_image();
    before = read_fsinfo_sector(1);
    reopen_backend_image();
    assert(fat32_backend_open("/BIG.BIN", &file) == 0);
    assert(fat32_backend_write(&file, 900, data, sizeof(data)) == sizeof(data));
    assert(fat32_backend_size(&file) == 1200);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/BIG.BIN", &entry) == 0);
    assert(entry.size == 1200);
    assert(count_chain(&vol, entry.first_cluster) == 3);
    fat32_test_disk_close();

    after = read_fsinfo_sector(1);
    assert(after.free_count == before.free_count - 1u);
}

static void test_backend_extends_empty_file_with_zero_gap(void)
{
    Fat32File file;
    uint8_t buf[40];
    const uint8_t data[] = {0x12, 0x34, 0x56};

    build_write_image();
    reopen_backend_image();
    assert(fat32_backend_open("/EMPTY.TXT", &file) == 0);
    assert(fat32_backend_write(&file, 32, data, sizeof(data)) == sizeof(data));
    assert(fat32_backend_size(&file) == 35);

    memset(buf, 0xa5, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == 35);
    assert_range_is_zero(buf, 32);
    assert(memcmp(&buf[32], data, sizeof(data)) == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

static void test_backend_truncates_existing_file_to_zero(void)
{
    Fat32Volume vol;
    Fat32DirEntry entry;
    Fat32File file;
    uint32_t first_cluster;

    build_write_image();
    reopen_backend_image();
    assert(fat32_backend_open("/BIG.BIN", &file) == 0);
    first_cluster = file.first_cluster;
    assert(first_cluster >= 2);
    assert(fat32_backend_truncate(&file, 0) == 0);
    assert(fat32_backend_size(&file) == 0);
    assert(file.first_cluster == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();

    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    assert(fat32_lookup_path(&vol, "/BIG.BIN", &entry) == 0);
    assert(entry.size == 0);
    assert(entry.first_cluster == 0);
    fat32_test_disk_close();

    assert((read_raw_fat_entry(&vol, 0, first_cluster) & FAT32_ENTRY_MASK) == 0);
}

static void test_backend_opens_read_only_file_but_refuses_write(void)
{
    Fat32Volume vol;
    Fat32File file;
    uint8_t buf[5];
    const uint8_t patch = 0xff;

    build_write_image();
    fat32_test_disk_open(WRITE_IMAGE);
    assert(fat32_mount_from_disk(512, &vol) == 0);
    set_entry_attr(&vol, "/SMALL.TXT", FAT32_ATTR_READ_ONLY | FAT32_ATTR_ARCHIVE);
    fat32_test_disk_close();

    reopen_backend_image();
    assert(fat32_backend_open("/SMALL.TXT", &file) == 0);
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    assert(memcmp(buf, "small", sizeof(buf)) == 0);
    assert(fat32_backend_write(&file, 0, &patch, sizeof(patch)) == 0);
    assert(fat32_backend_truncate(&file, 0) == -1);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

int main(void)
{
    test_fat_entry_write_updates_all_mirrored_fats();
    test_fat_entry_write_preserves_high_nibble_per_copy();
    test_active_fat_mode_writes_and_reads_only_active_copy();
    test_alloc_cluster_marks_fat_zero_fills_and_updates_fsinfo();
    test_alloc_cluster_rolls_back_fat_when_zeroing_fails();
    test_free_chain_clears_fat_entries_and_restores_fsinfo();
    test_backend_overwrites_existing_file_without_changing_size();
    test_backend_appends_inside_existing_cluster_capacity();
    test_backend_appends_and_allocates_new_cluster();
    test_backend_extends_empty_file_with_zero_gap();
    test_backend_truncates_existing_file_to_zero();
    test_backend_opens_read_only_file_but_refuses_write();
    puts("fat32_write tests passed");
    return 0;
}
