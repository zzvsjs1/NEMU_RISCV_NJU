#include <device/map.h>
#include <memory/paddr.h>

#define NR_MAP 16
#define NR_DIRECT_WRITE_REGION 16

static IOMap maps[NR_MAP] = {};
static int nr_map = 0;
static IOMap *last_map = NULL;
static IODirectWriteRegion direct_write_regions[NR_DIRECT_WRITE_REGION] = {};
static int nr_direct_write_region = 0;
static bool direct_routes_frozen = false;

static IOMap *fetch_mmio_map(paddr_t addr)
{
    if (last_map != NULL && map_inside(last_map, addr))
    {
        difftest_skip_ref();
        return last_map;
    }

    int mapid = find_mapid_by_addr(maps, nr_map, addr);

    last_map = mapid == -1 ? NULL : &maps[mapid];
    return last_map;
}

static void report_mmio_overlap(const char *name1, paddr_t l1, paddr_t r1,
                                const char *name2, paddr_t l2, paddr_t r2)
{
    panic("MMIO region %s@[" FMT_PADDR ", " FMT_PADDR "] is overlapped "
          "with %s@[" FMT_PADDR ", " FMT_PADDR "]",
          name1, l1, r1, name2, l2, r2);
}

/*
 * Register one MMIO map with an explicit direct-read contract.  Ordinary
 * add_mmio_map() callers enter below with no direct widths and therefore retain
 * the callback-backed behaviour by default.
 */
void add_mmio_map_with_direct_read(
    const char *name, paddr_t addr, void *space, uint32_t len,
    io_callback_t callback, uint8_t direct_read_widths)
{
    assert(nr_map < NR_MAP);
    assert(len > 0);
    assert((direct_read_widths & ~IO_MAP_DIRECT_READ_ALL) == 0u);
    assert(direct_read_widths == IO_MAP_DIRECT_READ_NONE ||
           !direct_routes_frozen);
    assert(direct_read_widths == IO_MAP_DIRECT_READ_NONE || space != NULL);
    assert((direct_read_widths & IO_MAP_DIRECT_READ_2) == 0u || len >= 2u);
    assert((direct_read_widths & IO_MAP_DIRECT_READ_4) == 0u || len >= 4u);
    assert((direct_read_widths & IO_MAP_DIRECT_READ_8) == 0u || len >= 8u);

    const paddr_t left = addr;
    const paddr_t right = addr + len - 1u;
    assert(right >= left);

    const paddr_t pmem_left = (paddr_t)CONFIG_MBASE;
    const paddr_t pmem_right = (paddr_t)CONFIG_MBASE + CONFIG_MSIZE;

    if (in_pmem(left) || in_pmem(right) || in_pmem_range(left, len) ||
        (left < pmem_left && right >= pmem_right))
    {
        report_mmio_overlap(name, left, right,
                            "pmem", pmem_left, pmem_right - 1u);
    }

    for (int i = 0; i < nr_map; i++)
    {
        if (left <= maps[i].high && right >= maps[i].low)
        {
            report_mmio_overlap(name, left, right,
                                maps[i].name, maps[i].low, maps[i].high);
        }
    }

    /*
     * MMIO maps live in the physical address space and are selected by paddr.c
     * when an access is outside PMEM.  Ranges are inclusive because map_read()
     * and map_write() check the final byte address against high.
     */
    maps[nr_map] = (IOMap){
        .name = name,
        .low = addr,
        .high = addr + len - 1,
        .space = space,
        .callback = callback,
        .direct_read_widths = direct_read_widths,
    };

    Log("Add mmio map '%s' at [" FMT_PADDR ", " FMT_PADDR "]",
        maps[nr_map].name, maps[nr_map].low, maps[nr_map].high);

    nr_map++;
    last_map = NULL;
}

/* Conservatively register a callback-backed map with no generated-code bypass. */
void add_mmio_map(const char *name, paddr_t addr, void *space, uint32_t len,
                  io_callback_t callback)
{
    add_mmio_map_with_direct_read(
        name, addr, space, len, callback, IO_MAP_DIRECT_READ_NONE);
}

