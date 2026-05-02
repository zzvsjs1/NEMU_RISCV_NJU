#!/usr/bin/env bash

# Source this file from the repository root:
#   source scripts/codex-env.sh

_codex_env_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_codex_env_repo_root="$(cd "${_codex_env_script_dir}/.." && pwd)"

export AM_HOME="${_codex_env_repo_root}/abstract-machine"
export NEMU_HOME="${_codex_env_repo_root}/nemu"
export NAVY_HOME="${_codex_env_repo_root}/navy-apps"
export ISA="riscv32"
export ARCH="riscv32-nemu"

echo "Codex/NEMU environment configured:"
echo "  AM_HOME=${AM_HOME}"
echo "  NEMU_HOME=${NEMU_HOME}"
echo "  NAVY_HOME=${NAVY_HOME}"
echo "  ISA=${ISA}"
echo "  ARCH=${ARCH}"
echo
echo "Common commands:"
echo "  cd ${_codex_env_repo_root}/nanos-lite"
echo "  make ARCH=${ARCH} update"
echo "  make ARCH=${ARCH} run"
echo
echo "Cleanup:"
echo "  ${_codex_env_repo_root}/scripts/codex-clean.sh"

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo
  echo "Note: this script was executed, so exports only apply to this process."
  echo "Use 'source scripts/codex-env.sh' to configure your current shell."
fi

unset _codex_env_script_dir
unset _codex_env_repo_root
