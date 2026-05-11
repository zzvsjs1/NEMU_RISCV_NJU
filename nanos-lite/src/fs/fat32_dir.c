#include "fat32.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

#define FAT32_SECTOR_SIZE 512u
#define FAT32_DIR_ENTRY_SIZE 32u
#define FAT32_DIR_ENTRIES_PER_SECTOR (FAT32_SECTOR_SIZE / FAT32_DIR_ENTRY_SIZE)
#define FAT32_MAX_LFN_PIECES (FAT32_MAX_NAME / 13u)
#define FAT32_MAX_PATH 1024u
#define FAT32_EOC_VALUE 0x0fffffffu

typedef struct
{
    /*
     * True after seeing the LFN slot with LAST_LONG_ENTRY set.  FAT stores that
     * final logical name piece first on disk, so a valid LFN chain must start
     * from this marker before lower sequence numbers are accepted.
     */
    bool saw_last;
    /*
     * Set when the current LFN run violates the FAT ordering/checksum rules.
     * The following short entry then falls back to its 8.3 alias instead of
     * using possibly stale long-name text.
     */
    bool malformed;
    /*
     * LDIR_Chksum value that every LFN slot must share.  It must also match
     * the checksum recomputed from the following short DIR_Name field.
     */
    uint8_t checksum;
    /*
     * Number of 13-character LFN pieces advertised by the LAST_LONG_ENTRY
     * slot.  Sequence numbers are one-based, so valid pieces occupy
     * pieces[1..count].
     */
    unsigned count;
    /*
     * Next sequence number expected while scanning forwards on disk.  For a
     * three-piece name, the on-disk order is 3|LAST, 2, 1, short entry.
     */
    unsigned expected_sequence;
    /*
     * Per-sequence presence bits.  They prevent duplicate or missing pieces
     * from being silently joined into a wrong pathname.
     */
    bool present[FAT32_MAX_LFN_PIECES + 1u];
    /*
     * ASCII form of each decoded 13-code-unit LFN piece.  This driver accepts
     * only characters that fat32_lfn_entry_to_ascii() can represent, matching
     * the rest of the small Navy pathname layer.
     */
    char pieces[FAT32_MAX_LFN_PIECES + 1u][14];
} Fat32LfnState;

typedef struct
{
    Fat32DirEntry entry;
    uint32_t parent_cluster;
    uint32_t entry_index;
    uint32_t lfn_start_index;
    uint32_t lfn_entry_count;
} Fat32LocatedEntry;

/*
 * Decode a little-endian 16-bit value from a raw FAT directory entry.
 */
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Decode a little-endian 32-bit value from a raw FAT directory entry.
 */
static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Store a little-endian 16-bit value into a raw FAT directory entry.
 */
static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/*
 * Store a little-endian 32-bit value into a raw FAT directory entry.
 */
static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/*
 * Convert one ASCII letter to lower case for FAT's case-insensitive matching.
 */
static char ascii_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return (char)(ch - 'A' + 'a');
    }

    return ch;
}

/*
 * Compare two ASCII path components using FAT-style case-insensitive matching.
 */
static int ascii_case_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (ascii_lower(*a) != ascii_lower(*b))
        {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

/*
 * Return non-zero if a NUL-terminated string contains one byte value.
 */
static int string_contains_char(const char *text, char ch)
{
    while (*text != '\0')
    {
        if (*text == ch)
        {
            return 1;
        }
        text++;
    }

    return 0;
}

/*
 * Find the last dot in a filename, returning NULL when the name has no
 * extension separator.
 */
static const char *find_last_dot(const char *name)
{
    const char *last = 0;

    while (*name != '\0')
    {
        if (*name == '.')
        {
            last = name;
        }
        name++;
    }

    return last;
}

/*
 * Reset the accumulated long-filename state before scanning a new entry run.
 */
static void lfn_reset(Fat32LfnState *state)
{
    memset(state, 0, sizeof(*state));
}

/*
 * Add one raw LFN slot to the current long-filename chain, checking sequence
 * numbers and checksums before accepting the decoded ASCII fragment.
 */
static void lfn_add(Fat32LfnState *state, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE])
{
    const Fat32LfnEntry *entry = (const Fat32LfnEntry *)raw_entry;
    const unsigned sequence = entry->order & 0x1fu;
    const bool is_last = (entry->order & 0x40u) != 0;

    if (sequence == 0 || sequence > FAT32_MAX_LFN_PIECES)
    {
        lfn_reset(state);
        state->malformed = true;
        return;
    }

    if (is_last)
    {
        lfn_reset(state);
        state->saw_last = true;
        state->count = sequence;
        state->expected_sequence = sequence - 1u;
        state->checksum = entry->checksum;
    }
    else if (!state->saw_last || sequence != state->expected_sequence || entry->checksum != state->checksum || state->present[sequence])
    {
        lfn_reset(state);
        state->malformed = true;
        return;
    }
    else
    {
        state->expected_sequence--;
    }

    if (fat32_lfn_entry_to_ascii(entry, state->pieces[sequence], sizeof(state->pieces[sequence])) < 0)
    {
        lfn_reset(state);
        state->malformed = true;
        return;
    }

    state->present[sequence] = true;
}

/*
 * Join the validated LFN fragments into one ASCII pathname component.
 */
