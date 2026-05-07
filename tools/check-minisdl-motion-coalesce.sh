#!/usr/bin/env sh
set -eu

event_c="navy-apps/libs/libminiSDL/src/event.c"
test_c="navy-apps/tests/ons-sdl-test/main.c"

# Mouse motion can arrive much faster than an old SDL game consumes events.
# The static checks keep the coalescing helper and its regression test visible
# even when the full ONScripter workflow is too expensive for a quick run.
grep -q "replacePendingMouseMotion" "$event_c"
grep -q "enqueueMouseMotionEvent" "$event_c"
grep -q "Coalesce only motion events" "$event_c"
grep -q "check_mouse_motion_coalescing" "$test_c"
