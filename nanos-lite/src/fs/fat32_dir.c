#include "fat32.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

size_t disk_read(void *buf, size_t offset, size_t len);

#define FAT32_SECTOR_SIZE 512u
#define FAT32_DIR_ENTRY_SIZE 32u
#define FAT32_DIR_ENTRIES_PER_SECTOR (FAT32_SECTOR_SIZE / FAT32_DIR_ENTRY_SIZE)
#define FAT32_MAX_LFN_CHARS 260u
#define FAT32_MAX_LFN_PIECES (FAT32_MAX_LFN_CHARS / 13u)

typedef struct {
  bool saw_last;
  bool malformed;
  uint8_t checksum;
  unsigned count;
  unsigned expected_sequence;
  bool present[FAT32_MAX_LFN_PIECES + 1u];
  char pieces[FAT32_MAX_LFN_PIECES + 1u][14];
} Fat32LfnState;

static uint16_t get_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char ascii_lower(char ch)
{
  if (ch >= 'A' && ch <= 'Z') {
    return (char)(ch - 'A' + 'a');
  }

  return ch;
}

static int ascii_case_equal(const char *a, const char *b)
{
  while (*a != '\0' && *b != '\0') {
    if (ascii_lower(*a) != ascii_lower(*b)) {
      return 0;
    }
    a++;
    b++;
  }

  return *a == '\0' && *b == '\0';
}

static void lfn_reset(Fat32LfnState *state)
{
  memset(state, 0, sizeof(*state));
}

static void lfn_add(Fat32LfnState *state, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE])
{
  const Fat32LfnEntry *entry = (const Fat32LfnEntry *)raw_entry;
  const unsigned sequence = entry->order & 0x1fu;
  const bool is_last = (entry->order & 0x40u) != 0;

  if (sequence == 0 || sequence > FAT32_MAX_LFN_PIECES) {
    lfn_reset(state);
    state->malformed = true;
    return;
  }

  if (is_last) {
    lfn_reset(state);
    state->saw_last = true;
    state->count = sequence;
    state->expected_sequence = sequence - 1u;
    state->checksum = entry->checksum;
  } else if (!state->saw_last || sequence != state->expected_sequence
             || entry->checksum != state->checksum || state->present[sequence]) {
    lfn_reset(state);
    state->malformed = true;
    return;
  } else {
    state->expected_sequence--;
  }

  if (fat32_lfn_entry_to_ascii(entry, state->pieces[sequence], sizeof(state->pieces[sequence])) < 0) {
    lfn_reset(state);
    state->malformed = true;
    return;
  }

  state->present[sequence] = true;
}

static int lfn_build_name(const Fat32LfnState *state, const uint8_t short_name[11],
                          char *out, size_t out_size)
{
  size_t pos = 0;

  if (out_size == 0 || state->malformed || !state->saw_last || state->count == 0
      || state->count > FAT32_MAX_LFN_PIECES || state->expected_sequence != 0
      || state->checksum != fat32_lfn_checksum(short_name)) {
    return -1;
  }

  for (unsigned sequence = 1; sequence <= state->count; sequence++) {
    const char *piece = state->pieces[sequence];

    if (!state->present[sequence]) {
      return -1;
    }

    while (*piece != '\0') {
      if (pos + 1 >= out_size) {
        return -1;
      }
      out[pos++] = *piece++;
    }
  }

  out[pos] = '\0';
  return 0;
}

static void short_name_to_alias(const uint8_t short_name[11], char out[13])
{
  size_t pos = 0;
  int base_end = 7;
  int ext_end = 10;

  while (base_end >= 0 && short_name[base_end] == ' ') {
    base_end--;
  }

  for (int i = 0; i <= base_end; i++) {
    out[pos++] = ascii_lower((char)short_name[i]);
  }

  while (ext_end >= 8 && short_name[ext_end] == ' ') {
    ext_end--;
  }

  if (ext_end >= 8) {
    out[pos++] = '.';
    for (int i = 8; i <= ext_end; i++) {
      out[pos++] = ascii_lower((char)short_name[i]);
    }
  }

  out[pos] = '\0';
}

static uint32_t entry_first_cluster(const Fat32Volume *vol, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE])
{
  uint32_t cluster = ((uint32_t)get_le16(&raw_entry[20]) << 16) | get_le16(&raw_entry[26]);

  /*
   * FAT32 stores cluster zero in root ".." directory entries.  Regular files
   * may also have cluster zero when their size is zero, so only directories get
   * normalised to the root cluster for later directory-chain reads.
   */
  if (cluster == 0 && memcmp(raw_entry, "..         ", 11) == 0) {
    cluster = vol->root_cluster;
  }

  return cluster;
}

static void fill_dir_entry(const Fat32Volume *vol, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE],
                           Fat32DirEntry *out)
{
  out->attr = raw_entry[11];
  out->first_cluster = entry_first_cluster(vol, raw_entry);
  out->size = get_le32(&raw_entry[28]);
}