static int lfn_build_name(const Fat32LfnState *state, const uint8_t short_name[11], char *out, size_t out_size)
{
    size_t pos = 0;

    if (out_size == 0 || state->malformed || !state->saw_last || state->count == 0 || state->count > FAT32_MAX_LFN_PIECES || state->expected_sequence != 0 || state->checksum != fat32_lfn_checksum(short_name))
    {
        return -1;
    }

    for (unsigned sequence = 1; sequence <= state->count; sequence++)
    {
        const char *piece = state->pieces[sequence];

        if (!state->present[sequence])
        {
            return -1;
        }

        while (*piece != '\0')
        {
            if (pos + 1 >= out_size)
            {
                return -1;
            }
            out[pos++] = *piece++;
        }
    }

    out[pos] = '\0';
    return 0;
}

/*
 * Render an on-disk short 8.3 name to the lower-case alias accepted by lookup.
 */
static void short_name_to_alias(const uint8_t short_name[11], char out[13])
{
    size_t pos = 0;
    int base_end = 7;
    int ext_end = 10;

    while (base_end >= 0 && short_name[base_end] == ' ')
    {
        base_end--;
    }

    for (int i = 0; i <= base_end; i++)
    {
        out[pos++] = ascii_lower((char)short_name[i]);
    }

    while (ext_end >= 8 && short_name[ext_end] == ' ')
    {
        ext_end--;
    }

    if (ext_end >= 8)
    {
        out[pos++] = '.';
        for (int i = 8; i <= ext_end; i++)
        {
            out[pos++] = ascii_lower((char)short_name[i]);
        }
    }

    out[pos] = '\0';
}

/*
 * Render a short 8.3 name in its canonical uppercase form for LFN decisions.
 */
static void render_short_name(const uint8_t short_name[11], char out[13])
{
    size_t pos = 0;
    int base_end = 7;
    int ext_end = 10;

    while (base_end >= 0 && short_name[base_end] == ' ')
    {
        base_end--;
    }
    for (int i = 0; i <= base_end; i++)
    {
        out[pos++] = (char)short_name[i];
    }

    while (ext_end >= 8 && short_name[ext_end] == ' ')
    {
        ext_end--;
    }

    if (ext_end >= 8)
    {
        out[pos++] = '.';
        for (int i = 8; i <= ext_end; i++)
        {
            out[pos++] = (char)short_name[i];
        }
    }
    out[pos] = '\0';
}

/*
 * Validate one new FAT pathname component before encoding it as short/LFN
 * directory entries.
 */
static int validate_new_name(const char *name)
{
    const size_t len = name == 0 ? 0 : strlen(name);

    if (len == 0 || len > FAT32_MAX_NAME || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        return -1;
    }

    for (size_t i = 0; i < len; i++)
    {
        const unsigned char ch = (unsigned char)name[i];

        if (ch < 0x20 || ch > 0x7e || string_contains_char("/\\:*?\"<>|", (char)ch))
        {
            return -1;
        }
    }

    return 0;
}

/*
 * Keep only FAT short-name characters, replacing unsupported characters with
 * underscores in the generated alias.
 */
static void clean_short_part(const char *src, size_t len, char *out, size_t out_size)
{
    static const char allowed[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&";
    size_t pos = 0;

    for (size_t i = 0; i < len && pos + 1 < out_size; i++)
    {
        char ch = src[i];

        if (ch >= 'a' && ch <= 'z')
        {
            ch = (char)(ch - 'a' + 'A');
        }
        out[pos++] = string_contains_char(allowed, ch) ? ch : '_';
    }

    out[pos] = '\0';
}

/*
 * Split a name into base and extension parts using the last dot, mirroring the
 * deterministic FAT image builder.
 */
static void split_short_parts(const char *name, char *base, size_t base_size, char *ext, size_t ext_size)
{
    const char *dot = find_last_dot(name);

    if (dot != 0)
    {
        clean_short_part(name, (size_t)(dot - name), base, base_size);
        clean_short_part(dot + 1, strlen(dot + 1), ext, ext_size);
    }
    else
    {
        clean_short_part(name, strlen(name), base, base_size);
        ext[0] = '\0';
    }

    if (base[0] == '\0')
    {
        strcpy(base, "_");
    }
}

/*
 * Pad a generated base/ext pair into the raw 11-byte FAT short-name field.
 */
static void format_short_name(const char *base, const char *ext, uint8_t out[11])
{
    memset(out, ' ', 11);
    for (size_t i = 0; i < 8 && base[i] != '\0'; i++)
    {
        out[i] = (uint8_t)base[i];
    }
    for (size_t i = 0; i < 3 && ext[i] != '\0'; i++)
    {
        out[8 + i] = (uint8_t)ext[i];
    }
}

/*
 * Render the decimal part of a generated ~N short-name suffix without relying
 * on stdio, which keeps this code usable in the small Nanos-lite kernel build.
 */
static int make_tilde_suffix(unsigned value, char out[8])
{
    char digits[10];
    size_t count = 0;
    size_t pos = 0;

    out[pos++] = '~';
    do
    {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));

    if (value != 0 || pos + count >= 8)
    {
        return -1;
    }

    while (count > 0)
    {
        out[pos++] = digits[--count];
    }
    out[pos] = '\0';
    return 0;
}

/*
 * Return the number of 32-byte directory slots in one cluster.
 */
