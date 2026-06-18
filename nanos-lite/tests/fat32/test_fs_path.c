#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fs_path.h"

static void assert_normalised_path(const char *input, const char *expected)
{
    char buf[FS_NORMALISED_PATH_MAX];
    const char *normalised = fs_normalise_path(input, buf, sizeof(buf));

    assert(normalised != 0);
    assert(strcmp(normalised, expected) == 0);
}

static void test_relative_paths_are_rooted_at_fs_root(void)
{
    /*
     * A leading dot is only special when it is exactly "." or followed by '/'.
     * Hidden file names such as ".default.cfg" and ".savegame" must stay as
     * normal path components; otherwise Doom's config and save directories would
     * be misread as current-directory markers.
     */
    assert_normalised_path(".default.cfg", "/.default.cfg");
    assert_normalised_path("./.savegame/temp.dsg", "/.savegame/temp.dsg");
    assert_normalised_path("././.savegame/", "/.savegame/");
    assert_normalised_path("share/games/doom/doom.wad", "/share/games/doom/doom.wad");
}

static void test_dot_names_resolve_to_root(void)
{
    /*
     * Nanos-lite does not maintain a per-process cwd.  The model is "cwd is
     * filesystem root", so every spelling that names just the current directory
     * must resolve to "/".
     */
    assert_normalised_path(".", "/");
    assert_normalised_path("./", "/");
    assert_normalised_path("././", "/");
}

static void test_absolute_paths_are_preserved(void)
{
    char buf[FS_NORMALISED_PATH_MAX];
    const char *path = "/tmp/recovery.dsg";
    const char *normalised = fs_normalise_path(path, buf, sizeof(buf));

    /*
     * Absolute paths already satisfy the backend contract.  Returning the input
     * pointer avoids needless copying and lets callers pass the exact special
     * pathname onward when it was already absolute.
     */
    assert(normalised == path);
    assert(strcmp(normalised, path) == 0);
}

static void test_small_output_buffer_is_rejected(void)
{
    char buf[4];

    assert(fs_normalise_path("./.savegame/temp.dsg", buf, sizeof(buf)) == 0);
}

int main(void)
{
    test_relative_paths_are_rooted_at_fs_root();
    test_dot_names_resolve_to_root();
    test_absolute_paths_are_preserved();
    test_small_output_buffer_is_rejected();
    puts("fs_path tests passed");
    return 0;
}
