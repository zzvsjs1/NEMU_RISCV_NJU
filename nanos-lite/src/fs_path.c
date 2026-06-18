#include "fs_path.h"

/*
 * Keep this helper local instead of relying on libc strlen().  Nanos-lite is a
 * freestanding kernel: most files can use klib through common.h, but this path
 * normaliser is also compiled by small host-side unit tests.  A tiny local loop
 * avoids adding extra include or platform assumptions to that test boundary.
 */
static size_t local_strlen(const char *s)
{
    size_t len = 0;

    while (s[len] != '\0')
    {
        len++;
    }
    return len;
}

const char *fs_normalise_path(const char *pathname, char *buf, size_t buf_size)
{
    const char *relative;
    size_t len;

    /*
     * Empty paths have no POSIX meaning for open/stat/mkdir.  Returning NULL
     * here lets every syscall entry point report the same simple failure instead
     * of duplicating this guard in fs.c.
     */
    if (pathname == 0 || pathname[0] == '\0')
    {
        return 0;
    }

    /*
     * The regular backends already require absolute Navy paths.  If the caller
     * supplied one, do not copy it: returning the original pointer keeps normal
     * absolute paths cheap and also preserves exact special names like /dev/fb.
     */
    if (pathname[0] == '/')
    {
        return pathname;
    }

    /*
     * Relative paths need at least room for "/" and the trailing NUL.  The
     * caller owns the buffer because syscall handlers run on the kernel stack;
     * this helper never allocates memory or stores global state.
     */
    if (buf == 0 || buf_size < 2)
    {
        return 0;
    }

    /*
     * Navy apps currently have no real per-process current working directory.
     * Treat cwd as "/" and remove leading "./" markers before adding the root
     * slash.  This is exactly what Doom Generic needs:
     *
     *   "./.savegame/temp.dsg" -> "/.savegame/temp.dsg"
     *
     * Multiple leading "./" segments are folded for robustness, so "././x" is
     * handled the same as "x".  Internal "." or ".." components are deliberately
     * not interpreted here; the FAT32 backend keeps validating final paths and
     * this helper is only a cwd-rooting shim, not a full POSIX path resolver.
     */
    relative = pathname;
    while (relative[0] == '.' && (relative[1] == '/' || relative[1] == '\0'))
    {
        /*
         * "." by itself means the root directory under our cwd-is-root model.
         * Move to the terminator so the common empty-relative case below writes
         * exactly "/".
         */
        if (relative[1] == '\0')
        {
            relative++;
            break;
        }

        relative += 2;
        /*
         * Collapse extra separators after a stripped "./".  For example,
         * ".///tmp" becomes "/tmp", matching the backend's absolute-path style.
         */
        while (*relative == '/')
        {
            relative++;
        }
    }

    /*
     * Inputs such as ".", "./", or "././" all describe the current directory.
     * Because the current directory is modelled as the filesystem root, the
     * normalised path is just "/".
     */
    if (*relative == '\0')
    {
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }

    /*
     * Need one byte for the leading slash and one for the trailing NUL.  Refuse
     * paths that would be truncated; silently shortening save-file names would
     * be worse than returning an ordinary open failure.
     */
    len = local_strlen(relative);
    if (len + 2 > buf_size)
    {
        return 0;
    }

    /*
     * Build the rooted path by hand so this stays usable in both the kernel and
     * host tests.  The loop copies the trailing NUL too, hence i <= len.
     */
    buf[0] = '/';
    for (size_t i = 0; i <= len; i++)
    {
        buf[i + 1] = relative[i];
    }
    return buf;
}