static uint32_t entries_per_cluster(const Fat32Volume *vol)
{
    return (uint32_t)vol->sectors_per_cluster * FAT32_DIR_ENTRIES_PER_SECTOR;
}

/*
 * Translate a raw directory slot index into its byte offset and read the slot.
 * Return 1 for a slot, 0 after the current directory chain, and -1 on damage.
 */
static int read_directory_slot(const Fat32Volume *vol, uint32_t dir_cluster, uint32_t entry_index, uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], uint64_t *entry_offset)
{
    uint32_t cluster = dir_cluster;
    uint32_t index = entry_index;
    const uint32_t per_cluster = entries_per_cluster(vol);

    if (per_cluster == 0)
    {
        return -1;
    }

    for (uint32_t links = 0; links < vol->cluster_count; links++)
    {
        if (index < per_cluster)
        {
            uint32_t first_sector;
            const uint32_t sector_index = index / FAT32_DIR_ENTRIES_PER_SECTOR;
            const uint32_t slot_index = index % FAT32_DIR_ENTRIES_PER_SECTOR;
            size_t offset;

            if (fat32_first_sector_of_cluster(vol, cluster, &first_sector) != 0)
            {
                return -1;
            }
            const uint64_t real_sector = (uint64_t)first_sector + sector_index;

            if (real_sector > SIZE_MAX / FAT32_SECTOR_SIZE)
            {
                return -1;
            }
            offset = (size_t)real_sector * FAT32_SECTOR_SIZE + (size_t)slot_index * FAT32_DIR_ENTRY_SIZE;

            if (disk_read(raw_entry, offset, FAT32_DIR_ENTRY_SIZE) != FAT32_DIR_ENTRY_SIZE)
            {
                return -1;
            }

            if (entry_offset != 0)
            {
                *entry_offset = (uint64_t)offset;
            }
            return 1;
        }

        uint32_t next_cluster;

        index -= per_cluster;

        if (fat32_read_fat_entry(vol, cluster, &next_cluster) != 0)
        {
            return -1;
        }

        if (fat32_is_bad_cluster(next_cluster))
        {
            return -1;
        }

        if (fat32_is_end_of_chain(next_cluster))
        {
            return 0;
        }
        cluster = next_cluster;
    }

    return -1;
}

/*
 * Write a raw 32-byte directory slot at an existing slot index.
 */
static int write_directory_slot(const Fat32Volume *vol, uint32_t dir_cluster, uint32_t entry_index, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE])
{
    uint8_t old_entry[FAT32_DIR_ENTRY_SIZE];
    uint64_t offset;
    const int ret = read_directory_slot(vol, dir_cluster, entry_index, old_entry, &offset);

    (void)old_entry;

    if (ret != 1 || offset > SIZE_MAX)
    {
        return -1;
    }
    return disk_write(raw_entry, (size_t)offset, FAT32_DIR_ENTRY_SIZE) == FAT32_DIR_ENTRY_SIZE ? 0 : -1;
}

/*
 * Read the first cluster field from a short directory entry, normalising the
 * root ".." entry to the mounted root cluster for traversal.
 */
static uint32_t entry_first_cluster(const Fat32Volume *vol, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE])
{
    uint32_t cluster = ((uint32_t)get_le16(&raw_entry[20]) << 16) | get_le16(&raw_entry[26]);

    if (cluster == 0 && memcmp(raw_entry, "..         ", 11) == 0)
    {
        cluster = vol->root_cluster;
    }

    return cluster;
}

/*
 * Convert a raw short directory entry into the public Fat32DirEntry shape.
 */
static void fill_dir_entry(const Fat32Volume *vol, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], uint64_t dir_entry_offset, Fat32DirEntry *out)
{
    out->attr = raw_entry[11];
    out->first_cluster = entry_first_cluster(vol, raw_entry);
    out->size = get_le32(&raw_entry[28]);
    out->dir_entry_offset = dir_entry_offset;
}

/*
 * Test a short entry against a requested component using both its short alias
 * and any valid preceding LFN run.
 */
static int entry_name_matches(const Fat32LfnState *lfn, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], const char *component)
{
    char alias[13];
    char long_name[FAT32_MAX_NAME + 1u];

    short_name_to_alias(raw_entry, alias);

    if (ascii_case_equal(alias, component))
    {
        return 1;
    }

    if (lfn_build_name(lfn, raw_entry, long_name, sizeof(long_name)) == 0 && ascii_case_equal(long_name, component))
    {
        return 1;
    }

    return 0;
}

/*
 * Locate a visible entry inside one directory and keep enough slot information
 * to later delete or rename its whole LFN+short entry set.
 */
