#include "nanos_abi.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Return a bounded string length.  Some freestanding/newlib configurations do
 * not provide strnlen(), so libos keeps this tiny helper local to the ABI
 * translation layer.
 */
static size_t bounded_strlen(const char *text, size_t max_len)
{
    size_t len = 0;

    while (len < max_len && text[len] != '\0')
    {
        len++;
    }
    return len;
}

/*
 * newlib's directory reader expects each returned record to be aligned to a
 * long boundary.  This is a libc packing rule, so it belongs in libos rather
 * than in Nanos-lite's filesystem implementation.
 */
static size_t align_newlib_dirent_len(size_t len)
{
    const size_t align = sizeof(long);

    return (len + align - 1u) & ~(align - 1u);
}

void nanos_stat_to_newlib(struct stat *dst, const NanosStat *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->st_dev = (dev_t)src->dev;
    dst->st_ino = (ino_t)src->ino;
    dst->st_mode = (mode_t)src->mode;
    dst->st_nlink = (nlink_t)src->nlink;
    dst->st_uid = (uid_t)src->uid;
    dst->st_gid = (gid_t)src->gid;
    dst->st_rdev = (dev_t)src->rdev;
    dst->st_size = (off_t)src->size;
    dst->st_atim.tv_sec = (time_t)src->atim.sec;
    dst->st_atim.tv_nsec = src->atim.nsec;
    dst->st_mtim.tv_sec = (time_t)src->mtim.sec;
    dst->st_mtim.tv_nsec = src->mtim.nsec;
    dst->st_ctim.tv_sec = (time_t)src->ctim.sec;
    dst->st_ctim.tv_nsec = src->ctim.nsec;
    dst->st_blksize = (blksize_t)src->blksize;
    dst->st_blocks = (blkcnt_t)src->blocks;
}

int nanos_dirents_to_newlib(void *dst, int dst_len, const NanosDirent *src, int src_len)
{
    uint8_t *out = (uint8_t *)dst;
    size_t used = 0;
    const size_t source_count = (size_t)src_len / sizeof(NanosDirent);

    if (dst == 0 || src == 0 || dst_len <= 0 || src_len < 0 || (size_t)src_len % sizeof(NanosDirent) != 0)
    {
        return -1;
    }

    for (size_t i = 0; i < source_count; i++)
    {
        const NanosDirent *nanos = &src[i];
        const size_t name_len = bounded_strlen(nanos->name, NANOS_NAME_MAX + 1u);

        if (name_len > NANOS_NAME_MAX)
        {
            return used != 0 ? (int)used : -1;
        }

        const size_t raw_len = offsetof(struct dirent, d_name) + name_len + 1u;
        const size_t record_len = align_newlib_dirent_len(raw_len);

        if (record_len > (size_t)USHRT_MAX || used + record_len > (size_t)dst_len)
        {
            return used != 0 ? (int)used : -1;
        }

        struct dirent *newlib = (struct dirent *)(out + used);
        memset(newlib, 0, record_len);
        newlib->d_ino = (long)nanos->ino;
        newlib->d_off = (off_t)nanos->off;
        newlib->d_reclen = (unsigned short)record_len;
        memcpy(newlib->d_name, nanos->name, name_len + 1u);
        used += record_len;
    }

    return (int)used;
}
