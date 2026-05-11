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
   * Resize a file.  The current FAT32 implementation supports truncating to
   * zero for save-file replacement; the flat backend rejects truncation because
   * its generated image has fixed extents.
   */
    int (*truncate)(FsFile *file, size_t size);
    /* Release backend state held in FsFile.  Most current state is by value. */
    int (*close)(FsFile *file);
} FsBackend;

extern const FsBackend regular_fs_backend;

#endif
