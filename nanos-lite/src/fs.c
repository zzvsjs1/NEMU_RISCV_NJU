#include <fs.h>
#include "fs/backend.h"

#include <stddef.h>

/*
 * Device-file read callback.  The offset is the descriptor's current special
 * file offset, not a backend regular-file offset.  Poll-like devices can ignore
 * it, while synthetic files such as /proc/dispinfo may use it if they later
 * grow seek-style behaviour.
 */
typedef size_t (*ReadFn)(void *buf, size_t offset, size_t len);

/*
 * Device-file write callback.  Stream devices may ignore offset; /dev/fb uses
 * it as a byte position inside the linear framebuffer.
 */
typedef size_t (*WriteFn)(const void *buf, size_t offset, size_t len);

// device.c
size_t serial_write(const void *buf, size_t offset, size_t len);
size_t events_read(void *buf, size_t offset, size_t len);
size_t dispinfo_read(void *buf, size_t offset, size_t len);
size_t fb_write(const void *buf, size_t offset, size_t len);
size_t sb_write(const void *buf, size_t offset, size_t len);
size_t sbctl_write(const void *buf, size_t offset, size_t len);
size_t sbctl_read(void *buf, size_t offset, size_t len);

typedef struct
{
    /*
     * Kernel-visible pathname.  Special files bypass the regular backend, so
     * these names are matched before any FAT32 or flat-image lookup.
     */
    const char *name;
    /*
     * Logical size for seek-bounded special files.  Stream-like devices keep
     * this as zero because their callbacks define the valid operation range.
     */
    size_t size;
    /* Callback used by fs_read() for this special descriptor. */
    ReadFn read;
    /* Callback used by fs_write() for this special descriptor. */
    WriteFn write;
} SpecialFile;

typedef enum
{
    OPEN_NONE,
    OPEN_REGULAR,
    OPEN_DIRECTORY
} OpenKind;

typedef struct
{
    /* Distinguishes unused, regular-file, and directory descriptor slots. */
    OpenKind kind;
    /*
     * Current logical file offset for read/write/lseek.  fs.c advances this
     * after successful backend reads or writes, mirroring POSIX descriptors.
     */
    size_t offset;
    /* Non-zero when reads are allowed through this descriptor. */
    int readable;
    /* Non-zero when writes and truncation are allowed through this descriptor. */
    int writable;
    /* Non-zero when every write should first seek to the current file end. */
    int append;
    /* Metadata snapshot used by fstat() without storing path strings. */
    FsMetadata metadata;
    /* Backend-specific regular-file state. */
    FsFile file;
    /* Backend-specific directory iterator state. */
    FsDir dir;
} OpenFile;

enum
{
    FD_STDIN,
    FD_STDOUT,
    FD_STDERR,
    FD_FB,
    FD_EVENTS,
    FD_DISPINFO,
    FD_SB,
    FD_SBCTL
};

enum
{
    FIRST_REGULAR_FD = FD_SBCTL + 1,
    MAX_OPEN_FILES = 128,
    MAX_REGULAR_OPEN_FILES = MAX_OPEN_FILES - FIRST_REGULAR_FD,
};

enum
{
    FS_O_RDONLY = 0x0000,
    FS_O_WRONLY = 0x0001,
    FS_O_RDWR = 0x0002,
    FS_O_ACCMODE = 0x0003,
    FS_O_APPEND = 0x0008,
    FS_O_CREAT = 0x0200,
    FS_O_TRUNC = 0x0400,
    FS_O_EXCL = 0x0800,
    FS_O_DIRECTORY = 0x200000,
};

static SpecialFile special_files[] = {
    [FD_STDIN] = {"stdin", 0, 0, 0},
    [FD_STDOUT] = {"stdout", 0, 0, serial_write},
    [FD_STDERR] = {"stderr", 0, 0, serial_write},
    [FD_FB] = {"/dev/fb", 0, 0, fb_write},
    [FD_EVENTS] = {"/dev/events", 0, events_read, 0},
    [FD_DISPINFO] = {"/proc/dispinfo", 0, dispinfo_read, 0},
    [FD_SB] = {"/dev/sb", 0, 0, sb_write},
    [FD_SBCTL] = {"/dev/sbctl", 0, sbctl_read, sbctl_write},
};

