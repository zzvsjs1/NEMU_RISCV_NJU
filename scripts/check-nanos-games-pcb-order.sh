#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "nanos games PCB order check failed: $*" >&2
  exit 1
}

# The games profile should boot the app under active regression testing in
# pcb[0], because fg_pcb starts there and the first scheduler entry runs it
# immediately.  Keep the other foreground apps eagerly loaded so startup
# regressions are not hidden by lazy loading.
if grep -Eq 'load_games_foreground_pcb[[:space:]]*\(' nanos-lite/src/proc.c; then
  fail "games profile still contains lazy foreground loading"
fi

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*0[[:space:]]*\][[:space:]]*,[[:space:]]*"/bin/fceux"' nanos-lite/src/proc.c \
  || fail "pcb[0] is not loaded with /bin/fceux"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*1[[:space:]]*\][[:space:]]*,[[:space:]]*"/bin/onscripter"' nanos-lite/src/proc.c \
  || fail "pcb[1] is not loaded with /bin/onscripter"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*2[[:space:]]*\][[:space:]]*,[[:space:]]*"/bin/pal"' nanos-lite/src/proc.c \
  || fail "pcb[2] is not loaded with /bin/pal"

grep -Eq 'fg_pcb[[:space:]]*=[[:space:]]*&pcb\[[[:space:]]*0[[:space:]]*\]' nanos-lite/src/proc.c \
  || fail "games profile does not start from pcb[0]"

echo "nanos games PCB order check passed"
