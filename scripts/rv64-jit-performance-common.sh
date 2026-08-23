#!/usr/bin/env bash

# Shared lifecycle support for the RV64 JIT performance checks.
#
# The benchmark runners must call these functions directly, rather than from
# command substitutions.  Bash executes command substitutions in a subshell,
# which would discard both the registered temporary paths and the fact that a
# non-production configuration had been selected.

_rv64_jit_perf_require_result_variable() {
  local _rv64_jit_perf_name=${1-}

  # printf -v accepts a variable name, so reject malformed names before
  # creating a resource that the caller could not receive or clean up safely.
  case "$_rv64_jit_perf_name" in
    '' | [0-9]* | *[!a-zA-Z0-9_]*)
      printf 'Invalid RV64 JIT performance result variable: %s\n' \
        "$_rv64_jit_perf_name" >&2
      return 2
      ;;
  esac
}

rv64_jit_perf_enable_cleanup() {
  if [ "$#" -gt 1 ]; then
    printf '%s\n' \
      'rv64_jit_perf_enable_cleanup accepts at most one NEMU directory' >&2
    return 2
  fi

  # These arrays live in the calling shell.  Registering them here and using
  # direct runner calls ensures that an EXIT trap can see every later entry.
  RV64_JIT_PERF_NEMU_HOME=${1-}
  RV64_JIT_PERF_CONFIG_DIRTY=0
  RV64_JIT_PERF_TEMP_FILES=()
  RV64_JIT_PERF_TEMP_DIRS=()

  trap rv64_jit_perf_cleanup EXIT
}

rv64_jit_perf_make_temp_file() {
  if [ "$#" -ne 1 ]; then
    printf '%s\n' \
      'rv64_jit_perf_make_temp_file requires one result variable' >&2
    return 2
  fi

  local _rv64_jit_perf_result_variable=$1
  local _rv64_jit_perf_created_path

  _rv64_jit_perf_require_result_variable \
    "$_rv64_jit_perf_result_variable" || return

  if ! _rv64_jit_perf_created_path=$(mktemp); then
    printf '%s\n' \
      'Failed to create an RV64 JIT performance temporary file' >&2
    return 1
  fi

  # Registration is deliberately the first operation after mktemp succeeds.
  # The EXIT trap may therefore remove the exact file even if assignment to
  # the caller or any subsequent benchmark operation fails.
  RV64_JIT_PERF_TEMP_FILES+=("$_rv64_jit_perf_created_path")
  printf -v "$_rv64_jit_perf_result_variable" '%s' \
    "$_rv64_jit_perf_created_path"
}

rv64_jit_perf_make_temp_dir() {
  if [ "$#" -ne 1 ]; then
    printf '%s\n' \
      'rv64_jit_perf_make_temp_dir requires one result variable' >&2
    return 2
  fi

  local _rv64_jit_perf_result_variable=$1
  local _rv64_jit_perf_created_path

  _rv64_jit_perf_require_result_variable \
    "$_rv64_jit_perf_result_variable" || return

  if ! _rv64_jit_perf_created_path=$(mktemp -d); then
    printf '%s\n' \
      'Failed to create an RV64 JIT performance temporary directory' >&2
    return 1
  fi

  # Recursive deletion is restricted to these exact mktemp -d results.  Do
  # not provide a general directory-registration function: it would weaken
  # the provenance guarantee that makes the EXIT cleanup safe.
  RV64_JIT_PERF_TEMP_DIRS+=("$_rv64_jit_perf_created_path")
  printf -v "$_rv64_jit_perf_result_variable" '%s' \
    "$_rv64_jit_perf_created_path"
}

rv64_jit_perf_use_defconfig() {
  if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    printf '%s\n' \
      'rv64_jit_perf_use_defconfig requires one defconfig target' >&2
    return 2
  fi

  if [ -z "${RV64_JIT_PERF_NEMU_HOME-}" ]; then
    printf '%s\n' \
      'Cannot select an RV64 JIT defconfig without a NEMU directory' >&2
    return 2
  fi

  # Set this before invoking Make.  A failed configuration command may still
  # have changed .config, so every attempted selection requires restoration.
  RV64_JIT_PERF_CONFIG_DIRTY=1
  make -C "$RV64_JIT_PERF_NEMU_HOME" "$1" >/dev/null
}

rv64_jit_perf_cleanup() {
  local _rv64_jit_perf_original_status=$?
  local _rv64_jit_perf_final_status=$_rv64_jit_perf_original_status
  local _rv64_jit_perf_cleanup_failed=0
  local _rv64_jit_perf_path

  # Avoid recursive cleanup and prevent errexit from skipping later cleanup
  # work.  In particular, production configuration restoration must still be
  # attempted when removal of an earlier temporary resource fails.
  trap - EXIT
  set +e

  for _rv64_jit_perf_path in "${RV64_JIT_PERF_TEMP_FILES[@]}"; do
    if ! rm -f -- "$_rv64_jit_perf_path"; then
      printf 'Failed to remove RV64 JIT performance temporary file: %s\n' \
        "$_rv64_jit_perf_path" >&2
      _rv64_jit_perf_cleanup_failed=1
    fi
  done

  for _rv64_jit_perf_path in "${RV64_JIT_PERF_TEMP_DIRS[@]}"; do
    # mktemp -d never returns these values.  Refusing them adds a final guard
    # against accidental mutation of the registry before recursive removal.
    case "$_rv64_jit_perf_path" in
      '' | / | . | ..)
        printf 'Refusing unsafe RV64 JIT temporary directory: %s\n' \
          "$_rv64_jit_perf_path" >&2
        _rv64_jit_perf_cleanup_failed=1
        continue
        ;;
    esac

    if ! rm -rf -- "$_rv64_jit_perf_path"; then
      printf 'Failed to remove RV64 JIT performance temporary directory: %s\n' \
        "$_rv64_jit_perf_path" >&2
      _rv64_jit_perf_cleanup_failed=1
    fi
  done

  if [ "${RV64_JIT_PERF_CONFIG_DIRTY:-0}" -eq 1 ]; then
    if [ -z "${RV64_JIT_PERF_NEMU_HOME-}" ]; then
      printf '%s\n' \
        'Cannot restore the RV64 JIT configuration without a NEMU directory' \
        >&2
      _rv64_jit_perf_cleanup_failed=1
    elif ! make -C "$RV64_JIT_PERF_NEMU_HOME" \
        riscv64-am-headless-jit_defconfig >/dev/null; then
      printf '%s\n' \
        'Failed to restore the production RV64 JIT configuration' >&2
      _rv64_jit_perf_cleanup_failed=1
    else
      RV64_JIT_PERF_CONFIG_DIRTY=0
    fi
  fi

  # Preserve the benchmark's exact non-zero status.  Cleanup only supplies a
  # generic failure when the script body itself succeeded, so the original
  # diagnostic remains authoritative whenever both phases fail.
  if [ "$_rv64_jit_perf_final_status" -eq 0 ] &&
      [ "$_rv64_jit_perf_cleanup_failed" -ne 0 ]; then
    _rv64_jit_perf_final_status=1
  fi

  exit "$_rv64_jit_perf_final_status"
}
