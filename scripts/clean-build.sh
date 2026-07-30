#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

export AM_HOME="${repo_root}/abstract-machine"
export NEMU_HOME="${repo_root}/nemu"
export NAVY_HOME="${repo_root}/navy-apps"
export ISA="${ISA:-riscv32}"
export ARCH="${ARCH:-riscv32-nemu}"

clean_nemu=0

# NEMU's menuconfig output is deliberately preserved by default.  Most local
# runs depend on the current .config, while Navy/AM/Nanos generated files are
# cheap to rebuild and are the usual source of stale-image confusion.
usage() {
  cat <<EOF
Usage: $(basename "$0") [--nemu] [--help]

Clean generated build artifacts for the local riscv32-nemu workflow.

Default cleanup preserves:
  - NEMU menuconfig/autoconfig state
  - Navy fsimg/share data, including PAL game data
  - source files and local edits

Options:
  --nemu   Also run make clean in the NEMU tree
  --help   Show this help
EOF
}

while (($#)); do
  case "$1" in
    --nemu)
      clean_nemu=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

echo "Cleaning Navy apps build output and fsimg/bin..."
make -C "${NAVY_HOME}" ISA="${ISA}" clean-all

echo "Cleaning AbstractMachine build output..."
make -C "${AM_HOME}" ARCH="${ARCH}" clean-all

echo "Cleaning Nanos-lite build output..."
make -C "${repo_root}/nanos-lite" ARCH="${ARCH}" clean

for generated in \
  "${repo_root}/nanos-lite/src/files.h" \
  "${repo_root}/nanos-lite/src/syscall.h" \
  "${repo_root}/nanos-lite/build/ramdisk.img"; do
  # These compatibility symlinks may point into Navy build output.  Remove only
  # symlinks here so a real checked-in file with the same name is never deleted.
  if [[ -L "${generated}" ]]; then
    echo "Removing generated symlink ${generated}"
    rm -f "${generated}"
  fi
done

if ((clean_nemu)); then
  echo "Cleaning NEMU build output..."
  make -C "${NEMU_HOME}" clean
fi

echo "Cleanup complete."
