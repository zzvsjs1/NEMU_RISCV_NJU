# NEMU Full System Emulator

## Overview

This project is built on top of the
[Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).

NEMU is a lightweight full-system emulator framework. The upstream project can
support several guest ISAs and provides a monitor, register and memory
inspection, expression evaluation, watchpoints, snapshotting, differential
testing, paging, interrupts, exceptions, and a small set of emulated devices.

This repository focuses on RV32IM system-mode execution. It keeps the NJU-style
layout while adding practical work needed by this local RISC-V32 project:

- `nemu`: the emulator core, RISC-V32 executor, device models, and optional JIT.
- `abstract-machine`: the AM runtime and the RISC-V32 NEMU device abstraction.
- `nanos-lite`: the small OS used to run Navy applications on NEMU.
- `navy-apps`: user programs, libraries, file-system image generation, and PAL
  game integration.
- `am-kernels`: CPU tests and benchmarks, including MicroBench and JITBench.

The current `master` branch is the JIT performance-improved version. Older
branches are kept as comparison points, so behaviour and speed can be compared
across the original baseline, disk/ONScripter work, non-JIT performance work,
and the JIT version.

## Branch Roles

| Branch | Role |
|--------|------|
| `master` | Current RISC-V32 JIT performance version |
| `legacy/baseline-master` | Original baseline before disk, ONScripter, performance, and JIT work |
| `legacy/onscripter-disk` | Legacy disk-backed Navy/ONScripter branch |
| `performance_improve` | Non-JIT performance baseline |
| `performance_improve_jit` | Old pre-migration name for the current JIT work |

## Environment

From the repository root, initialise the environment before building or running:

```bash
source scripts/setup-env.sh
```

The setup script derives paths from the current checkout and exports:

```bash
export AM_HOME="$PWD/abstract-machine"
export NEMU_HOME="$PWD/nemu"
export NAVY_HOME="$PWD/navy-apps"
export ISA=riscv32
export ARCH=riscv32-nemu
```

If you set variables manually, replace `$PWD` with the repository root of your
own checkout.

## Supported Devices

| Device | Status | Notes |
|--------|--------|-------|
| Serial | Supported | Guest output is written through NEMU's serial device. |
| Clock / RTC | Supported | 64-bit microsecond timer and AM uptime support. |
| Keyboard | Supported | SDL scancode queue mapped to AM key events. |
| VGA / framebuffer | Supported | ARGB8888 framebuffer with SDL display output. |
| Audio | Supported | SDL audio backend, optional dummy backend for headless runs. |
| Disk | Supported | Disk-backed Navy ramdisk image with embedded ramdisk fallback in Nanos-lite. |
| SD card | Experimental / disabled by default | Source exists, but normal workflow uses the disk device. |

## Device Implementation Details

RISC-V32 devices are exposed through the NEMU MMIO area. The AM platform header
defines the guest-visible addresses in
`abstract-machine/am/src/platform/nemu/include/nemu.h`:

| Area | Address |
|------|---------|
| Device base | `0xa0000000` |
| Serial | `0xa00003f8` |
| Keyboard | `0xa0000060` |
| RTC | `0xa0000048` |
| VGA control | `0xa0000100` |
| Audio control | `0xa0000200` |
| Disk control | `0xa0000300` |
| Framebuffer | `0xa1000000` |
| Audio stream buffer | `0xa1200000` |

The same AM code can still support port I/O for x86-style NEMU builds, but the
normal `riscv32-nemu` path uses MMIO.

### Serial

The guest writes bytes to `SERIAL_PORT`. AM exposes this through `putch()`, and
Nanos-lite maps it to `/dev/serial`. In native NEMU runs the byte is printed to
the host standard error stream, so logs and guest console output remain visible
without a separate UART window.

### Timer and RTC

The RTC device exposes a 64-bit microsecond counter as two 32-bit registers.
AM records the boot counter value and reports `AM_TIMER_UPTIME` as the elapsed
microseconds since boot. `AM_TIMER_RTC` is stubbed to a fixed date in the NEMU
platform path, because the project mainly needs monotonic time for scheduling,
SDL timers, and benchmark timing.

NEMU also installs an alarm callback for device interrupts when running in
system mode. The CPU loop batches device update checks for performance, but the
batch is bounded so timer and input processing are not starved.

### Keyboard and Events

NEMU translates SDL scancodes into AM key codes and stores them in a small FIFO.
AM reads `KBD_ADDR` and returns a key code with `0x8000` set for key-down events.
Nanos-lite exposes this as `/dev/events`, returning strings such as:

```text
kd A
ku A
```

F1, F2, and F3 are intercepted by Nanos-lite for foreground application
switching. Those hotkeys do not pass through to user applications.

### VGA and Framebuffer

The VGA control register reports the configured screen size, either `400x300` or
`800x600` depending on `nemu/menuconfig`. The framebuffer is a linear ARGB8888
MMIO region at `FB_ADDR`. AM implements `AM_GPU_FBDRAW` by copying pixels into
that framebuffer and then writing the sync register when a flush is requested.

