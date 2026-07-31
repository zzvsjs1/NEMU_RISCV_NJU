#ifndef __DEVICE_MAP_H__
#define __DEVICE_MAP_H__

#include <cpu/difftest.h>

typedef void (*io_callback_t)(uint32_t, int, bool);
uint8_t *new_space(int size);

/*
 * A direct-read bit is an explicit device contract for one access width.  It
 * promises that bypassing the map's read callback is semantically identical to
 * reading the current bytes from `space`, and that `space` remains valid for
 * the machine lifetime.  Generated code must still issue exactly one backing
 * load per guest load; these bits do not permit value caching or speculation.
 */
enum
{
    IO_MAP_DIRECT_READ_NONE = 0u,
    IO_MAP_DIRECT_READ_1 = 1u << 0,
    IO_MAP_DIRECT_READ_2 = 1u << 1,
    IO_MAP_DIRECT_READ_4 = 1u << 2,
    IO_MAP_DIRECT_READ_8 = 1u << 3,
    IO_MAP_DIRECT_READ_ALL =
        IO_MAP_DIRECT_READ_1 | IO_MAP_DIRECT_READ_2 |
        IO_MAP_DIRECT_READ_4 | IO_MAP_DIRECT_READ_8,
};

/*
 * Direct-write width bits are separate from direct-read bits because I/O PMAs
 * permit a device region to be idempotent in only one direction. A registered
 * write region promises that bypassing the callback is semantically identical
 * to exactly one current-width write into its backing bytes. It does not permit
 * caching, combining, speculation, duplication, widening, or splitting.
 */
enum
{
    IO_MAP_DIRECT_WRITE_NONE = 0u,
    IO_MAP_DIRECT_WRITE_1 = 1u << 0,
    IO_MAP_DIRECT_WRITE_2 = 1u << 1,
    IO_MAP_DIRECT_WRITE_4 = 1u << 2,
    IO_MAP_DIRECT_WRITE_8 = 1u << 3,
    IO_MAP_DIRECT_WRITE_ALL =
        IO_MAP_DIRECT_WRITE_1 | IO_MAP_DIRECT_WRITE_2 |
        IO_MAP_DIRECT_WRITE_4 | IO_MAP_DIRECT_WRITE_8,
};

typedef struct
{
    const char *name;
    // we treat ioaddr_t as paddr_t here
    paddr_t low;
    paddr_t high;
    void *space;
    io_callback_t callback;
    uint8_t direct_read_widths;
} IOMap;

/*
 * Direct-write permission is a subregion rather than an IOMap-wide property:
 * one device map may contain passive staging words beside command registers.
 * The backing pointer corresponds exactly to `low`, and remains stable for the
 * machine lifetime just like its owning map.
 */
typedef struct
{
    const char *map_name;
    paddr_t low;
    paddr_t high;
    void *space;
    uint8_t direct_write_widths;
} IODirectWriteRegion;

static inline bool map_inside(const IOMap *map, paddr_t addr)
{
    return (addr >= map->low && addr <= map->high);
}

/* Return the direct-read contract bit corresponding to one architectural width. */
static inline uint8_t io_map_direct_read_width(int len)
{
    switch (len)
    {
    case 1:
        return IO_MAP_DIRECT_READ_1;
    case 2:
        return IO_MAP_DIRECT_READ_2;
    case 4:
        return IO_MAP_DIRECT_READ_4;
    case 8:
        return IO_MAP_DIRECT_READ_8;
    default:
        return IO_MAP_DIRECT_READ_NONE;
    }
}

/* Test the explicit contract; never infer direct-read safety from the callback. */
static inline bool map_supports_direct_read(const IOMap *map, int len)
{
    return map != NULL &&
           (map->direct_read_widths & io_map_direct_read_width(len)) != 0u;
}

/* Return the direct-write contract bit for one architectural store width. */
static inline uint8_t io_map_direct_write_width(int len)
{
    switch (len)
    {
    case 1:
        return IO_MAP_DIRECT_WRITE_1;
    case 2:
        return IO_MAP_DIRECT_WRITE_2;
    case 4:
        return IO_MAP_DIRECT_WRITE_4;
    case 8:
        return IO_MAP_DIRECT_WRITE_8;
    default:
        return IO_MAP_DIRECT_WRITE_NONE;
    }
}

/* Test an explicit subregion contract; never infer safety from its callback. */
static inline bool io_direct_write_region_supports(
    const IODirectWriteRegion *region, int len)
{
    return region != NULL &&
           (region->direct_write_widths &
            io_map_direct_write_width(len)) != 0u;
}

static inline int find_mapid_by_addr(IOMap *maps, int size, paddr_t addr)
{
    int i;

    for (i = 0; i < size; i++)
    {
        if (map_inside(maps + i, addr))
        {
            difftest_skip_ref();
            return i;
        }
    }

    return -1;
}

void add_pio_map(const char *name, ioaddr_t addr,
                 void *space, uint32_t len, io_callback_t callback);
void add_mmio_map(const char *name, paddr_t addr,
                  void *space, uint32_t len, io_callback_t callback);
void add_mmio_map_with_direct_read(
    const char *name, paddr_t addr, void *space, uint32_t len,
    io_callback_t callback, uint8_t direct_read_widths);

/*
 * The table is populated during device initialisation and has no removal or
 * remapping operation.  These const views are therefore stable while generated
 * blocks execute.
 */
size_t mmio_direct_read_map_count(void);
const IOMap *mmio_direct_read_map(size_t direct_index);
void add_mmio_direct_write_region(
    paddr_t addr, uint32_t len, uint8_t direct_write_widths);
size_t mmio_direct_write_region_count(void);
const IODirectWriteRegion *mmio_direct_write_region(size_t direct_index);
void mmio_freeze_direct_routes(void);

word_t map_read(paddr_t addr, int len, IOMap *map);
void map_write(paddr_t addr, int len, word_t data, IOMap *map);

#endif
