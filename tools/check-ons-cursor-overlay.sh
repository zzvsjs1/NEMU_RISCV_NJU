#!/usr/bin/env sh
set -eu

patch_file="navy-apps/apps/onscripter/patch/mouse.diff"

grep -q "showNavyMouseCursor" "$patch_file"
grep -q "hideNavyMouseCursor" "$patch_file"
grep -q "last_mouse_cursor_rect" "$patch_file"
grep -q "last_mouse_cursor_visible" "$patch_file"
grep -q "SDL_UpdateRect(screen_surface, last_mouse_cursor_rect.x" "$patch_file"
grep -q "if (moved) showNavyMouseCursor();" "$patch_file"
