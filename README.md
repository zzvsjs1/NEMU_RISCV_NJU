# NEMU RISC-V32 Legacy ONScripter/Disk Branch

This branch preserves the old `onscripter` work. It is kept as the legacy
branch for disk-backed Navy apps and ONScripter changes before the later
performance and JIT work.

The recommended clearer branch name is:

```text
legacy/onscripter-disk
```

Use the new `master` branch after migration for current development. Use this
legacy branch when you specifically want to compare or inspect the original
disk/ONScripter implementation.

## Branch Role

| Branch | Role |
|--------|------|
| `legacy/onscripter-disk` | Legacy disk-backed ONScripter branch |
| `legacy/baseline-master` | Original baseline before disk, ONScripter, performance, and JIT work |
| `performance_improve` | Non-JIT performance baseline that builds on this work |
| `master` | Current JIT performance version after migration |

## What This Branch Contains

This branch is useful for checking the disk and ONScripter behaviour without the
later executor and JIT performance changes. It includes the Navy ramdisk flow
used by Nanos-lite GUI programs and keeps PAL game data outside generated build
cleanup.

It does not include the RISC-V32 JIT.

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
| Disk | Yes, for the Navy/ramdisk workflow |

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

## Nanos-lite GUI Flow

The normal RISC-V32 Nanos-lite GUI workflow is:

```bash
source scripts/setup-env.sh
cd nanos-lite
make ARCH=riscv32-nemu update
make ARCH=riscv32-nemu run
```

`make ARCH=riscv32-nemu update` rebuilds Navy apps, regenerates the ramdisk, and
updates generated Nanos-lite links. `make ARCH=riscv32-nemu run` starts the GUI
program under NEMU.

The PAL game data is local under:

```text
navy-apps/fsimg/share/games/pal
```

Do not delete or recreate that data when cleaning, renaming branches, or making
new checkouts.

## Benchmarking This Branch

MicroBench can still be used for comparison with later performance branches:

```bash
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

Compare against later branches with:

```text
speed-up = later branch instr/s / this branch instr/s
```

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