static int find_in_directory_full(const Fat32Volume *vol, uint32_t dir_cluster, const char *component, Fat32LocatedEntry *out)
{
    Fat32LfnState lfn;
    uint32_t lfn_start_index = 0;

    lfn_reset(&lfn);

    for (uint32_t entry_index = 0; entry_index < vol->cluster_count * entries_per_cluster(vol); entry_index++)
    {
        uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
        uint64_t entry_offset;
        const int ret = read_directory_slot(vol, dir_cluster, entry_index, raw_entry, &entry_offset);

        if (ret < 0)
        {
            return -1;
        }

        if (ret == 0)
        {
            return -1;
        }

        const uint8_t first_byte = raw_entry[0];
        const uint8_t attr = raw_entry[11];

        if (first_byte == 0x00)
        {
            return -1;
        }

        if (first_byte == 0xe5)
        {
            lfn_reset(&lfn);
            continue;
        }

        if (attr == FAT32_ATTR_LONG_NAME)
        {
            if ((raw_entry[0] & 0x40u) != 0)
            {
                lfn_start_index = entry_index;
            }
            lfn_add(&lfn, raw_entry);
            continue;
        }

        if ((attr & FAT32_ATTR_VOLUME_ID) != 0)
        {
            lfn_reset(&lfn);
            continue;
        }

        if (entry_name_matches(&lfn, raw_entry, component))
        {
            char long_name[FAT32_MAX_NAME + 1u];
            const int has_valid_lfn = lfn_build_name(&lfn, raw_entry, long_name, sizeof(long_name)) == 0;

            fill_dir_entry(vol, raw_entry, entry_offset, &out->entry);
            out->parent_cluster = dir_cluster;
            out->entry_index = entry_index;
            out->lfn_start_index = has_valid_lfn ? lfn_start_index : entry_index;
            out->lfn_entry_count = has_valid_lfn ? lfn.count : 0;
            return 0;
        }

        lfn_reset(&lfn);
    }

    return -1;
}

/*
 * Resolve one component in a directory when only the public entry is needed.
 */
static int find_in_directory(const Fat32Volume *vol, uint32_t dir_cluster, const char *component, Fat32DirEntry *out)
{
    Fat32LocatedEntry located;

    if (find_in_directory_full(vol, dir_cluster, component, &located) != 0)
    {
        return -1;
    }

    *out = located.entry;
    return 0;
}

/*
 * Split a path cursor into the next component and whether more components
 * follow it.
 */
static int next_component(const char **cursor, char *component, size_t component_size, int *has_more)
{
    size_t len = 0;
    const char *p = *cursor;

    while (*p == '/')
    {
        p++;
    }

    if (*p == '\0')
    {
        *cursor = p;
        *has_more = 0;
        return 0;
    }

    while (p[len] != '\0' && p[len] != '/')
    {
        if (len + 1 >= component_size)
        {
            return -1;
        }
        component[len] = p[len];
        len++;
    }
    component[len] = '\0';

    p += len;
    while (*p == '/')
    {
        p++;
    }

    *has_more = *p != '\0';
    *cursor = p;
    return 1;
}

/*
 * Split an absolute path into parent path and final component.
 */
static int split_parent_path(const char *path, char parent[FAT32_MAX_PATH], char name[FAT32_MAX_NAME + 1u])
{
    size_t len;
    size_t end;
    size_t slash;
    size_t name_len;

    if (path == 0 || path[0] != '/')
    {
        return -1;
    }

    len = strlen(path);

    if (len == 0 || len >= FAT32_MAX_PATH)
    {
        return -1;
    }

    end = len;
    while (end > 1 && path[end - 1] == '/')
    {
        end--;
    }

    if (end == 1)
    {
        return -1;
    }

    slash = end;
    while (slash > 0 && path[slash - 1] != '/')
    {
        slash--;
    }

    if (slash == 0)
    {
        return -1;
    }

    name_len = end - slash;

    if (name_len == 0 || name_len > FAT32_MAX_NAME)
    {
        return -1;
    }
    memcpy(name, &path[slash], name_len);
    name[name_len] = '\0';

    if (slash == 1)
    {
        strcpy(parent, "/");
    }
    else
    {
        memcpy(parent, path, slash - 1);
        parent[slash - 1] = '\0';
    }

    return validate_new_name(name);
}

/*
 * Resolve the parent directory and final component for a mutating path.
 */
static int lookup_parent(const Fat32Volume *vol, const char *path, uint32_t *parent_cluster, char name[FAT32_MAX_NAME + 1u])
{
    char parent[FAT32_MAX_PATH];
    Fat32DirEntry parent_entry;

    if (split_parent_path(path, parent, name) != 0)
    {
        return -1;
    }

    if (fat32_lookup_path(vol, parent, &parent_entry) != 0 || (parent_entry.attr & FAT32_ATTR_DIRECTORY) == 0)
    {
        return -1;
    }

    *parent_cluster = parent_entry.first_cluster;
    return 0;
}

/*
 * Check whether a raw short-name candidate already exists in a directory.
 */
static int short_name_in_use(const Fat32Volume *vol, uint32_t dir_cluster, const uint8_t short_name[11])
{
    for (uint32_t entry_index = 0; entry_index < vol->cluster_count * entries_per_cluster(vol); entry_index++)
    {
        uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
        const int ret = read_directory_slot(vol, dir_cluster, entry_index, raw_entry, 0);

        if (ret < 0)
        {
            return 1;
        }

        if (ret == 0 || raw_entry[0] == 0x00)
        {
            return 0;
        }

        if (raw_entry[0] == 0xe5 || raw_entry[11] == FAT32_ATTR_LONG_NAME)
        {
            continue;
        }

        if (memcmp(raw_entry, short_name, 11) == 0)
        {
            return 1;
        }
    }

    return 1;
}

/*
 * Generate a unique short 8.3 alias for a new long or short name.
 */
