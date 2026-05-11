#include <common.h>
#include <utils.h>
#include <device/alarm.h>
#ifndef CONFIG_TARGET_AM
#include <SDL2/SDL.h>
#endif

void init_map();
void init_serial();
void init_timer();
void init_vga();
void init_i8042();
void init_mouse();
void init_audio();
void init_disk();
void init_sdcard();
void init_alarm();

void vga_translate_mouse_position(int *, int *);
void send_key(uint8_t, bool);
void send_mouse_motion(int, int, uint32_t);
void send_mouse_button(uint8_t, bool, int, int);
void send_mouse_wheel(int, int);
void vga_update_screen();

void device_update()
{
    static uint64_t last = 0;
    uint64_t now = get_time();
    /*
   * The CPU loop may execute a small batch of guest instructions before asking
   * devices to poll host state.  This time gate keeps SDL and timer work close
   * to TIMER_HZ while still allowing the interpreter/JIT loop to avoid a host
   * syscall after every single guest instruction.
   */

    if (now - last < 1000000 / TIMER_HZ)
    {
        return;
    }
    last = now;

    IFDEF(CONFIG_HAS_VGA, vga_update_screen());

#ifndef CONFIG_TARGET_AM
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            nemu_state.state = NEMU_QUIT;
            break;
#ifdef CONFIG_HAS_KEYBOARD
        // If a key was pressed
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            uint8_t k = event.key.keysym.scancode;
            bool is_keydown = (event.key.type == SDL_KEYDOWN);
            send_key(k, is_keydown);
            break;
        }
#endif
#ifdef CONFIG_HAS_MOUSE
        case SDL_MOUSEMOTION:
        {
            int x = event.motion.x;
            int y = event.motion.y;
#ifdef CONFIG_HAS_VGA
            /*
         * SDL reports window coordinates.  The guest sees framebuffer
         * coordinates, which may differ when the visible window is scaled, so
         * translate before the event is queued in the emulated mouse device.
         */
            vga_translate_mouse_position(&x, &y);
#endif
            send_mouse_motion(x, y, event.motion.state);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            int x = event.button.x;
            int y = event.button.y;
#ifdef CONFIG_HAS_VGA
            /*
         * Button events carry the pointer position at the time of the click.
         * Translate the position with the same rule as motion events so Navy's
         * /dev/events stream never mixes host-window and guest-screen axes.
         */
            vga_translate_mouse_position(&x, &y);
#endif
            send_mouse_button(event.button.button,
                              event.button.type == SDL_MOUSEBUTTONDOWN,
                              x, y);
            break;
        }
        case SDL_MOUSEWHEEL:
            /*
         * SDL2 wheel events do not carry a reliable cursor position.  The mouse
         * device therefore reuses the last translated motion/button position so
         * the downstream /dev/events record still has coordinates in guest space.
         */
            send_mouse_wheel(event.wheel.x, event.wheel.y);
            break;
#endif
        default:
            break;
        }
    }
#endif
}

void sdl_clear_event_queue()
{
#ifndef CONFIG_TARGET_AM
    SDL_Event event;
    while (SDL_PollEvent(&event))
        ;
#endif
}

void init_device()
{
    IFDEF(CONFIG_TARGET_AM, ioe_init());
    init_map();

    IFDEF(CONFIG_HAS_SERIAL, init_serial());
    IFDEF(CONFIG_HAS_TIMER, init_timer());
    IFDEF(CONFIG_HAS_VGA, init_vga());
    IFDEF(CONFIG_HAS_KEYBOARD, init_i8042());
    IFDEF(CONFIG_HAS_MOUSE, init_mouse());
    IFDEF(CONFIG_HAS_AUDIO, init_audio());
    IFDEF(CONFIG_HAS_DISK, init_disk());
    IFDEF(CONFIG_HAS_SDCARD, init_sdcard());

    IFNDEF(CONFIG_TARGET_AM, init_alarm());
}
