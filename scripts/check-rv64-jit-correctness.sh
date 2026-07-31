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
RV64_FPU_JIT_TEST=riscv64-fpu-jit-fallback
RV64_FPU_TRAP_TEST=riscv64-fpu-traps
RV64_FPU_EXACT_TEST=riscv64-fpu-transfer
RV64_FPU_MEMORY_NATIVE_TEST=riscv64-fpu-memory-native
RV64_FPU_MMIO_BOUNDARY_TEST=riscv64-fpu-mmio-boundary
RV64_MMIO_BOUNDARY_TEST=riscv64-jit-mmio-boundary
RV64_MMIO_STORE_BOUNDARY_TEST=riscv64-jit-mmio-store-boundary
TESTS=(
  riscv64-jit-stable-loop riscv64-jit-multibranch-loop
  "$RV64_FPU_JIT_TEST" "$RV64_FPU_TRAP_TEST" "$RV64_FPU_EXACT_TEST"
  "$RV64_FPU_MEMORY_NATIVE_TEST"
  "$RV64_FPU_MMIO_BOUNDARY_TEST"
  riscv64-jit-strict
  riscv64-jit-smc riscv64-jit-negative-cache
  riscv64-jit-load-fast riscv64-jit-store-fast riscv64-jit-jump-fast
  riscv64-jit-return-link riscv64-jit-indirect-link
  riscv64-jit-direct-link riscv64-jit-trace
  riscv64-jit-m-fast riscv64-jit-sv39-remap riscv64-jit-sv39-cross-page riscv64-jit-mprv-ifetch
  riscv64-jit-reg-cache riscv64-jit-memory-entry riscv64-jit-sv39-data riscv64-jit-sv39-dtlb
)

fail() {
  echo "RISC-V64 JIT correctness check failed: $*" >&2
  exit 1
}

require_mmio_cross_map_rejection() {
  local test_name=$1
  local access_kind=$2
  local out
  local run_status

  out=$(mktemp)
  trap 'rm -f "$out"' EXIT

  # This is an expected host-side rejection rather than a successful guest
  # trap. The CPU-test wrapper records an inner emulator failure in its output
  # and may itself return success, so check the precise diagnostic as well as
  # ensuring that the guest never reached HIT GOOD TRAP.
  if NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests \
      ARCH="$ARCH" ALL="$test_name" run >"$out" 2>&1; then
    run_status=0
  else
    run_status=$?
  fi

  if grep -q \
      "I/O $access_kind access at 0xa0000220 with len=8 is out of bound {audio}" \
      "$out" &&
      ! grep -q 'HIT GOOD TRAP' "$out"; then
    rm -f "$out"
    trap - EXIT
    return
  fi

  echo "Expected a whole-span MMIO $access_kind rejection for" \
    "$test_name; wrapper status=$run_status" >&2
  cat "$out" >&2
  exit 1
}

require_good_trap() {
  local log=$1
  local test_name=$2
  local good_trap_count
  local bad_trap_count
  local stats_header_count

  # A normal process exit is not proof that the CPU test reached its
  # architectural success trap. Require one complete execution and one report;
  # otherwise a duplicated run could hide an earlier failure behind plausible
  # statistics from the final run.
  good_trap_count=$(grep -c 'HIT GOOD TRAP' "$log" || true)
  bad_trap_count=$(grep -c 'HIT BAD TRAP' "$log" || true)
  stats_header_count=$(grep -c 'jit: RV64 JIT statistics' "$log" || true)

  if [ "$good_trap_count" -ne 1 ] ||
    [ "$bad_trap_count" -ne 0 ] ||
    [ "$stats_header_count" -ne 1 ]; then
    echo "Expected one unambiguous successful execution for $test_name;" \
      "got good-traps=$good_trap_count bad-traps=$bad_trap_count" \
      "statistics-reports=$stats_header_count" >&2
    cat "$log" >&2
    exit 1
  fi
}

