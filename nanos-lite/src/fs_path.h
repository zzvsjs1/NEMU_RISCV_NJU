#ifndef NANOS_LITE_FS_PATH_H
#define NANOS_LITE_FS_PATH_H

#include <stddef.h>

enum
{
    /*
     * FAT32 path handling in this tree already caps paths at 1024 bytes.  Keep
     * the VFS-side scratch buffer at the same size so a path accepted here is
     * not made longer than the backend can later split into parent/name parts.
     */
    FS_NORMALISED_PATH_MAX = 1024,
};

/*
 * Convert an app-visible pathname into the absolute form expected by storage
 * backends.  Navy has no per-process cwd yet, so relative names are treated as
 * living below the filesystem root.
 *
 * Return value contract:
 * - Absolute input, such as "/share/games/doom/doom.wad", is returned unchanged.
 *   Callers can pass it directly to special-file matching or the storage backend.
 * - Relative input, such as "./.savegame/temp.dsg" or ".default.cfg", is copied
 *   into buf with a leading '/' and the returned pointer is buf.
 * - Invalid input or an output buffer too small for the converted path returns
 *   NULL.  Syscall handlers translate that into a normal open/stat failure.
 */
const char *fs_normalise_path(const char *pathname, char *buf, size_t buf_size);

#endif
