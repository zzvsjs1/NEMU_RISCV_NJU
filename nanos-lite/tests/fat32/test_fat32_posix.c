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

#define POSIX_IMAGE "fat32-posix-work/ramdisk.img"
#define FAT32_SECTOR_SIZE 512u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define LARGE_IMAGE_FILLER_SIZE (70000u * FAT32_SECTOR_SIZE)

/*
 * Decode a little-endian 32-bit value from raw on-disk FAT or FSInfo bytes.
 */
static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Write a small text file into the host-side source tree used by mkfat32.py.
 */
static void write_text_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");

    assert(file != 0);
    assert(fwrite(content, 1, strlen(content), file) == strlen(content));
    assert(fclose(file) == 0);
}

/*
 * Write a deterministic byte pattern so later reads can prove truncation keeps
 * the surviving prefix unchanged.
 */
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

/*
 * Create a large sparse host file.  The generated FAT32 image still contains
 * the full zero-filled data stream, but preparing the source tree stays quick
 * enough for a regression test.
 */
static void write_sparse_file(const char *path, size_t size)
{
    FILE *file = fopen(path, "wb");

    assert(file != 0);
    if (size > 0)
    {
        assert(fseek(file, (long)(size - 1u), SEEK_SET) == 0);
        assert(fputc(0, file) != EOF);
    }
    assert(fclose(file) == 0);
}

/*
 * Build a FAT32 image with a mix of short names, long names, files, and
 * directories.  This keeps each POSIX-like operation test independent.
 */
static void build_posix_image(void)
{
    assert(system("rm -rf fat32-posix-work && mkdir -p fat32-posix-work/fsimg/DIR fat32-posix-work/fsimg/EMPTYDIR") == 0);
    write_text_file("fat32-posix-work/fsimg/DIR/ALPHA.TXT", "alpha");
    write_text_file("fat32-posix-work/fsimg/DIR/Long File.txt", "long");
    write_pattern_file("fat32-posix-work/fsimg/DIR/BIG.BIN", 900);
    assert(system("python3 ../../../navy-apps/scripts/mkfat32.py fat32-posix-work/fsimg " POSIX_IMAGE) == 0);
}

/*
 * Open the generated image and remount the backend so every test starts from a
 * fresh in-memory FAT32 volume cache.
 */
static void reopen_backend_image(void)
{
    fat32_test_disk_open(POSIX_IMAGE);
    fat32_test_reset_backend();
    assert(fat32_backend_init() == 0);
}

/*
 * Read raw bytes directly from the generated image.  Tests use this for FAT and
 * FSInfo checks that should not depend on backend caches.
 */
static void image_read(size_t offset, void *buf, size_t size)
{
    FILE *image = fopen(POSIX_IMAGE, "rb");

    assert(image != 0);
    assert(fseek(image, (long)offset, SEEK_SET) == 0);
    assert(fread(buf, 1, size, image) == size);
    assert(fclose(image) == 0);
}

/*
 * Locate the raw FAT slot for a cluster in a selected FAT copy.
 */
static size_t fat_entry_offset(const Fat32Volume *vol, unsigned fat_index, uint32_t cluster)
{
    return ((size_t)vol->reserved_sector_count + (size_t)fat_index * (size_t)vol->fat_size_sectors) * FAT32_SECTOR_SIZE + (size_t)cluster * 4u;
}

/*
 * Read one raw FAT32 entry without applying the driver's low-28-bit mask.
 */
static uint32_t read_raw_fat_entry(const Fat32Volume *vol, unsigned fat_index, uint32_t cluster)
{
    uint8_t raw[4];

    image_read(fat_entry_offset(vol, fat_index, cluster), raw, sizeof(raw));
    return get_le32(raw);
}

/*
 * Check that every byte in a range is zero.  FAT32 must zero newly allocated
 * clusters and POSIX truncate growth must expose zero-filled holes.
 */
static void assert_range_is_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        assert(buf[i] == 0);
    }
}

/*
 * Count the visible names returned by the FAT32 directory iterator.
 */
static unsigned collect_directory_names(Fat32Dir *dir, char names[][FAT32_MAX_NAME + 1u], unsigned capacity)
{
    Fat32Dirent entry;
    unsigned count = 0;

    while (fat32_backend_readdir(dir, &entry) == 1)
    {
        assert(count < capacity);
        strcpy(names[count], entry.name);
        count++;
    }

    return count;
}

/*
 * Test that the iterator exposes normal short entries and validated long-name
 * entries, but never leaks raw LFN slots as separate directory names.
 */
