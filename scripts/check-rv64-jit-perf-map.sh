#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=riscv64
export ARCH=riscv64-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

BENCH_DIR="$ROOT/am-kernels/benchmarks/branchmark"
JIT_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig
out=$(mktemp)
disabled_out=$(mktemp)
failure_out=$(mktemp)
reset_out=$(mktemp)
snapshot_path=$(mktemp)
rm -f "$snapshot_path"
perf_map=
perf_map_identity=
reset_map=
reset_map_identity=
unsafe_map=
unsafe_map_identity=
unset_map=
unset_map_identity=
restore_jit_config=0

remove_unchanged_map() {
  local map_path=$1
  local expected_identity=$2
  local current_identity

  if [[ ! "$map_path" =~ ^/tmp/perf-[0-9]+\.map$ ]] ||
      [ -z "$expected_identity" ] || [ ! -f "$map_path" ] ||
      [ -L "$map_path" ]; then
    return
  fi

  current_identity=$(stat -c '%d:%i:%u' "$map_path" 2>/dev/null || true)
  if [ "$current_identity" = "$expected_identity" ] &&
      [ "${current_identity##*:}" = "$(id -u)" ]; then
    rm -f "$map_path"
  fi
}

remove_unchanged_fifo() {
  local fifo_path=$1
  local expected_identity=$2
  local current_identity

  if [[ ! "$fifo_path" =~ ^/tmp/perf-[0-9]+\.map$ ]] ||
      [ -z "$expected_identity" ] || [ ! -p "$fifo_path" ] ||
      [ -L "$fifo_path" ]; then
    return
  fi

  current_identity=$(stat -c '%d:%i:%u' "$fifo_path" 2>/dev/null || true)
  if [ "$current_identity" = "$expected_identity" ] &&
      [ "${current_identity##*:}" = "$(id -u)" ]; then
    rm -f "$fifo_path"
  fi
}

restore_config() {
  if [ "$restore_jit_config" -eq 0 ]; then
    return
  fi

  make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null
  make -C "$NEMU_HOME" -j2 >/dev/null
  restore_jit_config=0
}

cleanup() {
  rm -f "$out" "$disabled_out" "$failure_out" "$reset_out" "$snapshot_path"
  remove_unchanged_map "$perf_map" "$perf_map_identity"
  remove_unchanged_map "$reset_map" "$reset_map_identity"
  remove_unchanged_map "$unset_map" "$unset_map_identity"
  remove_unchanged_fifo "$unsafe_map" "$unsafe_map_identity"

  restore_config || true
}
trap cleanup EXIT

fail() {
  echo "RISC-V64 JIT perf-map check failed: $*" >&2
  exit 1
}

validate_map() {
  local map_path=$1
  local expected_generation=$2

  # POSIX awk has no portable hexadecimal conversion routine. This local
  # helper keeps the check independent of gawk while remaining exact for normal
  # x86-64 user-space addresses, which fit in an awk number's integer precision.
  awk -v expected_generation="$expected_generation" '
  function hex_value(text, result, digit_index, digit) {
    text = tolower(text)
    result = 0

    for (digit_index = 1; digit_index <= length(text); digit_index++) {
      digit = index("0123456789abcdef", substr(text, digit_index, 1)) - 1
      if (digit < 0) {
        return -1
      }
      result = result * 16 + digit
    }

    return result
  }

  {
    if (NF != 3 || $1 !~ /^[0-9a-fA-F]+$/ || $2 !~ /^[0-9a-fA-F]+$/) {
      printf "invalid perf-map fields on line %d: %s\n", NR, $0 > "/dev/stderr"
      exit 1
    }

    part_count = split($3, parts, "_")
    if (part_count != 9 || parts[1] != "rv64" || parts[2] != "pc" ||
        length(parts[3]) != 16 || parts[3] !~ /^[0-9a-fA-F]+$/ ||
        parts[4] != "ctx" || parts[5] != "00000003" ||
        parts[6] != "n" || parts[7] !~ /^[1-9][0-9]*$/ ||
        parts[8] != "g" || parts[9] != expected_generation) {
      printf "invalid RV64 JIT symbol on line %d: %s\n", NR, $3 > "/dev/stderr"
      exit 1
    }

    address = hex_value($1)
    size = hex_value($2)
    if (address < 0 || size <= 0) {
      printf "invalid native range on line %d: %s %s\n", NR, $1, $2 > "/dev/stderr"
      exit 1
    }

    if (NR > 1 && address < previous_end) {
      printf "overlapping or unordered native range on line %d\n", NR > "/dev/stderr"
      exit 1
    }
    previous_end = address + size
  }

  END {
    if (NR == 0) {
      print "perf map contained no records" > "/dev/stderr"
      exit 1
    }
  }
  ' "$map_path" ||
    fail "map records did not satisfy generation $expected_generation contract"
}

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

