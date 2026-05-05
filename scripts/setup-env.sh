#!/usr/bin/env bash

# Source this file from the repository root:
#   source scripts/setup-env.sh

_nemu_env_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_nemu_env_repo_root="$(cd "${_nemu_env_script_dir}/.." && pwd)"

export AM_HOME="${_nemu_env_repo_root}/abstract-machine"
export NEMU_HOME="${_nemu_env_repo_root}/nemu"
export NAVY_HOME="${_nemu_env_repo_root}/navy-apps"
export ISA="riscv32"
export ARCH="riscv32-nemu"

echo "NEMU RISC-V32 environment configured:"
echo "  AM_HOME=${AM_HOME}"
echo "  NEMU_HOME=${NEMU_HOME}"
echo "  NAVY_HOME=${NAVY_HOME}"
echo "  ISA=${ISA}"
echo "  ARCH=${ARCH}"
echo
echo "Common commands:"
echo "  cd ${_nemu_env_repo_root}/nanos-lite"
echo "  make ARCH=${ARCH} update"
echo "  make ARCH=${ARCH} run"
echo
echo "Cleanup:"
echo "  ${_nemu_env_repo_root}/scripts/clean-build.sh"

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo
  echo "Note: this script was executed, so exports only apply to this process."
  echo "Use 'source scripts/setup-env.sh' to configure your current shell."
fi

unset _nemu_env_script_dir
unset _nemu_env_repo_root
