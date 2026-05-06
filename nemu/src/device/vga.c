#include <common.h>
#include <device/map.h>
#include <isa.h>
#include <memory/paddr.h>
#include <utils.h>

#define SCREEN_W (MUXDEF(CONFIG_VGA_SIZE_800x600, 800, 400))
#define SCREEN_H (MUXDEF(CONFIG_VGA_SIZE_800x600, 600, 300))
#define VGACTL_NR_REGS 6u
#define VGACTL_REG_INFO 0u
#define VGACTL_REG_SYNC 1u
#define VGACTL_REG_BLIT_SRC 2u
#define VGACTL_REG_BLIT_POS 3u
#define VGACTL_REG_BLIT_SIZE 4u
#define VGACTL_REG_BLIT_CMD 5u
#define VGACTL_BLIT_CMD_COPY 1u
#define VGA_PAGE_SIZE 4096u
#define VGA_PAGE_MASK (VGA_PAGE_SIZE - 1u)

static uint32_t screen_width() {
	return MUXDEF(CONFIG_TARGET_AM, io_read(AM_GPU_CONFIG).width, SCREEN_W);
}

static uint32_t screen_height() {
	return MUXDEF(CONFIG_TARGET_AM, io_read(AM_GPU_CONFIG).height, SCREEN_H);
}

static uint32_t screen_size() {
	return screen_width() * screen_height() * sizeof(uint32_t);
}

static void *vmem = NULL;
static uint32_t *vgactl_port_base = NULL;
static bool vmem_dirty = false;
static int dirty_x0 = 0;
static int dirty_y0 = 0;
static int dirty_x1 = 0;
static int dirty_y1 = 0;

static void mark_vmem_dirty_rect(int x0, int y0, int x1, int y1);

#ifndef CONFIG_TARGET_AM
#define VGA_FPS_INTERVAL_US 5000000ull

static bool vga_fps_enabled = false;
static uint64_t vga_fps_last_us = 0;
static uint64_t vga_fps_frames = 0;
static uint64_t vga_fps_dirty_frames = 0;
static uint64_t vga_fps_full_frames = 0;
static uint64_t vga_fps_empty_frames = 0;
static uint64_t vga_fps_dirty_area = 0;
static uint64_t vga_fps_vmem_writes = 0;
static uint64_t vga_fps_vmem_bytes = 0;
static uint64_t vga_fps_blits = 0;
static uint64_t vga_fps_blit_bytes = 0;

