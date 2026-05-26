#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=x86
export ARCH=x86-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

DEFCONFIG="$NEMU_HOME/configs/x86-am-jit_defconfig"
TESTS=(add sum fib mov-c bit movsx shift rotate incdec byte-div mul-longlong jit-alu-jcc-fusion jit-call-ret jit-direct-load jit-direct-store jit-incdec-reg jit-incdec-loop jit-imul jit-indirect-jmp jit-mem-alu jit-moffs jit-movzx jit-mul-div-native jit-not jit-remaining-helpers jit-shift-native jit-signed-jcc jit-stack-reg jit-word-ops)

tmp_files=()

cleanup() {
  rm -f "${tmp_files[@]}"
}

trap cleanup EXIT

fail() {
  echo "x86 JIT smoke check failed: $*" >&2
  exit 1
}

required_jit_instructions() {
  case "$1" in
    bit) echo 400 ;;
    byte-div) echo 20 ;;
    fib) echo 700 ;;
    incdec) echo 80 ;;
    jit-alu-jcc-fusion) echo 10000 ;;
    jit-call-ret) echo 100 ;;
    jit-direct-load) echo 50 ;;
    jit-direct-store) echo 50 ;;
    jit-incdec-reg) echo 100 ;;
    jit-incdec-loop) echo 10000 ;;
    jit-indirect-jmp) echo 10000 ;;
    jit-mem-alu) echo 20 ;;
    jit-moffs) echo 20 ;;
    jit-movzx) echo 10000 ;;
    jit-mul-div-native) echo 10000 ;;
    jit-not) echo 10000 ;;
    jit-remaining-helpers) echo 20 ;;
    jit-shift-native) echo 20 ;;
    jit-signed-jcc) echo 20 ;;
    jit-stack-reg) echo 20 ;;
    jit-word-ops) echo 12000 ;;
    mov-c) echo 100 ;;
    movsx) echo 150 ;;
    jit-imul) echo 10000 ;;
    mul-longlong) echo 100 ;;
    rotate) echo 20 ;;
    shift) echo 300 ;;
    sum) echo 250 ;;
    *) echo 1 ;;
  esac
}

required_max_unsupported_hits() {
  case "$1" in
    jit-indirect-jmp) echo 1000 ;;
    jit-alu-jcc-fusion) echo 20 ;;
    jit-imul) echo 100 ;;
    jit-movzx) echo 100 ;;
    jit-mul-div-native) echo 100 ;;
    jit-not) echo 100 ;;
    jit-incdec-reg) echo 20 ;;
    jit-incdec-loop) echo 10 ;;
    incdec) echo 50 ;;
    jit-direct-load) echo 20 ;;
    jit-direct-store) echo 20 ;;
    byte-div) echo 50 ;;
    jit-word-ops) echo 1000 ;;
    movsx) echo 20 ;;
    mul-longlong) echo 100 ;;
    rotate) echo 20 ;;
    shift) echo 20 ;;
    *) echo "" ;;
  esac
}

required_max_executed_blocks() {
  case "$1" in
    jit-word-ops) echo 1000 ;;
    *) echo "" ;;
  esac
}

required_min_native_alu_ops() {
  case "$1" in
    sum) echo 5 ;;
    *) echo "" ;;
  esac
}

required_min_native_incdec_ops() {
  case "$1" in
    jit-incdec-reg) echo 2 ;;
    jit-incdec-loop) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_incdec_jcc_backedges() {
  case "$1" in
    jit-incdec-loop) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_incdec_resident_loops() {
  case "$1" in
    jit-incdec-loop) echo 5 ;;
    *) echo "" ;;
  esac
}

required_max_helper_incdec_reg_calls() {
  case "$1" in
    jit-incdec-reg) echo 0 ;;
    jit-incdec-loop) echo 0 ;;
    *) echo "" ;;
  esac
}

required_min_native_pmem_loads() {
  case "$1" in
    jit-direct-load) echo 1 ;;
    jit-moffs) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_pmem_stores() {
  case "$1" in
    jit-direct-store) echo 1 ;;
    jit-moffs) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_imul_ops() {
  case "$1" in
    jit-imul) echo 1 ;;
    jit-mul-div-native) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_mul_ops() {
  case "$1" in
    jit-mul-div-native) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_div_ops() {
  case "$1" in
    jit-mul-div-native) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_alu_jcc_fusions() {
  case "$1" in
    jit-alu-jcc-fusion) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_alu_jcc_resident_loops() {
  case "$1" in
    jit-alu-jcc-fusion) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_shift_ops() {
  case "$1" in
    rotate) echo 1 ;;
    shift) echo 1 ;;
    jit-shift-native) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_not_ops() {
  case "$1" in
    jit-not) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_movzx_ops() {
  case "$1" in
    jit-movzx) echo 1 ;;
    *) echo "" ;;
  esac
}

