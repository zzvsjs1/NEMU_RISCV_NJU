#ifndef NANOS_LITE_FS_BACKEND_H
#define NANOS_LITE_FS_BACKEND_H

#include <common.h>

/*
 * Backend-neutral description of one opened regular file.  fs.c stores this by
 * value in each descriptor slot, so every backend must keep enough state here
 * to continue read/write/lseek operations without relying on hidden per-fd
 * tables.
 */
typedef struct
{
    /*
   * Current logical file length in bytes.  Flat files copy this from the
   * generated files.h table; FAT32 files copy it from DIR_FileSize and update
   * it after successful extending writes.  Device files do not use FsFile.
   */
    size_t size;
    union
    {
        struct
        {
            /*
       * Absolute byte offset inside build/ramdisk.img where the flat file's
       * first byte was packed.  The flat backend has no directories or block
       * allocation, so reading file offset N is simply disk_offset + N.
       */
            size_t disk_offset;
        } flat;
        struct
        {
            /*
       * Raw FAT directory attribute byte from DIR_Attr.  The backend uses this
       * to reject writes to read-only files and to avoid treating directories
       * as regular files.  Long-name slots are filtered before FsFile is built.
       */
            uint8_t attr;
            /*
       * First data cluster assembled from DIR_FstClusHI:DIR_FstClusLO.  FAT32
       * reserves cluster numbers 0 and 1; zero is valid only for an empty file.
       */
            uint32_t first_cluster;
            /*
       * Absolute byte offset of this file's 32-byte short directory entry.
       * Writes and truncates patch this entry so DIR_FileSize and the first
       * cluster fields stay consistent with the FAT chain.
       */
            uint64_t dir_entry_offset;
            /*
       * File-relative cluster index for cached_cluster.  Sequential reads and
       * writes normally move forwards, so keeping this index prevents repeated
       * scans from first_cluster through the same FAT links.
       */
            uint32_t cached_cluster_index;
            /*
       * Actual FAT32 cluster number at cached_cluster_index.  This is a chain
       * position, not just first_cluster + index, because FAT32 files may be
       * fragmented unless contiguity has already been verified.
       */
            uint32_t cached_cluster;
            /*
       * Count of consecutive clusters, starting at first_cluster, whose FAT
       * links have been verified to point to the next physical cluster.  This
       * lets large ONScripter archive reads map directly to contiguous disk
       * byte ranges while still respecting the FAT specification.
       */
            uint32_t contiguous_cluster_count;
        } fat32;
    } u;
} FsFile;

/*
 * Backend-neutral snapshot of a directory iterator.  FAT32 stores raw slot
 * progress so getdents() can resume without rescanning visible entries.
 */
typedef struct
{
    union
    {
        struct
        {
            /* First cluster of the directory being iterated. */
            uint32_t first_cluster;
            /* Next raw 32-byte directory slot to inspect. */
            uint32_t next_entry_index;
        } fat32;
    } u;
} FsDir;

/*
 * Backend-neutral metadata for stat/fstat and getdents.  FAT32 has no inode
 * table, so dir_entry_offset is used as a stable lightweight inode substitute.
 */
typedef struct
{
    /* Non-zero for directories, zero for regular files. */
    int is_dir;
    /* Logical file size.  Directories report zero, matching the FAT spec. */
    size_t size;
    /* Stable per-entry identifier derived from the on-disk directory offset. */
    uint64_t inode;
} FsMetadata;

/*
 * Backend-neutral directory entry returned to fs.c before it is packed into
 * newlib's struct dirent layout.
 */
typedef struct
{
    char name[261];
    FsMetadata metadata;
} FsDirent;

/*
 * Operations supplied by the selected regular-file backend.  fs.c owns file
 * descriptor allocation and device files; the backend owns pathname lookup,
 * storage layout, and regular-file mutation rules.
 */
typedef struct
{
    /* Mount or initialise backend-private state before the first regular open. */
    int (*init)(void);
    /*
   * Resolve an absolute Navy pathname to an FsFile snapshot.  Return 0 only for
   * regular files; directories and missing paths are reported as -1.
     */
    int (*open)(const char *path, FsFile *out);
    /*
     * Create a new regular file and return it as an opened file snapshot.
     */
    int (*create)(const char *path, FsFile *out);
    /*
     * Resolve metadata for either a regular file or a directory.
     */
    int (*lookup)(const char *path, FsMetadata *out);
    /*
     * Open a directory path for getdents-style iteration.
     */
    int (*opendir)(const char *path, FsDir *out);
    /*
     * Return one visible directory entry.  The result is 1 for an entry, 0 for
     * end of directory, and -1 for a damaged directory chain.
     */
    int (*readdir)(FsDir *dir, FsDirent *out);
    /*
   * Copy up to len bytes starting at the caller-supplied logical file offset.
   * The backend returns the byte count actually read, which may be short at EOF
   * or when a damaged FAT chain ends before DIR_FileSize.
   */
    size_t (*read)(FsFile *file, size_t offset, void *buf, size_t len);
    /*
   * Write bytes at a logical file offset.  Backends may update fields inside
   * FsFile, for example FAT32 size, first cluster, and chain-position caches.
   */
    size_t (*write)(FsFile *file, size_t offset, const void *buf, size_t len);
    /*
   * Convert SEEK_SET/SEEK_CUR/SEEK_END into a new logical offset.  fs.c stores
   * the descriptor offset; the backend checks the chosen backend's bounds.
   */
    size_t (*lseek)(FsFile *file, size_t current_offset, size_t offset, int whence);
    /*
     * Resize a file.  FAT32 supports both shrinking and zero-filled growth; the
     * flat backend rejects truncation because its generated image has fixed
     * extents.
     */
    int (*truncate)(FsFile *file, size_t size);
    /* Remove a regular file and free its storage. */
    int (*unlink)(const char *path);
    /* Create a directory with dot and dotdot entries. */
    int (*mkdir)(const char *path);
    /* Remove an empty directory. */
    int (*rmdir)(const char *path);
    /* Rename a file or supported directory entry. */
    int (*rename)(const char *old_path, const char *new_path);
    /* Release backend state held in FsFile.  Most current state is by value. */
    int (*close)(FsFile *file);
} FsBackend;

extern const FsBackend regular_fs_backend;

#endif
