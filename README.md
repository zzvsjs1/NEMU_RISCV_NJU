# NEMU RISC-V32 Non-JIT Performance Branch

This branch contains the executor performance-improvement work before the
x86-64 JIT was added. It is kept as the non-JIT performance baseline.

Use this branch when you want to compare:

- original baseline performance;
- disk/ONScripter performance before JIT;
- current JIT performance after migration to `master`.

Use the new `master` branch after migration for current development.

## Branch Role

| Branch | Role |
|--------|------|
| `performance_improve` | Non-JIT performance baseline |
| `legacy/baseline-master` | Original baseline before disk, ONScripter, performance, and JIT work |
| `legacy/onscripter-disk` | Legacy disk-backed ONScripter branch |
| `master` | Current JIT performance version after migration |

This branch is useful because it separates "normal C executor improvements" from
"JIT improvements". That makes the JIT gain easier to calculate:

```text
JIT gain over this branch = JIT instr/s / performance_improve instr/s
```

For example, if the JIT branch reports `494,788,583 instr/s` and this branch
reports `250,000,000 instr/s`, the JIT gain is:

```text
494,788,583 / 250,000,000 = 1.98x
```

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

## Benchmarking

MicroBench is the main benchmark for this branch:

```bash
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

When comparing with the current JIT branch, use the same host load and same NEMU
configuration where possible. This branch has no JIT, so there is no
`NEMU_DISABLE_JIT` comparison mode here.

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

Do not delete or recreate that data when cleaning or making new checkouts.

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
