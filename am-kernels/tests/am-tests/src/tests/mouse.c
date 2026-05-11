#include <amtest.h>

typedef struct
{
    int x, y, w, h;
    uint32_t idle, active;
    bool hit;
} Target;

static int screen_w, screen_h;
static int cursor_x, cursor_y;
static int wheel_total;
static int buttons;

static Target targets[] = {
    {.x = 40, .y = 40, .w = 80, .h = 50, .idle = 0x00336699, .active = 0x0000cc66, .hit = false},
    {.x = 160, .y = 40, .w = 80, .h = 50, .idle = 0x00996633, .active = 0x00cc3300, .hit = false},
    {.x = 280, .y = 40, .w = 80, .h = 50, .idle = 0x00663399, .active = 0x00cc66ff, .hit = false},
};

static const char *mouse_type_name(int type)
{
    switch (type)
    {
    case AM_MOUSE_MOVE:
        return "MOVE";
    case AM_MOUSE_BUTTON_DOWN:
        return "BUTTON_DOWN";
    case AM_MOUSE_BUTTON_UP:
        return "BUTTON_UP";
    case AM_MOUSE_WHEEL:
        return "WHEEL";
    default:
        return "NONE";
    }
}

static int clamp_int(int value, int low, int high)
{
    if (value < low)
        return low;

    if (value > high)
        return high;
    return value;
}

static void draw_rect(int x, int y, int w, int h, uint32_t colour)
{
    static uint32_t pixels[800 * 64];

    // The scratch buffer is intentionally capped. Large clears are split by
    // fill_screen(), which keeps stack and data usage predictable on small AM
    // targets while still exercising AM_GPU_FBDRAW.
    assert(w > 0);
    assert(h > 0);
    assert(w <= 800);
    assert(h <= 64);
    assert(w * h <= (int)LENGTH(pixels));

    for (int i = 0; i < w * h; i++)
    {
        pixels[i] = colour;
    }
    io_write(AM_GPU_FBDRAW, x, y, pixels, w, h, false);
}

static void fill_screen(uint32_t colour)
{
    for (int y = 0; y < screen_h; y += 64)
    {
        // Split the full-screen clear so the reusable framebuffer buffer stays small.
        int h = screen_h - y;

        if (h > 64)
            h = 64;
        draw_rect(0, y, screen_w, h, colour);
    }
}

static bool in_target(const Target *target, int x, int y)
{
    return x >= target->x && x < target->x + target->w &&
           y >= target->y && y < target->y + target->h;
}

static void draw_button_indicator(int index, int mask)
{
    uint32_t held = 0x0000cc66;
    uint32_t idle = 0x00222222;
    int x = 40 + index * 40;
    int y = screen_h - 50;

    draw_rect(x, y, 28, 28, (buttons & mask) ? held : idle);
}

static void draw_scene(void)
{
    // Redraw the whole scene after each mouse event. That is simple but useful for
    // this device test because it verifies both input state changes and the GPU
    // sync path in the same loop.
    fill_screen(0x00111111);

    for (int i = 0; i < (int)LENGTH(targets); i++)
    {
        Target *target = &targets[i];
        draw_rect(target->x, target->y, target->w, target->h,
                  target->hit ? target->active : target->idle);
    }

    draw_button_indicator(0, AM_MOUSE_BUTTON_LEFT_MASK);
    draw_button_indicator(1, AM_MOUSE_BUTTON_MIDDLE_MASK);
    draw_button_indicator(2, AM_MOUSE_BUTTON_RIGHT_MASK);

    draw_rect(cursor_x, cursor_y, 8, 8, 0x00ffffff);
    io_write(AM_GPU_FBDRAW, 0, 0, NULL, 0, 0, true);
}

static void update_mouse_state(AM_INPUT_MOUSE_T mouse)
{
    // Keep the cursor drawable even if a backend reports a coordinate on the edge.
    cursor_x = clamp_int(mouse.x, 0, screen_w - 8);
    cursor_y = clamp_int(mouse.y, 0, screen_h - 8);
    buttons = mouse.buttons;

    if (mouse.type == AM_MOUSE_WHEEL)
    {
        wheel_total += mouse.wheel_y;
        printf("mouse wheel total=%d delta=(%d,%d)\n",
               wheel_total, mouse.wheel_x, mouse.wheel_y);
    }

    if (mouse.type == AM_MOUSE_BUTTON_DOWN &&
        mouse.button == AM_MOUSE_BUTTON_LEFT)
    {
        for (int i = 0; i < (int)LENGTH(targets); i++)
        {
            if (in_target(&targets[i], mouse.x, mouse.y))
            {
                targets[i].hit = !targets[i].hit;
                printf("mouse target %d toggled at %d,%d\n", i, mouse.x, mouse.y);
            }
        }
    }
}

void mouse_test(void)
{
    // Mouse coordinates are interpreted in framebuffer pixels. Reading GPU config
    // first lets the same test run across NEMU window sizes without hard-coding a
    // particular display resolution.
    AM_GPU_CONFIG_T gpu = io_read(AM_GPU_CONFIG);
    screen_w = gpu.width;
    screen_h = gpu.height;
    cursor_x = screen_w / 2;
    cursor_y = screen_h / 2;
    wheel_total = 0;
    buttons = 0;

    printf("Mouse test: move, click targets, and wheel. Press ESC to exit.\n");
    draw_scene();

    while (1)
    {
        AM_INPUT_KEYBRD_T key = io_read(AM_INPUT_KEYBRD);

        if (key.keydown && key.keycode == AM_KEY_ESCAPE)
        {
            printf("Mouse test exit\n");
            return;
        }

        AM_INPUT_MOUSE_T mouse = io_read(AM_INPUT_MOUSE);

        if (mouse.type == AM_MOUSE_NONE)
        {
            continue;
        }

        printf("mouse %s x=%d y=%d button=%d buttons=%d wheel=(%d,%d)\n",
               mouse_type_name(mouse.type), mouse.x, mouse.y, mouse.button,
               mouse.buttons, mouse.wheel_x, mouse.wheel_y);

        update_mouse_state(mouse);
        draw_scene();
    }
}
