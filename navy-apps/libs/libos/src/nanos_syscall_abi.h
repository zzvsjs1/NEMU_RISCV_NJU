#ifndef NANOS_SYSCALL_ABI_H
#define NANOS_SYSCALL_ABI_H

#include <stdint.h>

/*
 * This header describes the binary records exchanged between Navy's libos and
 * Nanos-lite.  It is intentionally not a copy of newlib's struct stat or struct
 * dirent: libc owns its public structures, while the kernel owns this small
 * syscall ABI.  That split is similar to Linux UAPI practice and lets a future
 * libc update change its internal layout without forcing Nanos-lite's FS code to
 * know about that layout.
 */

enum
{
    /*
     * FAT32 long-file-name entries can expose 260-character path components.
     * The syscall record stores one terminated component name, not a whole path.
     */
    NANOS_NAME_MAX = 260
};

/*
 * POSIX file type and permission bits.  These numeric values are part of the
 * portable POSIX API surface, so it is reasonable for the Nanos syscall ABI to
 * carry them directly.  libos still performs the final copy into newlib's
 * mode_t field.
 */
enum
{
    NANOS_S_IFMT = 0170000u,
    NANOS_S_IFDIR = 0040000u,
    NANOS_S_IFCHR = 0020000u,
    NANOS_S_IFREG = 0100000u
};

/*
 * Small directory-entry type tags.  POSIX readdir() does not require d_type, but
 * carrying it in the Nanos ABI keeps the kernel record useful without tying it
 * to any libc-specific struct dirent extension.
 */
enum
{
    NANOS_DT_UNKNOWN = 0,
    NANOS_DT_REG = 8,
    NANOS_DT_DIR = 4,
    NANOS_DT_CHR = 2
};

typedef struct
{
    int64_t sec;
    int32_t nsec;
    int32_t reserved;
} NanosTimespec;

typedef struct
{
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    int64_t size;
    NanosTimespec atim;
    NanosTimespec mtim;
    NanosTimespec ctim;
    int64_t blksize;
    int64_t blocks;
} NanosStat;

typedef struct
{
    uint64_t ino;
    int64_t off;
    uint8_t type;
    uint8_t reserved[7];
    char name[NANOS_NAME_MAX + 1];
} NanosDirent;

#endif