static int make_short_alias(const Fat32Volume *vol, uint32_t dir_cluster, const char *name, uint8_t out[11])
{
    char base[FAT32_MAX_NAME + 1u];
    char ext[FAT32_MAX_NAME + 1u];
    char candidate_base[9];
    char candidate_ext[4];

    split_short_parts(name, base, sizeof(base), ext, sizeof(ext));
    memset(candidate_base, 0, sizeof(candidate_base));
    memset(candidate_ext, 0, sizeof(candidate_ext));
    strncpy(candidate_ext, ext, 3);

    if (strlen(base) <= 8)
    {
        strncpy(candidate_base, base, 8);
        format_short_name(candidate_base, candidate_ext, out);
        if (!short_name_in_use(vol, dir_cluster, out))
        {
            return 0;
        }
    }

    for (unsigned index = 1; index < 1000000u; index++)
    {
        char suffix[8];
        size_t suffix_len;
        size_t prefix_len;

        if (make_tilde_suffix(index, suffix) != 0)
        {
            return -1;
        }
        suffix_len = strlen(suffix);
        prefix_len = 8u > suffix_len ? 8u - suffix_len : 1u;

        if (prefix_len > strlen(base))
        {
            prefix_len = strlen(base);
        }

        if (prefix_len == 0)
        {
            prefix_len = 1;
        }

        memset(candidate_base, 0, sizeof(candidate_base));
        strncpy(candidate_base, base, prefix_len);
        strcat(candidate_base, suffix);
        format_short_name(candidate_base, candidate_ext, out);
        if (!short_name_in_use(vol, dir_cluster, out))
        {
            return 0;
        }
    }

    return -1;
}

/*
 * Decide whether a visible name needs LFN entries before its short entry.
 */
static int name_needs_lfn(const char *name, const uint8_t short_name[11])
{
    char rendered[13];

    render_short_name(short_name, rendered);
    return strcmp(name, rendered) != 0;
}

/*
 * Count the LFN slots needed for an ASCII name.
 */
static uint32_t lfn_entry_count_for_name(const char *name)
{
    const size_t len = strlen(name);

    return (uint32_t)((len + 12u) / 13u);
}

/*
 * Fill one raw LFN entry for the selected one-based sequence number.
 */
static void fill_lfn_entry(uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], const char *name, uint32_t sequence, uint32_t total_count, const uint8_t short_name[11])
{
    const size_t name_len = strlen(name);
    const size_t start = (size_t)(sequence - 1u) * 13u;
    uint16_t units[13];
    Fat32LfnEntry *entry = (Fat32LfnEntry *)raw_entry;

    for (size_t i = 0; i < 13; i++)
    {
        const size_t pos = start + i;

        if (pos < name_len)
        {
            units[i] = (uint16_t)(uint8_t)name[pos];
        }
        else if (pos == name_len)
        {
            units[i] = 0x0000;
        }
        else
        {
            units[i] = 0xffff;
        }
    }

    memset(raw_entry, 0, FAT32_DIR_ENTRY_SIZE);
    entry->order = (uint8_t)(sequence | (sequence == total_count ? 0x40u : 0u));
    entry->attr = FAT32_ATTR_LONG_NAME;
    entry->type = 0;
    entry->checksum = fat32_lfn_checksum(short_name);
    entry->first_cluster_low = 0;
    for (int i = 0; i < 5; i++)
    {
        entry->name1[i] = units[i];
    }
    for (int i = 0; i < 6; i++)
    {
        entry->name2[i] = units[5 + i];
    }
    for (int i = 0; i < 2; i++)
    {
        entry->name3[i] = units[11 + i];
    }
}

/*
 * Fill one short directory entry with FAT32 cluster and size metadata.
 */
static void fill_short_entry(uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], const uint8_t short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t size)
{
    memset(raw_entry, 0, FAT32_DIR_ENTRY_SIZE);
    memcpy(raw_entry, short_name, 11);
    raw_entry[11] = attr;
    put_le16(&raw_entry[20], (uint16_t)(first_cluster >> 16));
    put_le16(&raw_entry[26], (uint16_t)first_cluster);
    put_le32(&raw_entry[28], size);
}

/*
 * Fill a dot or dotdot entry for a newly created directory cluster.
 */
static void fill_dot_entry(uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE], const char *name, uint32_t first_cluster)
{
    uint8_t short_name[11];

    memset(short_name, ' ', sizeof(short_name));
    for (size_t i = 0; i < 2 && name[i] != '\0'; i++)
    {
        short_name[i] = (uint8_t)name[i];
    }
    fill_short_entry(raw_entry, short_name, FAT32_ATTR_DIRECTORY, first_cluster, 0);
}

/*
 * Find the final cluster of a directory chain and count allocated clusters.
 */
static int find_directory_tail(const Fat32Volume *vol, uint32_t dir_cluster, uint32_t *out_tail, uint32_t *out_cluster_count)
{
    uint32_t cluster = dir_cluster;

    for (uint32_t links = 0; links < vol->cluster_count; links++)
    {
        uint32_t next;

        if (fat32_read_fat_entry(vol, cluster, &next) != 0)
        {
            return -1;
        }

        if (fat32_is_bad_cluster(next))
        {
            return -1;
        }

        if (fat32_is_end_of_chain(next))
        {
            *out_tail = cluster;
            *out_cluster_count = links + 1u;
            return 0;
        }
        cluster = next;
    }

    return -1;
}

/*
 * Append enough zeroed clusters to a directory to hold a new entry run.
 */
