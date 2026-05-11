#include "backend.h"
#include "fat32.h"

#ifndef SEEK_SET
enum
{
    SEEK_SET,
    SEEK_CUR,
    SEEK_END
};
#endif

/*
 * fs.c stores regular-file metadata through the backend-neutral FsFile type,
 * while the FAT32 implementation works with Fat32File.  Convert by value at
 * each call boundary so the generic descriptor table stays independent from
 * FAT32 headers, then copy back any changed size, cluster, or cache fields.
 */
static Fat32File to_fat32_file(const FsFile *file)
{
    Fat32File fat_file;

    fat_file.attr = file->u.fat32.attr;
    fat_file.first_cluster = file->u.fat32.first_cluster;
    fat_file.size = (uint32_t)file->size;
    fat_file.dir_entry_offset = file->u.fat32.dir_entry_offset;
    fat_file.cached_cluster_index = file->u.fat32.cached_cluster_index;
    fat_file.cached_cluster = file->u.fat32.cached_cluster;
    fat_file.contiguous_cluster_count = file->u.fat32.contiguous_cluster_count;
    return fat_file;
}

/*
 * Copy FAT32 file state back into the backend-neutral descriptor snapshot.
 */
static void from_fat32_file(FsFile *file, const Fat32File *fat_file)
{
    file->size = fat_file->size;
    file->u.fat32.attr = fat_file->attr;
    file->u.fat32.first_cluster = fat_file->first_cluster;
    file->u.fat32.dir_entry_offset = fat_file->dir_entry_offset;
    file->u.fat32.cached_cluster_index = fat_file->cached_cluster_index;
    file->u.fat32.cached_cluster = fat_file->cached_cluster;
    file->u.fat32.contiguous_cluster_count = fat_file->contiguous_cluster_count;
}

/*
 * Convert backend-neutral directory iterator state into FAT32 iterator state.
 */
static Fat32Dir to_fat32_dir(const FsDir *dir)
{
    Fat32Dir fat_dir;

    fat_dir.first_cluster = dir->u.fat32.first_cluster;
    fat_dir.next_entry_index = dir->u.fat32.next_entry_index;
    return fat_dir;
}

/*
 * Copy FAT32 directory iterator progress back into the generic descriptor.
 */
static void from_fat32_dir(FsDir *dir, const Fat32Dir *fat_dir)
{
    dir->u.fat32.first_cluster = fat_dir->first_cluster;
    dir->u.fat32.next_entry_index = fat_dir->next_entry_index;
}

/*
 * Convert a FAT32 directory entry into generic metadata for fs.c.
 */
static void metadata_from_fat32_entry(FsMetadata *out, const Fat32DirEntry *entry)
{
    out->is_dir = (entry->attr & FAT32_ATTR_DIRECTORY) != 0;
    out->size = entry->size;
    out->inode = out->is_dir ? entry->first_cluster : (entry->dir_entry_offset != 0 ? entry->dir_entry_offset : entry->first_cluster);
}

/*
 * Mount the FAT32 backend.
 */
static int fat32_fs_init(void)
{
    return fat32_backend_init();
}

/*
 * Open an existing FAT32 regular file.
 */
static int fat32_fs_open(const char *path, FsFile *out)
{
    Fat32File fat_file;

    if (fat32_backend_open(path, &fat_file) != 0)
    {
        return -1;
    }

    from_fat32_file(out, &fat_file);
    return 0;
}

/*
 * Create a FAT32 regular file and export its opened-file snapshot to fs.c.
 */
static int fat32_fs_create(const char *path, FsFile *out)
{
    Fat32File fat_file;

    if (fat32_backend_create(path, &fat_file) != 0)
    {
        return -1;
    }

    from_fat32_file(out, &fat_file);
    return 0;
}

/*
 * Resolve FAT32 file or directory metadata for stat().
 */
static int fat32_fs_lookup(const char *path, FsMetadata *out)
{
    Fat32DirEntry entry;

    if (fat32_backend_lookup(path, &entry) != 0)
    {
        return -1;
    }

    metadata_from_fat32_entry(out, &entry);
    return 0;
}

/*
 * Open a FAT32 directory iterator for getdents().
 */