/* Count the maps which have explicitly approved at least one direct-read width. */
size_t mmio_direct_read_map_count(void)
{
    size_t count = 0;

    assert(direct_routes_frozen);

    for (int i = 0; i < nr_map; i++)
    {
        count += maps[i].direct_read_widths != IO_MAP_DIRECT_READ_NONE ? 1u : 0u;
    }

    return count;
}

/* Return one stable, const direct-readable map selected by dense index. */
const IOMap *mmio_direct_read_map(size_t direct_index)
{
    assert(direct_routes_frozen);

    for (int i = 0; i < nr_map; i++)
    {
        if (maps[i].direct_read_widths == IO_MAP_DIRECT_READ_NONE)
        {
            continue;
        }

        if (direct_index == 0u)
        {
            return &maps[i];
        }

        direct_index--;
    }

    return NULL;
}

/*
 * Register one immutable, callback-free write subregion inside an existing MMIO
 * map. The owning map is resolved here so callers cannot accidentally pair a
 * physical range with unrelated host backing.
 */
void add_mmio_direct_write_region(
    paddr_t addr, uint32_t len, uint8_t direct_write_widths)
{
    assert(nr_direct_write_region < NR_DIRECT_WRITE_REGION);
    assert(!direct_routes_frozen);
    assert(len > 0u);
    assert(direct_write_widths != IO_MAP_DIRECT_WRITE_NONE);
    assert((direct_write_widths & ~IO_MAP_DIRECT_WRITE_ALL) == 0u);
    assert((direct_write_widths & IO_MAP_DIRECT_WRITE_2) == 0u || len >= 2u);
    assert((direct_write_widths & IO_MAP_DIRECT_WRITE_4) == 0u || len >= 4u);
    assert((direct_write_widths & IO_MAP_DIRECT_WRITE_8) == 0u || len >= 8u);

    const paddr_t left = addr;
    const paddr_t right = addr + len - 1u;
    IOMap *owner = NULL;

    assert(right >= left);

    for (int i = 0; i < nr_map; i++)
    {
        if (left >= maps[i].low && right <= maps[i].high)
        {
            owner = &maps[i];
            break;
        }
    }

    assert(owner != NULL);
    assert(owner->space != NULL);

    for (int i = 0; i < nr_direct_write_region; i++)
    {
        const IODirectWriteRegion *region = &direct_write_regions[i];

        assert(right < region->low || left > region->high);
    }

    direct_write_regions[nr_direct_write_region] =
        (IODirectWriteRegion){
            .map_name = owner->name,
            .low = left,
            .high = right,
            .space = (uint8_t *)owner->space +
                     (size_t)(left - owner->low),
            .direct_write_widths = direct_write_widths,
        };

    nr_direct_write_region++;
}

/* Direct-write regions are fixed after device initialisation. */
size_t mmio_direct_write_region_count(void)
{
    assert(direct_routes_frozen);
    return (size_t)nr_direct_write_region;
}

/* Return one stable, const direct-write region by dense index. */
const IODirectWriteRegion *mmio_direct_write_region(size_t direct_index)
{
    assert(direct_routes_frozen);

    if (direct_index >= (size_t)nr_direct_write_region)
    {
        return NULL;
    }

    return &direct_write_regions[direct_index];
}

/*
 * Generated code may retain exact backing pointers for an arena generation.
 * Freeze the direct-route topology before CPU execution so no later
 * registration can invalidate those pointers or their width contracts.
 */
void mmio_freeze_direct_routes(void)
{
    assert(!direct_routes_frozen);
    direct_routes_frozen = true;
}

/* bus interface */
word_t mmio_read(paddr_t addr, int len)
{
    return map_read(addr, len, fetch_mmio_map(addr));
}

void mmio_write(paddr_t addr, int len, word_t data)
{
    map_write(addr, len, data, fetch_mmio_map(addr));
}