NEMU_JIT_PERFMAP=1 NEMU_JIT_STATS=1 \
  make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
    cat "$out" >&2
    exit 2
  }

grep -q 'BranchMark PASS' "$out" || {
  cat "$out" >&2
  fail "BranchMark did not pass"
}

perf_map=$(
  sed -n 's|.*jit: perf map = \(/tmp/perf-[0-9][0-9]*\.map\).*|\1|p' \
    "$out" | tail -n 1
)
[ -n "$perf_map" ] || {
  cat "$out" >&2
  fail "could not find the standard perf-map path in NEMU output"
}
[ -f "$perf_map" ] || fail "reported perf map does not exist: $perf_map"
[ -s "$perf_map" ] || fail "reported perf map is empty: $perf_map"
perf_map_identity=$(stat -c '%d:%i:%u' "$perf_map")

permissions=$(stat -c '%a' "$perf_map")
[ "$permissions" = "600" ] ||
  fail "expected owner-only map permissions 600, got $permissions"

compiled_blocks=$(
  sed -n 's/.*compiled blocks = \([0-9][0-9]*\).*/\1/p' "$out" |
    tail -n 1
)
[ -n "$compiled_blocks" ] || {
  cat "$out" >&2
  fail "could not parse the successfully compiled block count"
}
[ "$compiled_blocks" -gt 0 ] ||
  fail "expected at least one successfully compiled block"

arena_resets=$(
  sed -n 's/.*arena resets = \([0-9][0-9]*\).*/\1/p' "$out" |
    tail -n 1
)
[ "$arena_resets" = "0" ] || {
  cat "$out" >&2
  fail "single-generation count comparison requires zero arena resets"
}

validate_map "$perf_map" 1
map_lines=$(wc -l <"$perf_map")
[ "$map_lines" -eq "$compiled_blocks" ] || {
  cat "$out" >&2
  fail "expected one map record per compiled block ($compiled_blocks), got $map_lines"
}

guest_elf="$BENCH_DIR/build/branchmark-riscv64-nemu.elf"
guest_bin="$BENCH_DIR/build/branchmark-riscv64-nemu.bin"
nemu_bin="$NEMU_HOME/build/riscv64-nemu-interpreter"
hot_loop_pc=$(
  riscv64-linux-gnu-nm -n "$guest_elf" |
    awk '$3 ~ /^branch_hot_loop/ { print tolower($1); exit }'
)
[[ "$hot_loop_pc" =~ ^[0-9a-f]{16}$ ]] ||
  fail "could not resolve the BranchMark hot-loop guest PC"

# This directed workload runs in Bare M-mode. Its known 68-instruction trace
# checks that PC, fetch context, instruction count, and generation are values
# from the published block rather than constants with the right syntax.
hot_symbol="rv64_pc_${hot_loop_pc}_ctx_00000003_n_68_g_1"
grep -Fq " $hot_symbol" "$perf_map" ||
  fail "missing expected semantic hot-loop symbol: $hot_symbol"

