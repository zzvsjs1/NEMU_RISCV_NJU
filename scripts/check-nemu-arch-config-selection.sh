#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NEMU_HOME="$ROOT/nemu"
OUT=$(mktemp)

cleanup() {
    rm -f "$OUT"
}
trap cleanup EXIT

fail() {
    echo "check-nemu-arch-config-selection: $*" >&2
    exit 1
}

make -C "$NEMU_HOME" riscv32-am-headless-jit_defconfig >/dev/null
if make -C "$NEMU_HOME" -n ISA=riscv64 run >"$OUT" 2>&1; then
    cat "$OUT" >&2
    fail "NEMU accepted ISA=riscv64 while .config selects riscv32"
fi

if ! grep -q "does not match current NEMU config" "$OUT"; then
    cat "$OUT" >&2
    fail "mismatched ISA failed without the expected diagnostic"
fi

make -C "$NEMU_HOME" riscv64-am-headless_defconfig >/dev/null
make -C "$NEMU_HOME" -n ISA=riscv64 run >"$OUT" 2>&1
if ! grep -q "riscv64-nemu-interpreter" "$OUT"; then
    cat "$OUT" >&2
    fail "matching RV64 config did not select the RV64 interpreter"
fi

echo "NEMU arch config selection check passed"
