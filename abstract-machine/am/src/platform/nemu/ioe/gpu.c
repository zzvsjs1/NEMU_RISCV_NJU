#include <am.h>
#include <nemu.h>
#include <stdio.h>

#define SYNC_ADDR (VGACTL_ADDR + 4)
#define BLIT_SRC_ADDR (VGACTL_ADDR + 8)
#define BLIT_POS_ADDR (VGACTL_ADDR + 12)
#define BLIT_SIZE_ADDR (VGACTL_ADDR + 16)
#define BLIT_CMD_ADDR (VGACTL_ADDR + 20)
#define BLIT_CMD_COPY 1u

static int W;
static int H;

void __am_gpu_init() 
{
	const uint32_t vgainfo = inl(VGACTL_ADDR);
	/* VGACTL packs width in the high half-word and height in the low half-word.
	 * Cache both values because the guest-visible display mode is fixed after
	 * NEMU starts, and later config reads should not pay another MMIO access.
	 */
	W = vgainfo >> 16;
	H = (vgainfo << 16) >> 16;
}

void __am_gpu_config(AM_GPU_CONFIG_T *cfg) 
{
	*cfg = (AM_GPU_CONFIG_T) {
		.present = true, 
		.has_accel = false,
		.width = W,
		.height = H,
		.vmemsz = W * H * sizeof(uint32_t),
	};
}

void __am_gpu_fbdraw(AM_GPU_FBDRAW_T *ctl) 
{
	const uint32_t* pixel = (uint32_t*)ctl->pixels;

	if (pixel)
	{
		/*
		 * NEMU provides a private rectangle-copy command behind the normal VGA
		 * control registers.  Use it to avoid one MMIO write per pixel; PAL can
		 * update tens of thousands of pixels per frame, so a single command per
		 * AM_GPU_FBDRAW call removes a large amount of guest instruction and
		 * device-dispatch overhead.  The command is still synchronous from AM's
		 * view: the source pointer and rectangle are fully published before the
		 * copy command is written.
		 */
		outl(BLIT_SRC_ADDR, (uintptr_t)pixel);
		/* Position and size are packed as y:x and h:w.  Width/x are masked to
		 * 16 bits to match the device register layout used by NEMU.
		 */
		outl(BLIT_POS_ADDR, ((uint32_t)ctl->y << 16) | (uint32_t)(uint16_t)ctl->x);
		outl(BLIT_SIZE_ADDR, ((uint32_t)ctl->h << 16) | (uint32_t)(uint16_t)ctl->w);
		outl(BLIT_CMD_ADDR, BLIT_CMD_COPY);
	}

	if (ctl->sync) 
	{	
		outl(SYNC_ADDR, 1);
	}
}

void __am_gpu_status(AM_GPU_STATUS_T *status) 
{
	status->ready = true;
}