enum
{
    NR_SPECIAL_FILES = sizeof(special_files) / sizeof(special_files[0])
};

static size_t special_offsets[NR_SPECIAL_FILES];
static OpenFile open_files[MAX_REGULAR_OPEN_FILES];
/*
 * Find a special device or proc file by exact pathname.
 */
static int find_special_file(const char *pathname)
{
    for (int i = 0; i < NR_SPECIAL_FILES; i++)
    {
        if (strcmp(pathname, special_files[i].name) == 0)
        {
            return i;
        }
    }

    return -1;
}

/*
 * Return an allocated regular descriptor slot after validating the fd range.
 */
static OpenFile *open_file(int fd)
{
    assert(fd >= FIRST_REGULAR_FD && fd < MAX_OPEN_FILES);

    OpenFile *open = &open_files[fd - FIRST_REGULAR_FD];
    assert(open->kind != OPEN_NONE);
    return open;
}

/*
 * Allocate a descriptor slot for a regular file or directory.
 */
static int allocate_fd(OpenKind kind, int readable, int writable, int append)
{
    for (int i = 0; i < MAX_REGULAR_OPEN_FILES; i++)
    {
        if (open_files[i].kind == OPEN_NONE)
        {
            open_files[i].kind = kind;
            open_files[i].offset = 0;
            open_files[i].readable = readable;
            open_files[i].writable = writable;
            open_files[i].append = append;
            return FIRST_REGULAR_FD + i;
        }
    }

    return -1;
}

/*
 * Decode POSIX access-mode bits into descriptor read/write permissions.
 */
static void access_from_flags(int flags, int *readable, int *writable)
{
    const int access = flags & FS_O_ACCMODE;

    *readable = access != FS_O_WRONLY;
    *writable = access == FS_O_WRONLY || access == FS_O_RDWR;
}

/*
 * Build metadata from an opened regular file when the backend-specific open
 * path has already done the expensive pathname lookup.
 */
static FsMetadata metadata_from_file(const FsFile *file)
{
    FsMetadata metadata;

    metadata.is_dir = 0;
    metadata.size = file->size;
    metadata.inode = file->u.fat32.dir_entry_offset != 0 ? file->u.fat32.dir_entry_offset : file->u.flat.disk_offset;
    return metadata;
}

/*
 * Fill a project-owned syscall stat record from backend-neutral metadata.
 * Nanos-lite deliberately writes NanosStat instead of newlib's struct stat:
 * libos performs the libc-specific translation after the syscall returns, which
 * keeps the kernel filesystem independent from newlib's current RV32 layout.
 */
static void fill_stat_from_metadata(NanosStat *buf, const FsMetadata *metadata)
{
    memset(buf, 0, sizeof(*buf));
    buf->ino = metadata->inode;
    buf->mode = (metadata->is_dir ? NANOS_S_IFDIR : NANOS_S_IFREG) | (metadata->is_dir ? 0755u : 0644u);
    buf->nlink = metadata->is_dir ? 2u : 1u;
    buf->size = (int64_t)metadata->size;
    buf->blksize = 512;
    buf->blocks = (int64_t)((metadata->size + 511u) / 512u);
}

/*
 * Fill a project-owned syscall stat record for a special file descriptor.
 */
static void fill_stat_from_special(NanosStat *buf, int fd)
{
    memset(buf, 0, sizeof(*buf));
    buf->ino = (uint64_t)(fd + 1);
    buf->mode = NANOS_S_IFCHR | 0666u;
    buf->nlink = 1;
    buf->size = (int64_t)special_files[fd].size;
    buf->blksize = 512;
    buf->blocks = (int64_t)((special_files[fd].size + 511u) / 512u);
}

/*
 * Initialise the filesystem service and mount the selected regular backend.
 */