static int append_directory_clusters(Fat32Volume *vol, uint32_t dir_cluster, uint32_t needed_entries, uint32_t *out_start_index)
{
    uint32_t tail;
    uint32_t cluster_count;
    uint32_t clusters_needed;
    const uint32_t per_cluster = entries_per_cluster(vol);

    if (find_directory_tail(vol, dir_cluster, &tail, &cluster_count) != 0 || per_cluster == 0)
    {
        return -1;
    }

    *out_start_index = cluster_count * per_cluster;
    clusters_needed = (needed_entries + per_cluster - 1u) / per_cluster;
    for (uint32_t i = 0; i < clusters_needed; i++)
    {
        uint32_t new_cluster;

        if (fat32_alloc_cluster(vol, tail, &new_cluster) != 0)
        {
            return -1;
        }

        if (fat32_write_fat_entry(vol, tail, new_cluster) != 0)
        {
            (void)fat32_free_chain(vol, new_cluster);
            return -1;
        }
        tail = new_cluster;
    }

    return 0;
}

/*
 * Find or create a consecutive run of free directory slots for a new entry set.
 */
static int find_free_entry_run(Fat32Volume *vol, uint32_t dir_cluster, uint32_t needed_entries, uint32_t *out_start_index)
{
    uint32_t run_start = 0;
    uint32_t run_count = 0;
    const uint64_t limit = (uint64_t)vol->cluster_count * entries_per_cluster(vol);

    for (uint32_t entry_index = 0; (uint64_t)entry_index < limit; entry_index++)
    {
        uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
        const int ret = read_directory_slot(vol, dir_cluster, entry_index, raw_entry, 0);
        const int free_slot = ret == 1 && (raw_entry[0] == 0x00 || raw_entry[0] == 0xe5);

        if (ret < 0)
        {
            return -1;
        }

        if (ret == 0)
        {
            break;
        }

        if (free_slot)
        {
            if (run_count == 0)
            {
                run_start = entry_index;
            }
            run_count++;

            if (run_count >= needed_entries)
            {
                *out_start_index = run_start;
                return 0;
            }
        }
        else
        {
            run_count = 0;
        }
    }

    return append_directory_clusters(vol, dir_cluster, needed_entries, out_start_index);
}

/*
 * Write all LFN slots followed by the final short entry into a directory.
 */
static int write_entry_set(Fat32Volume *vol, uint32_t parent_cluster, uint32_t start_index, const char *name, const uint8_t short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t size, Fat32DirEntry *out)
{
    const uint32_t lfn_count = name_needs_lfn(name, short_name) ? lfn_entry_count_for_name(name) : 0;
    uint32_t slot = start_index;
    uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
    uint64_t short_offset;

    for (uint32_t sequence = lfn_count; sequence >= 1; sequence--)
    {
        fill_lfn_entry(raw_entry, name, sequence, lfn_count, short_name);

        if (write_directory_slot(vol, parent_cluster, slot, raw_entry) != 0)
        {
            return -1;
        }
        slot++;

        if (sequence == 1)
        {
            break;
        }
    }

    fill_short_entry(raw_entry, short_name, attr, first_cluster, size);

    if (write_directory_slot(vol, parent_cluster, slot, raw_entry) != 0)
    {
        return -1;
    }

    if (read_directory_slot(vol, parent_cluster, slot, raw_entry, &short_offset) != 1)
    {
        return -1;
    }

    if (out != 0)
    {
        fill_dir_entry(vol, raw_entry, short_offset, out);
    }

    return 0;
}

/*
 * Create a directory entry in a known parent directory.
 */
static int create_entry_in_directory(Fat32Volume *vol, uint32_t parent_cluster, const char *name, uint8_t attr, uint32_t first_cluster, uint32_t size, Fat32DirEntry *out)
{
    uint8_t short_name[11];
    uint32_t start_index;
    uint32_t needed_entries;

    if (validate_new_name(name) != 0 || make_short_alias(vol, parent_cluster, name, short_name) != 0)
    {
        return -1;
    }

    needed_entries = 1u + (name_needs_lfn(name, short_name) ? lfn_entry_count_for_name(name) : 0u);

    if (find_free_entry_run(vol, parent_cluster, needed_entries, &start_index) != 0)
    {
        return -1;
    }

    return write_entry_set(vol, parent_cluster, start_index, name, short_name, attr, first_cluster, size, out);
}

/*
 * Create the initial dot and dotdot entries in a newly allocated directory.
 */
static int initialise_directory_cluster(Fat32Volume *vol, uint32_t dir_cluster, uint32_t parent_cluster)
{
    uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
    const uint32_t parent_for_dotdot = parent_cluster == vol->root_cluster ? 0 : parent_cluster;

    fill_dot_entry(raw_entry, ".", dir_cluster);

    if (write_directory_slot(vol, dir_cluster, 0, raw_entry) != 0)
    {
        return -1;
    }

    fill_dot_entry(raw_entry, "..", parent_for_dotdot);

    if (write_directory_slot(vol, dir_cluster, 1, raw_entry) != 0)
    {
        return -1;
    }

    return 0;
}

/*
 * Check whether a directory has visible entries other than dot and dotdot.
 */
