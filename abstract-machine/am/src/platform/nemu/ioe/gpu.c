#include <am.h>
#include <klib.h>
#include <nemu.h>

#define GPU_ACCEL_VMEM_SIZE (512 << 10)

static inline uintptr_t vgactl_reg_addr(uint32_t reg)
{
    return VGACTL_ADDR + (uintptr_t)reg * sizeof(uint32_t);
}

static int W;
static int H;
static uint8_t gpu_accel_vmem[GPU_ACCEL_VMEM_SIZE];
static uint32_t gpu_accel_frame[GPU_ACCEL_VMEM_SIZE / sizeof(uint32_t)];
static uint8_t gpu_accel_scratch[GPU_ACCEL_VMEM_SIZE];
static uint8_t *gpu_accel_scratch_head;

static size_t gpu_framebuffer_bytes()
{
    assert(W > 0 && H > 0);
    return (size_t)W * (size_t)H * sizeof(uint32_t);
}

static bool gpu_accel_available()
{
    return gpu_framebuffer_bytes() <= GPU_ACCEL_VMEM_SIZE;
}

static void *gpu_accel_to_host(gpuptr_t ptr, size_t bytes)
{
    assert(ptr != AM_GPU_NULL);
    assert((uint64_t)ptr + (uint64_t)bytes <= GPU_ACCEL_VMEM_SIZE);
    return gpu_accel_vmem + ptr;
}

static struct gpu_canvas *gpu_accel_canvas_or_null(gpuptr_t ptr)
{
    return ptr == AM_GPU_NULL ? NULL : gpu_accel_to_host(ptr, sizeof(struct gpu_canvas));
}

static void *gpu_accel_alloc(size_t bytes)
{
    uintptr_t cur = (uintptr_t)gpu_accel_scratch_head;
    cur = (cur + (uintptr_t)sizeof(uint32_t) - 1u) & ~((uintptr_t)sizeof(uint32_t) - 1u);
    gpu_accel_scratch_head = (uint8_t *)cur;

    assert((size_t)(gpu_accel_scratch_head - gpu_accel_scratch) + bytes <=
           sizeof(gpu_accel_scratch));

    void *ret = gpu_accel_scratch_head;
    memset(ret, 0, bytes);
    gpu_accel_scratch_head += bytes;
    return ret;
}

static void gpu_accel_render_canvas(struct gpu_canvas *cv,
                                    struct gpu_canvas *parent,
                                    uint32_t *parent_pixels)
{
    uint32_t *local_pixels = NULL;
    int local_w = 0;
    int local_h = 0;

    assert(cv != NULL);
    assert(parent != NULL);
    assert(parent_pixels != NULL);
    assert(cv->w1 > 0 && cv->h1 > 0);

    switch (cv->type)
    {
    case AM_GPU_TEXTURE:
    {
        local_w = cv->texture.w;
        local_h = cv->texture.h;
        assert(local_w > 0 && local_h > 0);
        local_pixels = gpu_accel_to_host(
            cv->texture.pixels,
            (size_t)local_w * (size_t)local_h * sizeof(uint32_t));
        break;
    }

    case AM_GPU_SUBTREE:
    {
        local_w = cv->w;
        local_h = cv->h;
        assert(local_w > 0 && local_h > 0);
        local_pixels = gpu_accel_alloc((size_t)local_w * (size_t)local_h *
                                       sizeof(uint32_t));

        for (struct gpu_canvas *child = gpu_accel_canvas_or_null(cv->child);
             child != NULL;
             child = gpu_accel_canvas_or_null(child->sibling))
        {
            gpu_accel_render_canvas(child, cv, local_pixels);
        }
        break;
    }

    default:
        panic("unsupported GPU canvas type");
    }

    /*
     * Scale the local canvas into its rectangle in the parent canvas.  This
     * mirrors the QEMU AM implementation, but the final destination is the
     * NEMU framebuffer format, so pixels stay as guest ARGB words.
     */
    for (int y = 0; y < cv->h1; y++)
    {
        const int dst_y = cv->y1 + y;
        assert(dst_y >= 0 && dst_y < parent->h);

        const int src_y = y * local_h / cv->h1;
        for (int x = 0; x < cv->w1; x++)
        {
            const int dst_x = cv->x1 + x;
            assert(dst_x >= 0 && dst_x < parent->w);

            const int src_x = x * local_w / cv->w1;
            parent_pixels[(size_t)dst_y * (size_t)parent->w + (size_t)dst_x] =
                local_pixels[(size_t)src_y * (size_t)local_w + (size_t)src_x];
        }
    }
}