static bool vga_fps_env_enabled() {
	const char *env = getenv("NEMU_VGA_FPS");

	/*
	 * Keep the switch simple for benchmark use: unset, empty, and "0" mean
	 * disabled; any other value enables host-side frame counting.  The check is
	 * done once during VGA initialisation, so normal frame updates only pay one
	 * predictable boolean branch.
	 */
	return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static void init_vga_fps_counter() {
	vga_fps_enabled = vga_fps_env_enabled();
	if (!vga_fps_enabled) return;

	vga_fps_last_us = get_time();
	vga_fps_frames = 0;
	vga_fps_dirty_frames = 0;
	vga_fps_full_frames = 0;
	vga_fps_empty_frames = 0;
	vga_fps_dirty_area = 0;
	vga_fps_vmem_writes = 0;
	vga_fps_vmem_bytes = 0;
	vga_fps_blits = 0;
	vga_fps_blit_bytes = 0;
	Log("vga: host FPS counter enabled, print interval = %" PRIu64 " us",
			(uint64_t)VGA_FPS_INTERVAL_US);
}

static void vga_fps_count_vmem_write(uint32_t offset, int len, uint64_t fb_size) {
	if (!vga_fps_enabled || len <= 0 || (uint64_t)offset >= fb_size) return;

	uint64_t bytes = (uint64_t)len;
	if (bytes > fb_size - (uint64_t)offset) {
		bytes = fb_size - (uint64_t)offset;
	}

	vga_fps_vmem_writes++;
	vga_fps_vmem_bytes += bytes;
}

static void vga_fps_count_blit(uint64_t bytes) {
	if (!vga_fps_enabled) return;

	vga_fps_blits++;
	vga_fps_blit_bytes += bytes;
}

static void vga_fps_count_frame(bool had_dirty, uint64_t dirty_area) {
	if (!vga_fps_enabled) return;

	const uint64_t now = get_time();
	if (vga_fps_last_us == 0) {
		vga_fps_last_us = now;
		return;
	}

	vga_fps_frames++;
	if (had_dirty) {
		vga_fps_dirty_frames++;
		vga_fps_dirty_area += dirty_area;
		if (dirty_area >= (uint64_t)screen_width() * (uint64_t)screen_height()) {
			vga_fps_full_frames++;
		}
	} else {
		vga_fps_empty_frames++;
	}

	const uint64_t elapsed = now - vga_fps_last_us;
	if (elapsed < VGA_FPS_INTERVAL_US) return;

	/*
	 * Print from the host side rather than from PAL.  This avoids guest printf,
	 * syscall, and serial-device work, so the measurement disturbs PAL much less
	 * than an in-game FPS print.
	 */
	const double seconds = (double)elapsed / 1000000.0;
	const double fps = (double)vga_fps_frames / seconds;
	const uint64_t screen_area = (uint64_t)screen_width() * (uint64_t)screen_height();
	const double avg_dirty_pct = vga_fps_dirty_frames == 0 || screen_area == 0
		? 0.0
		: (double)vga_fps_dirty_area * 100.0 /
			((double)vga_fps_dirty_frames * (double)screen_area);
	const double mb = (double)vga_fps_vmem_bytes / (1024.0 * 1024.0);
	const double blit_mb = (double)vga_fps_blit_bytes / (1024.0 * 1024.0);
	const double writes_per_frame = vga_fps_frames == 0
		? 0.0
		: (double)vga_fps_vmem_writes / (double)vga_fps_frames;
	printf("[vga] frames=%" PRIu64 " elapsed=%.3f s fps=%.2f "
			"dirty=%" PRIu64 " full=%" PRIu64 " empty=%" PRIu64
			" avg_dirty=%.1f%% vmem_writes=%" PRIu64
			" writes/frame=%.0f vmem=%.2f MiB blits=%" PRIu64
			" blit=%.2f MiB\n",
			vga_fps_frames, seconds, fps,
			vga_fps_dirty_frames, vga_fps_full_frames, vga_fps_empty_frames,
			avg_dirty_pct, vga_fps_vmem_writes, writes_per_frame, mb,
			vga_fps_blits, blit_mb);
	fflush(stdout);

	vga_fps_last_us = now;
	vga_fps_frames = 0;
	vga_fps_dirty_frames = 0;
	vga_fps_full_frames = 0;
	vga_fps_empty_frames = 0;
	vga_fps_dirty_area = 0;
	vga_fps_vmem_writes = 0;
	vga_fps_vmem_bytes = 0;
	vga_fps_blits = 0;
	vga_fps_blit_bytes = 0;
}
#else
static void init_vga_fps_counter() {}
static void vga_fps_count_vmem_write(uint32_t offset, int len, uint64_t fb_size) {
	(void)offset;
	(void)len;
	(void)fb_size;
}

static void vga_fps_count_frame(bool had_dirty, uint64_t dirty_area) {
	(void)had_dirty;
	(void)dirty_area;
}

static void vga_fps_count_blit(uint64_t bytes) {
	(void)bytes;
}
#endif

static bool vga_guest_read_chunk(vaddr_t addr, size_t wanted, uint8_t **host,
		size_t *len) {
	if (wanted == 0) return false;

	paddr_t paddr = 0;
	const int mmu = isa_mmu_check(addr, 1, MEM_TYPE_READ);
	if (mmu == MMU_DIRECT) {
		paddr = (paddr_t)addr;
	} else if (mmu == MMU_TRANSLATE) {
		const paddr_t ret = isa_mmu_translate(addr, 1, MEM_TYPE_READ);
		if ((ret & (paddr_t)VGA_PAGE_MASK) != MEM_RET_OK) {
			return false;
		}
		paddr = (ret & ~(paddr_t)VGA_PAGE_MASK) | (paddr_t)(addr & VGA_PAGE_MASK);
	} else {
		return false;
	}

	if (!in_pmem(paddr)) return false;

	size_t chunk = VGA_PAGE_SIZE - (size_t)(addr & VGA_PAGE_MASK);
	const paddr_t pmem_end = (paddr_t)CONFIG_MBASE + (paddr_t)CONFIG_MSIZE;
	if ((paddr_t)(paddr + chunk) > pmem_end) {
		chunk = (size_t)(pmem_end - paddr);
	}
	if (chunk > wanted) {
		chunk = wanted;
	}

	*host = guest_to_host(paddr);
	*len = chunk;
	return chunk > 0;
}

static void vga_blit_from_guest(vaddr_t src, int x, int y, int w, int h) {
	if (src == 0 || w <= 0 || h <= 0) return;

	const int sw = (int)screen_width();
	const int sh = (int)screen_height();
	Assert(x >= 0 && y >= 0 && x + w <= sw && y + h <= sh,
			"vga: invalid blit rectangle x=%d y=%d w=%d h=%d screen=%dx%d",
			x, y, w, h, sw, sh);

	const size_t row_bytes = (size_t)w * sizeof(uint32_t);
	for (int row = 0; row < h; row++) {
		size_t done = 0;
		uint8_t *dst = (uint8_t *)vmem
			+ ((size_t)(y + row) * (size_t)sw + (size_t)x) * sizeof(uint32_t);
		vaddr_t row_src = src + (vaddr_t)((size_t)row * row_bytes);

		while (done < row_bytes) {
			uint8_t *host = NULL;
			size_t chunk = 0;
			const vaddr_t cur = row_src + (vaddr_t)done;
			const size_t remain = row_bytes - done;
			Assert(vga_guest_read_chunk(cur, remain, &host, &chunk),
					"vga: cannot translate blit source vaddr=0x%08x", cur);
			memcpy(dst + done, host, chunk);
			done += chunk;
		}
	}

	mark_vmem_dirty_rect(x, y, x + w - 1, y + h - 1);
	vga_fps_count_blit((uint64_t)row_bytes * (uint64_t)h);
}

static void vgactl_io_handler(uint32_t offset, int len, bool is_write) {
	if (!is_write || len != sizeof(uint32_t)) return;

	const uint32_t reg = offset / sizeof(uint32_t);
	if (reg != VGACTL_REG_BLIT_CMD ||
			vgactl_port_base[VGACTL_REG_BLIT_CMD] != VGACTL_BLIT_CMD_COPY) {
		return;
	}

	const uint32_t pos = vgactl_port_base[VGACTL_REG_BLIT_POS];
	const uint32_t size = vgactl_port_base[VGACTL_REG_BLIT_SIZE];
	const int x = (int)(pos & 0xffffu);
	const int y = (int)(pos >> 16);
	const int w = (int)(size & 0xffffu);
	const int h = (int)(size >> 16);
	vga_blit_from_guest((vaddr_t)vgactl_port_base[VGACTL_REG_BLIT_SRC],
			x, y, w, h);
	vgactl_port_base[VGACTL_REG_BLIT_CMD] = 0;
}

static void mark_vmem_dirty_rect(int x0, int y0, int x1, int y1) {
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= (int)screen_width()) x1 = (int)screen_width() - 1;
	if (y1 >= (int)screen_height()) y1 = (int)screen_height() - 1;
	if (x0 > x1 || y0 > y1) return;

	if (!vmem_dirty) {
		dirty_x0 = x0;
		dirty_y0 = y0;
		dirty_x1 = x1;
		dirty_y1 = y1;
		vmem_dirty = true;
		return;
	}

	if (x0 < dirty_x0) dirty_x0 = x0;
	if (y0 < dirty_y0) dirty_y0 = y0;
	if (x1 > dirty_x1) dirty_x1 = x1;
	if (y1 > dirty_y1) dirty_y1 = y1;
}

