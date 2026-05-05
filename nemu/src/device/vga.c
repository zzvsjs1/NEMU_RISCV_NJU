#include <common.h>
#include <device/map.h>

#define SCREEN_W (MUXDEF(CONFIG_VGA_SIZE_800x600, 800, 400))
#define SCREEN_H (MUXDEF(CONFIG_VGA_SIZE_800x600, 600, 300))

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
	if (vgactl_port_base[1] != 0)
	{
		update_screen();
		vgactl_port_base[1] = 0;
	}
}

void init_vga() {
	vgactl_port_base = (uint32_t *)new_space(8);
	vgactl_port_base[0] = (screen_width() << 16) | screen_height();
#ifdef CONFIG_HAS_PORT_IO
	add_pio_map ("vgactl", CONFIG_VGA_CTL_PORT, vgactl_port_base, 8, NULL);
#else
	add_mmio_map("vgactl", CONFIG_VGA_CTL_MMIO, vgactl_port_base, 8, NULL);
#endif

	vmem = new_space(screen_size());
	IFDEF(CONFIG_VGA_SHOW_SCREEN, memset(vmem, 0, screen_size()));
	add_mmio_map("vmem", CONFIG_FB_ADDR, vmem, screen_size(), vmem_io_handler);
	IFDEF(CONFIG_VGA_SHOW_SCREEN, mark_vmem_dirty_full());
	IFDEF(CONFIG_VGA_SHOW_SCREEN, init_screen());
}