static int directory_is_empty(Fat32Volume *vol, uint32_t dir_cluster)
{
    Fat32Dir dir;
    Fat32Dirent entry;
    int ret;

    dir.first_cluster = dir_cluster;
    dir.next_entry_index = 0;
    while ((ret = fat32_readdir(vol, &dir, &entry)) == 1)
    {
        if (strcmp(entry.name, ".") != 0 && strcmp(entry.name, "..") != 0)
        {
            return 0;
        }
    }

    return ret == 0 ? 1 : -1;
}

/*
 * Mark a raw directory slot as deleted while preserving the remaining bytes for
 * FAT-compatible undelete/debug behaviour.
 */
static int mark_entry_deleted(const Fat32Volume *vol, uint32_t dir_cluster, uint32_t entry_index)
{
    uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];

    if (read_directory_slot(vol, dir_cluster, entry_index, raw_entry, 0) != 1)
    {
        return -1;
    }
    raw_entry[0] = 0xe5;
    return write_directory_slot(vol, dir_cluster, entry_index, raw_entry);
}

/*
 * Delete the LFN slots and short entry for a located path, optionally freeing
 * its cluster chain.
 */
static int delete_located_entry(Fat32Volume *vol, const Fat32LocatedEntry *located, int free_chain)
{
    const uint32_t start = located->lfn_entry_count != 0 ? located->lfn_start_index : located->entry_index;
    const uint32_t end = located->entry_index;

    if (free_chain && located->entry.first_cluster != 0)
    {
        if (fat32_free_chain(vol, located->entry.first_cluster) != 0)
        {
            return -1;
        }
    }

    for (uint32_t index = start; index <= end; index++)
    {
        if (mark_entry_deleted(vol, located->parent_cluster, index) != 0)
        {
            return -1;
        }
    }

    return 0;
}

/*
 * Locate a full absolute path and return mutation metadata for its final entry.
 */
static int locate_path(const Fat32Volume *vol, const char *path, Fat32LocatedEntry *out)
{
    uint32_t parent_cluster;
    char name[FAT32_MAX_NAME + 1u];

    if (lookup_parent(vol, path, &parent_cluster, name) != 0)
    {
        return -1;
    }

    return find_in_directory_full(vol, parent_cluster, name, out);
}

/*
 * Mount a FAT32 volume from the block device and load its FSInfo hints.
 */
int fat32_mount_from_disk(uint32_t disk_block_size, Fat32Volume *out)
{
    uint8_t sector[FAT32_SECTOR_SIZE];

    if (out == 0)
    {
        return -1;
    }

    if (disk_read(sector, 0, sizeof(sector)) != sizeof(sector))
    {
        return -1;
    }

    if (fat32_parse_bpb(sector, disk_block_size, out) != 0)
    {
        return -1;
    }

    return fat32_load_fsinfo(out);
}

/*
 * Resolve an absolute path to a file or directory entry.
 */
int fat32_lookup_path(const Fat32Volume *vol, const char *path, Fat32DirEntry *out)
{
    const char *cursor = path;
    uint32_t current_cluster;
    Fat32DirEntry current;
    char component[FAT32_MAX_NAME + 1u];

    if (vol == 0 || path == 0 || out == 0 || path[0] != '/')
    {
        return -1;
    }

    current_cluster = vol->root_cluster;
    current.attr = FAT32_ATTR_DIRECTORY;
    current.first_cluster = current_cluster;
    current.size = 0;
    current.dir_entry_offset = 0;

    while (1)
    {
        int has_more;
        const int ret = next_component(&cursor, component, sizeof(component), &has_more);

        if (ret < 0)
        {
            return -1;
        }

        if (ret == 0)
        {
            *out = current;
            return 0;
        }

        if ((current.attr & FAT32_ATTR_DIRECTORY) == 0)
        {
            return -1;
        }

        if (strcmp(component, ".") == 0)
        {
            continue;
        }

        if (strcmp(component, "..") == 0 && current_cluster == vol->root_cluster)
        {
            current.attr = FAT32_ATTR_DIRECTORY;
            current.first_cluster = vol->root_cluster;
            current.size = 0;
            current.dir_entry_offset = 0;
            continue;
        }

        if (find_in_directory(vol, current_cluster, component, &current) != 0)
        {
            return -1;
        }

        if (has_more && (current.attr & FAT32_ATTR_DIRECTORY) == 0)
        {
            return -1;
        }

        current_cluster = current.first_cluster;
    }
}

/*
 * Open a directory path for sequential visible-entry iteration.
 */
int fat32_opendir_path(const Fat32Volume *vol, const char *path, Fat32Dir *out)
{
    Fat32DirEntry entry;

    if (vol == 0 || path == 0 || out == 0)
    {
        return -1;
    }

    if (fat32_lookup_path(vol, path, &entry) != 0 || (entry.attr & FAT32_ATTR_DIRECTORY) == 0)
    {
        return -1;
    }

    out->first_cluster = entry.first_cluster;
    out->next_entry_index = 0;
    return 0;
}

/*
 * Return the next visible entry from a FAT32 directory iterator.
 */