static int entry_name_matches(const Fat32LfnState *lfn, const uint8_t raw_entry[FAT32_DIR_ENTRY_SIZE],
                              const char *component)
{
  char alias[13];
  char long_name[FAT32_MAX_LFN_CHARS + 1u];

  short_name_to_alias(raw_entry, alias);
  if (ascii_case_equal(alias, component)) {
    return 1;
  }

  if (lfn_build_name(lfn, raw_entry, long_name, sizeof(long_name)) == 0
      && ascii_case_equal(long_name, component)) {
    return 1;
  }

  return 0;
}

static int find_in_directory(const Fat32Volume *vol, uint32_t dir_cluster,
                             const char *component, Fat32DirEntry *out)
{
  uint32_t cluster = dir_cluster;
  Fat32LfnState lfn;

  lfn_reset(&lfn);

  for (uint32_t links = 0; links < vol->cluster_count; links++) {
    uint32_t first_sector;
    uint8_t sector[FAT32_SECTOR_SIZE];

    if (fat32_first_sector_of_cluster(vol, cluster, &first_sector) != 0) {
      return -1;
    }

    for (uint32_t sector_index = 0; sector_index < vol->sectors_per_cluster; sector_index++) {
      const uint64_t sector_number = (uint64_t)first_sector + sector_index;
      if (sector_number > SIZE_MAX / FAT32_SECTOR_SIZE) {
        return -1;
      }
      if (disk_read(sector, (size_t)sector_number * FAT32_SECTOR_SIZE, sizeof(sector))
          != sizeof(sector)) {
        return -1;
      }

      for (size_t i = 0; i < FAT32_DIR_ENTRIES_PER_SECTOR; i++) {
        const uint8_t *raw_entry = &sector[i * FAT32_DIR_ENTRY_SIZE];
        const uint8_t first_byte = raw_entry[0];
        const uint8_t attr = raw_entry[11];

        if (first_byte == 0x00) {
          return -1;
        }

        if (first_byte == 0xe5) {
          lfn_reset(&lfn);
          continue;
        }

        if (attr == FAT32_ATTR_LONG_NAME) {
          lfn_add(&lfn, raw_entry);
          continue;
        }

        if ((attr & FAT32_ATTR_VOLUME_ID) != 0) {
          lfn_reset(&lfn);
          continue;
        }

        if (entry_name_matches(&lfn, raw_entry, component)) {
          fill_dir_entry(vol, raw_entry, out);
          return 0;
        }

        lfn_reset(&lfn);
      }
    }

    uint32_t next_cluster;
    if (fat32_read_fat_entry(vol, cluster, &next_cluster) != 0) {
      return -1;
    }
    if (fat32_is_bad_cluster(next_cluster)) {
      return -1;
    }
    if (fat32_is_end_of_chain(next_cluster)) {
      return -1;
    }
    cluster = next_cluster;
  }

  return -1;
}

static int next_component(const char **cursor, char *component, size_t component_size,
                          int *has_more)
{
  size_t len = 0;
  const char *p = *cursor;

  while (*p == '/') {
    p++;
  }

  if (*p == '\0') {
    *cursor = p;
    *has_more = 0;
    return 0;
  }

  while (p[len] != '\0' && p[len] != '/') {
    if (len + 1 >= component_size) {
      return -1;
    }
    component[len] = p[len];
    len++;
  }
  component[len] = '\0';

  p += len;
  while (*p == '/') {
    p++;
  }

  *has_more = *p != '\0';
  *cursor = p;
  return 1;
}

int fat32_mount_from_disk(uint32_t disk_block_size, Fat32Volume *out)
{
  uint8_t sector[FAT32_SECTOR_SIZE];

  if (out == 0) {
    return -1;
  }

  if (disk_read(sector, 0, sizeof(sector)) != sizeof(sector)) {
    return -1;
  }

  return fat32_parse_bpb(sector, disk_block_size, out);
}

int fat32_lookup_path(const Fat32Volume *vol, const char *path, Fat32DirEntry *out)
{
  const char *cursor = path;
  uint32_t current_cluster;
  Fat32DirEntry current;
  char component[FAT32_MAX_LFN_CHARS + 1u];

  if (vol == 0 || path == 0 || out == 0 || path[0] != '/') {
    return -1;
  }

  current_cluster = vol->root_cluster;
  current.attr = FAT32_ATTR_DIRECTORY;
  current.first_cluster = current_cluster;
  current.size = 0;

  while (1) {
    int has_more;
    const int ret = next_component(&cursor, component, sizeof(component), &has_more);

    if (ret < 0) {
      return -1;
    }
    if (ret == 0) {
      *out = current;
      return 0;
    }

    if ((current.attr & FAT32_ATTR_DIRECTORY) == 0) {
      return -1;
    }

    if (strcmp(component, ".") == 0) {
      continue;
    }

    if (strcmp(component, "..") == 0 && current_cluster == vol->root_cluster) {
      current.attr = FAT32_ATTR_DIRECTORY;
      current.first_cluster = vol->root_cluster;
      current.size = 0;
      continue;
    }

    if (find_in_directory(vol, current_cluster, component, &current) != 0) {
      return -1;
    }
    if (has_more && (current.attr & FAT32_ATTR_DIRECTORY) == 0) {
      return -1;
    }

    current_cluster = current.first_cluster;
  }
}
