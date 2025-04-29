#include <am.h>
#include <nemu.h>
#include <stdio.h>

#define SYNC_ADDR (VGACTL_ADDR + 4)

static int W;
static int H;

void __am_gpu_init() 
{
	const uint32_t vgainfo = inl(VGACTL_ADDR);
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

	for (int i = 0; i < ctl->h; ++i)
	{
		for (int j = 0; j < ctl->w; ++j)
		{
			const uint32_t pOffset = i * ctl->w + j;
			const uint32_t baseOffset = ((ctl->y + i) * W + (ctl->x + j));
			outl(FB_ADDR + baseOffset * sizeof(uint32_t), pixel[pOffset]);
		}
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