# Prove that the absent environment variable is the default-off state and that
# it creates no file, even if a future regression accidentally omits the log.
unset_status=3
for unused_attempt in 1 2 3; do
  if timeout --kill-after=2s 15s bash -c '
    candidate="/tmp/perf-${BASHPID}.map"
    if [ -e "$candidate" ] || [ -L "$candidate" ]; then
      exit 3
    fi
    printf "perf_map_test_unset=%s\n" "$candidate"
    exec env -u NEMU_JIT_PERFMAP NEMU_JIT_STATS=1 \
      NAVY_HOME="$NAVY_HOME" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
      "$1" "$2"
  ' perf-map-unset "$nemu_bin" "$guest_bin" >"$disabled_out" 2>&1; then
    unset_status=0
    break
  else
    unset_status=$?
  fi

  [ "$unset_status" -eq 3 ] || break
done

if [ "$unset_status" -ne 0 ]; then
  cat "$disabled_out" >&2
  fail "default-off child failed or reused three occupied PID paths"
fi

unset_map=$(
  sed -n 's/^perf_map_test_unset=\(\/tmp\/perf-[0-9][0-9]*\.map\)$/\1/p' \
    "$disabled_out" | tail -n 1
)
[[ "$unset_map" =~ ^/tmp/perf-[0-9]+\.map$ ]] ||
  fail "could not recover the default-off child PID path"
grep -q 'BranchMark PASS' "$disabled_out" ||
  fail "default-off BranchMark did not pass"
if [ -e "$unset_map" ] || [ -L "$unset_map" ]; then
  if [ -f "$unset_map" ] && [ ! -L "$unset_map" ]; then
    unset_map_identity=$(stat -c '%d:%i:%u' "$unset_map")
  fi
  fail "unset NEMU_JIT_PERFMAP unexpectedly created $unset_map"
fi
if grep -q 'jit: perf map =' "$disabled_out"; then
  fail "unset NEMU_JIT_PERFMAP unexpectedly enabled map output"
fi

# Retain the existing environment-flag convention: exactly "0" is also false.
NEMU_JIT_PERFMAP=0 NEMU_JIT_STATS=1 \
  make -C "$BENCH_DIR" ARCH="$ARCH" run >"$disabled_out" 2>&1 || {
    cat "$disabled_out" >&2
    exit 2
  }
grep -q 'BranchMark PASS' "$disabled_out" ||
  fail "disabled-mode BranchMark did not pass"
if grep -q 'jit: perf map =' "$disabled_out"; then
  cat "$disabled_out" >&2
  fail "NEMU_JIT_PERFMAP=0 unexpectedly opened a map"
fi

# Pre-create the exact child-PID pathname as a FIFO. O_NONBLOCK must keep NEMU
# from hanging, and map failure must leave normal JIT execution fully usable.
fifo_status=3
for unused_attempt in 1 2 3; do
  if timeout --kill-after=2s 15s bash -c '
    candidate="/tmp/perf-${BASHPID}.map"
    if [ -e "$candidate" ] || [ -L "$candidate" ]; then
      exit 3
    fi
    mkfifo -m 600 "$candidate" || exit 4
    printf "perf_map_test_fifo=%s\n" "$candidate"
    printf "perf_map_test_fifo_identity=%s\n" \
      "$(stat -c "%d:%i:%u" "$candidate")"
    exec env NEMU_JIT_PERFMAP=1 NEMU_JIT_STATS=1 \
      NAVY_HOME="$NAVY_HOME" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
      "$1" "$2"
  ' perf-map-fifo "$nemu_bin" "$guest_bin" >"$failure_out" 2>&1; then
    fifo_status=0
    break
  else
    fifo_status=$?
  fi

  [ "$fifo_status" -eq 3 ] || break
done

unsafe_map=$(
  sed -n 's/^perf_map_test_fifo=\(\/tmp\/perf-[0-9][0-9]*\.map\)$/\1/p' \
    "$failure_out" | tail -n 1
)
unsafe_map_identity=$(
  sed -n \
    's/^perf_map_test_fifo_identity=\([0-9][0-9]*:[0-9][0-9]*:[0-9][0-9]*\)$/\1/p' \
    "$failure_out" | tail -n 1
)

if [ "$fifo_status" -ne 0 ]; then
  cat "$failure_out" >&2
  if [ "$fifo_status" -eq 124 ] || [ "$fifo_status" -eq 137 ]; then
    fail "unsafe FIFO path blocked JIT initialisation"
  fi
  fail "unsafe-path child failed with status $fifo_status"
