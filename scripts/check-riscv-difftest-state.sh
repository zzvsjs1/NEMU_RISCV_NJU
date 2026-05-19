#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NEMU_HOME="$ROOT/nemu"
TMPDIR=$(mktemp -d)

cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

fail() {
    echo "check-riscv-difftest-state: $*" >&2
    exit 1
}

PROBE="$TMPDIR/riscv-difftest-state.c"
cat >"$PROBE" <<'PROBE'
#include <stddef.h>
#include <isa.h>
#include <difftest-def.h>

#if !defined(CONFIG_ISA_riscv)
#error "this probe must be compiled with a RISC-V config"
#endif

_Static_assert(sizeof(CPU_state) == DIFFTEST_REG_SIZE,
               "RISC-V DiffTest size must cover the full CPU_state");
_Static_assert(sizeof(riscv_difftest_state_t) == DIFFTEST_REG_SIZE,
               "RISC-V DiffTest state type must define the public ABI size");
_Static_assert(offsetof(CPU_state, gpr) == offsetof(riscv_difftest_state_t, gpr),
               "RISC-V DiffTest GPR offset drifted");
_Static_assert(offsetof(CPU_state, pc) == offsetof(riscv_difftest_state_t, pc),
               "RISC-V DiffTest PC offset drifted");
_Static_assert(offsetof(CPU_state, csr.satp) == offsetof(riscv_difftest_state_t, csr.satp),
               "RISC-V DiffTest satp offset drifted");
_Static_assert(offsetof(CPU_state, csr.mtval) == offsetof(riscv_difftest_state_t, csr.mtval),
               "RISC-V DiffTest mtval offset drifted");
_Static_assert(offsetof(CPU_state, prvi) == offsetof(riscv_difftest_state_t, prvi),
               "RISC-V DiffTest privilege offset drifted");
_Static_assert(offsetof(CPU_state, INTR) == offsetof(riscv_difftest_state_t, INTR),
               "RISC-V DiffTest interrupt-pending offset drifted");

int main(void) { return 0; }
PROBE

compile_probe() {
    local isa=$1
    local defconfig=$2

    make -C "$NEMU_HOME" "$defconfig" >/dev/null
    gcc -std=gnu11 \
        -I"$NEMU_HOME/include" \
        -I"$NEMU_HOME/src/isa/$isa/include" \
        -D__GUEST_ISA__="$isa" \
        -c "$PROBE" \
        -o "$TMPDIR/$isa.o" || fail "$isa DiffTest state ABI probe failed"
}

compile_probe riscv32 riscv32-am-headless-jit_defconfig
compile_probe riscv64 riscv64-am-headless_defconfig

echo "RISC-V DiffTest state ABI check passed"
