#!/usr/bin/env sh
set -eu

event_c="navy-apps/libs/libminiSDL/src/event.c"
test_c="navy-apps/tests/ons-sdl-test/main.c"

grep -q "replacePendingMouseMotion" "$event_c"
grep -q "enqueueMouseMotionEvent" "$event_c"
grep -q "Coalesce only motion events" "$event_c"
grep -q "check_mouse_motion_coalescing" "$test_c"