void init_fs()
{
    const AM_GPU_CONFIG_T gpuConfig = io_read(AM_GPU_CONFIG);

    assert(gpuConfig.present);
    special_files[FD_FB].size = (size_t)gpuConfig.vmemsz;
    assert(regular_fs_backend.init() == 0);
}

/*
 * Open a special file, regular file, or directory according to POSIX flags.
 */
int fs_open(const char *pathname, int flags, int mode)
{
    int readable;
    int writable;
    int fd;

    (void)mode;

    if (pathname == 0)
    {
        return -1;
    }

    fd = find_special_file(pathname);

    if (fd >= 0)
    {
        if ((flags & FS_O_DIRECTORY) != 0)
        {
            return -1;
        }
        special_offsets[fd] = 0;
        return fd;
    }

    access_from_flags(flags, &readable, &writable);

    if ((flags & FS_O_DIRECTORY) != 0)
    {
        FsDir dir;
        int new_fd;

        if (regular_fs_backend.opendir(pathname, &dir) != 0)
        {
            return -1;
        }
        new_fd = allocate_fd(OPEN_DIRECTORY, 1, 0, 0);

        if (new_fd < 0)
        {
            return -1;
        }
        open_files[new_fd - FIRST_REGULAR_FD].dir = dir;

        if (regular_fs_backend.lookup(pathname, &open_files[new_fd - FIRST_REGULAR_FD].metadata) != 0)
        {
            open_files[new_fd - FIRST_REGULAR_FD].metadata.is_dir = 1;
            open_files[new_fd - FIRST_REGULAR_FD].metadata.size = 0;
            open_files[new_fd - FIRST_REGULAR_FD].metadata.inode = dir.u.fat32.first_cluster;
        }
        return new_fd;
    }

    FsFile file;

    if (regular_fs_backend.open(pathname, &file) == 0)
    {
        int new_fd;

        if ((flags & FS_O_EXCL) != 0 && (flags & FS_O_CREAT) != 0)
        {
            regular_fs_backend.close(&file);
            return -1;
        }

        if ((flags & FS_O_TRUNC) != 0)
        {
            if (!writable || regular_fs_backend.truncate(&file, 0) != 0)
            {
                regular_fs_backend.close(&file);
                return -1;
            }
        }

        new_fd = allocate_fd(OPEN_REGULAR, readable, writable, (flags & FS_O_APPEND) != 0);

        if (new_fd < 0)
        {
            regular_fs_backend.close(&file);
            return -1;
        }
        open_files[new_fd - FIRST_REGULAR_FD].file = file;
        open_files[new_fd - FIRST_REGULAR_FD].metadata = metadata_from_file(&file);

        if ((flags & FS_O_APPEND) != 0)
        {
            open_files[new_fd - FIRST_REGULAR_FD].offset = file.size;
        }
        return new_fd;
    }

    if ((flags & FS_O_CREAT) != 0)
    {
        int new_fd;

        if (regular_fs_backend.create(pathname, &file) != 0)
        {
            return -1;
        }
        new_fd = allocate_fd(OPEN_REGULAR, readable, writable, (flags & FS_O_APPEND) != 0);

        if (new_fd < 0)
        {
            regular_fs_backend.close(&file);
            return -1;
        }
        open_files[new_fd - FIRST_REGULAR_FD].file = file;
        open_files[new_fd - FIRST_REGULAR_FD].metadata = metadata_from_file(&file);
        return new_fd;
    }

#ifdef CONFIG_TRACE_FS_OPEN_MISS
    Log("fs_open: Invalid pathname: %s", pathname);
#endif
    return -1;
}

/*
 * Read up to len bytes from a descriptor and advance its offset on success.
 */
size_t fs_read(int fd, void *buf, size_t len)
{
    assert(fd >= 0 && fd < MAX_OPEN_FILES);

    if (fd < FIRST_REGULAR_FD)
    {
        SpecialFile *file = &special_files[fd];

        if (file->read == 0)
        {
            return (size_t)-1;
        }
        return file->read(buf, special_offsets[fd], len);
    }

    OpenFile *open = open_file(fd);

    if (open->kind != OPEN_REGULAR || !open->readable)
    {
        return (size_t)-1;
    }

    const size_t ret = regular_fs_backend.read(&open->file, open->offset, buf, len);

    if (ret != (size_t)-1)
    {
        open->offset += ret;
    }
    return ret;
}

