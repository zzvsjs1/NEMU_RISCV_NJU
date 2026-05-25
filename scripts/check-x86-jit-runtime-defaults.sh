#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

fail() {
  echo "x86 JIT runtime defaults check failed: $*" >&2
  exit 1
}

cd "$ROOT"

aggressive_init=$(sed -n \
  '/jit_paged_aggressive_enabled =/,/;/p' \
  nemu/src/isa/x86/jit.c)
fast_chain_init=$(sed -n \
  '/jit_fast_chain_enabled =/,/;/p' \
  nemu/src/isa/x86/jit.c)
paged_batch_init=$(sed -n \
  '/jit_paged_batch_enabled =/,/;/p' \
  nemu/src/isa/x86/jit.c)
native_idiv_init=$(sed -n \
  '/jit_native_idiv_enabled =/,/;/p' \
  nemu/src/isa/x86/jit.c)
high_byte_test_init=$(sed -n \
  '/jit_native_high_byte_test_enabled =/,/;/p' \
  nemu/src/isa/x86/jit.c)

case "$aggressive_init" in
  *'jit_env_flag_enabled("NEMU_X86_JIT_PAGED_AGGRESSIVE")'*)
    ;;
  *)
    fail "NEMU_X86_JIT_PAGED_AGGRESSIVE must be opt-in"
    ;;
esac

case "$fast_chain_init" in
  *'jit_env_flag_default_enabled("NEMU_X86_JIT_FAST_CHAIN")'*)
    ;;
  *)
    fail "NEMU_X86_JIT_FAST_CHAIN must be default-on"
    ;;
esac

case "$paged_batch_init" in
  *'jit_env_flag_default_enabled("NEMU_X86_JIT_PAGED_BATCH")'*)
    ;;
  *)
    fail "NEMU_X86_JIT_PAGED_BATCH must be default-on"
    ;;
esac

case "$native_idiv_init" in
  *'jit_env_flag_enabled("NEMU_X86_JIT_NATIVE_IDIV")'*)
    ;;
  *)
    fail "NEMU_X86_JIT_NATIVE_IDIV must remain opt-in until visual regressions are cleared"
    ;;
esac

case "$high_byte_test_init" in
  *'jit_env_flag_default_enabled("NEMU_X86_JIT_HIGH_BYTE_TEST")'*)
    ;;
  *)
    fail "NEMU_X86_JIT_HIGH_BYTE_TEST must be default-on after FCEUX helper validation"
    ;;
esac

flush_data_tlb_body=$(sed -n \
  '/void isa_jit_flush_data_tlb(void)/,/^}/p' \
  nemu/src/isa/x86/jit.c)

case "$flush_data_tlb_body" in
  *'jit_paging_bump_generation'*)
    fail "data TLB flush must not invalidate paged translation keys"
    ;;
esac

echo "x86 JIT runtime defaults check passed"