Nanos-lite exposes:

- `/proc/dispinfo` for width and height;
- `/dev/fb` for framebuffer writes.

For foreground switching, Nanos-lite keeps a full-screen shadow buffer for each
foreground process. When switching back to an application, it restores that
application's last framebuffer state so old pixels from another program do not
remain on screen.

### Audio

The audio device has control registers for frequency, channel count, callback
sample count, stream-buffer size, init, and used-byte count. The sample buffer is
a ring buffer mapped at `AUDIO_SBUF_ADDR`.

AM writes sample bytes into the ring buffer and then writes a delta to the count
register. NEMU keeps the real occupancy internally and serialises updates with
the SDL audio callback, which avoids stale count races when the host consumes
audio while the guest is producing more.

Nanos-lite exposes:

- `/dev/sb` for sample bytes;
- `/dev/sbctl` for audio format and free-buffer queries.

Because NEMU has only one host SDL audio device, Nanos-lite remembers the audio
format for each foreground application. When switching back to an app such as
PAL, it restores that app's audio configuration before scheduling it again.

`CONFIG_AUDIO_DUMMY` keeps the guest audio MMIO interface visible but discards
queued samples immediately. This is useful for headless benchmark runs.

### Headless SDL, Dummy VGA, and Dummy Audio

There are two different "dummy" layers that are useful for tests and benchmarks:

- Host SDL dummy drivers: set `SDL_VIDEODRIVER=dummy` and
  `SDL_AUDIODRIVER=dummy` before running NEMU. This keeps SDL from requiring a
  real display or audio device on the host.
- NEMU device options: disable `CONFIG_VGA_SHOW_SCREEN` if a run does not need
  a visible SDL window, and enable `CONFIG_AUDIO_DUMMY` if guest audio should
  be accepted but discarded immediately.

For benchmark runs, dummy video/audio avoids measuring host window or speaker
setup. For GUI correctness work, keep the real SDL video path enabled so the
framebuffer can be inspected.

### Disk and Ramdisk

The normal disk image is:

```text
$NAVY_HOME/build/ramdisk.img
```

NEMU opens that image first. If it is not available, it can fall back to
`CONFIG_DISK_IMG_PATH` from `nemu/menuconfig`. The disk controller exposes
512-byte blocks and uses guest physical memory as a DMA buffer. Reads from the
padded tail of a non-512-byte-aligned image return zero-filled bytes, which keeps
short final-block reads deterministic.

Nanos-lite uses `nanos-lite/src/disk.c` as the high-level block interface. If no
disk device is present, it falls back to the embedded ramdisk symbols. For large
aligned reads it batches multiple blocks to reduce MMIO traffic and host file
operations.

When a disk read writes into guest memory, the RISC-V32 JIT invalidates affected
translated code-cache entries. This keeps self-loading or disk-loaded code
consistent with the interpreter path.

### JIT and Device Correctness

The JIT only fast-paths ordinary RAM cases. MMIO, PIO, traps, unusual virtual
memory cases, and device side effects fall back to the normal NEMU memory and
execution helpers. This is important: a faster JIT must not skip guest-visible
device behaviour.

## Build and Run

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

## JIT Configuration

The current branch adds RISC-V32 JIT options under `nemu/menuconfig`:

```text
Build Options
  [*] Enable RISC-V32 JIT
  [ ] Enable RISC-V32 JIT statistics
```

Normal performance runs should keep JIT enabled and JIT statistics disabled.
Statistics are useful for diagnosis, but the extra counters add overhead.

Runtime controls:

```bash
# Force interpreter / non-JIT execution for comparison.
NEMU_DISABLE_JIT=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Print JIT counters when CONFIG_RV32_JIT_STATS is enabled.
NEMU_JIT_STATS=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

## Performance Measurements

These are local reference numbers from the current JIT branch. They are useful
for checking trend direction, but you should re-measure on your own CPU because
host frequency scaling, scheduler load, thermal limits, and laptop
performance-core / efficiency-core placement can change the result.

| Branch / mode | Benchmark | Result |
|---------------|-----------|--------|
| `master`, JIT enabled | MicroBench | `5069 Marks`, `494,788,583 instr/s` |
| `master`, JIT enabled | JITBench | `183,037,705 instr/s` |

Use these commands to collect comparable numbers:

```bash
source scripts/setup-env.sh

# Current JIT path.
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
make -C am-kernels/benchmarks/jitbench ARCH=riscv32-nemu run

# Current branch with JIT disabled at runtime.
NEMU_DISABLE_JIT=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Non-JIT performance branch.
git checkout performance_improve
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Original baseline branch.
git checkout legacy/baseline-master
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

The speed-up calculation is simple division:

```text
speed-up = faster instr/s / slower instr/s
```

Example:

```text
494,788,583 / 200,000,000 = 2.47x
```

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
program under NEMU. If a GUI window opens, closing it after interaction is a
valid terminal success condition.

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