parse_single_stat_value() {
  local log=$1
  local expression=$2
  local label=$3
  local test_name=$4
  local -a values

  mapfile -t values < <(sed -n "$expression" "$log")
  if [ "${#values[@]}" -ne 1 ]; then
    echo "Expected exactly one $label for $test_name, got ${#values[@]}" >&2
    cat "$log" >&2
    exit 2
  fi

  printf "%s\n" "${values[0]}"
}

require_count_expectation() {
  local actual=$1
  local expectation=$2
  local counter_name=$3
  local test_name=$4
  local log=$5

  case "$expectation" in
    any)
      return
      ;;
    positive)
      if [ "$actual" -gt 0 ]; then
        return
      fi
      ;;
    zero)
      if [ "$actual" -eq 0 ]; then
        return
      fi
      ;;
    *)
      if [[ "$expectation" =~ ^[0-9]+$ ]] && [ "$actual" -eq "$expectation" ]; then
        return
      fi
      ;;
  esac

  echo "Expected FP $counter_name for $test_name to be $expectation, got $actual" >&2
  cat "$log" >&2
  exit 1
}

require_fp_helper_stats() {
  local log=$1
  local test_name=$2
  local expected_sites=$3
  local expected_calls=$4
  local expected_continuations=$5
  local expected_trap_exits=$6
  local expected_memory_exits=$7
  local summary
  local sites
  local calls
  local continuations
  local trap_exits
  local memory_exits
  local classified_calls

  summary=$(sed -n \
    's/.*jit: FP helper sites = \([0-9][0-9]*\), calls = \([0-9][0-9]*\), continuations = \([0-9][0-9]*\), trap exits = \([0-9][0-9]*\), memory exits = \([0-9][0-9]*\).*/\1 \2 \3 \4 \5/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find exact FP helper stats summary for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r sites calls continuations trap_exits memory_exits <<<"$summary"

  classified_calls=$((continuations + trap_exits + memory_exits))
  if [ "$calls" -ne "$classified_calls" ]; then
    echo "FP helper calls for $test_name were not fully classified: calls=$calls" \
      "continuations=$continuations trap_exits=$trap_exits memory_exits=$memory_exits" >&2
    cat "$log" >&2
    exit 1
  fi

  # Exact integer expectations are the hook for a focused guest whose stable
  # instruction mix covers all seven FP major-opcode families. Aggregate
  # positive counts alone cannot prove per-family coverage.
  require_count_expectation "$sites" "$expected_sites" "helper sites" "$test_name" "$log"
  require_count_expectation "$calls" "$expected_calls" "helper calls" "$test_name" "$log"
  require_count_expectation "$continuations" "$expected_continuations" "helper continuations" "$test_name" "$log"
  require_count_expectation "$trap_exits" "$expected_trap_exits" "helper trap exits" "$test_name" "$log"
  require_count_expectation "$memory_exits" "$expected_memory_exits" "helper memory exits" "$test_name" "$log"
}

require_all_native_fp_exact_executions() {
  local log=$1
  local test_name=$2
  local expected_total=$3
  local operation
  local count
  local total=0
  local operations=(
    FMV.X.W FMV.W.X FMV.X.D FMV.D.X
    FSGNJ.S FSGNJN.S FSGNJX.S
    FSGNJ.D FSGNJN.D FSGNJX.D
    FCLASS.S FCLASS.D
  )

  for operation in "${operations[@]}"; do
    count=$(sed -n \
      "s/.*native exact FP ${operation} executions = \\([0-9][0-9]*\\).*/\\1/p" \
      "$log" | tail -n 1)

    if [ -z "$count" ]; then
      echo "Failed to find native exact FP $operation execution stats for $test_name" >&2
      cat "$log" >&2
      exit 2
    fi

    if [ "$count" -le 0 ]; then
      echo "Expected native exact FP $operation execution for $test_name, got $count" >&2
      cat "$log" >&2
      exit 2
    fi

    total=$((total + count))
  done

  if [ "$total" -ne "$expected_total" ]; then
    echo "Expected exactly $expected_total native exact-FP executions for" \
      "$test_name, got $total" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_all_native_fp_memory_executions() {
  local log=$1
  local test_name=$2
  local expected_flw=$3
  local expected_fld=$4
  local expected_fsw=$5
  local expected_fsd=$6
  local operation
  local expected
  local count

  for operation in FLW FLD FSW FSD; do
    case "$operation" in
      FLW) expected=$expected_flw ;;
      FLD) expected=$expected_fld ;;
      FSW) expected=$expected_fsw ;;
      FSD) expected=$expected_fsd ;;
    esac

    count=$(sed -n \
      "s/.*native FP memory ${operation} executions = \\([0-9][0-9]*\\).*/\\1/p" \
      "$log" | tail -n 1)

    if [ -z "$count" ]; then
      echo "Failed to find native FP memory $operation execution stats for $test_name" >&2
      cat "$log" >&2
      exit 2
    fi

    if [ "$count" -ne "$expected" ]; then
      echo "Expected exactly $expected native FP memory $operation executions" \
        "for $test_name, got $count" >&2
      cat "$log" >&2
      exit 1
    fi
  done
}

