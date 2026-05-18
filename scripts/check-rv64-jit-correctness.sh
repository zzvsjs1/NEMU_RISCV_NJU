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

DEFAULT_DEFCONFIG="$NEMU_HOME/configs/riscv64-am-headless-jit_defconfig"
DEFCONFIG="$NEMU_HOME/configs/riscv64-am-headless-jit-stats_defconfig"
TESTS=(riscv64-jit-strict riscv64-jit-smc riscv64-jit-negative-cache riscv64-jit-load-fast riscv64-jit-store-fast riscv64-jit-jump-fast riscv64-jit-direct-link riscv64-jit-trace riscv64-jit-m-fast riscv64-jit-sv39-remap riscv64-jit-sv39-cross-page riscv64-jit-mprv-ifetch riscv64-jit-reg-cache riscv64-jit-memory-entry riscv64-jit-sv39-data riscv64-jit-sv39-dtlb)

fail() {
  echo "RISC-V64 JIT correctness check failed: $*" >&2
  exit 1
}

require_positive_jit_instructions() {
  local log=$1
  local test_name=$2
  local jit_insns

  jit_insns=$(sed -n 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$jit_insns" ]; then
    echo "Failed to find JIT instruction stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$jit_insns" -le 0 ]; then
    echo "Expected positive JIT instruction count for $test_name, got $jit_insns" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_loads() {
  local log=$1
  local test_name=$2
  local native_loads

  native_loads=$(sed -n 's/.*native loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_loads" ]; then
    echo "Failed to find native load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_loads" -le 0 ]; then
    echo "Expected positive native load count for $test_name, got $native_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_stores() {
  local log=$1
  local test_name=$2
  local native_stores

  native_stores=$(sed -n 's/.*native stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_stores" ]; then
    echo "Failed to find native store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_stores" -le 0 ]; then
    echo "Expected positive native store count for $test_name, got $native_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_jumps() {
  local log=$1
  local test_name=$2
  local native_jumps

  native_jumps=$(sed -n 's/.*native jumps = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_jumps" ]; then
    echo "Failed to find native jump stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_jumps" -le 0 ]; then
    echo "Expected positive native jump count for $test_name, got $native_jumps" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_m_ops() {
  local log=$1
  local test_name=$2
  local native_m_ops

  native_m_ops=$(sed -n 's/.*native M ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_m_ops" ]; then
    echo "Failed to find native M-op stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_m_ops" -le 0 ]; then
    echo "Expected positive native M-op count for $test_name, got $native_m_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_translated_blocks() {
  local log=$1
  local test_name=$2
  local translated_blocks

  translated_blocks=$(sed -n 's/.*translated blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$translated_blocks" ]; then
    echo "Failed to find translated block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$translated_blocks" -le 0 ]; then
    echo "Expected positive translated block count for $test_name, got $translated_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_translated_cross_page_blocks() {
  local log=$1
  local test_name=$2
  local translated_cross_page_blocks

  translated_cross_page_blocks=$(sed -n 's/.*translated cross-page blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$translated_cross_page_blocks" ]; then
    echo "Failed to find translated cross-page block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$translated_cross_page_blocks" -le 0 ]; then
    echo "Expected positive translated cross-page block count for $test_name, got $translated_cross_page_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_segmented_source_blocks() {
  local log=$1
  local test_name=$2
  local segmented_source_blocks

  segmented_source_blocks=$(sed -n 's/.*segmented source blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$segmented_source_blocks" ]; then
    echo "Failed to find segmented source-block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$segmented_source_blocks" -le 0 ]; then
    echo "Expected positive segmented source-block count for $test_name, got $segmented_source_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_trace_blocks() {
  local log=$1
  local test_name=$2
  local trace_blocks

  trace_blocks=$(sed -n 's/.*trace blocks = \([0-9][0-9]*\), trace instructions = [0-9][0-9]*.*/\1/p' "$log" | tail -n 1)
  if [ -z "$trace_blocks" ]; then
    echo "Failed to find trace-block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$trace_blocks" -le 0 ]; then
    echo "Expected positive trace-block count for $test_name, got $trace_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_reg_cache_spills() {
  local log=$1
  local test_name=$2
  local reg_cache_spills

  reg_cache_spills=$(sed -n 's/.*reg cache spills = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$reg_cache_spills" ]; then
    echo "Failed to find register-cache spill stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$reg_cache_spills" -le 0 ]; then
    echo "Expected positive register-cache spill count for $test_name, got $reg_cache_spills" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_reg_cache_spills_at_most() {
  local log=$1
  local test_name=$2
  local max_spills=$3
  local reg_cache_spills

  reg_cache_spills=$(sed -n 's/.*reg cache spills = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$reg_cache_spills" ]; then
    echo "Failed to find register-cache spill stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$reg_cache_spills" -gt "$max_spills" ]; then
    echo "Expected register-cache spill count for $test_name to be <= $max_spills, got $reg_cache_spills" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_store_continuations() {
  local log=$1
  local test_name=$2
  local store_continuations

  store_continuations=$(sed -n 's/.*native store continuations = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$store_continuations" ]; then
    echo "Failed to find native store continuation stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$store_continuations" -le 0 ]; then
    echo "Expected positive native store continuation count for $test_name, got $store_continuations" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_paged_loads() {
  local log=$1
  local test_name=$2
  local native_paged_loads

  native_paged_loads=$(sed -n 's/.*native paged loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_paged_loads" ]; then
    echo "Failed to find native paged load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_paged_loads" -le 0 ]; then
    echo "Expected positive native paged load count for $test_name, got $native_paged_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_paged_stores() {
  local log=$1
  local test_name=$2
  local native_paged_stores

  native_paged_stores=$(sed -n 's/.*native paged stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_paged_stores" ]; then
    echo "Failed to find native paged store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_paged_stores" -le 0 ]; then
    echo "Expected positive native paged store count for $test_name, got $native_paged_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_invalidated_blocks() {
  local log=$1
  local test_name=$2
  local invalidated_blocks

  invalidated_blocks=$(sed -n 's/.*invalidated blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$invalidated_blocks" ]; then
    echo "Failed to find invalidated block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$invalidated_blocks" -le 0 ]; then
    echo "Expected positive invalidated block count for $test_name, got $invalidated_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_ifetch_generation_fast_hits() {
  local log=$1
  local test_name=$2
  local fast_hits

  fast_hits=$(sed -n 's/.*ifetch generation fast hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$fast_hits" ]; then
    echo "Failed to find ifetch generation fast-hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$fast_hits" -le 0 ]; then
    echo "Expected positive ifetch generation fast-hit count for $test_name, got $fast_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_source_reverse_invalidations() {
  local log=$1
  local test_name=$2
  local reverse_walks

  reverse_walks=$(sed -n 's/.*source reverse invalidations = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$reverse_walks" ]; then
    echo "Failed to find source reverse-invalidation stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$reverse_walks" -le 0 ]; then
    echo "Expected positive source reverse-invalidation count for $test_name, got $reverse_walks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_zero_side_exits() {
  local log=$1
  local test_name=$2
  local zero_side_exits

  zero_side_exits=$(sed -n 's/.*zero side exits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$zero_side_exits" ]; then
    echo "Failed to find zero side-exit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$zero_side_exits" -le 0 ]; then
    echo "Expected positive zero side-exit count for $test_name, got $zero_side_exits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_data_tlb_hits() {
  local log=$1
  local test_name=$2
  local data_tlb_hits

  data_tlb_hits=$(sed -n 's/.*data TLB hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$data_tlb_hits" ]; then
    echo "Failed to find data TLB hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$data_tlb_hits" -le 0 ]; then
    echo "Expected positive data TLB hit count for $test_name, got $data_tlb_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_data_tlb_fills() {
  local log=$1
  local test_name=$2
  local data_tlb_fills

  data_tlb_fills=$(sed -n 's/.*data TLB fills = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$data_tlb_fills" ]; then
    echo "Failed to find data TLB fill stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$data_tlb_fills" -le 0 ]; then
    echo "Expected positive data TLB fill count for $test_name, got $data_tlb_fills" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_data_tlb_flushes() {
  local log=$1
  local test_name=$2
  local data_tlb_flushes

  data_tlb_flushes=$(sed -n 's/.*data TLB flushes = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$data_tlb_flushes" ]; then
    echo "Failed to find data TLB flush stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$data_tlb_flushes" -le 0 ]; then
    echo "Expected positive data TLB flush count for $test_name, got $data_tlb_flushes" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_data_tlb_page_table_flushes() {
  local log=$1
  local test_name=$2
  local data_tlb_page_table_flushes

  data_tlb_page_table_flushes=$(sed -n 's/.*data TLB page-table flushes = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$data_tlb_page_table_flushes" ]; then
    echo "Failed to find data TLB page-table flush stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$data_tlb_page_table_flushes" -le 0 ]; then
    echo "Expected positive data TLB page-table flush count for $test_name, got $data_tlb_page_table_flushes" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_inline_paged_loads() {
  local log=$1
  local test_name=$2
  local inline_paged_loads

  inline_paged_loads=$(sed -n 's/.*inline paged loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$inline_paged_loads" ]; then
    echo "Failed to find inline paged load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$inline_paged_loads" -le 0 ]; then
    echo "Expected positive inline paged load count for $test_name, got $inline_paged_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_inline_paged_stores() {
  local log=$1
  local test_name=$2
  local inline_paged_stores

  inline_paged_stores=$(sed -n 's/.*inline paged stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$inline_paged_stores" ]; then
    echo "Failed to find inline paged store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$inline_paged_stores" -le 0 ]; then
    echo "Expected positive inline paged store count for $test_name, got $inline_paged_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_inline_paged_load_hits() {
  local log=$1
  local test_name=$2
  local inline_paged_load_hits

  inline_paged_load_hits=$(sed -n 's/.*inline paged load hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$inline_paged_load_hits" ]; then
    echo "Failed to find inline paged load hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$inline_paged_load_hits" -le 0 ]; then
    echo "Expected positive inline paged load hit count for $test_name, got $inline_paged_load_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_inline_paged_store_hits() {
  local log=$1
  local test_name=$2
  local inline_paged_store_hits

  inline_paged_store_hits=$(sed -n 's/.*inline paged store hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$inline_paged_store_hits" ]; then
    echo "Failed to find inline paged store hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$inline_paged_store_hits" -le 0 ]; then
    echo "Expected positive inline paged store hit count for $test_name, got $inline_paged_store_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_helper_loads() {
  local log=$1
  local test_name=$2
  local helper_loads

  helper_loads=$(sed -n 's/.*helper loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$helper_loads" ]; then
    echo "Failed to find helper load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$helper_loads" -le 0 ]; then
    echo "Expected positive helper load count for $test_name, got $helper_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_helper_stores() {
  local log=$1
  local test_name=$2
  local helper_stores

  helper_stores=$(sed -n 's/.*helper stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$helper_stores" ]; then
    echo "Failed to find helper store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$helper_stores" -le 0 ]; then
    echo "Expected positive helper store count for $test_name, got $helper_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_unsupported_opcode() {
  local log=$1
  local test_name=$2
  local opcode=$3
  local count

  count=$(sed -n "s/.*unsupported opcode $opcode = \\([0-9][0-9]*\\).*/\\1/p" "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find unsupported opcode $opcode stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive unsupported opcode $opcode count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_block_end_reason() {
  local log=$1
  local test_name=$2
  local reason=$3
  local count

  count=$(sed -n "s/.*block end $reason = \\([0-9][0-9]*\\).*/\\1/p" "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find block-end $reason stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive block-end $reason count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_side_exit_reason() {
  local log=$1
  local test_name=$2
  local reason=$3
  local count

  count=$(sed -n "s/.*side exit $reason = \\([0-9][0-9]*\\).*/\\1/p" "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find side-exit $reason stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive side-exit $reason count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_direct_link_stats() {
  local log=$1
  local test_name=$2

  if ! grep -q 'direct links taken = [0-9][0-9]*, misses = [0-9][0-9]*' "$log"; then
    echo "Failed to find direct-link stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi
}

require_positive_direct_links() {
  local log=$1
  local test_name=$2
  local count

  count=$(sed -n 's/.*direct links taken = \([0-9][0-9]*\), misses = [0-9][0-9]*.*/\1/p' "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find direct-link taken stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive direct-link taken count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 2
  fi
}

require_positive_direct_branch_links() {
  local log=$1
  local test_name=$2
  local count

  count=$(sed -n 's/.*direct branch links taken = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find direct branch-link stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive direct branch-link taken count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 2
  fi
}

require_positive_guarded_direct_links() {
  local log=$1
  local test_name=$2
  local count

  count=$(sed -n 's/.*direct guarded links taken = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$count" ]; then
    echo "Failed to find guarded direct-link stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -le 0 ]; then
    echo "Expected positive guarded direct-link count for $test_name, got $count" >&2
    cat "$log" >&2
    exit 2
  fi
}

cd "$ROOT"

[ -f "$DEFAULT_DEFCONFIG" ] || fail "missing $DEFAULT_DEFCONFIG"
[ -f "$DEFCONFIG" ] || fail "missing $DEFCONFIG"
make -C "$NEMU_HOME" riscv64-am-headless-jit-stats_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  trap 'rm -f "$out"' EXIT

  if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_positive_jit_instructions "$out" "$test_name"
  if [ "$test_name" = "riscv64-jit-load-fast" ]; then
    require_positive_native_loads "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-store-fast" ]; then
    require_positive_native_stores "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-negative-cache" ]; then
    require_positive_invalidated_blocks "$out" "$test_name"
    require_positive_source_reverse_invalidations "$out" "$test_name"
    require_positive_unsupported_opcode "$out" "$test_name" "0x0f"
    require_direct_link_stats "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-jump-fast" ]; then
    require_positive_native_jumps "$out" "$test_name"
    require_positive_block_end_reason "$out" "$test_name" "jump"
  fi
  if [ "$test_name" = "riscv64-jit-direct-link" ]; then
    require_positive_native_jumps "$out" "$test_name"
    require_positive_direct_links "$out" "$test_name"
    require_positive_direct_branch_links "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-trace" ]; then
    require_positive_trace_blocks "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-m-fast" ]; then
    require_positive_native_m_ops "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-sv39-remap" ]; then
    require_positive_translated_blocks "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-sv39-cross-page" ]; then
    require_positive_translated_blocks "$out" "$test_name"
    require_positive_translated_cross_page_blocks "$out" "$test_name"
    require_positive_segmented_source_blocks "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-mprv-ifetch" ]; then
    require_positive_translated_blocks "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-reg-cache" ]; then
    require_positive_reg_cache_spills "$out" "$test_name"
    require_reg_cache_spills_at_most "$out" "$test_name" 11
  fi
  if [ "$test_name" = "riscv64-jit-memory-entry" ]; then
    require_positive_native_loads "$out" "$test_name"
    require_positive_native_stores "$out" "$test_name"
    require_positive_store_continuations "$out" "$test_name"
    require_positive_zero_side_exits "$out" "$test_name"
    require_positive_helper_stores "$out" "$test_name"
    require_positive_side_exit_reason "$out" "$test_name" "load-guard"
    require_positive_side_exit_reason "$out" "$test_name" "store-guard"
    require_positive_side_exit_reason "$out" "$test_name" "store-source"
  fi
  if [ "$test_name" = "riscv64-jit-sv39-data" ]; then
    require_positive_translated_blocks "$out" "$test_name"
    require_positive_native_paged_loads "$out" "$test_name"
    require_positive_native_paged_stores "$out" "$test_name"
    require_positive_guarded_direct_links "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-sv39-dtlb" ]; then
    require_positive_translated_blocks "$out" "$test_name"
    require_positive_ifetch_generation_fast_hits "$out" "$test_name"
    require_positive_native_paged_loads "$out" "$test_name"
    require_positive_native_paged_stores "$out" "$test_name"
    require_positive_helper_loads "$out" "$test_name"
    require_positive_helper_stores "$out" "$test_name"
    require_positive_data_tlb_hits "$out" "$test_name"
    require_positive_data_tlb_fills "$out" "$test_name"
    require_positive_data_tlb_flushes "$out" "$test_name"
    require_positive_data_tlb_page_table_flushes "$out" "$test_name"
    require_positive_inline_paged_loads "$out" "$test_name"
    require_positive_inline_paged_stores "$out" "$test_name"
    require_positive_inline_paged_load_hits "$out" "$test_name"
    require_positive_inline_paged_store_hits "$out" "$test_name"
    require_positive_side_exit_reason "$out" "$test_name" "paged-store-helper"
    require_positive_invalidated_blocks "$out" "$test_name"
  fi
  rm -f "$out"
  trap - EXIT
done

make -C "$NEMU_HOME" riscv64-am-headless-jit_defconfig >/dev/null
echo "RISC-V64 JIT correctness gate passed: ${TESTS[*]}"