/*
 * Write up to len bytes to a descriptor and advance its offset on success.
 */
size_t fs_write(int fd, const void *buf, size_t len)
{
    assert(fd >= 0 && fd < MAX_OPEN_FILES);

    if (fd < FIRST_REGULAR_FD)
    {
        SpecialFile *file = &special_files[fd];

        if (file->write == 0)
        {
            return (size_t)-1;
        }
        return file->write(buf, special_offsets[fd], len);
    }

    OpenFile *open = open_file(fd);

    if (open->kind != OPEN_REGULAR || !open->writable)
    {
        return (size_t)-1;
    }

    if (open->append)
    {
        open->offset = open->file.size;
    }

    const size_t ret = regular_fs_backend.write(&open->file, open->offset, buf, len);

    if (ret != (size_t)-1)
    {
        open->offset += ret;
        open->metadata.size = open->file.size;
    }
    return ret;
}

/*
 * Reposition a descriptor using SEEK_SET, SEEK_CUR, or SEEK_END.
 */
size_t fs_lseek(int fd, size_t offset, int whence)
{
    assert(fd >= 0 && fd < MAX_OPEN_FILES);

    if (fd >= FIRST_REGULAR_FD)
    {
        OpenFile *open = open_file(fd);

        if (open->kind == OPEN_DIRECTORY)
        {
            if (whence != SEEK_SET || offset != 0)
            {
                return (size_t)-1;
            }
            open->offset = 0;
            open->dir.u.fat32.next_entry_index = 0;
            return 0;
        }

        const size_t new_offset = regular_fs_backend.lseek(&open->file, open->offset, offset, whence);
        open->offset = new_offset;
        return new_offset;
    }

    const size_t old_offset = special_offsets[fd];
    const size_t file_size = special_files[fd].size;
    size_t new_offset = -1;

    switch (whence)
    {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = old_offset + offset;
        break;
    case SEEK_END:
        new_offset = file_size + offset;
        break;
    default:
        panic("fs_lseek: invalid whence");
        return (size_t)-1;
    }

    assert(new_offset <= file_size);
    special_offsets[fd] = new_offset;
    return new_offset;
}

/*
 * Close a regular or directory descriptor and make its slot reusable.
 */
int fs_close(int fd)
{
    assert(fd >= 0 && fd < MAX_OPEN_FILES);

    if (fd < FIRST_REGULAR_FD)
    {
        return 0;
    }

    OpenFile *open = &open_files[fd - FIRST_REGULAR_FD];

    if (open->kind == OPEN_NONE)
    {
        return 0;
    }

    const int ret = open->kind == OPEN_REGULAR ? regular_fs_backend.close(&open->file) : 0;
    memset(open, 0, sizeof(*open));
    return ret;
}

/*
 * Fill metadata for a path.
 */
int fs_stat(const char *pathname, NanosStat *buf)
{
    int fd;
    FsMetadata metadata;

    if (pathname == 0 || buf == 0)
    {
        return -1;
    }

    fd = find_special_file(pathname);

    if (fd >= 0)
    {
        fill_stat_from_special(buf, fd);
        return 0;
    }

    if (regular_fs_backend.lookup(pathname, &metadata) != 0)
    {
        return -1;
    }

    fill_stat_from_metadata(buf, &metadata);
    return 0;
}

/*
 * Fill metadata for an open descriptor.
 */
int fs_fstat(int fd, NanosStat *buf)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || buf == 0)
    {
        return -1;
    }

    if (fd < FIRST_REGULAR_FD)
    {
        fill_stat_from_special(buf, fd);
        return 0;
    }

    OpenFile *open = open_file(fd);
    FsMetadata metadata;

    metadata = open->metadata;

    if (open->kind == OPEN_REGULAR)
    {
        metadata.size = open->file.size;
    }
    fill_stat_from_metadata(buf, &metadata);
    return 0;
}