required_max_helper_jcc_rel_calls() {
  case "$1" in
    jit-signed-jcc) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_jmp_rm_calls() {
  case "$1" in
    jit-indirect-jmp) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_moffs_calls() {
  case "$1" in
    jit-moffs) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_call_ret_calls() {
  case "$1" in
    jit-call-ret) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_mem_alu_calls() {
  case "$1" in
    jit-mem-alu) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_remaining_calls() {
  case "$1" in
    jit-remaining-helpers) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_shift_rm_calls() {
  case "$1" in
    jit-shift-native) echo 2 ;;
    *) echo "" ;;
  esac
}

required_max_helper_stack_reg_calls() {
  case "$1" in
    jit-stack-reg) echo 0 ;;
    *) echo "" ;;
  esac
}

required_max_helper_mul_div_calls() {
  case "$1" in
    jit-mul-div-native) echo 0 ;;
    *) echo "" ;;
  esac
}

require_min_jit_instructions() {
  local log=$1
  local test_name=$2
  local min_insns=$3
  local jit_insns

  jit_insns=$(sed -n 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$jit_insns" ]; then
    echo "Failed to find JIT instruction stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$jit_insns" -lt "$min_insns" ]; then
    echo "Expected at least $min_insns JIT instructions for $test_name, got $jit_insns" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_unsupported_hits() {
  local log=$1
  local test_name=$2
  local max_hits=$3
  local unsupported_hits

  [ -n "$max_hits" ] || return 0

  unsupported_hits=$(sed -n 's/.*unsupported hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$unsupported_hits" ]; then
    echo "Failed to find unsupported-hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$unsupported_hits" -gt "$max_hits" ]; then
    echo "Expected at most $max_hits unsupported hits for $test_name, got $unsupported_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_executed_blocks() {
  local log=$1
  local test_name=$2
  local max_blocks=$3
  local executed_blocks

  [ -n "$max_blocks" ] || return 0

  executed_blocks=$(sed -n 's/.*executed blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$executed_blocks" ]; then
    echo "Failed to find executed-block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$executed_blocks" -gt "$max_blocks" ]; then
    echo "Expected at most $max_blocks executed JIT blocks for $test_name, got $executed_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_alu_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native ALU ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native ALU stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native ALU ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_incdec_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native inc\/dec ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native inc/dec stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native inc/dec ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_incdec_jcc_backedges() {
  local log=$1
  local test_name=$2
  local min_edges=$3
  local native_edges

  [ -n "$min_edges" ] || return 0

  native_edges=$(sed -n 's/.*native inc\/dec Jcc backedges = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_edges" ]; then
    echo "Failed to find native inc/dec Jcc backedge stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_edges" -lt "$min_edges" ]; then
    echo "Expected at least $min_edges native inc/dec Jcc backedges for $test_name, got $native_edges" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_incdec_resident_loops() {
  local log=$1
  local test_name=$2
  local min_loops=$3
  local native_loops

  [ -n "$min_loops" ] || return 0

  native_loops=$(sed -n 's/.*native inc\/dec resident loops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_loops" ]; then
    echo "Failed to find native inc/dec resident-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_loops" -lt "$min_loops" ]; then
    echo "Expected at least $min_loops native inc/dec resident loops for $test_name, got $native_loops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_incdec_reg_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local helper_calls

  [ -n "$max_calls" ] || return 0

  helper_calls=$(sed -n 's/.*helper inc\/dec register calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$helper_calls" ]; then
    echo "Failed to find helper register inc/dec stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$helper_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls helper register inc/dec calls for $test_name, got $helper_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_pmem_loads() {
  local log=$1
  local test_name=$2
  local min_loads=$3
  local native_loads

  [ -n "$min_loads" ] || return 0

  native_loads=$(sed -n 's/.*native PMEM loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_loads" ]; then
    echo "Failed to find native PMEM load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_loads" -lt "$min_loads" ]; then
    echo "Expected at least $min_loads native PMEM loads for $test_name, got $native_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_pmem_stores() {
  local log=$1
  local test_name=$2
  local min_stores=$3
  local native_stores

  [ -n "$min_stores" ] || return 0

  native_stores=$(sed -n 's/.*native PMEM stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_stores" ]; then
    echo "Failed to find native PMEM store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_stores" -lt "$min_stores" ]; then
    echo "Expected at least $min_stores native PMEM stores for $test_name, got $native_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_imul_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native imul ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native imul stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native imul ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_mul_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native mul ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native mul stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native mul ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_div_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native div ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native div stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native div ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_alu_jcc_fusions() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native ALU\/Jcc fusions = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native ALU/Jcc fusion stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native ALU/Jcc fusions for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_alu_jcc_resident_loops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native ALU\/Jcc resident loops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native ALU/Jcc resident-loop stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native ALU/Jcc resident loops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_shift_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native shift\/rotate ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native shift/rotate stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native shift/rotate ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_not_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native not ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native not stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native not ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_movzx_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native movzx ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native movzx stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native movzx ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_jcc_rel_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local helper_calls

  [ -n "$max_calls" ] || return 0

  helper_calls=$(sed -n 's/.*helper profile jcc-rel[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$helper_calls" ]; then
    helper_calls=0
  fi

  if [ "$helper_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls Jcc helper calls for $test_name, got $helper_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_jmp_rm_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local helper_calls

  [ -n "$max_calls" ] || return 0

  helper_calls=$(sed -n 's/.*helper profile jmp-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  helper_calls=${helper_calls:-0}

  if [ "$helper_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls indirect-jump helper calls for $test_name, got $helper_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_moffs_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local load_calls
  local store_calls

  [ -n "$max_calls" ] || return 0

  load_calls=$(sed -n 's/.*helper profile mov-eax-moffs[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  store_calls=$(sed -n 's/.*helper profile mov-moffs-eax[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  load_calls=${load_calls:-0}
  store_calls=${store_calls:-0}

  if [ "$load_calls" -gt "$max_calls" ] || [ "$store_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls moffs helper calls for $test_name, got load=$load_calls store=$store_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_call_ret_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local call_calls
  local ret_calls

  [ -n "$max_calls" ] || return 0

  call_calls=$(sed -n 's/.*helper profile call-rel[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  ret_calls=$(sed -n 's/.*helper profile ret[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  call_calls=${call_calls:-0}
  ret_calls=${ret_calls:-0}

  if [ "$call_calls" -gt "$max_calls" ] || [ "$ret_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls call/ret helper calls for $test_name, got call=$call_calls ret=$ret_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_mem_alu_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local alu_rm_reg_calls
  local alu_imm_rm_calls
  local test_rm_reg_calls

  [ -n "$max_calls" ] || return 0

  alu_rm_reg_calls=$(sed -n 's/.*helper profile alu-rm-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  alu_imm_rm_calls=$(sed -n 's/.*helper profile alu-imm-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  test_rm_reg_calls=$(sed -n 's/.*helper profile test-rm-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  alu_rm_reg_calls=${alu_rm_reg_calls:-0}
  alu_imm_rm_calls=${alu_imm_rm_calls:-0}
  test_rm_reg_calls=${test_rm_reg_calls:-0}

  if [ "$alu_rm_reg_calls" -gt 0 ] ||
      [ "$alu_imm_rm_calls" -gt "$max_calls" ] ||
      [ "$test_rm_reg_calls" -gt "$max_calls" ]; then
    echo "Expected memory ALU helper calls within limits for $test_name, got alu-rm-reg=$alu_rm_reg_calls alu-imm-rm=$alu_imm_rm_calls test-rm-reg=$test_rm_reg_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_remaining_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local mov_rm_reg_calls
  local mov_reg_rm_calls
  local mov_imm_rm_calls
  local alu_rm_reg_calls
  local alu_reg_rm_calls
  local neg_rm_calls
  local incdec_rm_calls
  local test_eax_imm_calls
  local test_imm_rm_calls
  local push_imm_calls
  local push_rm_calls
  local leave_calls
  local setcc_calls
  local movsx8_calls
  local movsx16_calls

  [ -n "$max_calls" ] || return 0

  mov_rm_reg_calls=$(sed -n 's/.*helper profile mov-rm-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  mov_reg_rm_calls=$(sed -n 's/.*helper profile mov-reg-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  mov_imm_rm_calls=$(sed -n 's/.*helper profile mov-imm-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  alu_rm_reg_calls=$(sed -n 's/.*helper profile alu-rm-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  alu_reg_rm_calls=$(sed -n 's/.*helper profile alu-reg-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  neg_rm_calls=$(sed -n 's/.*helper profile neg-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  incdec_rm_calls=$(sed -n 's/.*helper profile incdec-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  test_eax_imm_calls=$(sed -n 's/.*helper profile test-eax-imm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  test_imm_rm_calls=$(sed -n 's/.*helper profile test-imm-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  push_imm_calls=$(sed -n 's/.*helper profile push-imm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  push_rm_calls=$(sed -n 's/.*helper profile push-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  leave_calls=$(sed -n 's/.*helper profile leave[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  setcc_calls=$(sed -n 's/.*helper profile setcc-rm8[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  movsx8_calls=$(sed -n 's/.*helper profile movsx-reg-rm8[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  movsx16_calls=$(sed -n 's/.*helper profile movsx-reg-rm16[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  mov_rm_reg_calls=${mov_rm_reg_calls:-0}
  mov_reg_rm_calls=${mov_reg_rm_calls:-0}
  mov_imm_rm_calls=${mov_imm_rm_calls:-0}
  alu_rm_reg_calls=${alu_rm_reg_calls:-0}
  alu_reg_rm_calls=${alu_reg_rm_calls:-0}
  neg_rm_calls=${neg_rm_calls:-0}
  incdec_rm_calls=${incdec_rm_calls:-0}
  test_eax_imm_calls=${test_eax_imm_calls:-0}
  test_imm_rm_calls=${test_imm_rm_calls:-0}
  push_imm_calls=${push_imm_calls:-0}
  push_rm_calls=${push_rm_calls:-0}
  leave_calls=${leave_calls:-0}
  setcc_calls=${setcc_calls:-0}
  movsx8_calls=${movsx8_calls:-0}
  movsx16_calls=${movsx16_calls:-0}

  if [ "$mov_rm_reg_calls" -gt "$max_calls" ] ||
      [ "$mov_reg_rm_calls" -gt "$max_calls" ] ||
      [ "$mov_imm_rm_calls" -gt "$max_calls" ] ||
      [ "$alu_rm_reg_calls" -gt "$max_calls" ] ||
      [ "$alu_reg_rm_calls" -gt "$max_calls" ] ||
      [ "$neg_rm_calls" -gt "$max_calls" ] ||
      [ "$incdec_rm_calls" -gt "$max_calls" ] ||
      [ "$test_eax_imm_calls" -gt "$max_calls" ] ||
      [ "$test_imm_rm_calls" -gt "$max_calls" ] ||
      [ "$push_imm_calls" -gt "$max_calls" ] ||
      [ "$push_rm_calls" -gt "$max_calls" ] ||
      [ "$leave_calls" -gt "$max_calls" ] ||
      [ "$setcc_calls" -gt "$max_calls" ] ||
      [ "$movsx8_calls" -gt "$max_calls" ] ||
      [ "$movsx16_calls" -gt "$max_calls" ]; then
    echo "Expected remaining helper calls within limits for $test_name, got mov-rm-reg=$mov_rm_reg_calls mov-reg-rm=$mov_reg_rm_calls mov-imm-rm=$mov_imm_rm_calls alu-rm-reg=$alu_rm_reg_calls alu-reg-rm=$alu_reg_rm_calls neg-rm=$neg_rm_calls incdec-rm=$incdec_rm_calls test-eax-imm=$test_eax_imm_calls test-imm-rm=$test_imm_rm_calls push-imm=$push_imm_calls push-rm=$push_rm_calls leave=$leave_calls setcc-rm8=$setcc_calls movsx-reg-rm8=$movsx8_calls movsx-reg-rm16=$movsx16_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_shift_rm_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local helper_calls

  [ -n "$max_calls" ] || return 0

  helper_calls=$(sed -n 's/.*helper profile shift-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  helper_calls=${helper_calls:-0}

  if [ "$helper_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls shift helper calls for $test_name, got $helper_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_stack_reg_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local push_calls
  local pop_calls

  [ -n "$max_calls" ] || return 0

  push_calls=$(sed -n 's/.*helper profile push-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  pop_calls=$(sed -n 's/.*helper profile pop-reg[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  push_calls=${push_calls:-0}
  pop_calls=${pop_calls:-0}

  if [ "$push_calls" -gt "$max_calls" ] || [ "$pop_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls stack-register helper calls for $test_name, got push=$push_calls pop=$pop_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_mul_div_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local mul_calls
  local imul_acc_calls
  local div_calls

  [ -n "$max_calls" ] || return 0

  mul_calls=$(sed -n 's/.*helper profile mul-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  imul_acc_calls=$(sed -n 's/.*helper profile imul-acc-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  div_calls=$(sed -n 's/.*helper profile div-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  mul_calls=${mul_calls:-0}
  imul_acc_calls=${imul_acc_calls:-0}
  div_calls=${div_calls:-0}

  if [ "$mul_calls" -gt "$max_calls" ] ||
      [ "$imul_acc_calls" -gt "$max_calls" ] ||
      [ "$div_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls mul/div helper calls for $test_name, got mul=$mul_calls imul-acc=$imul_acc_calls div=$div_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

cd "$ROOT"

[ -f "$DEFCONFIG" ] || fail "missing $DEFCONFIG"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  tmp_files+=("$out")

  if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
      make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
      NEMU_DEFCONFIG=x86-am-jit_defconfig ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi
  if grep -q '\*\*\*FAIL\*\*\*\|HIT BAD TRAP' "$out"; then
    echo "$test_name reported failure" >&2
    cat "$out" >&2
    exit 2
  fi

  require_min_jit_instructions "$out" "$test_name" "$(required_jit_instructions "$test_name")"
  require_max_unsupported_hits "$out" "$test_name" "$(required_max_unsupported_hits "$test_name")"
  require_max_executed_blocks "$out" "$test_name" "$(required_max_executed_blocks "$test_name")"
  require_min_native_alu_ops "$out" "$test_name" "$(required_min_native_alu_ops "$test_name")"
  require_min_native_incdec_ops "$out" "$test_name" "$(required_min_native_incdec_ops "$test_name")"
  require_min_native_incdec_jcc_backedges "$out" "$test_name" "$(required_min_native_incdec_jcc_backedges "$test_name")"
  require_min_native_incdec_resident_loops "$out" "$test_name" "$(required_min_native_incdec_resident_loops "$test_name")"
  require_max_helper_incdec_reg_calls "$out" "$test_name" "$(required_max_helper_incdec_reg_calls "$test_name")"
  require_min_native_pmem_loads "$out" "$test_name" "$(required_min_native_pmem_loads "$test_name")"
  require_min_native_pmem_stores "$out" "$test_name" "$(required_min_native_pmem_stores "$test_name")"
  require_min_native_mul_ops "$out" "$test_name" "$(required_min_native_mul_ops "$test_name")"
  require_min_native_imul_ops "$out" "$test_name" "$(required_min_native_imul_ops "$test_name")"
  require_min_native_div_ops "$out" "$test_name" "$(required_min_native_div_ops "$test_name")"
  require_min_native_alu_jcc_fusions "$out" "$test_name" "$(required_min_native_alu_jcc_fusions "$test_name")"
  require_min_native_alu_jcc_resident_loops "$out" "$test_name" "$(required_min_native_alu_jcc_resident_loops "$test_name")"
  require_min_native_shift_ops "$out" "$test_name" "$(required_min_native_shift_ops "$test_name")"
  require_min_native_not_ops "$out" "$test_name" "$(required_min_native_not_ops "$test_name")"
  require_min_native_movzx_ops "$out" "$test_name" "$(required_min_native_movzx_ops "$test_name")"
  require_max_helper_jcc_rel_calls "$out" "$test_name" "$(required_max_helper_jcc_rel_calls "$test_name")"
  require_max_helper_jmp_rm_calls "$out" "$test_name" "$(required_max_helper_jmp_rm_calls "$test_name")"
  require_max_helper_moffs_calls "$out" "$test_name" "$(required_max_helper_moffs_calls "$test_name")"
  require_max_helper_call_ret_calls "$out" "$test_name" "$(required_max_helper_call_ret_calls "$test_name")"
  require_max_helper_mem_alu_calls "$out" "$test_name" "$(required_max_helper_mem_alu_calls "$test_name")"
  require_max_helper_remaining_calls "$out" "$test_name" "$(required_max_helper_remaining_calls "$test_name")"
  require_max_helper_shift_rm_calls "$out" "$test_name" "$(required_max_helper_shift_rm_calls "$test_name")"
  require_max_helper_stack_reg_calls "$out" "$test_name" "$(required_max_helper_stack_reg_calls "$test_name")"
  require_max_helper_mul_div_calls "$out" "$test_name" "$(required_max_helper_mul_div_calls "$test_name")"
done

echo "x86 JIT smoke gate passed: ${TESTS[*]}"
