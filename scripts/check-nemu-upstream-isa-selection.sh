#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NEMU_HOME="$ROOT/nemu"
CONFIG_DIR="$NEMU_HOME/configs"
TEMP_CONFIGS=()

cleanup() {
    local cfg
    for cfg in "${TEMP_CONFIGS[@]}"; do
        rm -f "$cfg"
    done
}
trap cleanup EXIT

fail() {
    echo "check-nemu-upstream-isa-selection: $*" >&2
    exit 1
}

check_config_value() {
    local pattern=$1
    if ! grep -q "$pattern" "$NEMU_HOME/.config"; then
        sed -n '1,80p' "$NEMU_HOME/.config" >&2
        fail "missing expected config pattern: $pattern"
    fi
}

check_isa() {
    local isa=$1
    local target="codex-${isa}_defconfig"
    local path="$CONFIG_DIR/$target"
    TEMP_CONFIGS+=("$path")

    cat >"$path" <<EOF
CONFIG_ISA_${isa}=y
CONFIG_ENGINE_INTERPRETER=y
CONFIG_MODE_SYSTEM=y
CONFIG_TARGET_NATIVE_ELF=y
CONFIG_CC_GCC=y
CONFIG_CC_O2=y
EOF

    make -C "$NEMU_HOME" "$target" >/dev/null
    check_config_value "^CONFIG_ISA_${isa}=y$"
    check_config_value "^CONFIG_ISA=\"${isa}\"$"
}

check_isa x86
check_isa mips32
check_isa loongarch32r

make -C "$NEMU_HOME" riscv32-am-headless-jit_defconfig >/dev/null
check_config_value '^CONFIG_ISA_riscv32=y$'
check_config_value '^CONFIG_ISA="riscv32"$'

make -C "$NEMU_HOME" riscv64-am-headless_defconfig >/dev/null
check_config_value '^CONFIG_ISA_riscv64=y$'
check_config_value '^CONFIG_ISA="riscv64"$'

echo "NEMU upstream ISA selection check passed"
