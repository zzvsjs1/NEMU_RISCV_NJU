#include <device/map.h>
#include <isa.h>

#define PORT_IO_SPACE_MAX 65535

#define NR_MAP 16
static IOMap maps[NR_MAP] = {};
static int nr_map = 0;
static IOMap *last_map = NULL;

static IOMap *fetch_pio_map(ioaddr_t addr)
{
    if (last_map != NULL && map_inside(last_map, addr))
    {
        difftest_skip_ref();
        return last_map;
    }

    const int mapid = find_mapid_by_addr(maps, nr_map, addr);

    last_map = mapid == -1 ? NULL : &maps[mapid];
    return last_map;
}

/* device interface */
void add_pio_map(const char *name, ioaddr_t addr, void *space, uint32_t len, io_callback_t callback)
{
    assert(nr_map < NR_MAP);
    assert(addr + len <= PORT_IO_SPACE_MAX);
    /*
     * Port IO is a separate 16-bit bus used by some configurations instead of
     * MMIO.  The same IOMap callback contract is reused, but the address never
     * goes through the physical-memory decoder.
     */
    maps[nr_map] = (IOMap){.name = name, .low = addr, .high = addr + len - 1, .space = space, .callback = callback};
    Log("Add port-io map '%s' at [" FMT_PADDR ", " FMT_PADDR "]",
        maps[nr_map].name, maps[nr_map].low, maps[nr_map].high);

    nr_map++;
    last_map = NULL;
}

/* CPU interface */
uint32_t pio_read(ioaddr_t addr, int len)
{
    assert(addr + len - 1 < PORT_IO_SPACE_MAX);
    IOMap *map = fetch_pio_map(addr);
    Assert(map != NULL, "unmapped port I/O read at port %#x, len %d, pc = " FMT_WORD, addr, len, cpu.pc);
    return map_read(addr, len, map);
}

void pio_write(ioaddr_t addr, int len, uint32_t data)
{
    assert(addr + len - 1 < PORT_IO_SPACE_MAX);
    IOMap *map = fetch_pio_map(addr);
    Assert(map != NULL, "unmapped port I/O write at port %#x, len %d, data %#x, pc = " FMT_WORD, addr, len, data, cpu.pc);
    map_write(addr, len, data, map);
}