static int fat32_fs_opendir(const char *path, FsDir *out)
{
    Fat32Dir fat_dir;

    if (fat32_backend_opendir(path, &fat_dir) != 0)
    {
        return -1;
    }

    from_fat32_dir(out, &fat_dir);
    return 0;
}

/*
 * Return one FAT32 directory entry through the backend-neutral shape.
 */
static int fat32_fs_readdir(FsDir *dir, FsDirent *out)
{
    Fat32Dir fat_dir = to_fat32_dir(dir);
    Fat32Dirent fat_entry;
    const int ret = fat32_backend_readdir(&fat_dir, &fat_entry);

    from_fat32_dir(dir, &fat_dir);

    if (ret != 1)
    {
        return ret;
    }

    strcpy(out->name, fat_entry.name);
    metadata_from_fat32_entry(&out->metadata, &fat_entry.entry);
    return 1;
}

/*
 * Read bytes from a FAT32 regular file and persist any cache updates.
 */
static size_t fat32_fs_read(FsFile *file, size_t offset, void *buf, size_t len)
{
    Fat32File fat_file = to_fat32_file(file);
    const size_t ret = fat32_backend_read(&fat_file, offset, buf, len);

    from_fat32_file(file, &fat_file);
    return ret;
}

/*
 * Write bytes to a FAT32 regular file and persist size/cluster changes.
 */
static size_t fat32_fs_write(FsFile *file, size_t offset, const void *buf, size_t len)
{
    Fat32File fat_file = to_fat32_file(file);
    const size_t ret = fat32_backend_write(&fat_file, offset, buf, len);

    from_fat32_file(file, &fat_file);
    return ret;
}

/*
 * Convert POSIX seek arguments into a FAT32 regular-file offset.
 */
static size_t fat32_fs_lseek(FsFile *file, size_t current_offset, size_t offset, int whence)
{
    size_t new_offset = -1;

    switch (whence)
    {
    case SEEK_SET:
    {
        new_offset = offset;
        break;
    }
    case SEEK_CUR:
    {
        new_offset = current_offset + offset;
        break;
    }
    case SEEK_END:
    {
        new_offset = file->size + offset;
        break;
    }
    default:
    {
        panic("fat32_fs_lseek: invalid whence");
        return (size_t)-1;
    }
    }

    assert(new_offset != (size_t)-1);
    return new_offset;
}

/*
 * Resize a FAT32 regular file.
 */
static int fat32_fs_truncate(FsFile *file, size_t size)
{
    Fat32File fat_file = to_fat32_file(file);
    int ret;

    if (size > UINT32_MAX)
    {
        return -1;
    }

    ret = fat32_backend_truncate(&fat_file, (uint32_t)size);
    from_fat32_file(file, &fat_file);
    return ret;
}

/*
 * Remove a regular FAT32 file.
 */
static int fat32_fs_unlink(const char *path)
{
    return fat32_backend_unlink(path);
}

/*
 * Create a FAT32 directory.
 */
static int fat32_fs_mkdir(const char *path)
{
    return fat32_backend_mkdir(path);
}

/*
 * Remove an empty FAT32 directory.
 */
static int fat32_fs_rmdir(const char *path)
{
    return fat32_backend_rmdir(path);
}

/*
 * Rename a FAT32 entry.
 */
static int fat32_fs_rename(const char *old_path, const char *new_path)
{
    return fat32_backend_rename(old_path, new_path);
}

/*
 * Close a FAT32 regular-file descriptor snapshot.
 */
static int fat32_fs_close(FsFile *file)
{
    Fat32File fat_file = to_fat32_file(file);
    const int ret = fat32_backend_close(&fat_file);

    from_fat32_file(file, &fat_file);
    return ret;
}

const FsBackend regular_fs_backend = {
    .init = fat32_fs_init,
    .open = fat32_fs_open,
    .create = fat32_fs_create,
    .lookup = fat32_fs_lookup,
    .opendir = fat32_fs_opendir,
    .readdir = fat32_fs_readdir,
    .read = fat32_fs_read,
    .write = fat32_fs_write,
    .lseek = fat32_fs_lseek,
    .truncate = fat32_fs_truncate,
    .unlink = fat32_fs_unlink,
    .mkdir = fat32_fs_mkdir,
    .rmdir = fat32_fs_rmdir,
    .rename = fat32_fs_rename,
    .close = fat32_fs_close,
};
