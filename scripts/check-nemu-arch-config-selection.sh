#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export NEMU_HOME="$ROOT/nemu"
export AM_HOME="$ROOT/abstract-machine"
OUT=$(mktemp)
TEMP_CONFIGS=()

cleanup() {
    rm -f "$OUT"
    local cfg

    for cfg in "${TEMP_CONFIGS[@]}"; do
        rm -f "$cfg"
    done
}

trap cleanup EXIT

fail() {
    echo "check-nemu-arch-config-selection: $*" >&2
    exit 1
}

if grep -R -n '^NEMU_DEFCONFIG[[:space:]]*?=' "$ROOT/abstract-machine/scripts" >"$OUT"; then
    cat "$OUT" >&2
    fail "AM scripts must not choose a NEMU defconfig by default"
fi

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

BAD_RV64_CONFIG="$NEMU_HOME/configs/codex-riscv64-bad-mbase_defconfig"
TEMP_CONFIGS+=("$BAD_RV64_CONFIG")
cat >"$BAD_RV64_CONFIG" <<'EOF'
CONFIG_ISA_riscv64=y
CONFIG_ENGINE_INTERPRETER=y
CONFIG_MODE_SYSTEM=y
CONFIG_TARGET_NATIVE_ELF=y
CONFIG_CC_GCC=y
CONFIG_CC_O2=y
CONFIG_MBASE=0x0
CONFIG_PC_RESET_OFFSET=0x100000
CONFIG_DEVICE=y
CONFIG_RV64_JIT=y
EOF

make -C "$NEMU_HOME" codex-riscv64-bad-mbase_defconfig >/dev/null
make -C "$ROOT/am-kernels/benchmarks/microbench" ARCH=riscv64-nemu nemu-config >"$OUT" 2>&1

if ! grep -q "adjusting NEMU memory layout for ARCH=riscv64-nemu" "$OUT"; then
    cat "$OUT" >&2
    fail "bad RV64 memory layout was not adjusted"
fi

if ! grep -q '^CONFIG_MBASE=0x80000000$' "$NEMU_HOME/.config" ||
   ! grep -q '^CONFIG_PC_RESET_OFFSET=0$' "$NEMU_HOME/.config"; then
    sed -n '1,90p' "$NEMU_HOME/.config" >&2
    fail "bad RV64 memory layout was not repaired in .config"
fi

make -C "$NEMU_HOME" riscv64-am-headless_defconfig >/dev/null

BAD_X86_CONFIG="$NEMU_HOME/configs/codex-x86-bad-mbase_defconfig"
TEMP_CONFIGS+=("$BAD_X86_CONFIG")
cat >"$BAD_X86_CONFIG" <<'EOF'
CONFIG_ISA_x86=y
CONFIG_ENGINE_INTERPRETER=y
CONFIG_MODE_SYSTEM=y
CONFIG_TARGET_NATIVE_ELF=y
CONFIG_CC_GCC=y
CONFIG_CC_O2=y
CONFIG_MBASE=0x80000000
CONFIG_PC_RESET_OFFSET=0
CONFIG_DEVICE=y
CONFIG_X86_JIT=y
EOF

make -C "$NEMU_HOME" codex-x86-bad-mbase_defconfig >/dev/null
make -C "$ROOT/am-kernels/benchmarks/microbench" ARCH=x86-nemu nemu-config >"$OUT" 2>&1

if ! grep -q "adjusting NEMU memory layout for ARCH=x86-nemu" "$OUT"; then
    cat "$OUT" >&2
    fail "bad x86 memory layout was not adjusted"
fi

if ! grep -q '^CONFIG_MBASE=0x0$' "$NEMU_HOME/.config" ||
   ! grep -q '^CONFIG_PC_RESET_OFFSET=0x100000$' "$NEMU_HOME/.config"; then
    sed -n '1,90p' "$NEMU_HOME/.config" >&2
    fail "bad x86 memory layout was not repaired in .config"
fi

make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

echo "NEMU arch config selection check passed"
