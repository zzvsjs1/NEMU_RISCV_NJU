#ifndef __FS_H__
#define __FS_H__

#include <common.h>

#ifndef SEEK_SET
enum
{
    SEEK_SET,
    SEEK_CUR,
    SEEK_END
};
#endif

/*
 * Open a special device file or a regular backend file.  pathname is always an
 * absolute Navy path for regular files; flags currently matter mainly for
 * truncate-on-open save-file handling.
 */
int fs_open(const char *pathname, int flags, int mode);

/* Read from the descriptor's current offset and advance that offset on success. */
size_t fs_read(int fd, void *buf, size_t len);

/* Write at the descriptor's current offset and advance it by the accepted count. */
size_t fs_write(int fd, const void *buf, size_t len);

/*
 * Reposition a descriptor using SEEK_SET, SEEK_CUR, or SEEK_END.  Regular files
 * delegate bound checks to the selected backend; special files use their table
 * size, for example the framebuffer byte length.
 */
size_t fs_lseek(int fd, size_t offset, int whence);

/* Release a regular descriptor slot.  Special descriptors are stable and no-op. */
int fs_close(int fd);

#endif
