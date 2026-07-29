#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
JIT_CORE_C="$ROOT/nemu/src/isa/riscv32/jit-rv32-core.c"
JIT_COMPILE_C="$ROOT/nemu/src/isa/riscv32/jit-rv32-compile.c"
JIT_EMIT_C="$ROOT/nemu/src/isa/riscv32/jit-rv32-emit.c"

fail() {
  echo "RISC-V32 JIT branch-chain check failed: $*" >&2
  exit 1
}

require_pattern() {
  local source_file=$1
  local pattern=$2
  local description=$3

  if ! grep -Eq "$pattern" "$source_file"; then
    fail "missing $description"
  fi
}

# Intra-block branch chaining must keep cpu_exec() accounting honest. These
# checks follow the symbol owners introduced by the normal compilation-unit
# split: dispatch state in core, pre-scan policy in compile, and native loop
# accounting in emit.
require_pattern "$JIT_CORE_C" 'volatile uint32_t rv32_jit_entry_budget' 'per-entry JIT instruction budget'
require_pattern "$JIT_CORE_C" 'volatile uint32_t rv32_jit_loop_extra' 'dynamic chained-loop instruction accumulator'
require_pattern "$JIT_EMIT_C" 'emit_epilogue_return_eax' 'dynamic-count epilogue'
require_pattern "$JIT_EMIT_C" 'emit_epilogue_return_loop_count' 'loop-aware side-exit return helper'
require_pattern "$JIT_COMPILE_C" 'jit_block_has_chainable_backedge' 'chainable back-edge pre-scan'
require_pattern "$JIT_EMIT_C" 'emit_branch_chain_backedge' 'bounded conditional back-edge emitter'
require_pattern "$JIT_CORE_C" 'rv32_jit_loop_extra = 0' 'loop accumulator reset before native block entry'
require_pattern "$JIT_CORE_C" 'rv32_jit_entry_budget = remaining_budget' 'budget publication before native block entry'

echo "RISC-V32 JIT branch-chain check passed"
