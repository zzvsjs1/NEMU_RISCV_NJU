# NEMU RISC-V32 Legacy Baseline

This branch preserves the old `master` state as a baseline for comparison. It is
kept to make it easy to compare behaviour and performance before the later
disk-backed ONScripter work, executor performance work, and JIT work.

Use the new `master` branch after migration for current development. Use this
branch only when you need the original baseline.

## Branch Role

| Branch | Role |
|--------|------|
| `legacy/baseline-master` | Original baseline before disk, ONScripter, performance, and JIT work |
| `master` | Current JIT performance version after migration |
| `legacy/onscripter-disk` | Legacy disk-backed ONScripter branch |
| `performance_improve` | Non-JIT performance baseline |

This branch is intentionally not the fastest branch. Its value is that it gives
a clean before/after point for correctness and performance measurements.

## Environment

Always initialise the local build environment before building or running:

```bash
source scripts/setup-env.sh
```

That helper exports:

```bash
export AM_HOME=$PWD/abstract-machine
export NEMU_HOME=$PWD/nemu
export NAVY_HOME=$PWD/navy-apps
export ISA=riscv32
export ARCH=riscv32-nemu
```

## Supported Devices

| Device | Status |
|--------|--------|
| Serial | Yes |
| Clock | Yes, simple |
| Keyboard | Yes |
| VGA | Yes |
| Audio | Yes |
| SD card | No |
| Disk | No |

## Headless SDL and Dummy Devices

For benchmark or CI-style runs, the host SDL dummy drivers can avoid opening a
real window or audio device:

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

NEMU also has device-level options in `nemu/menuconfig`. Disable
`CONFIG_VGA_SHOW_SCREEN` when a visible SDL window is not needed, and enable
`CONFIG_AUDIO_DUMMY` when guest audio should be accepted but discarded
immediately. Keep the real SDL video path enabled when checking GUI behaviour.

## Build NEMU

Configure NEMU:

```bash
source scripts/setup-env.sh
make -C nemu ISA=riscv32 menuconfig
```

Build NEMU:

```bash
source scripts/setup-env.sh
make -B -C nemu ISA=riscv32
```

Useful dependencies include a RISC-V32 Newlib toolchain for
`-march=rv32im -mabi=ilp32`, LLVM 15, readline, ncurses, flex, and bison.

## Baseline Benchmarks

MicroBench is the main broad benchmark for this baseline branch:

```bash
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

When comparing with a faster branch, keep the same host, same shell environment,
and same NEMU configuration. The simple speed calculation is:

```text
speed-up = faster branch instr/s / baseline branch instr/s
```

For example, if the JIT branch reports `494,788,583 instr/s` and this baseline
reports `100,000,000 instr/s`, the speed-up is:

```text
494,788,583 / 100,000,000 = 4.95x
```

## Nanos-lite Flow

The normal RISC-V32 Nanos-lite workflow is:

```bash
source scripts/setup-env.sh
cd nanos-lite
make ARCH=riscv32-nemu update
make ARCH=riscv32-nemu run
```

This branch predates the later disk-backed ONScripter branch, so use the
ONScripter legacy branch or current `master` when testing that workflow.

## Cleanup

For a conservative cleanup of generated artefacts:

```bash
scripts/clean-build.sh
```

This preserves NEMU menuconfig/autoconfig state, PAL data, and source changes.
To also clean the NEMU build directory:

```bash
scripts/clean-build.sh --nemu
```

Do not delete or recreate the local PAL data under:

```text
navy-apps/fsimg/share/games/pal
```