void __am_gpu_init()
{
    const uint32_t vgainfo = inl(vgactl_reg_addr(NEMU_VGACTL_INFO));
    /* VGACTL packs width in the high half-word and height in the low half-word.
     * Cache both values because the guest-visible display mode is fixed after
     * NEMU starts, and later config reads should not pay another MMIO access.
     */
    W = vgainfo >> 16;
    H = (vgainfo << 16) >> 16;
}

void __am_gpu_config(AM_GPU_CONFIG_T *cfg)
{
    const size_t framebuffer_bytes = gpu_framebuffer_bytes();

    *cfg = (AM_GPU_CONFIG_T){
        .present = true,
        .has_accel = gpu_accel_available(),
        .width = W,
        .height = H,
        .vmemsz = (int)framebuffer_bytes,
    };
}

void __am_gpu_fbdraw(AM_GPU_FBDRAW_T *ctl)
{
    const uint32_t *pixel = (uint32_t *)ctl->pixels;

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
        outl(vgactl_reg_addr(NEMU_VGACTL_BLIT_SRC), (uintptr_t)pixel);
        /* Position and size are packed as y:x and h:w.  Width/x are masked to
         * 16 bits to match the device register layout used by NEMU.
         */
        outl(vgactl_reg_addr(NEMU_VGACTL_BLIT_POS),
             ((uint32_t)ctl->y << 16) | (uint32_t)(uint16_t)ctl->x);
        outl(vgactl_reg_addr(NEMU_VGACTL_BLIT_SIZE),
             ((uint32_t)ctl->h << 16) | (uint32_t)(uint16_t)ctl->w);
        outl(vgactl_reg_addr(NEMU_VGACTL_BLIT_CMD), NEMU_VGACTL_BLIT_CMD_COPY);
    }

    if (ctl->sync)
    {
        outl(vgactl_reg_addr(NEMU_VGACTL_SYNC), 1);
    }
}

void __am_gpu_status(AM_GPU_STATUS_T *status)
{
    status->ready = true;
}

void __am_gpu_memcpy(AM_GPU_MEMCPY_T *ctl)
{
    assert(gpu_accel_available());
    assert(ctl->size >= 0);
    assert((uint64_t)ctl->dest + (uint64_t)ctl->size <= GPU_ACCEL_VMEM_SIZE);
    memcpy(gpu_accel_vmem + ctl->dest, ctl->src, (size_t)ctl->size);
}

void __am_gpu_render(AM_GPU_RENDER_T *ctl)
{
    assert(gpu_accel_available());

    struct gpu_canvas display = {
        .type = AM_GPU_SUBTREE,
        .w = (uint16_t)W,
        .h = (uint16_t)H,
        .x1 = 0,
        .y1 = 0,
        .w1 = (uint16_t)W,
        .h1 = (uint16_t)H,
        .sibling = AM_GPU_NULL,
        .child = ctl->root,
    };

    assert((size_t)W * (size_t)H * sizeof(uint32_t) <= sizeof(gpu_accel_frame));

    memset(gpu_accel_frame, 0, (size_t)W * (size_t)H * sizeof(uint32_t));
    gpu_accel_scratch_head = gpu_accel_scratch;

    struct gpu_canvas *root = gpu_accel_to_host(ctl->root, sizeof(*root));
    gpu_accel_render_canvas(root, &display, gpu_accel_frame);

    AM_GPU_FBDRAW_T draw = {
        .x = 0,
        .y = 0,
        .pixels = gpu_accel_frame,
        .w = W,
        .h = H,
        .sync = true,
    };
    __am_gpu_fbdraw(&draw);
}