static void mark_vmem_dirty_full() {
	mark_vmem_dirty_rect(0, 0, (int)screen_width() - 1, (int)screen_height() - 1);
}

static void vmem_io_handler(uint32_t offset, int len, bool is_write) {
	if (!is_write || len <= 0) return;

	const uint64_t fb_size = screen_size();
	if ((uint64_t)offset >= fb_size) return;
	vga_fps_count_vmem_write(offset, len, fb_size);

	uint64_t end_byte = (uint64_t)offset + (uint64_t)len - 1;
	if (end_byte >= fb_size) end_byte = fb_size - 1;

	const uint64_t start_pixel = (uint64_t)offset / sizeof(uint32_t);
	const uint64_t end_pixel = end_byte / sizeof(uint32_t);
	const int width = (int)screen_width();
	const int start_row = (int)(start_pixel / (uint64_t)width);
	const int end_row = (int)(end_pixel / (uint64_t)width);

	if (start_row == end_row) {
		mark_vmem_dirty_rect((int)(start_pixel % (uint64_t)width), start_row,
				(int)(end_pixel % (uint64_t)width), end_row);
	} else {
		/*
		 * A single linear MMIO span can cover the tail of one row, full
		 * middle rows, and the head of the last row. Marking full rows is
		 * conservative, correct, and still much cheaper than uploading the
		 * whole 800x600 texture for small app canvases.
		 */
		mark_vmem_dirty_rect(0, start_row, width - 1, end_row);
	}
}

#ifdef CONFIG_VGA_SHOW_SCREEN
#ifndef CONFIG_TARGET_AM
#include <SDL2/SDL.h>

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

