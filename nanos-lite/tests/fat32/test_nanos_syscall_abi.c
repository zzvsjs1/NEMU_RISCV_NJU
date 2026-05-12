#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/stat.h>

#include "nanos_abi.h"

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (!(cond))                                               \
        {                                                          \
            printf("check failed at line %d: %s\n", __LINE__, #cond); \
            return 1;                                              \
        }                                                          \
    } while (0)

/*
 * Check the user-space translation from the project-owned Nanos syscall stat
 * record into newlib's public struct stat.  The kernel must not know newlib's
 * private layout, but applications must still observe POSIX mode/type bits and
 * file sizes through stat() and fstat().
 */
static int test_stat_translation_fills_newlib_stat(void)
{
    const NanosStat source = {
        .dev = 3,
        .ino = 0x12345678u,
        .mode = NANOS_S_IFDIR | 0755u,
        .nlink = 2,
        .uid = 1000,
        .gid = 1001,
        .rdev = 9,
        .size = 4096,
        .atim = {.sec = 11, .nsec = 12},
        .mtim = {.sec = 21, .nsec = 22},
        .ctim = {.sec = 31, .nsec = 32},
        .blksize = 512,
        .blocks = 8,
    };
    struct stat output;

    memset(&output, 0xa5, sizeof(output));
    nanos_stat_to_newlib(&output, &source);

    CHECK(output.st_dev == (dev_t)source.dev);
    CHECK(output.st_ino == (ino_t)source.ino);
    CHECK(S_ISDIR(output.st_mode));
    CHECK((output.st_mode & 0777) == 0755);
    CHECK(output.st_nlink == (nlink_t)source.nlink);
    CHECK(output.st_uid == (uid_t)source.uid);
    CHECK(output.st_gid == (gid_t)source.gid);
    CHECK(output.st_rdev == (dev_t)source.rdev);
    CHECK(output.st_size == (off_t)source.size);
    CHECK(output.st_atim.tv_sec == (time_t)source.atim.sec);
    CHECK(output.st_atim.tv_nsec == source.atim.nsec);
    CHECK(output.st_mtim.tv_sec == (time_t)source.mtim.sec);
    CHECK(output.st_mtim.tv_nsec == source.mtim.nsec);
    CHECK(output.st_ctim.tv_sec == (time_t)source.ctim.sec);
    CHECK(output.st_ctim.tv_nsec == source.ctim.nsec);
    CHECK(output.st_blksize == (blksize_t)source.blksize);
    CHECK(output.st_blocks == (blkcnt_t)source.blocks);
    return 0;
}

/*
 * Check getdents() translation in the same direction Linux uses: kernel ABI
 * records are consumed by libc, then libc presents its own struct dirent layout
 * to readdir().  Two entries prove the helper advances by newlib d_reclen
 * instead of assuming the source and destination layouts have the same size.
 */
static int test_dirent_translation_packs_newlib_dirents(void)
{
    const NanosDirent source[] = {
        {
            .ino = 7,
            .off = 3,
            .type = NANOS_DT_REG,
            .name = "alpha.txt",
        },
        {
            .ino = 8,
            .off = 4,
            .type = NANOS_DT_DIR,
            .name = "save slots",
        },
    };
    unsigned char output[512];
    int written;
    const struct dirent *first;
    const struct dirent *second;

    memset(output, 0xa5, sizeof(output));
    written = nanos_dirents_to_newlib(output, sizeof(output), source, sizeof(source));

    CHECK(written > 0);
    first = (const struct dirent *)output;
    CHECK(first->d_ino == (long)source[0].ino);
    CHECK(first->d_off == (off_t)source[0].off);
    CHECK(first->d_reclen > 0);
    CHECK(strcmp(first->d_name, "alpha.txt") == 0);

    second = (const struct dirent *)(output + first->d_reclen);
    CHECK((const unsigned char *)second < output + written);
    CHECK(second->d_ino == (long)source[1].ino);
    CHECK(second->d_off == (off_t)source[1].off);
    CHECK(second->d_reclen > 0);
    CHECK(strcmp(second->d_name, "save slots") == 0);
    CHECK(first->d_reclen + second->d_reclen == written);
    return 0;
}

int main(void)
{
    if (test_stat_translation_fills_newlib_stat() != 0 || test_dirent_translation_packs_newlib_dirents() != 0)
    {
        return 1;
    }
    puts("nanos_syscall_abi tests passed");
    return 0;
}