int fat32_readdir(Fat32Volume *vol, Fat32Dir *dir, Fat32Dirent *out)
{
    Fat32LfnState lfn;

    if (vol == 0 || dir == 0 || out == 0)
    {
        return -1;
    }

    lfn_reset(&lfn);
    for (;;)
    {
        uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE];
        uint64_t entry_offset;
        const int ret = read_directory_slot(vol, dir->first_cluster, dir->next_entry_index, raw_entry, &entry_offset);

        if (ret < 0)
        {
            return -1;
        }

        if (ret == 0)
        {
            return 0;
        }

        const uint8_t first_byte = raw_entry[0];
        const uint8_t attr = raw_entry[11];

        if (first_byte == 0x00)
        {
            return 0;
        }

        dir->next_entry_index++;

        if (first_byte == 0xe5)
        {
            lfn_reset(&lfn);
            continue;
        }

        if (attr == FAT32_ATTR_LONG_NAME)
        {
            lfn_add(&lfn, raw_entry);
            continue;
        }

        if ((attr & FAT32_ATTR_VOLUME_ID) != 0)
        {
            lfn_reset(&lfn);
            continue;
        }

        if (lfn_build_name(&lfn, raw_entry, out->name, sizeof(out->name)) != 0)
        {
            short_name_to_alias(raw_entry, out->name);
        }
        fill_dir_entry(vol, raw_entry, entry_offset, &out->entry);
        return 1;
    }
}

/*
 * Create a regular file or directory entry at an absolute path.
 */
int fat32_create_path(Fat32Volume *vol, const char *path, uint8_t attr, Fat32DirEntry *out)
{
    uint32_t parent_cluster;
    uint32_t first_cluster = 0;
    char name[FAT32_MAX_NAME + 1u];

    if (vol == 0 || path == 0 || lookup_parent(vol, path, &parent_cluster, name) != 0)
    {
        return -1;
    }

    if (fat32_lookup_path(vol, path, &(Fat32DirEntry){0}) == 0)
    {
        return -1;
    }

    if ((attr & FAT32_ATTR_DIRECTORY) != 0)
    {
        if (fat32_alloc_cluster(vol, parent_cluster, &first_cluster) != 0)
        {
            return -1;
        }

        if (initialise_directory_cluster(vol, first_cluster, parent_cluster) != 0)
        {
            (void)fat32_free_chain(vol, first_cluster);
            return -1;
        }
    }

    if (create_entry_in_directory(vol, parent_cluster, name, attr, first_cluster, 0, out) != 0)
    {
        if (first_cluster != 0)
        {
            (void)fat32_free_chain(vol, first_cluster);
        }
        return -1;
    }

    return 0;
}

/*
 * Remove a regular file path and free its allocated cluster chain.
 */
int fat32_unlink_path(Fat32Volume *vol, const char *path)
{
    Fat32LocatedEntry located;

    if (vol == 0 || path == 0 || locate_path(vol, path, &located) != 0)
    {
        return -1;
    }

    if ((located.entry.attr & FAT32_ATTR_DIRECTORY) != 0)
    {
        return -1;
    }

    return delete_located_entry(vol, &located, 1);
}

/*
 * Remove an empty directory path and free its directory cluster chain.
 */
int fat32_rmdir_path(Fat32Volume *vol, const char *path)
{
    Fat32LocatedEntry located;

    if (vol == 0 || path == 0 || strcmp(path, "/") == 0 || locate_path(vol, path, &located) != 0)
    {
        return -1;
    }

    if ((located.entry.attr & FAT32_ATTR_DIRECTORY) == 0)
    {
        return -1;
    }

    if (directory_is_empty(vol, located.entry.first_cluster) != 1)
    {
        return -1;
    }

    return delete_located_entry(vol, &located, 1);
}

/*
 * Rename a file or directory by creating a new entry pointing at the same
 * cluster chain, then deleting the old entry without freeing that chain.
 */
int fat32_rename_path(Fat32Volume *vol, const char *old_path, const char *new_path)
{
    Fat32LocatedEntry old_entry;
    Fat32LocatedEntry new_entry;
    uint32_t new_parent_cluster;
    char new_name[FAT32_MAX_NAME + 1u];
    int new_exists;
    int old_is_dir;
    int new_is_dir;

    if (vol == 0 || locate_path(vol, old_path, &old_entry) != 0)
    {
        return -1;
    }

    if (lookup_parent(vol, new_path, &new_parent_cluster, new_name) != 0)
    {
        return -1;
    }

    new_exists = find_in_directory_full(vol, new_parent_cluster, new_name, &new_entry) == 0;

    if (new_exists && new_entry.parent_cluster == old_entry.parent_cluster && new_entry.entry_index == old_entry.entry_index)
    {
        return 0;
    }

    old_is_dir = (old_entry.entry.attr & FAT32_ATTR_DIRECTORY) != 0;
    new_is_dir = new_exists && (new_entry.entry.attr & FAT32_ATTR_DIRECTORY) != 0;

    if (old_is_dir && old_entry.parent_cluster != new_parent_cluster)
    {
        return -1;
    }

    if (new_exists)
    {
        if (old_is_dir != new_is_dir)
        {
            return -1;
        }

        if (new_is_dir && directory_is_empty(vol, new_entry.entry.first_cluster) != 1)
        {
            return -1;
        }

        if (delete_located_entry(vol, &new_entry, 1) != 0)
        {
            return -1;
        }
    }

    if (create_entry_in_directory(vol, new_parent_cluster, new_name, old_entry.entry.attr, old_entry.entry.first_cluster, old_entry.entry.size, 0) != 0)
    {
        return -1;
    }

    if (delete_located_entry(vol, &old_entry, 0) != 0)
    {
        return -1;
    }

    return 0;
}
