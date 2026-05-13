#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/nemu-libc-soft-math"
host_bin="$build_dir/libc-soft-math-host"
rv32_obj="$build_dir/math-rv32.o"
rv32_dis="$build_dir/math-rv32.dis"

mkdir -p "$build_dir"

gcc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fno-builtin \
  "$repo_root/navy-apps/tests/libc-soft-math-test.c" \
  "$repo_root/navy-apps/libs/libc/src/math/math.c" \
  -o "$host_bin"

"$host_bin"

riscv64-linux-gnu-gcc -std=gnu17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-asynchronous-unwind-tables \
  -march=rv32im_zicsr -mabi=ilp32 \
  -I"$repo_root/navy-apps/libs/libc/include" \
  -c "$repo_root/navy-apps/libs/libc/src/math/math.c" \
  -o "$rv32_obj"

riscv64-linux-gnu-objdump -d "$rv32_obj" > "$rv32_dis"
if grep -Eq '\b(flw|fsw|fld|fsd|fadd|fsub|fmul|fdiv|fsqrt|fcvt|fmv|feq|flt|fle|fclass)\b|\bfrcsr\b|\bfscsr\b' "$rv32_dis"; then
  echo "error: RV32 math object contains hardware floating-point instructions" >&2
  exit 1
fi

riscv64-linux-gnu-readelf -A "$rv32_obj" | grep -q 'rv32i2p1_m2p0_zicsr2p0'

echo "RV32 soft math compile and instruction checks passed"