/*
 * Copy one backend-neutral directory entry into the fixed Nanos syscall record.
 * The record is larger than most names need, but it avoids embedding a libc
 * variable-length dirent layout in the kernel.  libos later packs this record
 * into whatever struct dirent shape newlib currently uses.
 */
static int fill_nanos_dirent(NanosDirent *dent, const FsDirent *entry, int64_t next_offset)
{
    const size_t name_len = strlen(entry->name);

    if (name_len > NANOS_NAME_MAX)
    {
        return -1;
    }

    memset(dent, 0, sizeof(*dent));
    dent->ino = entry->metadata.inode;
    dent->off = next_offset;
    dent->type = entry->metadata.is_dir ? NANOS_DT_DIR : NANOS_DT_REG;
    memcpy(dent->name, entry->name, name_len + 1u);
    return 0;
}

/*
 * Return fixed-size NanosDirent records for an open directory descriptor.
 */
int fs_getdents(int fd, void *buf, int len)
{
    NanosDirent *dst = (NanosDirent *)buf;
    size_t used = 0;
    const size_t record_len = sizeof(NanosDirent);

    if (fd < FIRST_REGULAR_FD || fd >= MAX_OPEN_FILES || buf == 0 || len <= 0 || (size_t)len < record_len)
    {
        return -1;
    }

    OpenFile *open = open_file(fd);

    if (open->kind != OPEN_DIRECTORY)
    {
        return -1;
    }

    while (used + record_len <= (size_t)len)
    {
        FsDir saved_dir = open->dir;
        FsDirent entry;
        const int ret = regular_fs_backend.readdir(&open->dir, &entry);
        const int64_t next_offset = (int64_t)open->dir.u.fat32.next_entry_index;

        if (ret < 0)
        {
            return used != 0 ? (int)used : -1;
        }

        if (ret == 0)
        {
            break;
        }

        if (fill_nanos_dirent(&dst[used / record_len], &entry, next_offset) != 0)
        {
            open->dir = saved_dir;

            if (used == 0)
            {
                return -1;
            }
            break;
        }
        used += record_len;
        open->offset = open->dir.u.fat32.next_entry_index;
    }

    return (int)used;
}

/*
 * Resize an open regular file.
 */
int fs_ftruncate(int fd, size_t size)
{
    if (fd < FIRST_REGULAR_FD || fd >= MAX_OPEN_FILES)
    {
        return -1;
    }

    OpenFile *open = open_file(fd);

    if (open->kind != OPEN_REGULAR || !open->writable)
    {
        return -1;
    }

    const int ret = regular_fs_backend.truncate(&open->file, size);

    if (ret == 0)
    {
        open->metadata.size = open->file.size;
    }
    return ret;
}

/*
 * Resize a regular file by pathname.
 */
int fs_truncate(const char *pathname, size_t size)
{
    FsFile file;
    int ret;

    if (pathname == 0 || regular_fs_backend.open(pathname, &file) != 0)
    {
        return -1;
    }

    ret = regular_fs_backend.truncate(&file, size);
    regular_fs_backend.close(&file);
    return ret;
}

/*
 * Remove a regular file.
 */
int fs_unlink(const char *pathname)
{
    if (pathname == 0)
    {
        return -1;
    }

    return regular_fs_backend.unlink(pathname);
}

/*
 * Create a directory.  mode is accepted for POSIX compatibility but FAT32
 * cannot persist Unix permission bits.
 */
int fs_mkdir(const char *pathname, int mode)
{
    (void)mode;

    if (pathname == 0)
    {
        return -1;
    }

    return regular_fs_backend.mkdir(pathname);
}

/*
 * Remove an empty directory.
 */
int fs_rmdir(const char *pathname)
{
    if (pathname == 0)
    {
        return -1;
    }

    return regular_fs_backend.rmdir(pathname);
}

/*
 * Rename a regular file or supported directory entry.
 */
int fs_rename(const char *old_path, const char *new_path)
{
    if (old_path == 0 || new_path == 0)
    {
        return -1;
    }

    return regular_fs_backend.rename(old_path, new_path);
}
