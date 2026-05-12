#include "backend.h"

#ifndef SEEK_SET
enum
{
    SEEK_SET,
    SEEK_CUR,
    SEEK_END
};
#endif

// disk.c
size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

typedef struct
{
    /*
     * Absolute Navy pathname generated from the fsimg tree, for example
     * /share/games/ons/arc.nsa.  The generator sorts entries, allowing flat_open()
     * to use binary search instead of a linear scan.
     */
    const char *name;
    /*
     * Exact byte length of the packed file.  The flat backend is read-mostly and
     * has no allocation table, so writes are clipped to this fixed size.
     */
    size_t size;
    /*
     * Absolute byte offset inside ramdisk.img where the file's first byte starts.
     * Files are packed one after another, so the whole file is one contiguous
     * disk extent.
     */
    size_t disk_offset;
} FlatEntry;

/* This is the generated information about all regular files in the ramdisk. */
static const FlatEntry flat_entries[] __attribute__((used)) = {
#include "../files.h"
};

enum
{
    NR_FLAT_ENTRIES = sizeof(flat_entries) / sizeof(flat_entries[0])
};

/*
 * Return the smaller byte count for clipped reads and writes.
 */
static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

/*
 * Initialise the generated flat file table backend.
 */
static int flat_init(void)
{
    return 0;
}

/*
 * Resolve an existing flat-image file through the generated sorted file table.
 */
static int flat_open(const char *path, FsFile *out)
{
    /*
     * navy-apps/Makefile writes files.h from `find ... | sort`, so every normal
     * ramdisk pathname is lexicographically ordered.  ONScripter opens many
     * PNG/archive paths while changing scenes; binary search cuts that path
     * lookup from about 1,600 string comparisons to about 11 in the current large
     * game image.
     */
    int left = 0;
    int right = NR_FLAT_ENTRIES;

    while (left < right)
    {
        const int mid = left + (right - left) / 2;
        const int cmp = strcmp(path, flat_entries[mid].name);

        if (cmp == 0)
        {
            out->size = flat_entries[mid].size;
            out->u.flat.disk_offset = flat_entries[mid].disk_offset;
            return 0;
        }

        if (cmp < 0)
        {
            right = mid;
        }
        else
        {
            left = mid + 1;
        }
    }

    return -1;
}

/*
 * The flat backend cannot create files because the generated image has fixed
 * extents and no allocation table.
 */
static int flat_create(const char *path, FsFile *out)
{
    (void)path;
    (void)out;
    return -1;
}

/*
 * Resolve metadata for an existing flat regular file.
 */
static int flat_lookup(const char *path, FsMetadata *out)
{
    FsFile file;

    if (flat_open(path, &file) != 0)
    {
        return -1;
    }

    out->is_dir = 0;
    out->size = file.size;
    out->inode = file.u.flat.disk_offset;
    return 0;
}

/*
 * The flat backend has no directories to iterate.
 */
static int flat_opendir(const char *path, FsDir *out)
{
    (void)path;
    (void)out;
    return -1;
}

/*
 * The flat backend has no directory iterator state.
 */
static int flat_readdir(FsDir *dir, FsDirent *out)
{
    (void)dir;
    (void)out;
    return -1;
}

/*
 * Read a byte range from one fixed flat-file extent.
 */
static size_t flat_read(FsFile *file, size_t offset, void *buf, size_t len)
{
    if (offset >= file->size)
    {
        return 0;
    }

    const size_t rlen = min_size(len, file->size - offset);
    return disk_read(buf, file->u.flat.disk_offset + offset, rlen);
}

/*
 * Write inside one fixed flat-file extent, clipping at the original size.
 */
static size_t flat_write(FsFile *file, size_t offset, const void *buf, size_t len)
{
    assert(offset <= file->size);

    const size_t wlen = min_size(len, file->size - offset);
    return disk_write(buf, file->u.flat.disk_offset + offset, wlen);
}

/*
 * Convert POSIX seek arguments into an offset inside the fixed file extent.
 */
static size_t flat_lseek(FsFile *file, size_t current_offset, size_t offset, int whence)
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
        panic("flat_lseek: invalid whence");
        return (size_t)-1;
    }
    }

    assert(new_offset <= file->size);
    return new_offset;
}

/*
 * Reject truncation because flat-file extents cannot be resized.
 */
static int flat_truncate(FsFile *file, size_t size)
{
    (void)file;
    (void)size;

    return -1;
}

/*
 * Reject unlink because the flat backend has no free-space tracking.
 */
static int flat_unlink(const char *path)
{
    (void)path;
    return -1;
}

/*
 * Reject mkdir because the flat backend does not model directories.
 */
static int flat_mkdir(const char *path)
{
    (void)path;
    return -1;
}

/*
 * Reject rmdir because the flat backend does not model directories.
 */
static int flat_rmdir(const char *path)
{
    (void)path;
    return -1;
}

/*
 * Reject rename because flat image entries are generated at build time.
 */
static int flat_rename(const char *old_path, const char *new_path)
{
    (void)old_path;
    (void)new_path;
    return -1;
}

/*
 * Close a flat-file descriptor snapshot.
 */
static int flat_close(FsFile *file)
{
    (void)file;
    return 0;
}

const FsBackend regular_fs_backend = {
    .init = flat_init,
    .open = flat_open,
    .create = flat_create,
    .lookup = flat_lookup,
    .opendir = flat_opendir,
    .readdir = flat_readdir,
    .read = flat_read,
    .write = flat_write,
    .lseek = flat_lseek,
    .truncate = flat_truncate,
    .unlink = flat_unlink,
    .mkdir = flat_mkdir,
    .rmdir = flat_rmdir,
    .rename = flat_rename,
    .close = flat_close,
};
