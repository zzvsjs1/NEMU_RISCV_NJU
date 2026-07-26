#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "nanos games PCB order check failed: $*" >&2
  exit 1
}

# The MIPS32 multiprogram configuration eagerly creates three switchable
# foreground programs and keeps hello in the reserved background slot. Checking
# the argv declarations as well as the PCB calls makes accidental slot swaps
# visible without requiring an interactive SDL run.
grep -Eq 'argv_pal\[\].*"/bin/pal"' nanos-lite/src/proc.c \
  || fail "the MIPS32 process layout does not declare /bin/pal"

grep -Eq 'argv_bird\[\].*"/bin/bird"' nanos-lite/src/proc.c \
  || fail "the MIPS32 process layout does not declare /bin/bird"

grep -Eq 'argv_nslider\[\].*"/bin/nslider"' nanos-lite/src/proc.c \
  || fail "the MIPS32 process layout does not declare /bin/nslider"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*0[[:space:]]*\][[:space:]]*,[[:space:]]*argv_pal\[0\]' nanos-lite/src/proc.c \
  || fail "pcb[0] is not loaded with /bin/pal"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*1[[:space:]]*\][[:space:]]*,[[:space:]]*argv_bird\[0\]' nanos-lite/src/proc.c \
  || fail "pcb[1] is not loaded with /bin/bird"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*2[[:space:]]*\][[:space:]]*,[[:space:]]*argv_nslider\[0\]' nanos-lite/src/proc.c \
  || fail "pcb[2] is not loaded with /bin/nslider"

grep -Eq 'context_uload[[:space:]]*\([[:space:]]*&pcb\[[[:space:]]*HELLO_PROC[[:space:]]*\][[:space:]]*,[[:space:]]*argv_hello\[0\]' nanos-lite/src/proc.c \
  || fail "the background slot is not loaded with /bin/hello"

grep -Eq 'fg_pcb[[:space:]]*=[[:space:]]*&pcb\[[[:space:]]*0[[:space:]]*\]' nanos-lite/src/proc.c \
  || fail "games profile does not start from pcb[0]"

echo "nanos games PCB order check passed"