static void init_screen() {
	char title[128];
	sprintf(title, "%s-NEMU", str(__GUEST_ISA__));
	SDL_Init(SDL_INIT_VIDEO);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	SDL_CreateWindowAndRenderer(
			SCREEN_W * (MUXDEF(CONFIG_VGA_SIZE_400x300, 2, 1)),
			SCREEN_H * (MUXDEF(CONFIG_VGA_SIZE_400x300, 2, 1)),
			SDL_WINDOW_RESIZABLE, &window, &renderer);
	SDL_SetWindowTitle(window, title);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STATIC, SCREEN_W, SCREEN_H);
	SDL_UpdateTexture(texture, NULL, vmem, SCREEN_W * sizeof(uint32_t));
	SDL_RenderPresent(renderer);
}

static void upload_dirty_texture() {
	if (!vmem_dirty) return;

	SDL_Rect rect = {
		.x = dirty_x0,
		.y = dirty_y0,
		.w = dirty_x1 - dirty_x0 + 1,
		.h = dirty_y1 - dirty_y0 + 1,
	};
	uint8_t *pixels = (uint8_t *)vmem
		+ ((size_t)rect.y * SCREEN_W + (size_t)rect.x) * sizeof(uint32_t);
	SDL_UpdateTexture(texture, &rect, pixels, SCREEN_W * sizeof(uint32_t));
	vmem_dirty = false;
}

static SDL_Rect screen_dst_rect() {
	int out_w = SCREEN_W;
	int out_h = SCREEN_H;
	SDL_GetRendererOutputSize(renderer, &out_w, &out_h);

	SDL_Rect dst = { .x = 0, .y = 0, .w = out_w, .h = out_h };
	if (out_w <= 0 || out_h <= 0) return dst;

	int64_t w_from_height = (int64_t)out_h * SCREEN_W / SCREEN_H;
	int64_t h_from_width = (int64_t)out_w * SCREEN_H / SCREEN_W;

	if (h_from_width <= out_h) {
		dst.w = out_w;
		dst.h = (int)h_from_width;
		dst.x = 0;
		dst.y = (out_h - dst.h) / 2;
	} else {
		dst.w = (int)w_from_height;
		dst.h = out_h;
		dst.x = (out_w - dst.w) / 2;
		dst.y = 0;
	}

	return dst;
}

static inline void update_screen() {
	upload_dirty_texture();
	SDL_RenderClear(renderer);
	SDL_Rect dst = screen_dst_rect();
	SDL_RenderCopy(renderer, texture, NULL, &dst);
	SDL_RenderPresent(renderer);
}
#else
static void init_screen() {}

static inline void update_screen() {
	io_write(AM_GPU_FBDRAW, 0, 0, vmem, screen_width(), screen_height(), true);
}
#endif
#endif

#ifndef CONFIG_VGA_SHOW_SCREEN
static inline void update_screen() {}
#endif

void vga_update_screen() 
{
	if (vgactl_port_base[VGACTL_REG_SYNC] != 0)
	{
		const bool had_dirty = vmem_dirty;
		const uint64_t dirty_area = had_dirty
			? (uint64_t)(dirty_x1 - dirty_x0 + 1) * (uint64_t)(dirty_y1 - dirty_y0 + 1)
			: 0;
		update_screen();
		vga_fps_count_frame(had_dirty, dirty_area);
		vgactl_port_base[VGACTL_REG_SYNC] = 0;
	}
}

void init_vga() {
	vgactl_port_base = (uint32_t *)new_space(VGACTL_NR_REGS * sizeof(uint32_t));
	vgactl_port_base[VGACTL_REG_INFO] = (screen_width() << 16) | screen_height();
#ifdef CONFIG_HAS_PORT_IO
	add_pio_map ("vgactl", CONFIG_VGA_CTL_PORT, vgactl_port_base,
			VGACTL_NR_REGS * sizeof(uint32_t), vgactl_io_handler);
#else
	add_mmio_map("vgactl", CONFIG_VGA_CTL_MMIO, vgactl_port_base,
			VGACTL_NR_REGS * sizeof(uint32_t), vgactl_io_handler);
#endif

	vmem = new_space(screen_size());
	IFDEF(CONFIG_VGA_SHOW_SCREEN, memset(vmem, 0, screen_size()));
	add_mmio_map("vmem", CONFIG_FB_ADDR, vmem, screen_size(), vmem_io_handler);
	IFDEF(CONFIG_VGA_SHOW_SCREEN, mark_vmem_dirty_full());
	IFDEF(CONFIG_VGA_SHOW_SCREEN, init_screen());
	init_vga_fps_counter();
}
