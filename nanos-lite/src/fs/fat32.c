#include "fat32.h"

#include <stdint.h>
#include <string.h>

size_t disk_read(void *buf, size_t offset, size_t len);
size_t disk_write(const void *buf, size_t offset, size_t len);

#define FAT32_SECTOR_SIZE 512u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_RESERVED_CLUSTER_MIN 0x0ffffff0u
#define FAT32_RESERVED_CLUSTER_MAX 0x0ffffff6u
#define FAT32_READ_RUN_MAX (128u * 1024u)

static Fat32Volume mounted_volume;
static int mounted;

/*
 * Return the smaller of two byte counts without relying on a libc helper.
 */
static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

/*
 * Convert the mounted FAT32 cluster geometry into a byte count.
 */
static int cluster_size_bytes(const Fat32Volume *vol, size_t *out)
{
    const uint64_t size = (uint64_t)vol->sectors_per_cluster * FAT32_SECTOR_SIZE;

    if (size == 0 || size > SIZE_MAX)
    {
        return -1;
    }

    *out = (size_t)size;
    return 0;
}

/*
 * Test whether a cluster number names an addressable FAT32 data cluster.
 */
static int is_data_cluster(const Fat32Volume *vol, uint32_t cluster)
{
    return cluster >= 2 && (uint64_t)cluster <= (uint64_t)vol->cluster_count + 1u;
}

/*
 * Test whether a non-empty regular file may start at this cluster number.
 */
static int is_valid_file_data_cluster(const Fat32Volume *vol, uint32_t cluster)
{
    /*
   * Non-empty files must start at a real data cluster.  FAT values in the
   * reserved, bad, or end-of-chain ranges are not usable cluster numbers even
   * if a malformed BPB claims a very large cluster count.
   */
    if (!is_data_cluster(vol, cluster))
    {
        return 0;
    }
    if (cluster >= FAT32_RESERVED_CLUSTER_MIN || fat32_is_bad_cluster(cluster) || fat32_is_end_of_chain(cluster))
    {
        return 0;
    }

    return 1;
}

/*
 * Reject FAT chain values that cannot be followed to another data cluster.
 */
static int is_unusable_chain_value(const Fat32Volume *vol, uint32_t cluster)
{
    const uint32_t value = cluster & FAT32_ENTRY_MASK;

    if (value == 0 || fat32_is_bad_cluster(value) || fat32_is_end_of_chain(value))
    {
        return 1;
    }
    if (value >= FAT32_RESERVED_CLUSTER_MIN && value <= FAT32_RESERVED_CLUSTER_MAX)
    {
        return 1;
    }

    return !is_data_cluster(vol, value);
}

/*
 * Convert a sector number to a byte offset while checking size_t overflow.
 */
static int checked_sector_byte_offset(uint64_t sector_number, size_t *out)
{
    if (sector_number > SIZE_MAX / FAT32_SECTOR_SIZE)
    {
        return -1;
    }

    *out = (size_t)sector_number * FAT32_SECTOR_SIZE;
    return 0;
}

/*
 * Convert a cluster-relative byte offset to an absolute disk-image byte offset.
 */
static int checked_cluster_byte_offset(uint32_t first_sector, size_t offset_in_cluster,
                                       size_t *out)
{
    size_t cluster_base;

    if (checked_sector_byte_offset(first_sector, &cluster_base) != 0)
    {
        return -1;
    }
    if (offset_in_cluster > SIZE_MAX - cluster_base)
    {
        return -1;
    }

    *out = cluster_base + offset_in_cluster;
    return 0;
}

/*
 * Round a file byte length up to the number of clusters that must remain linked.
 */
static uint32_t clusters_for_file_size(size_t file_size, size_t cluster_size)
{
    if (file_size == 0 || cluster_size == 0)
    {
        return 0;
    }

    const size_t clusters = (file_size + cluster_size - 1u) / cluster_size;
    return clusters > UINT32_MAX ? UINT32_MAX : (uint32_t)clusters;
}

/*
 * Extend the verified contiguous-cluster prefix when a followed FAT link proves
 * that the next logical cluster is also the next physical cluster.
 */