require_absent_block_end_reason() {
  local log=$1
  local test_name=$2
  local reason=$3
  local count

  count=$(sed -n \
    "s/.*block end $reason = \\([0-9][0-9]*\\).*/\\1/p" \
    "$log" | tail -n 1)

  if [ -n "$count" ] && [ "$count" -ne 0 ]; then
    echo "Expected no $reason block endings for $test_name, got $count" >&2
    cat "$log" >&2
    exit 1
  fi
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

require_all_native_m_executions() {
  local log=$1
  local test_name=$2
  local op
  local count
  local operations=(
    MUL MULH MULHSU MULHU DIV DIVU REM REMU
    MULW DIVW DIVUW REMW REMUW
  )

  for op in "${operations[@]}"; do
    count=$(sed -n \
      "s/.*native M ${op} executions = \\([0-9][0-9]*\\).*/\\1/p" \
      "$log" | tail -n 1)

    if [ -z "$count" ]; then
      echo "Failed to find native M ${op} execution stats for $test_name" >&2
      cat "$log" >&2
      exit 2
    fi

    if [ "$count" -le 0 ]; then
      echo "Expected positive native M ${op} executions for $test_name," \
        "got $count" >&2
      cat "$log" >&2
      exit 1
    fi
  done
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

require_positive_stable_register_loops() {
  local log=$1
  local test_name=$2
  local summary
  local stable_loops
  local preloaded_registers

  summary=$(sed -n \
    's/.*stable register loops = \([0-9][0-9]*\), preloaded registers = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find stable-register-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r stable_loops preloaded_registers <<<"$summary"

  if [ "$stable_loops" -lt 2 ] ||
    [ "$preloaded_registers" -ne $((stable_loops * 7)) ]; then
    echo "Expected at least two seven-register stable loops for $test_name," \
      "got loops=$stable_loops" \
      "preloaded=$preloaded_registers" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_native_fp_stable_loop() {
  local log=$1
  local test_name=$2
  local summary
  local stable_loops
  local preloaded_registers

  summary=$(sed -n \
    's/.*stable register loops = \([0-9][0-9]*\), preloaded registers = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find FP stable-register-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r stable_loops preloaded_registers <<<"$summary"

  # This focused binary has exactly one backedge which the stable-loop scanner
  # may accept, and that exact-FP loop carries four GPRs. Pinning both values
  # prevents an unrelated loop from masking loss of its stable register map.
  if [ "$stable_loops" -ne 1 ] ||
    [ "$preloaded_registers" -ne 4 ]; then
    echo "Expected exactly one four-register native exact-FP stable loop for $test_name," \
      "got loops=$stable_loops" \
      "preloaded=$preloaded_registers" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_native_m_stable_loop() {
  local log=$1
  local test_name=$2
  local summary
  local stable_loops
  local preloaded_registers

  summary=$(sed -n \
    's/.*stable register loops = \([0-9][0-9]*\), preloaded registers = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find M stable-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r stable_loops preloaded_registers <<<"$summary"

  # The focused guest contains exactly one six-register self-backedge. Before
  # all M operations became helper-free, that same loop compiled without a
  # stable mapping and both counters were zero.
  if [ "$stable_loops" -ne 1 ] || [ "$preloaded_registers" -ne 6 ]; then
    echo "Expected one six-register native M stable loop for $test_name," \
      "got loops=$stable_loops preloaded=$preloaded_registers" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_zero_stable_register_loops() {
  local log=$1
  local test_name=$2
  local summary
  local stable_loops
  local preloaded_registers

  summary=$(sed -n \
    's/.*stable register loops = \([0-9][0-9]*\), preloaded registers = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find stable-register-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r stable_loops preloaded_registers <<<"$summary"

  if [ "$stable_loops" -ne 0 ] || [ "$preloaded_registers" -ne 0 ]; then
    echo "Expected the multi-branch loop to reject stable mapping," \
      "got loops=$stable_loops preloaded=$preloaded_registers" >&2
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

require_mmio_store_continuation_stats() {
  local log=$1
  local test_name=$2
  local summary
  local calls
  local continuations
  local boundary_exits

  summary=$(parse_single_stat_value \
    "$log" \
    's/.*bare MMIO store calls = \([0-9][0-9]*\), continuations = \([0-9][0-9]*\), boundary exits = \([0-9][0-9]*\).*/\1 \2 \3/p' \
    "bare-MMIO store summary" "$test_name")

  read -r calls continuations boundary_exits <<<"$summary"

  # The focused fixture performs one helper-backed eight-byte VGACTL staging
  # write, two adjacent-word writes, two command writes, one dynamically
  # recompiled unsupported-width SD, sixteen RTC writes, and two serial writes.
  # Every helper call must have exactly one outcome.
  if [ "$calls" -ne 24 ] ||
    [ "$continuations" -le 0 ] ||
    [ "$boundary_exits" -lt 2 ] ||
    [ "$calls" -ne $((continuations + boundary_exits)) ]; then
    echo "Expected repeated, positive, and balanced bare-MMIO store stats for $test_name," \
      "got calls=$calls continuations=$continuations" \
      "boundary-exits=$boundary_exits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_direct_mmio_store_routing_stats() {
  local log=$1
  local test_name=$2
  local direct_hits

  direct_hits=$(parse_single_stat_value \
    "$log" \
    's/.*inline direct MMIO store hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO store total" "$test_name")

  # The original site contributes one direct SW. The dedicated route site adds
  # seven committed SW accesses; its PMEM, command, misaligned, and SD cases do
  # not qualify for the direct-write contract.
  if [ "$direct_hits" -ne 8 ]; then
    echo "Expected exactly eight direct MMIO stores for $test_name, got $direct_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_bare_mmio_load_routing_stats() {
  local log=$1
  local test_name=$2
  local calls
  local direct_hits

  calls=$(parse_single_stat_value \
    "$log" \
    's/.*bare MMIO load calls = \([0-9][0-9]*\).*/\1/p' \
    "bare-MMIO load total" "$test_name")
  direct_hits=$(parse_single_stat_value \
    "$log" \
    's/.*inline direct MMIO load hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO load total" "$test_name")

  # RTC and two destructive mouse reads remain callback-driven, so exactly
  # those three accesses reach paddr_read().
  if [ "$calls" -ne 3 ]; then
    echo "Expected exactly three helper-backed MMIO loads for $test_name, got $calls" >&2
    cat "$log" >&2
    exit 1
  fi

  # The initial-reset assertion adds one uncached direct access. The route
  # fixture also switches between two contracted direct addresses, forcing two
  # run-time refills after its compile-time seed.
  if [ "$direct_hits" -ne 26 ]; then
    echo "Expected exactly twenty-six inline direct-MMIO loads for $test_name, got $direct_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_direct_mmio_route_cache_stats() {
  local log=$1
  local test_name=$2
  local load_summary
  local store_summary
  local direct_loads
  local direct_stores
  local load_hits
  local load_misses
  local load_fills
  local store_hits
  local store_misses
  local store_fills

  load_summary=$(parse_single_stat_value \
    "$log" \
    's/.*direct MMIO load routes: warm hits = \([0-9][0-9]*\), misses = \([0-9][0-9]*\), fills = \([0-9][0-9]*\).*/\1 \2 \3/p' \
    "direct-MMIO load-route summary" "$test_name")
  store_summary=$(parse_single_stat_value \
    "$log" \
    's/.*direct MMIO store routes: warm hits = \([0-9][0-9]*\), misses = \([0-9][0-9]*\), fills = \([0-9][0-9]*\).*/\1 \2 \3/p' \
    "direct-MMIO store-route summary" "$test_name")
  direct_loads=$(parse_single_stat_value \
    "$log" \
    's/.*inline direct MMIO load hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO load total" "$test_name")
  direct_stores=$(parse_single_stat_value \
    "$log" \
    's/.*inline direct MMIO store hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO store total" "$test_name")

  read -r load_hits load_misses load_fills <<<"$load_summary"
  read -r store_hits store_misses store_fills <<<"$store_summary"

  # Every selected route starts with a valid exact compile-time seed. The
  # forced direct-address change performs two refills, while the reset-state
  # read and eight other direct operations retain the uncached classifier.
  if [ "$load_hits" -ne 15 ] ||
    [ "$load_misses" -ne 6 ] ||
    [ "$load_fills" -ne 2 ] ||
    [ "$direct_loads" -ne $((load_hits + load_fills + 9)) ]; then
    echo "Expected load routes hits=15 misses=6 fills=2 and nine uncached directs" \
      "for $test_name; got hits=$load_hits misses=$load_misses" \
      "fills=$load_fills direct=$direct_loads" >&2
    cat "$log" >&2
    exit 1
  fi

  if [ "$store_hits" -ne 8 ] ||
    [ "$store_misses" -ne 5 ] ||
    [ "$store_fills" -ne 0 ] ||
    [ "$direct_stores" -ne $((store_hits + store_fills)) ]; then
    echo "Expected store routes hits=8 misses=5 fills=0 and direct=fills+hits" \
      "for $test_name; got hits=$store_hits misses=$store_misses" \
      "fills=$store_fills direct=$direct_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_cpu_boundary_breaks() {
  local log=$1
  local test_name=$2
  local expectation=$3
  local boundary_breaks

  boundary_breaks=$(sed -n \
    's/.*CPU boundary breaks = \([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)

  if [ -z "$boundary_breaks" ]; then
    echo "Failed to find CPU-boundary-break stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  require_count_expectation \
    "$boundary_breaks" "$expectation" "CPU boundary breaks" \
    "$test_name" "$log"
}

require_exact_serial_mmio_marker() {
  local log=$1
  local test_name=$2
  local marker_count

  marker_count=$(
    awk '{
      line = $0
      sub(/\r$/, "", line)
      if (line == "~") {
        count++
      }
    }
    END {
      print count + 0
    }' "$log"
  )

  if [ "$marker_count" -ne 1 ]; then
    echo "Expected exactly one serial MMIO marker for $test_name," \
      "got $marker_count" >&2
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

require_exact_side_exit_reason() {
  local log=$1
  local test_name=$2
  local reason=$3
  local expected=$4
  local count

  count=$(sed -n \
    "s/.*side exit $reason = \\([0-9][0-9]*\\).*/\\1/p" \
    "$log" | tail -n 1)

  if [ -z "$count" ]; then
    echo "Failed to find side-exit $reason stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$count" -ne "$expected" ]; then
    echo "Expected side-exit $reason count $expected for $test_name, got $count" >&2
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

require_positive_direct_return_links() {
  local log=$1
  local test_name=$2
  local minimum_taken=4000
  local summary
  local taken
  local misses
  local attempts

  summary=$(sed -n \
    's/.*direct return links taken = \([0-9][0-9]*\), misses = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find direct return-link stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r taken misses <<<"$summary"
  attempts=$((taken + misses))

  # The focused guest executes 4,096 alternating returns. A substantial floor
  # prevents unrelated startup or trap-handler returns from satisfying this
  # check, while the ratio proves that the hot site normally stays linked.
  if [ "$taken" -lt "$minimum_taken" ] ||
    [ "$misses" -le 0 ] ||
    [ $((taken * 100)) -lt $((attempts * 99)) ]; then
    echo "Expected at least $minimum_taken direct return-link hits, positive" \
      "misses, and a 99% hit rate for $test_name; got taken=$taken" \
      "misses=$misses" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_direct_jalr_links() {
  local log=$1
  local test_name=$2
  local minimum_taken=7000
  local summary
  local taken
  local misses
  local attempts

  summary=$(sed -n \
    's/.*direct JALR links taken = \([0-9][0-9]*\), misses = \([0-9][0-9]*\).*/\1 \2/p' \
    "$log" | tail -n 1)

  if [ -z "$summary" ]; then
    echo "Failed to find direct-JALR-link stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r taken misses <<<"$summary"
  attempts=$((taken + misses))

  # The focused guest executes 8,192 eligible non-canonical JALRs. Requiring
  # 7,000 hits prevents only the alternating call or only its returns from
  # satisfying the test; misses remain non-exact because cold targets and
  # legitimate direct-map collisions can add more.
  if [ "$taken" -lt "$minimum_taken" ] ||
    [ "$misses" -lt 2 ] ||
    [ $((taken * 100)) -lt $((attempts * 99)) ]; then
    echo "Expected at least $minimum_taken direct JALR-link hits, at least two" \
      "misses, and a 99% hit rate for $test_name; got taken=$taken" \
      "misses=$misses" >&2
    cat "$log" >&2
    exit 1
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
bash "$SCRIPT_DIR/check-rv64-new-interpreter.sh"
make -C "$NEMU_HOME" riscv64-am-headless-jit-stats_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  trap 'rm -f "$out"' EXIT

  run_env=(NEMU_JIT_STATS=1)
  if [ "$test_name" = "riscv64-jit-memory-entry" ]; then
    # The statistics-only hook makes the first two bare MMIO stores exercise
    # instruction-generation and outer CPU boundaries deterministically.
    run_env+=(NEMU_RV64_JIT_TEST_MMIO_BOUNDARIES=1)
    # Poison allocator storage so the initial VGACTL read proves that device
    # reset state comes from explicit initialisation rather than fresh pages.
    run_env+=(MALLOC_PERTURB_=165)
  fi
  if [ "$test_name" = "$RV64_FPU_MMIO_BOUNDARY_TEST" ]; then
    # The statistics-only hook makes the successful FP MMIO helper raise one
    # deterministic interrupt edge after its device callback has returned.
    run_env+=(NEMU_RV64_JIT_TEST_FP_MMIO_BOUNDARY=1)
  fi

  if ! env "${run_env[@]}" make -C am-kernels/tests/cpu-tests ARCH="$ARCH" ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_good_trap "$out" "$test_name"
  require_positive_jit_instructions "$out" "$test_name"

  if [ "$test_name" = "riscv64-jit-stable-loop" ]; then
    require_positive_stable_register_loops "$out" "$test_name"
    require_positive_side_exit_reason \
      "$out" "$test_name" "chained-over-budget"
  fi

  if [ "$test_name" = "riscv64-jit-multibranch-loop" ]; then
    require_zero_stable_register_loops "$out" "$test_name"
  fi

  if [ "$test_name" = "$RV64_FPU_JIT_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 16 16 zero zero
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 0 6 0 10
    require_positive_source_reverse_invalidations "$out" "$test_name"
    require_positive_side_exit_reason \
      "$out" "$test_name" "store-source"
  fi

  if [ "$test_name" = "$RV64_FPU_TRAP_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 10 zero 10 zero
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 1 1 1 1
    require_exact_side_exit_reason \
      "$out" "$test_name" "fp-fs-off" 23
    require_exact_side_exit_reason \
      "$out" "$test_name" "load-guard" 9
    require_exact_side_exit_reason \
      "$out" "$test_name" "store-guard" 9
  fi

  if [ "$test_name" = "$RV64_FPU_EXACT_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 14 14 zero zero
    # The state-effect check includes one FMV.X.W readback after changing the
    # self-aliased FSGNJN.S destination, making 472 exact native operations.
    require_all_native_fp_exact_executions "$out" "$test_name" 472
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 0 0 1 0
    require_native_fp_stable_loop "$out" "$test_name"
  fi

  if [ "$test_name" = "$RV64_FPU_MEMORY_NATIVE_TEST" ]; then
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 66 66 66 66
    require_fp_helper_stats "$out" "$test_name" positive zero zero zero zero
    require_absent_block_end_reason "$out" "$test_name" "fp-memory"
    require_positive_block_end_reason "$out" "$test_name" "chained-loop"
  fi

  if [ "$test_name" = "$RV64_FPU_MMIO_BOUNDARY_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 4 zero zero 4
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 0 0 0 0
    require_cpu_boundary_breaks "$out" "$test_name" 1
  fi

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

  if [ "$test_name" = "riscv64-jit-return-link" ]; then
    require_positive_native_jumps "$out" "$test_name"
    require_positive_direct_return_links "$out" "$test_name"
  fi

  if [ "$test_name" = "riscv64-jit-indirect-link" ]; then
    require_positive_native_jumps "$out" "$test_name"
    require_positive_direct_jalr_links "$out" "$test_name"
    require_positive_side_exit_reason \
      "$out" "$test_name" "jalr-misaligned"
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
    require_all_native_m_executions "$out" "$test_name"
    require_native_m_stable_loop "$out" "$test_name"
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
    require_reg_cache_spills_at_most "$out" "$test_name" 12
  fi

  if [ "$test_name" = "riscv64-jit-memory-entry" ]; then
    require_positive_native_loads "$out" "$test_name"
    require_positive_native_stores "$out" "$test_name"
    require_positive_store_continuations "$out" "$test_name"
    require_positive_zero_side_exits "$out" "$test_name"
    require_positive_helper_loads "$out" "$test_name"
    require_positive_helper_stores "$out" "$test_name"
    require_bare_mmio_load_routing_stats "$out" "$test_name"
    require_mmio_store_continuation_stats "$out" "$test_name"
    require_direct_mmio_store_routing_stats "$out" "$test_name"
    require_direct_mmio_route_cache_stats "$out" "$test_name"
    require_cpu_boundary_breaks "$out" "$test_name" positive
    require_exact_serial_mmio_marker "$out" "$test_name"
    require_positive_side_exit_reason "$out" "$test_name" "load-guard"
    require_positive_side_exit_reason "$out" "$test_name" "store-source"
  fi

  if [ "$test_name" = "riscv64-jit-sv39-data" ]; then
    require_positive_translated_blocks "$out" "$test_name"
    require_positive_native_paged_loads "$out" "$test_name"
    require_positive_native_paged_stores "$out" "$test_name"
    require_all_native_fp_memory_executions \
      "$out" "$test_name" 131 129 129 129
    require_fp_helper_stats "$out" "$test_name" zero zero zero zero zero
    require_positive_inline_paged_load_hits "$out" "$test_name"
    require_positive_inline_paged_store_hits "$out" "$test_name"
    require_positive_guarded_direct_links "$out" "$test_name"
    require_exact_side_exit_reason \
      "$out" "$test_name" "load-guard" 2
    require_exact_side_exit_reason \
      "$out" "$test_name" "store-source" 3
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

require_mmio_cross_map_rejection "$RV64_MMIO_BOUNDARY_TEST" load
require_mmio_cross_map_rejection \
  "$RV64_MMIO_STORE_BOUNDARY_TEST" store

make -C "$NEMU_HOME" riscv64-am-headless-jit_defconfig >/dev/null
echo "RISC-V64 JIT correctness gate passed: ${TESTS[*]}"
