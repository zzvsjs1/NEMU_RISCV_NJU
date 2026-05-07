#include <isa.h>
#include <memory/host.h>
#include <memory/vaddr.h>
#include <device/map.h>
#include <utils.h>

#define IO_SPACE_MAX (32 * 1024 * 1024)

static uint8_t *io_space = NULL;
static uint8_t *p_space = NULL;

uint8_t* new_space(int size) {
	uint8_t *p = p_space;
	/*
	 * Device backing storage is carved from one private host arena.  Align each
	 * allocation to a guest page boundary so MMIO register blocks and large
	 * buffers never accidentally share a page-sized alias in helper code.
	 */
	size = (size + (PAGE_SIZE - 1)) & ~PAGE_MASK;
	p_space += size;
	assert(p_space - io_space < IO_SPACE_MAX);
	return p;
}

static void check_bound(IOMap *map, paddr_t addr) {
	if (map == NULL) {
		Assert(map != NULL, "address (" FMT_PADDR ") is out of bound at pc = " FMT_WORD, addr, cpu.pc);
	} else {
		Assert(addr <= map->high && addr >= map->low,
				"address (" FMT_PADDR ") is out of bound {%s} [" FMT_PADDR ", " FMT_PADDR "] at pc = " FMT_WORD,
				addr, map->name, map->low, map->high, cpu.pc);
	}
}

static void invoke_callback(io_callback_t c, paddr_t offset, int len, bool is_write) {
	if (c != NULL) { c(offset, len, is_write); }
}

void init_map() {
	io_space = (uint8_t*)malloc(IO_SPACE_MAX);
	assert(io_space);
	p_space = io_space;
}

word_t map_read(paddr_t addr, int len, IOMap *map) {
	assert(len >= 1 && len <= 8);
	check_bound(map, addr);
	paddr_t offset = addr - map->low;
	/*
	 * Read callbacks publish volatile device state into the mapped bytes before
	 * the CPU-facing load happens.  This is why timers, keyboard queues, and
	 * status registers can all expose a simple memory cell to map_read().
	 */
	invoke_callback(map->callback, offset, len, false); // prepare data to read
	word_t ret = host_read(map->space + offset, len);
	IFDEF(CONFIG_DTRACE, log_write("dtrace read  pc=" FMT_WORD " device=%s addr=" FMT_PADDR " len=%d data=" FMT_WORD "\n",
		cpu.pc, map->name, addr, len, ret));
	return ret;
}

void map_write(paddr_t addr, int len, word_t data, IOMap *map) {
	assert(len >= 1 && len <= 8);
	check_bound(map, addr);
	paddr_t offset = addr - map->low;
	host_write(map->space + offset, len, data);
	/*
	 * Writes are committed to the register image before the callback runs.  The
	 * device handler can therefore inspect the new command value and then update
	 * any response registers in the same mapped space.
	 */
	invoke_callback(map->callback, offset, len, true);
	IFDEF(CONFIG_DTRACE, log_write("dtrace write pc=" FMT_WORD " device=%s addr=" FMT_PADDR " len=%d data=" FMT_WORD "\n",
		cpu.pc, map->name, addr, len, data));
}