static void remember_contiguous_cluster(Fat32File *file, uint32_t cluster_index,
                                        uint32_t cluster)
{
    if (file == 0 || file->first_cluster == 0)
    {
        return;
    }

    /*
   * This cache records only the verified prefix starting at first_cluster.
   * FAT32 permits fragmented chains, so never infer contiguity from arithmetic
   * alone; callers reach this helper only after either reading the FAT link or
   * using an already verified prefix.
   */
    if (file->contiguous_cluster_count == cluster_index && (uint64_t)cluster == (uint64_t)file->first_cluster + cluster_index)
    {
        file->contiguous_cluster_count = cluster_index + 1u;
    }
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
 * Follow a FAT chain to a logical cluster index, using verified contiguity and
 * the last cached chain position to avoid repeatedly walking from the start.
 */
static int seek_cluster(Fat32File *file, uint32_t cluster_index, uint32_t *out_cluster)
{
    uint32_t cluster = file->first_cluster;
    uint32_t index = 0;

    if (!is_data_cluster(&mounted_volume, cluster))
    {
        return -1;
    }

    if (file->contiguous_cluster_count > cluster_index)
    {
        const uint64_t direct_cluster = (uint64_t)file->first_cluster + cluster_index;
        if (direct_cluster > UINT32_MAX || !is_data_cluster(&mounted_volume, (uint32_t)direct_cluster))
        {
            return -1;
        }

        file->cached_cluster_index = cluster_index;
        file->cached_cluster = (uint32_t)direct_cluster;
        *out_cluster = (uint32_t)direct_cluster;
        return 0;
    }

    if (file->contiguous_cluster_count > 0)
    {
        const uint32_t prefix_last_index = file->contiguous_cluster_count - 1u;
        const uint64_t prefix_last_cluster = (uint64_t)file->first_cluster + prefix_last_index;

        if (prefix_last_cluster <= UINT32_MAX && is_data_cluster(&mounted_volume, (uint32_t)prefix_last_cluster))
        {
            cluster = (uint32_t)prefix_last_cluster;
            index = prefix_last_index;
        }
    }

    /*
   * Reads commonly advance forwards.  Reusing the cached point keeps repeated
   * small reads from restarting at the first cluster every time.
   */
    if (file->cached_cluster != 0 && file->cached_cluster_index <= cluster_index && file->cached_cluster_index > index && is_data_cluster(&mounted_volume, file->cached_cluster))
    {
        cluster = file->cached_cluster;
        index = file->cached_cluster_index;
    }

    while (index < cluster_index)
    {
        uint32_t next_cluster;

        if (fat32_read_fat_entry(&mounted_volume, cluster, &next_cluster) != 0)
        {
            return -1;
        }
        if (is_unusable_chain_value(&mounted_volume, next_cluster))
        {
            return -1;
        }

        cluster = next_cluster & FAT32_ENTRY_MASK;
        index++;
        remember_contiguous_cluster(file, index, cluster);
    }

    file->cached_cluster_index = index;
    file->cached_cluster = cluster;
    *out_cluster = cluster;
    return 0;
}

/*
 * Ensure a logical cluster exists, allocating and linking new clusters as needed.
 */
static int ensure_cluster(Fat32File *file, uint32_t cluster_index, uint32_t *out_cluster)
{
    uint32_t cluster;
    uint32_t index = 0;

    if (file->first_cluster == 0)
    {
        if (fat32_alloc_cluster(&mounted_volume, 0, &cluster) != 0)
        {
            return -1;
        }
        file->first_cluster = cluster;
        file->cached_cluster_index = 0;
        file->cached_cluster = cluster;
        file->contiguous_cluster_count = 1;
    }

    cluster = file->first_cluster;
    if (!is_data_cluster(&mounted_volume, cluster))
    {
        return -1;
    }

    if (file->contiguous_cluster_count > cluster_index)
    {
        const uint64_t direct_cluster = (uint64_t)file->first_cluster + cluster_index;
        if (direct_cluster > UINT32_MAX || !is_data_cluster(&mounted_volume, (uint32_t)direct_cluster))
        {
            return -1;
        }

        file->cached_cluster_index = cluster_index;
        file->cached_cluster = (uint32_t)direct_cluster;
        *out_cluster = (uint32_t)direct_cluster;
        return 0;
    }

    if (file->contiguous_cluster_count > 0)
    {
        const uint32_t prefix_last_index = file->contiguous_cluster_count - 1u;
        const uint64_t prefix_last_cluster = (uint64_t)file->first_cluster + prefix_last_index;

        if (prefix_last_cluster <= UINT32_MAX && is_data_cluster(&mounted_volume, (uint32_t)prefix_last_cluster))
        {
            cluster = (uint32_t)prefix_last_cluster;
            index = prefix_last_index;
        }
    }

    if (file->cached_cluster != 0 && file->cached_cluster_index <= cluster_index && file->cached_cluster_index > index && is_data_cluster(&mounted_volume, file->cached_cluster))
    {
        cluster = file->cached_cluster;
        index = file->cached_cluster_index;
    }

    while (index < cluster_index)
    {
        uint32_t next_cluster;

        if (fat32_read_fat_entry(&mounted_volume, cluster, &next_cluster) != 0)
        {
            return -1;
        }

        if (fat32_is_end_of_chain(next_cluster))
        {
            uint32_t new_cluster;

            if (fat32_alloc_cluster(&mounted_volume, cluster, &new_cluster) != 0)
            {
                return -1;
            }
            if (fat32_write_fat_entry(&mounted_volume, cluster, new_cluster) != 0)
            {
                (void)fat32_free_chain(&mounted_volume, new_cluster);
                return -1;
            }
            next_cluster = new_cluster;
        }
        else if (is_unusable_chain_value(&mounted_volume, next_cluster))
        {
            return -1;
        }

        cluster = next_cluster & FAT32_ENTRY_MASK;
        index++;
        remember_contiguous_cluster(file, index, cluster);
    }

    file->cached_cluster_index = index;
    file->cached_cluster = cluster;
    *out_cluster = cluster;
    return 0;
}

/*
 * Scan the opened file's FAT chain once to discover the contiguous prefix that
 * can later be read as large disk runs.
 */
static void discover_contiguous_prefix(Fat32File *file)
{
    size_t cluster_size;
    uint32_t total_clusters;
    uint32_t cluster;

    if (file == 0 || file->first_cluster == 0 || file->size == 0)
    {
        return;
    }
    if (cluster_size_bytes(&mounted_volume, &cluster_size) != 0)
    {
        return;
    }

    total_clusters = clusters_for_file_size(file->size, cluster_size);
    if (total_clusters == 0)
    {
        return;
    }

    file->contiguous_cluster_count = 1;
    cluster = file->first_cluster;

    while (file->contiguous_cluster_count < total_clusters)
    {
        uint32_t next_cluster;

        if (fat32_read_fat_entry(&mounted_volume, cluster, &next_cluster) != 0)
        {
            break;
        }
        if (is_unusable_chain_value(&mounted_volume, next_cluster))
        {
            break;
        }

        next_cluster &= FAT32_ENTRY_MASK;
        if ((uint64_t)next_cluster != (uint64_t)cluster + 1u)
        {
            break;
        }

        cluster = next_cluster;
        remember_contiguous_cluster(file, file->contiguous_cluster_count, cluster);
    }

    file->cached_cluster_index = file->contiguous_cluster_count - 1u;
    file->cached_cluster = cluster;
}

/*
 * Build the longest bounded disk-read run that stays inside verified contiguous
 * clusters, starting from the already resolved cluster.
 */
static int build_read_run(Fat32File *file, uint32_t cluster_index, uint32_t cluster,
                          size_t offset_in_cluster, size_t cluster_size,
                          size_t remaining, size_t *out_len,
                          uint32_t *out_last_index, uint32_t *out_last_cluster)
{
    const size_t limit = min_size(remaining, (size_t)FAT32_READ_RUN_MAX);
    uint32_t current_index = cluster_index;
    uint32_t current_cluster = cluster;
    size_t cluster_offset = offset_in_cluster;
    size_t run_len = 0;

    if (limit == 0 || cluster_size == 0 || offset_in_cluster >= cluster_size)
    {
        return -1;
    }

    while (run_len < limit)
    {
        const size_t cluster_left = cluster_size - cluster_offset;
        const size_t chunk = min_size(cluster_left, limit - run_len);

        run_len += chunk;
        if (chunk < cluster_left || run_len == limit)
        {
            break;
        }

        if (file->contiguous_cluster_count > current_index + 1u)
        {
            current_index++;
            current_cluster++;
            cluster_offset = 0;
            continue;
        }

        uint32_t next_cluster;
        if (fat32_read_fat_entry(&mounted_volume, current_cluster, &next_cluster) != 0)
        {
            break;
        }
        if (is_unusable_chain_value(&mounted_volume, next_cluster))
        {
            break;
        }

        next_cluster &= FAT32_ENTRY_MASK;
        if ((uint64_t)next_cluster != (uint64_t)current_cluster + 1u)
        {
            break;
        }

        current_index++;
        current_cluster = next_cluster;
        remember_contiguous_cluster(file, current_index, current_cluster);
        cluster_offset = 0;
    }

    *out_len = run_len;
    *out_last_index = current_index;
    *out_last_cluster = current_cluster;
    return run_len > 0 ? 0 : -1;
}

/*
 * Validate and narrow the on-disk directory-entry offset for metadata updates.
 */
static int checked_dir_entry_offset(const Fat32File *file, size_t *out)
{
    if (file == 0 || out == 0 || file->dir_entry_offset == 0 || file->dir_entry_offset > SIZE_MAX)
    {
        return -1;
    }

    *out = (size_t)file->dir_entry_offset;
    return 0;
}

/*
 * Write the current first-cluster and size fields back to the short directory
 * entry that owns an opened regular file.
 */
static int update_dir_entry(const Fat32File *file)
{
    uint8_t entry[32];
    size_t offset;

    if (checked_dir_entry_offset(file, &offset) != 0)
    {
        return -1;
    }
    if (disk_read(entry, offset, sizeof(entry)) != sizeof(entry))
    {
        return -1;
    }

    put_le16(&entry[20], (uint16_t)(file->first_cluster >> 16));
    put_le16(&entry[26], (uint16_t)file->first_cluster);
    put_le32(&entry[28], file->size);

    if (disk_write(entry, offset, sizeof(entry)) != sizeof(entry))
    {
        return -1;
    }

    return 0;
}

/*
 * Write bytes into a FAT32 file, allocating clusters on demand and preserving
 * untouched parts of partial sectors.
 */
static size_t write_bytes(Fat32File *file, size_t offset, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t cluster_size;
    size_t remaining = len;
    size_t copied = 0;

    if (file == 0 || buf == 0 || len == 0 || offset > UINT32_MAX || len > (size_t)UINT32_MAX - offset)
    {
        return 0;
    }
    if (cluster_size_bytes(&mounted_volume, &cluster_size) != 0)
    {
        return 0;
    }

    while (remaining > 0)
    {
        const uint32_t cluster_index = (uint32_t)(offset / cluster_size);
        const size_t offset_in_cluster = offset % cluster_size;
        const uint32_t first_sector_index = (uint32_t)(offset_in_cluster / FAT32_SECTOR_SIZE);
        size_t offset_in_sector = offset_in_cluster % FAT32_SECTOR_SIZE;
        size_t cluster_left = cluster_size - offset_in_cluster;
        uint32_t cluster;
        uint32_t first_sector;

        if (cluster_index != offset / cluster_size)
        {
            break;
        }
        if (ensure_cluster(file, cluster_index, &cluster) != 0)
        {
            break;
        }
        if (fat32_first_sector_of_cluster(&mounted_volume, cluster, &first_sector) != 0)
        {
            break;
        }

        for (uint32_t sector_index = first_sector_index;
             sector_index < mounted_volume.sectors_per_cluster && remaining > 0 && cluster_left > 0;
             sector_index++)
        {
            uint8_t sector[FAT32_SECTOR_SIZE];
            size_t byte_offset;
            size_t chunk;
            const uint64_t sector_number = (uint64_t)first_sector + sector_index;

            if (checked_sector_byte_offset(sector_number, &byte_offset) != 0)
            {
                return copied;
            }

            chunk = min_size(FAT32_SECTOR_SIZE - offset_in_sector, remaining);
            chunk = min_size(chunk, cluster_left);

            if (offset_in_sector == 0 && chunk == FAT32_SECTOR_SIZE)
            {
                if (disk_write(&src[copied], byte_offset, chunk) != chunk)
                {
                    return copied;
                }
            }
            else
            {
                if (disk_read(sector, byte_offset, sizeof(sector)) != sizeof(sector))
                {
                    return copied;
                }
                memcpy(&sector[offset_in_sector], &src[copied], chunk);
                if (disk_write(sector, byte_offset, sizeof(sector)) != sizeof(sector))
                {
                    return copied;
                }
            }

            copied += chunk;
            offset += chunk;
            remaining -= chunk;
            cluster_left -= chunk;
            offset_in_sector = 0;
        }
    }

    if (copied > 0 && offset > file->size)
    {
        file->size = (uint32_t)offset;
    }
    return copied;
}

/*
 * Write zero bytes through the normal allocation path so sparse POSIX growth
 * exposes deterministic zero-filled data.
 */
static size_t zero_fill_gap(Fat32File *file, size_t offset, size_t len)
{
    uint8_t zero[FAT32_SECTOR_SIZE];
    size_t done = 0;

    memset(zero, 0, sizeof(zero));
    while (done < len)
    {
        const size_t chunk = min_size(sizeof(zero), len - done);
        const size_t wrote = write_bytes(file, offset + done, zero, chunk);

        done += wrote;
        if (wrote != chunk)
        {
            break;
        }
    }

    return done;
}

/*
 * Populate an opened-file state object from a resolved FAT32 directory entry.
 */
static int open_from_dir_entry(const Fat32DirEntry *entry, Fat32File *out)
{
    memset(out, 0, sizeof(*out));
    out->attr = entry->attr;
    out->first_cluster = entry->first_cluster;
    out->size = entry->size;
    out->dir_entry_offset = entry->dir_entry_offset;
    if (entry->size != 0)
    {
        if (!is_valid_file_data_cluster(&mounted_volume, entry->first_cluster))
        {
            return -1;
        }
        out->contiguous_cluster_count = 1;
        out->cached_cluster = entry->first_cluster;
        discover_contiguous_prefix(out);
    }
    return 0;
}

/*
 * Mount the FAT32 backend once and keep its parsed BPB/FSInfo state cached.
 */
int fat32_backend_init(void)
{
    if (mounted)
    {
        return 0;
    }

    if (fat32_mount_from_disk(FAT32_SECTOR_SIZE, &mounted_volume) != 0)
    {
        return -1;
    }

    mounted = 1;
    return 0;
}

/*
 * Open an existing FAT32 regular file by path.
 */
int fat32_backend_open(const char *path, Fat32File *out)
{
    Fat32DirEntry entry;

    if (path == 0 || out == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }
    if (fat32_lookup_path(&mounted_volume, path, &entry) != 0)
    {
        return -1;
    }
    if ((entry.attr & FAT32_ATTR_DIRECTORY) != 0)
    {
        return -1;
    }

    return open_from_dir_entry(&entry, out);
}

/*
 * Resolve metadata for a FAT32 path without requiring it to be a regular file.
 */
int fat32_backend_lookup(const char *path, Fat32DirEntry *out)
{
    if (path == 0 || out == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_lookup_path(&mounted_volume, path, out);
}

/*
 * Create a new empty regular file and return it as an opened FAT32 file.
 */
int fat32_backend_create(const char *path, Fat32File *out)
{
    Fat32DirEntry entry;

    if (path == 0 || out == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }
    if (fat32_create_path(&mounted_volume, path, FAT32_ATTR_ARCHIVE, &entry) != 0)
    {
        return -1;
    }

    return open_from_dir_entry(&entry, out);
}

/*
 * Open a FAT32 directory for visible-entry iteration.
 */
int fat32_backend_opendir(const char *path, Fat32Dir *out)
{
    if (path == 0 || out == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_opendir_path(&mounted_volume, path, out);
}

/*
 * Return the next visible entry from an opened FAT32 directory.
 */
int fat32_backend_readdir(Fat32Dir *dir, Fat32Dirent *out)
{
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_readdir(&mounted_volume, dir, out);
}

/*
 * Create a FAT32 directory, including its dot and dotdot entries.
 */
int fat32_backend_mkdir(const char *path)
{
    if (path == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_create_path(&mounted_volume, path, FAT32_ATTR_DIRECTORY, 0);
}

/*
 * Remove a regular FAT32 file and free its cluster chain.
 */
int fat32_backend_unlink(const char *path)
{
    if (path == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_unlink_path(&mounted_volume, path);
}

/*
 * Remove an empty FAT32 directory and free its cluster chain.
 */
int fat32_backend_rmdir(const char *path)
{
    if (path == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_rmdir_path(&mounted_volume, path);
}

/*
 * Rename a FAT32 entry.  Regular files may move between directories; directory
 * renames are kept inside one parent so their dotdot entries stay correct.
 */
int fat32_backend_rename(const char *old_path, const char *new_path)
{
    if (old_path == 0 || new_path == 0)
    {
        return -1;
    }
    if (fat32_backend_init() != 0)
    {
        return -1;
    }

    return fat32_rename_path(&mounted_volume, old_path, new_path);
}

/*
 * Return the mounted volume for white-box host tests.
 */
#ifdef FAT32_HOST_TEST
const Fat32Volume *fat32_backend_volume(void)
{
    return mounted ? &mounted_volume : 0;
}
#endif

/*
 * Resize a FAT32 file to a shorter size, freeing any tail clusters past the
 * cluster that contains the new end of file.
 */
static int shrink_file(Fat32File *file, uint32_t size)
{
    size_t cluster_size;
    const uint32_t old_first_cluster = file->first_cluster;
    uint32_t keep_clusters;

    if (cluster_size_bytes(&mounted_volume, &cluster_size) != 0)
    {
        return -1;
    }

    keep_clusters = clusters_for_file_size(size, cluster_size);
    if (keep_clusters == 0)
    {
        if (old_first_cluster != 0 && fat32_free_chain(&mounted_volume, old_first_cluster) != 0)
        {
            return -1;
        }
        file->first_cluster = 0;
        file->cached_cluster = 0;
        file->cached_cluster_index = 0;
        file->contiguous_cluster_count = 0;
    }
    else
    {
        uint32_t last_kept;
        uint32_t first_freed;

        if (seek_cluster(file, keep_clusters - 1u, &last_kept) != 0)
        {
            return -1;
        }
        if (fat32_read_fat_entry(&mounted_volume, last_kept, &first_freed) != 0)
        {
            return -1;
        }
        if (fat32_write_fat_entry(&mounted_volume, last_kept, FAT32_ENTRY_MASK) != 0)
        {
            return -1;
        }
        if (!fat32_is_end_of_chain(first_freed))
        {
            first_freed &= FAT32_ENTRY_MASK;
            if (is_data_cluster(&mounted_volume, first_freed) && fat32_free_chain(&mounted_volume, first_freed) != 0)
            {
                return -1;
            }
        }
        file->cached_cluster_index = keep_clusters - 1u;
        file->cached_cluster = last_kept;
        if (file->contiguous_cluster_count > keep_clusters)
        {
            file->contiguous_cluster_count = keep_clusters;
        }
    }

    file->size = size;
    return 0;
}

/*
 * Read bytes from a regular FAT32 file, coalescing verified contiguous cluster
 * runs into larger disk reads for better sequential performance.
 */
size_t fat32_backend_read(Fat32File *file, size_t offset, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t cluster_size;
    size_t remaining;
    size_t copied = 0;

    if (file == 0 || buf == 0 || len == 0)
    {
        return 0;
    }

    if (file->size == 0 || offset >= file->size)
    {
        return 0;
    }

    if (cluster_size_bytes(&mounted_volume, &cluster_size) != 0)
    {
        return 0;
    }

    remaining = min_size(len, (size_t)file->size - offset);
    while (remaining > 0)
    {
        const uint32_t cluster_index = (uint32_t)(offset / cluster_size);
        const size_t offset_in_cluster = offset % cluster_size;
        uint32_t cluster;
        uint32_t first_sector;
        uint32_t last_cluster_index;
        uint32_t last_cluster;
        size_t byte_offset;
        size_t run_len;

        if (cluster_index != offset / cluster_size)
        {
            break;
        }
        if (seek_cluster(file, cluster_index, &cluster) != 0)
        {
            break;
        }
        if (fat32_first_sector_of_cluster(&mounted_volume, cluster, &first_sector) != 0)
        {
            break;
        }
        if (checked_cluster_byte_offset(first_sector, offset_in_cluster, &byte_offset) != 0)
        {
            return copied;
        }
        if (build_read_run(file, cluster_index, cluster, offset_in_cluster, cluster_size,
                           remaining, &run_len, &last_cluster_index, &last_cluster) != 0)
        {
            break;
        }

        /*
     * The sectors inside a FAT32 cluster, and inside a verified run of
     * consecutive clusters, are contiguous on disk.  Reading the whole byte run
     * lets disk.c use its larger DMA bounce buffer, matching the old flat
     * filesystem's large-asset behaviour while still honouring the FAT chain.
     */
        if (disk_read(&dst[copied], byte_offset, run_len) != run_len)
        {
            return copied;
        }

        file->cached_cluster_index = last_cluster_index;
        file->cached_cluster = last_cluster;
        copied += run_len;
        offset += run_len;
        remaining -= run_len;
    }

    return copied;
}

/*
 * Write bytes to a regular FAT32 file and persist any size or first-cluster
 * change back into its directory entry.
 */
size_t fat32_backend_write(Fat32File *file, size_t offset, const void *buf, size_t len)
{
    const uint32_t old_first_cluster = file != 0 ? file->first_cluster : 0;
    const uint32_t old_size = file != 0 ? file->size : 0;
    size_t copied;

    if (file == 0 || buf == 0 || len == 0)
    {
        return 0;
    }
    if ((file->attr & FAT32_ATTR_READ_ONLY) != 0 || file->dir_entry_offset == 0)
    {
        return 0;
    }
    if (offset > UINT32_MAX || len > (size_t)UINT32_MAX - offset)
    {
        return 0;
    }

    if (offset > file->size)
    {
        const size_t gap = offset - file->size;
        if (zero_fill_gap(file, file->size, gap) != gap)
        {
            (void)update_dir_entry(file);
            return 0;
        }
    }

    copied = write_bytes(file, offset, buf, len);
    if (file->first_cluster != old_first_cluster || file->size != old_size)
    {
        if (update_dir_entry(file) != 0)
        {
            return 0;
        }
    }

    return copied;
}

/*
 * Return the current logical size of an opened FAT32 file snapshot.
 */
size_t fat32_backend_size(const Fat32File *file)
{
    if (file == 0)
    {
        return 0;
    }

    return file->size;
}

/*
 * Resize a FAT32 regular file, freeing tail clusters on shrink and zero-filling
 * newly exposed bytes on growth.
 */
int fat32_backend_truncate(Fat32File *file, uint32_t size)
{
    const uint32_t old_first_cluster = file != 0 ? file->first_cluster : 0;
    const uint32_t old_size = file != 0 ? file->size : 0;

    if (file == 0 || file->dir_entry_offset == 0)
    {
        return -1;
    }
    if ((file->attr & FAT32_ATTR_READ_ONLY) != 0)
    {
        return -1;
    }

    if (size == file->size)
    {
        return 0;
    }

    if (size > file->size)
    {
        const size_t gap = (size_t)size - file->size;

        if (zero_fill_gap(file, file->size, gap) != gap)
        {
            return -1;
        }
    }
    else if (shrink_file(file, size) != 0)
    {
        return -1;
    }

    if (file->first_cluster != old_first_cluster || file->size != old_size)
    {
        return update_dir_entry(file);
    }

    return 0;
}

/*
 * Release an opened FAT32 file snapshot.
 */
int fat32_backend_close(Fat32File *file)
{
    if (file == 0)
    {
        return -1;
    }

    memset(file, 0, sizeof(*file));
    return 0;
}

#ifdef FAT32_HOST_TEST
/*
 * Clear the cached mounted volume between host-side FAT32 tests.
 */
void fat32_test_reset_backend(void)
{
    memset(&mounted_volume, 0, sizeof(mounted_volume));
    mounted = 0;
}
#endif
