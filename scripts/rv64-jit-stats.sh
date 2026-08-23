#!/usr/bin/env bash

# This file is sourced by correctness checks that already choose their own
# shell options and traps. Keep it limited to data and functions so sourcing it
# cannot change the caller's execution policy.
declare -gA RV64_JIT_STAT_VALUES=()
declare -g RV64_JIT_STATS_LOG=
declare -g RV64_JIT_STATS_TEST=

load_rv64_jit_stats() {
  local log=$1
  local test_name=$2
  local line
  local record
  local key
  local value
  local line_number=0

  RV64_JIT_STAT_VALUES=()
  RV64_JIT_STATS_LOG=
  RV64_JIT_STATS_TEST=

  if [ ! -r "$log" ]; then
    echo "Cannot read keyed JIT statistics for $test_name: $log" >&2
    return 2
  fi

  while IFS= read -r line || [ -n "$line" ]; do
    line_number=$((line_number + 1))

    # NEMU's Log() prefix contains source information and its suffix may
    # contain an ANSI colour reset. Neither is part of the machine contract.
    if [[ "$line" != *"jit-kv:"* ]]; then
      continue
    fi

    record=${line#*jit-kv:}
    record=${record# }
    # read removes the newline but preserves a CRLF carriage return. Remove
    # that byte first so an ANSI reset immediately before it becomes the true
    # suffix and can then be stripped as well.
    record=${record%$'\r'}
    record=${record%$'\033[0m'}

    if [[ ! "$record" =~ ^([a-z0-9_]+(\.[a-z0-9_]+)*)=([0-9]+)$ ]]; then
      echo "Malformed keyed JIT statistic for $test_name at" \
        "$log:$line_number: $record" >&2
      RV64_JIT_STAT_VALUES=()
      return 2
    fi

    key=${BASH_REMATCH[1]}
    value=${BASH_REMATCH[3]}
    if [ "${RV64_JIT_STAT_VALUES[$key]+present}" = present ]; then
      echo "Duplicate keyed JIT statistic '$key' for $test_name at" \
        "$log:$line_number" >&2
      RV64_JIT_STAT_VALUES=()
      return 2
    fi

    RV64_JIT_STAT_VALUES["$key"]=$value
  done <"$log"

  RV64_JIT_STATS_LOG=$log
  RV64_JIT_STATS_TEST=$test_name
}

rv64_jit_stat() {
  local key=$1
  local test_name=$2
  local log=$3

  # Binding the loaded values to their source log prevents a failed load from
  # accidentally reusing plausible counters left by an earlier guest.
  if [ "$RV64_JIT_STATS_LOG" != "$log" ] ||
    [ "$RV64_JIT_STATS_TEST" != "$test_name" ]; then
    echo "Keyed JIT statistics for $test_name were not loaded from $log" >&2
    return 2
  fi

  if [ "${RV64_JIT_STAT_VALUES[$key]+present}" != present ]; then
    echo "Missing keyed JIT statistic '$key' for $test_name in $log" >&2
    return 2
  fi

  printf '%s\n' "${RV64_JIT_STAT_VALUES[$key]}"
}