fi

[[ "$unsafe_map" =~ ^/tmp/perf-[0-9]+\.map$ ]] ||
  fail "could not recover the controlled FIFO path"
[ -p "$unsafe_map" ] || fail "controlled unsafe path is no longer a FIFO"
[ "$(stat -c '%d:%i:%u' "$unsafe_map")" = "$unsafe_map_identity" ] ||
  fail "controlled FIFO identity changed before validation"
grep -q 'BranchMark PASS' "$failure_out" ||
  fail "perf-map open failure changed guest execution"
grep -q 'disable perf-map output' "$failure_out" ||
  fail "unsafe map path did not disable only profiler output"
if grep -q 'jit: perf map =' "$failure_out"; then
  cat "$failure_out" >&2
  fail "unsafe FIFO was incorrectly accepted as a perf map"
fi

# Loading a real 128 MiB snapshot invokes isa_jit_flush_all() after native code
# has been published. This exercises truncation and generation advancement
# without adding a production-only reset test hook.
restore_jit_config=1
sed -i \
  's/^CONFIG_SDB_BATCH_DEFAULT=y$/# CONFIG_SDB_BATCH_DEFAULT is not set/' \
  "$NEMU_HOME/.config"
if grep -q '^CONFIG_SDB_BATCH_DEFAULT=y$' "$NEMU_HOME/.config"; then
  fail "could not prepare the interactive snapshot configuration"
fi
make -C "$NEMU_HOME" syncconfig >/dev/null
make -C "$NEMU_HOME" -j2 >/dev/null

printf 'si 1000\nsave %s\nsi 1000\nload %s\nc\nq\n' \
  "$snapshot_path" "$snapshot_path" |
  env NEMU_JIT_PERFMAP=1 NEMU_JIT_STATS=1 \
    NAVY_HOME="$NAVY_HOME" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    "$nemu_bin" "$guest_bin" >"$reset_out" 2>&1 || {
      cat "$reset_out" >&2
      exit 2
    }

grep -q 'BranchMark PASS' "$reset_out" || {
  cat "$reset_out" >&2
  fail "snapshot-reset BranchMark did not pass"
}
grep -q 'perf map arena reset drops old symbols' "$reset_out" ||
  fail "arena reset did not report the text-map attribution limit"

reset_map=$(
  sed -n 's|.*jit: perf map = \(/tmp/perf-[0-9][0-9]*\.map\).*|\1|p' \
    "$reset_out" | tail -n 1
)
[ -s "$reset_map" ] || fail "reset run did not leave a current-generation map"
reset_map_identity=$(stat -c '%d:%i:%u' "$reset_map")
validate_map "$reset_map" 2

reset_arena_resets=$(
  sed -n 's/.*arena resets = \([0-9][0-9]*\).*/\1/p' "$reset_out" |
    tail -n 1
)
[ "$reset_arena_resets" = "1" ] ||
  fail "expected exactly one snapshot-driven arena reset, got $reset_arena_resets"

reset_compiled_blocks=$(
  sed -n 's/.*compiled blocks = \([0-9][0-9]*\).*/\1/p' "$reset_out" |
    tail -n 1
)
[ -n "$reset_compiled_blocks" ] ||
  fail "could not parse reset-run compiled block count"
reset_map_lines=$(wc -l <"$reset_map")
[ "$reset_map_lines" -lt "$reset_compiled_blocks" ] ||
  fail "arena reset did not truncate records from the previous generation"

reset_hot_symbol="rv64_pc_${hot_loop_pc}_ctx_00000003_n_68_g_2"
grep -Fq " $reset_hot_symbol" "$reset_map" ||
  fail "missing generation-two semantic hot-loop symbol"

restore_config

printf "perf_map_records=%s compiled_blocks=%s permissions=%s " \
  "$map_lines" "$compiled_blocks" "$permissions"
printf "reset_records=%s reset_compiled_blocks=%s failure_path=nonfatal\n" \
  "$reset_map_lines" "$reset_compiled_blocks"
