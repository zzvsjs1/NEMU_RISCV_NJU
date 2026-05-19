#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RV64_DIR="$ROOT/nemu/src/isa/riscv64"
CPU_EXEC="$ROOT/nemu/src/cpu/cpu-exec.c"

fail() {
  echo "RV64 new-interpreter check failed: $*" >&2
  exit 1
}

[ -f "$RV64_DIR/inst.c" ] || fail "missing direct RV64 interpreter file"

old_files=(
  "$RV64_DIR/include/isa-all-instr.h"
  "$RV64_DIR/include/isa-exec.h"
  "$RV64_DIR/local-include/rtl.h"
)

for path in "${old_files[@]}"; do
  [ ! -e "$path" ] || fail "old table interpreter file still exists: ${path#$ROOT/}"
done

[ ! -d "$RV64_DIR/instr" ] || fail "old table interpreter directory still exists: ${RV64_DIR#$ROOT/}/instr"

if grep -Eq '^#ifndef CONFIG_ISA_riscv32$' "$CPU_EXEC"; then
  fail "cpu-exec still treats only RV32 as direct-interpreter ISA"
fi

if grep -R --include='*.c' --include='*.h' -nE 'isa-all-instr|isa-exec|EXEC_ID_|INSTR_LIST' "$RV64_DIR" >/dev/null; then
  fail "RV64 still references table-interpreter symbols"
fi

echo "RV64 new-interpreter structural check passed"
