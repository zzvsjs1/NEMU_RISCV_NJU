#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
SNAPSHOT_DIR=
COMPARE_DIR=
TEMP_DIR=$(mktemp -d)

cleanup() {
  rm -rf "$TEMP_DIR"
}

trap cleanup EXIT

fail() {
  echo "RV64 JIT statistics-report check failed: $*" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --snapshot-dir)
      [ "$#" -ge 2 ] || fail "--snapshot-dir requires a directory"
      SNAPSHOT_DIR=$2
      shift 2
      ;;
    --compare-dir)
      [ "$#" -ge 2 ] || fail "--compare-dir requires a directory"
      COMPARE_DIR=$2
      shift 2
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

# Reuse the generated configuration and authoritative ISA headers without
# changing the working build's selected configuration. The standalone report
# fixture needs compiled statistics even when the production binary omits them.
CONFIG_HEADER="$ROOT/nemu/include/generated/autoconf.h"
grep -q '^#define CONFIG_RV64 1$' "$CONFIG_HEADER" ||
  fail "select an RV64 configuration before running this host fixture"
mkdir -p "$TEMP_DIR/include/generated"
cp "$CONFIG_HEADER" "$TEMP_DIR/include/generated/autoconf.h"
printf '\n#undef CONFIG_RV64_JIT_STATS\n#define CONFIG_RV64_JIT_STATS 1\n' >>"$TEMP_DIR/include/generated/autoconf.h"

"${CC:-gcc}" -O2 -Wall -Wextra -Werror \
  -I"$TEMP_DIR/include" -I"$ROOT/nemu/include" \
  -I"$ROOT/nemu/src/isa/riscv32/include" -D__GUEST_ISA__=riscv64 \
  "$SCRIPT_DIR/fixtures/rv64-jit-stats-report.c" \
  -o "$TEMP_DIR/stats-report"

# shellcheck source=scripts/rv64-jit-stats.sh
source "$SCRIPT_DIR/rv64-jit-stats.sh"

if [ -n "$SNAPSHOT_DIR" ]; then
  mkdir -p "$SNAPSHOT_DIR"
fi

keys=(sites hits misses fills replacements stale_rejections budget_rejections)
# Keep the historical order independent of the C descriptor table.
pic_keys=(hits secondary_hits misses fills replacements stale_rejections budget_rejections \
  patch_resolutions patch_unlinks source_detaches target_detaches patched_entries patch_downgrades)
pic_kinds=(return jalr)
for scenario in zero distinct max; do
  case "$scenario" in
    zero) values=(0 0 0 0 0 0 0) ;;
    distinct) values=(11 13 17 19 23 29 31) ;;
    max) values=(18446744073709551615 18446744073709551615 18446744073709551615 \
      18446744073709551615 18446744073709551615 18446744073709551615 18446744073709551615) ;;
  esac

  for gate in unset empty 0 1 01 true 2; do
    output="$TEMP_DIR/$scenario-$gate.txt"
    argument=$gate
    if [ "$gate" = empty ]; then
      argument=
    fi

    "$TEMP_DIR/stats-report" "$scenario" "$argument" >"$output"
    printf -v legacy \
      'jit:   indirect jump cache sites = %s, hits = %s, misses = %s, fills = %s, replacements = %s, stale rejections = %s, budget rejects = %s' \
      "${values[@]}"
    [ "$(grep -Fxc "$legacy" "$output" || true)" -eq 1 ] ||
      fail "$scenario/$gate legacy line changed or was duplicated"

    if [ "$gate" = 1 ]; then
      load_rv64_jit_stats "$output" "$scenario-$gate" || fail "machine output is invalid"
      [ "$(grep -c '^jit-kv: indirect_jump_cache\.' "$output" || true)" -eq 7 ] ||
        fail "$scenario machine family contains missing or extra records"

      for index in "${!keys[@]}"; do
        actual=$(rv64_jit_stat "indirect_jump_cache.${keys[$index]}" "$scenario-$gate" "$output")
        [ "$actual" = "${values[$index]}" ] || fail "$scenario maps ${keys[$index]} incorrectly"
      done

      [ "$(grep -c '^jit-kv:' "$output" || true)" -eq 35 ] ||
        fail "$scenario machine report contains missing or extra exports"

      for kind in "${!pic_kinds[@]}"; do
        for event in "${!pic_keys[@]}"; do
          key="indirect_pic.${pic_kinds[$kind]}.${pic_keys[$event]}"
          case "$scenario" in
            zero) expected=0 ;;
            # Each event owns two consecutive values: return, then JALR.
            distinct) expected=$((301 + 2 * event + kind)) ;;
            max) expected=18446744073709551615 ;;
          esac

          actual=$(rv64_jit_stat "$key" "$scenario-$gate" "$output")
          [ "$actual" = "$expected" ] || fail "$scenario maps $key incorrectly"
        done
      done
    else
      if grep -q '^jit-kv:' "$output"; then
        fail "$scenario/$gate unexpectedly enabled machine output"
      fi
    fi

    # A captured pre-change report is an independent oracle for the entire
    # payload, including unrelated legacy lines, distributions and rounding.
    if [ -n "$COMPARE_DIR" ]; then
      diff -u "$COMPARE_DIR/$scenario-$gate.txt" "$output" ||
        fail "$scenario/$gate differs from the captured baseline"
    fi

    if [ -n "$SNAPSHOT_DIR" ]; then
      cp "$output" "$SNAPSHOT_DIR/$scenario-$gate.txt"
    fi
  done
done

echo "RV64 JIT statistics-report check passed (100 scalar counters, 26 PIC values, 3 synthetic states, 7 machine-output gates)"
