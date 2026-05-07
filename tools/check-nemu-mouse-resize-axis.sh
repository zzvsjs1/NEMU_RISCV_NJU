#!/usr/bin/env sh
set -eu

vga_c="nemu/src/device/vga.c"
device_c="nemu/src/device/device.c"

# Resized SDL windows report host-window coordinates, but Navy apps expect
# guest framebuffer coordinates.  These checks pin the translation path and the
# clamping rules that keep the right/bottom edges inside the guest screen.
grep -Fq "vga_translate_mouse_position" "$vga_c"
grep -Fq "SDL_GetRendererOutputSize" "$vga_c"
grep -Fq "screen_dst_rect()" "$vga_c"
grep -Fq "guest_x = (int64_t)(*x - dst.x)" "$vga_c"
grep -Fq "guest_y = (int64_t)(*y - dst.y)" "$vga_c"
grep -Fq "guest_x = width - 1" "$vga_c"
grep -Fq "guest_y = height - 1" "$vga_c"
grep -Fq "vga_translate_mouse_position(&x, &y)" "$device_c"
