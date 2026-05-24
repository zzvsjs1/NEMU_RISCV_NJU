#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=x86
export ARCH=x86-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

out=$(mktemp)

cleanup() {
  rm -f "$out"
}

trap cleanup EXIT

fail() {
  echo "x86 JIT paged memory fastpath check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"

alu_rm_reg_body=$(sed -n '/static bool emit_paged_dtlb_alu_rm_reg/,/^}/p' \
  "$ROOT/nemu/src/isa/x86/jit.c")
translate_line=$(printf '%s\n' "$alu_rm_reg_body" |
  sed -n '/emit_paged_dtlb_load_ea_eax/=' | head -n 1)
src_load_line=$(printf '%s\n' "$alu_rm_reg_body" |
  sed -n '/emit_load_reg_r11d/=' | head -n 1)

[ -n "$translate_line" ] || fail "missing paged ALU r/m,reg DTLB load"
[ -n "$src_load_line" ] || fail "missing paged ALU r/m,reg source load"
[ "$src_load_line" -gt "$translate_line" ] ||
  fail "paged ALU r/m,reg must load r11d after the DTLB C call"

grep -q 'emit_paged_dtlb_write_hit_inline' "$ROOT/nemu/src/isa/x86/jit.c" ||
  fail "missing inline paged write-DTLB hit path"

translate_addr_body=$(sed -n '/static bool emit_paged_dtlb_translate_addr_eax/,/^}/p' \
  "$ROOT/nemu/src/isa/x86/jit.c")
printf '%s\n' "$translate_addr_body" |
  grep -q 'emit_paged_dtlb_write_hit_inline' ||
  fail "paged writes still go straight to the DTLB C helper on hits"

mov_load_body=$(sed -n '/static bool emit_paged_dtlb_mov_reg_rm_load/,/^}/p' \
  "$ROOT/nemu/src/isa/x86/jit.c")
printf '%s\n' "$mov_load_body" |
  grep -q 'emit_paged_dtlb_translate_ea' ||
  fail "paged MOV loads still bypass the inline DTLB hit path"

mov_store_body=$(sed -n '/static bool emit_paged_dtlb_mov_rm_reg_store/,/^}/p' \
  "$ROOT/nemu/src/isa/x86/jit.c")
printf '%s\n' "$mov_store_body" |
  grep -q 'emit_paged_dtlb_translate_ea' ||
  fail "paged MOV stores still bypass the inline DTLB hit path"

make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 NEMU_EXIT_AFTER_INSTR=50000000 \
    make -C nanos-lite ARCH="$ARCH" NEMU_DEFCONFIG=x86-am-jit_defconfig \
    NANOS_INIT=fceux NAVY_APPS=fceux NAVY_TESTS= FS_MODE=fat32 update run \
    >"$out" 2>&1; then
  fail "nanos-lite FCEUX run failed"
fi

read_hits=$(sed -n 's/.*jit: DTLB read hits = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
write_hits=$(sed -n 's/.*jit: DTLB write hits = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
fills=$(sed -n 's/.*jit: DTLB fills = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
fallbacks=$(sed -n 's/.*jit: DTLB fallbacks = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
sbb_hits=$(sed -n 's/.*jit: unsupported-hit opcode 0x19 = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
shift_helpers=$(sed -n 's/.*helper profile shift-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
shift_helpers=${shift_helpers:-0}

[ -n "$read_hits" ] || fail "missing DTLB read hit stats"
[ -n "$write_hits" ] || fail "missing DTLB write hit stats"
[ -n "$fills" ] || fail "missing DTLB fill stats"
[ -n "$fallbacks" ] || fail "missing DTLB fallback stats"

[ "$read_hits" -gt 1000000 ] || fail "expected many DTLB read hits, got $read_hits"
[ "$write_hits" -gt 100000 ] || fail "expected DTLB write hits, got $write_hits"
[ "$fills" -gt 0 ] || fail "expected at least one DTLB fill"
[ -z "$sbb_hits" ] || [ "$sbb_hits" -eq 0 ] ||
  fail "expected native SBB coverage, got unsupported 0x19 hits = $sbb_hits"
[ "$shift_helpers" -le 10000 ] ||
  fail "expected native paged byte/word memory CL shifts, got shift-rm helpers = $shift_helpers"

echo "x86 JIT paged memory fastpath check passed: read_hits=$read_hits write_hits=$write_hits fills=$fills fallbacks=$fallbacks shift_helpers=$shift_helpers"
