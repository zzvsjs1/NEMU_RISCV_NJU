#include "fat32.h"

#include <stdbool.h>

/*
 * Compute the FAT long-name checksum over the following short DIR_Name field.
 * Every LFN slot in a run stores this same value; if it does not match the
 * short entry, the run is stale or belongs to another file and must be ignored.
 */
uint8_t fat32_lfn_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
    {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + short_name[i]);
    }

    return sum;
}

static int copy_ascii_unit(uint16_t unit, char *out, size_t out_size, size_t *pos)
{
    /*
   * FAT LFN text is UTF-16LE.  Nanos-lite currently keeps pathname matching in
   * a small ASCII subset, so reject non-printable and non-ASCII units instead
   * of returning a lossy name.
   */
    if (unit == 0x0000)
    {
        return 0;
    }

    if (unit < 0x20 || unit > 0x7e)
    {
        return -1;
    }

    if (*pos + 1 >= out_size)
    {
        return -1;
    }

    out[*pos] = (char)unit;
    *pos += 1;
    return 1;
}

static uint16_t lfn_unit_at(const Fat32LfnEntry *entry, int index)
{
    /* Read packed fields directly, avoiding unaligned pointers to UTF-16 units. */
    if (index < 5)
    {
        return entry->name1[index];
    }

    if (index < 11)
    {
        return entry->name2[index - 5];
    }

    return entry->name3[index - 11];
}

static bool lfn_has_only_padding_after(const Fat32LfnEntry *entry, int index)
{
    /*
   * After the first UTF-16 zero terminator in an LFN fragment, the FAT spec
   * pads remaining code units with 0xffff.  Any other value means the long-name
   * slot is malformed.
   */
    for (int i = index + 1; i < 13; i++)
    {
        if (lfn_unit_at(entry, i) != 0xffff)
        {
            return false;
        }
    }

    return true;
}

int fat32_lfn_entry_to_ascii(const Fat32LfnEntry *entry, char *out, size_t out_size)
{
    size_t pos = 0;

    if (out_size == 0 || entry->attr != FAT32_ATTR_LONG_NAME || entry->type != 0 || entry->first_cluster_low != 0)
    {
        return -1;
    }

    for (int i = 0; i < 13; i++)
    {
        int ret = copy_ascii_unit(lfn_unit_at(entry, i), out, out_size, &pos);
        if (ret < 0)
        {
            out[0] = '\0';
            return -1;
        }
        if (ret == 0)
        {
            if (!lfn_has_only_padding_after(entry, i))
            {
                out[0] = '\0';
                return -1;
            }
            out[pos] = '\0';
            return (int)pos;
        }
    }

    out[pos] = '\0';
    return (int)pos;
}

#ifdef FAT32_HOST_TEST
void fat32_test_fill_lfn_entry(Fat32LfnEntry *entry, const uint16_t chars[13])
{
    for (int i = 0; i < 5; i++)
    {
        entry->name1[i] = chars[i];
    }
    for (int i = 0; i < 6; i++)
    {
        entry->name2[i] = chars[5 + i];
    }
    for (int i = 0; i < 2; i++)
    {
        entry->name3[i] = chars[11 + i];
    }
}
#endif