static void test_readdir_returns_short_and_long_names(void)
{
    Fat32Dir dir;
    char names[8][FAT32_MAX_NAME + 1u];
    unsigned count;
    int saw_alpha = 0;
    int saw_long = 0;

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_opendir("/DIR", &dir) == 0);
    count = collect_directory_names(&dir, names, 8);
    assert(count >= 4);

    for (unsigned i = 0; i < count; i++)
    {
        saw_alpha |= strcmp(names[i], "alpha.txt") == 0;
        saw_long |= strcmp(names[i], "Long File.txt") == 0;
    }

    assert(saw_alpha);
    assert(saw_long);
    fat32_test_disk_close();
}

/*
 * Test regular-file creation with a long lowercase POSIX name, followed by a
 * normal write/read round trip through the existing file backend.
 */
static void test_create_file_with_lfn_then_write_and_read(void)
{
    Fat32File file;
    Fat32DirEntry entry;
    uint8_t buf[4];
    const uint8_t data[] = {'s', 'a', 'v', 'e'};

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_create("/DIR/new-save.dat", &file) == 0);
    assert(fat32_backend_write(&file, 0, data, sizeof(data)) == sizeof(data));
    assert(fat32_backend_close(&file) == 0);
    assert(fat32_lookup_path(fat32_backend_volume(), "/DIR/new-save.dat", &entry) == 0);
    assert(entry.size == sizeof(data));

    assert(fat32_backend_open("/DIR/new-save.dat", &file) == 0);
    memset(buf, 0, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    assert(memcmp(buf, data, sizeof(data)) == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

/*
 * Test the real ONScripter failure mode: a large fsimg can consume more than
 * the historical minimum FAT32 data area, so mkfat32.py must still leave free
 * clusters for runtime save files such as /share/games/ons/save2.dat.
 */
static void test_large_image_keeps_runtime_save_space(void)
{
    Fat32File file;
    Fat32DirEntry entry;
    uint8_t buf[512];
    uint8_t out[sizeof(buf)];

    assert(system("rm -rf fat32-posix-work && mkdir -p fat32-posix-work/fsimg/share/games/ons") == 0);
    write_sparse_file("fat32-posix-work/fsimg/share/games/ons/arc.nsa", LARGE_IMAGE_FILLER_SIZE);
    write_text_file("fat32-posix-work/fsimg/share/games/ons/save1.dat", "existing");
    assert(system("python3 ../../../navy-apps/scripts/mkfat32.py fat32-posix-work/fsimg " POSIX_IMAGE) == 0);
    reopen_backend_image();

    for (size_t i = 0; i < sizeof(buf); i++)
    {
        buf[i] = (uint8_t)(i & 0xffu);
    }

    assert(fat32_backend_create("/share/games/ons/save2.dat", &file) == 0);
    assert(fat32_backend_write(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    assert(fat32_backend_close(&file) == 0);
    assert(fat32_lookup_path(fat32_backend_volume(), "/share/games/ons/save2.dat", &entry) == 0);
    assert(entry.size == sizeof(buf));

    assert(fat32_backend_open("/share/games/ons/save2.dat", &file) == 0);
    memset(out, 0, sizeof(out));
    assert(fat32_backend_read(&file, 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, buf, sizeof(buf)) == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

/*
 * Test mkdir/rmdir rules: a new directory has dot entries, non-empty removal
 * fails, and empty removal deletes the path.
 */
static void test_mkdir_and_rmdir_follow_directory_rules(void)
{
    Fat32Dir dir;
    Fat32DirEntry entry;
    Fat32File child;

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_mkdir("/DIR/save slots") == 0);
    assert(fat32_lookup_path(fat32_backend_volume(), "/DIR/save slots", &entry) == 0);
    assert((entry.attr & FAT32_ATTR_DIRECTORY) != 0);

    assert(fat32_backend_opendir("/DIR/save slots", &dir) == 0);
    assert(fat32_backend_readdir(&dir, &(Fat32Dirent){0}) == 1);
    assert(fat32_backend_readdir(&dir, &(Fat32Dirent){0}) == 1);
    assert(fat32_backend_readdir(&dir, &(Fat32Dirent){0}) == 0);

    assert(fat32_backend_create("/DIR/save slots/child.txt", &child) == 0);
    assert(fat32_backend_close(&child) == 0);
    assert(fat32_backend_rmdir("/DIR/save slots") == -1);
    assert(fat32_backend_unlink("/DIR/save slots/child.txt") == 0);
    assert(fat32_backend_rmdir("/DIR/save slots") == 0);
    assert(fat32_lookup_path(fat32_backend_volume(), "/DIR/save slots", &entry) == -1);
    fat32_test_disk_close();
}

/*
 * Test unlink on a multi-cluster file: the directory entry disappears and the
 * FAT chain is released in both mirrored FAT copies.
 */
static void test_unlink_regular_file_frees_clusters(void)
{
    Fat32DirEntry entry;
    Fat32Volume snapshot;
    uint32_t first_cluster;

    build_posix_image();
    reopen_backend_image();

    assert(fat32_lookup_path(fat32_backend_volume(), "/DIR/BIG.BIN", &entry) == 0);
    snapshot = *fat32_backend_volume();
    first_cluster = entry.first_cluster;
    assert(first_cluster >= 2);

    assert(fat32_backend_unlink("/DIR/BIG.BIN") == 0);
    assert(fat32_lookup_path(fat32_backend_volume(), "/DIR/BIG.BIN", &entry) == -1);
    fat32_test_disk_close();

    assert((read_raw_fat_entry(&snapshot, 0, first_cluster) & FAT32_ENTRY_MASK) == 0);
    assert((read_raw_fat_entry(&snapshot, 1, first_cluster) & FAT32_ENTRY_MASK) == 0);
}

/*
 * Test POSIX truncation beyond the current limited truncate-to-zero support:
 * shrinking frees the tail chain and growing exposes zero-filled bytes.
 */
static void test_truncate_shrinks_and_extends_with_zero_fill(void)
{
    Fat32File file;
    uint8_t buf[700];

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_open("/DIR/BIG.BIN", &file) == 0);
    assert(fat32_backend_truncate(&file, 100) == 0);
    assert(fat32_backend_size(&file) == 100);
    assert(fat32_backend_truncate(&file, 700) == 0);
    assert(fat32_backend_size(&file) == 700);

    memset(buf, 0xa5, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    for (size_t i = 0; i < 100; i++)
    {
        assert(buf[i] == (uint8_t)(i & 0xffu));
    }
    assert_range_is_zero(&buf[100], 600);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

/*
 * Test rename by moving a regular file to a new long name in the same
 * directory without changing its file data.
 */
static void test_rename_regular_file_preserves_data(void)
{
    Fat32File file;
    uint8_t buf[5];

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_rename("/DIR/ALPHA.TXT", "/DIR/renamed alpha.txt") == 0);
    assert(fat32_backend_open("/DIR/ALPHA.TXT", &file) == -1);
    assert(fat32_backend_open("/DIR/renamed alpha.txt", &file) == 0);
    memset(buf, 0, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    assert(memcmp(buf, "alpha", sizeof(buf)) == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

/*
 * Test POSIX rename replacement: an existing regular-file target is removed and
 * the source file's data becomes visible at the target path.
 */
static void test_rename_replaces_existing_regular_file(void)
{
    Fat32File file;
    uint8_t buf[5];
    const uint8_t old_target[] = {'o', 'l', 'd'};

    build_posix_image();
    reopen_backend_image();

    assert(fat32_backend_create("/DIR/target.txt", &file) == 0);
    assert(fat32_backend_write(&file, 0, old_target, sizeof(old_target)) == sizeof(old_target));
    assert(fat32_backend_close(&file) == 0);

    assert(fat32_backend_rename("/DIR/ALPHA.TXT", "/DIR/target.txt") == 0);
    assert(fat32_backend_open("/DIR/ALPHA.TXT", &file) == -1);
    assert(fat32_backend_open("/DIR/target.txt", &file) == 0);
    memset(buf, 0, sizeof(buf));
    assert(fat32_backend_read(&file, 0, buf, sizeof(buf)) == sizeof(buf));
    assert(memcmp(buf, "alpha", sizeof(buf)) == 0);
    assert(fat32_backend_close(&file) == 0);
    fat32_test_disk_close();
}

/*
 * Run all POSIX-like FAT32 backend behaviour tests.
 */
int main(void)
{
    test_readdir_returns_short_and_long_names();
    test_create_file_with_lfn_then_write_and_read();
    test_large_image_keeps_runtime_save_space();
    test_mkdir_and_rmdir_follow_directory_rules();
    test_unlink_regular_file_frees_clusters();
    test_truncate_shrinks_and_extends_with_zero_fill();
    test_rename_regular_file_preserves_data();
    test_rename_replaces_existing_regular_file();
    puts("fat32_posix tests passed");
    return 0;
}
