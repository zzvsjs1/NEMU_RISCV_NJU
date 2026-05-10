#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/fs/fat32.h"

static void init_lfn_entry(Fat32LfnEntry *entry, const uint16_t chars[13])
{
  memset(entry, 0xff, sizeof(*entry));
  entry->order = 0x41;
  entry->attr = FAT32_ATTR_LONG_NAME;
  entry->type = 0;
  entry->checksum = 0x7a;
  entry->first_cluster_low = 0;
  fat32_test_fill_lfn_entry(entry, chars);
}

static void test_short_name_checksum(void)
{
  const uint8_t name[11] = "LONG-N~1TXT";
  assert(fat32_lfn_checksum(name) == 0x10);
}

static void test_ascii_lfn_piece_decodes(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'l', 'o', 'n', 'g', '-', 'n', 'a', 'm', 'e', '.', 't', 'x', 't'
  };
  init_lfn_entry(&entry, chars);

  char out[32];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == 13);
  assert(strcmp(out, "long-name.txt") == 0);
}

static void test_short_lfn_accepts_zero_terminator_before_padding(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'r', 'e', 'a', 'd', 'm', 'e', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == 6);
  assert(strcmp(out, "readme") == 0);
}

static void test_padding_before_terminator_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'b', 'a', 'd', 0xffff, 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);

  char out[16] = "unchanged";
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_non_padding_after_terminator_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'r', 'e', 'a', 'd', 0x0000, 'X', 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_invalid_attr_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'n', 'a', 'm', 'e', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);
  entry.attr = FAT32_ATTR_ARCHIVE;

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_non_zero_type_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'n', 'a', 'm', 'e', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);
  entry.type = 1;

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_non_zero_first_cluster_low_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'n', 'a', 'm', 'e', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);
  entry.first_cluster_low = 1;

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_non_ascii_unit_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    'n', 'a', 0x00e9, 'e', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);

  char out[16];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

static void test_too_small_output_buffer_is_rejected(void)
{
  Fat32LfnEntry entry;
  const uint16_t chars[13] = {
    't', 'o', 'o', '-', 'l', 'o', 'n', 'g', 0x0000, 0xffff, 0xffff, 0xffff, 0xffff
  };
  init_lfn_entry(&entry, chars);

  char out[4];
  assert(fat32_lfn_entry_to_ascii(&entry, out, sizeof(out)) == -1);
}

int main(void)
{
  test_short_name_checksum();
  test_ascii_lfn_piece_decodes();
  test_short_lfn_accepts_zero_terminator_before_padding();
  test_padding_before_terminator_is_rejected();
  test_non_padding_after_terminator_is_rejected();
  test_invalid_attr_is_rejected();
  test_non_zero_type_is_rejected();
  test_non_zero_first_cluster_low_is_rejected();
  test_non_ascii_unit_is_rejected();
  test_too_small_output_buffer_is_rejected();
  puts("fat32_lfn tests passed");
  return 0;
}
