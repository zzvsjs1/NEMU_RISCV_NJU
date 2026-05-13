#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
JIT_C="$ROOT/nemu/src/isa/riscv32/jit.c"

fail() {
  echo "RISC-V32 JIT branch-chain check failed: $*" >&2
  exit 1
}

require_pattern() {
  local pattern=$1
  local description=$2

  if ! grep -Eq "$pattern" "$JIT_C"; then
    fail "missing $description"
  fi
}

# Intra-block branch chaining must keep cpu_exec() accounting honest. These
# checks intentionally look for the budget, executed-count accumulator, and the
# dedicated back-edge emitter rather than only for a native jump instruction.
require_pattern 'static volatile uint32_t jit_entry_budget' 'per-entry JIT instruction budget'
require_pattern 'static volatile uint32_t jit_loop_extra' 'dynamic chained-loop instruction accumulator'
require_pattern 'emit_epilogue_return_eax' 'dynamic-count epilogue'
require_pattern 'emit_epilogue_return_loop_count' 'loop-aware side-exit return helper'
require_pattern 'jit_block_has_chainable_backedge' 'chainable back-edge pre-scan'
require_pattern 'emit_branch_chain_backedge' 'bounded conditional back-edge emitter'
require_pattern 'jit_loop_extra = 0' 'loop accumulator reset before native block entry'
require_pattern 'jit_entry_budget = remaining_budget' 'budget publication before native block entry'

echo "RISC-V32 JIT branch-chain check passed"
