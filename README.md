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
- `fceux-am`: an AM port of the FCEUX NES emulator for native and RISC-V32 NEMU
  runs.

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
| Mouse | Supported | SDL motion, left/middle/right buttons, and wheel events exposed through AM and `/dev/events`. |
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
| Mouse | `0xa0000070` |
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

The RTC device keeps the existing high-performance 64-bit microsecond uptime
counter as two 32-bit registers. AM records the boot counter value and reports
`AM_TIMER_UPTIME` as the elapsed microseconds since boot, so scheduling, SDL
timers, and benchmark timing keep using the monotonic fast path.

The same RTC MMIO block also exposes 64-bit UTC Unix epoch seconds and 64-bit
UTC Unix epoch microseconds. AM uses those realtime registers to implement
`AM_TIMER_RTC` with year, month, day, hour, minute, and second fields. Nanos-lite
uses the epoch-microsecond value for POSIX time syscalls such as
`gettimeofday()`, `time()`, and `clock_gettime(CLOCK_REALTIME)`, while
`clock_gettime(CLOCK_MONOTONIC)` still uses uptime. Guest timezone handling is
UTC-only.

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

### Mouse

The mouse device is an MMIO event FIFO at `MOUSE_ADDR`. Host SDL motion,
button, and wheel events are latched into one event record per AM read. AM
exposes the record through `AM_INPUT_MOUSE`, and Nanos-lite emits `/dev/events`
records:

```text
mm X Y BUTTONS
md BUTTON X Y BUTTONS
mu BUTTON X Y BUTTONS
mw DX DY X Y BUTTONS
```

`NEMU_MOUSE_SCRIPT` can inject deterministic mouse events for dummy-driver test
runs when no real host pointer is available.

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

Useful dependencies include a RISC-V toolchain that can emit RV32IM with Zicsr
using `-march=rv32im_zicsr -mabi=ilp32`, plus readline, ncurses, flex, and
bison. LLVM is only needed for instruction tracing/disassembly builds; this tree
uses `llvm-config` when `CONFIG_ITRACE` is enabled, and `nemu/llvm.sh` currently
defaults to LLVM 18 while still accepting explicit supported versions.

## JIT Configuration

The current branch adds RISC-V32 JIT options inside the `RISC-V32 JIT` menu in
`nemu/menuconfig`. The menu is visible only for RISC-V32 native ELF
interpreter builds when tracing, watchpoints, memory/function tracing, and
DiffTest are disabled, because those features require interpreter
per-instruction hooks.

```text
RISC-V32 JIT
  [*] Enable RISC-V32 x86-64 JIT
  [ ] Collect RISC-V32 JIT statistics
```

Normal performance runs should keep JIT enabled and JIT statistics disabled.
Statistics are useful for diagnosis, but the extra counters add overhead.

Runtime controls:

```bash
# Force interpreter / non-JIT execution for comparison.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Print JIT counters when CONFIG_RV32_JIT_STATS is enabled.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_JIT_STATS=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

## RISC-V Exception Model

Older teaching-style NEMU + Nanos-lite paths often treated traps as an emulator
or AM convention. In that model, `ecall`, `yield()`, syscall dispatch, and the
private `nemu_trap` stop instruction could look like direct control transfers
between the emulator, Abstract Machine, and Nanos-lite. That is practical for a
small project, but it is not the same boundary as real RISC-V hardware: software
does not necessarily observe architectural `mepc`, `mcause`, `mtval`,
`mstatus`, `mtvec`, privilege checks, or `mret` ordering for every faulting
instruction.

The current RISC-V32 path uses real architectural trap delivery for normal
exceptions. NEMU raises traps by writing the machine CSRs, updating privilege
state, and jumping through `mtvec`. The AM RISC-V trap shim then saves a
`Context`, calls the C trap handler, and returns through `mret`. Nanos-lite
therefore receives syscalls, yields, timer interrupts, and user/kernel context
switches through the same CSR-backed trap frame that guest software would expect
from hardware. The syscall number still lives in the normal ABI register `a7`;
`mcause` records the trap class, such as user ecall, machine ecall, illegal
instruction, breakpoint, or misaligned address.

This matters for correctness because the faulting instruction must not partially
commit. For example, a misaligned load must raise a load-address-misaligned trap
before writing its destination register, and a misaligned store must trap before
changing memory. The fast executor and JIT now keep that ordering: hot native
paths use guards first, and the cold side exits raise the same CSR-visible trap
that the interpreter would raise.

### Exception and JIT Limitations

- The RISC-V support is focused on RV32IM system-mode work used by this tree.
  It is not a complete privileged-platform model for every RISC-V extension.
- Machine-mode trap handling is the main target. U-mode entry, U-mode ecall,
  CSR privilege checks, `mret`, and Sv32 paths are implemented for the local
  Nanos-lite workflow, but this is not a full supervisor-mode or hypervisor
  implementation.
- This EEI chooses visible traps for naturally misaligned scalar loads, stores,
  branches, and jumps. Code that expects invisible host-style misaligned memory
  emulation should not assume that behaviour here.
- The private `nemu_trap` instruction remains as the AM/NEMU test-exit
  convention. It is not a standard RISC-V exception.
- The JIT is available only for supported x86-64 native ELF RISC-V32 builds with
  tracing, watchpoints, memory/function tracing, and DiffTest disabled. Those
  debugging features require per-instruction interpreter hooks.
- The JIT fast path is intentionally conservative. MMIO, unsupported
  instructions, unusual translation cases, source-code writes, page-table
  writes, and trap-sensitive paths fall back to helpers or leave native code.
- Device timing is bounded by instruction-count polling, but it is not a
  cycle-accurate hardware timing model.

## Performance Measurements

These are local reference numbers from the current JIT branch, measured in this
checkout on 2026-05-16 with dummy SDL video/audio drivers. They are useful for
checking trend direction, but re-measure on your own CPU because host frequency
scaling, scheduler load, thermal limits, and laptop performance-core /
efficiency-core placement can change the result.

| Branch / mode | Benchmark | Result |
|---------------|-----------|--------|
| `master`, strict exceptions, JIT enabled | MicroBench | `27098 Marks`, `2,860,733,499 instr/s` |
| `master`, strict exceptions, JIT enabled | JITBench | `ALU 8.436 ms`, `Memory 3.528 ms`, `4,049,658,568 instr/s` |
| `master`, strict exceptions, `NEMU_DISABLE_JIT=1` | MicroBench | `3322 Marks`, `275,864,060 instr/s` |
| `master`, strict exceptions, `NEMU_DISABLE_JIT=1` | JITBench | `ALU 157.041 ms`, `Memory 77.442 ms`, `302,722,837 instr/s` |
| exported non-strict `6d946ee`, JIT enabled | MicroBench | `25041 Marks`, `2,480,000,000 instr/s` |
| exported non-strict `6d946ee`, JIT enabled | JITBench | `ALU 7.304 ms`, `Memory 4.352 ms`, `4,520,000,000 instr/s` |
| `performance_improve` | MicroBench | `3141 Marks`, `271,000,633 instr/s` |
| `legacy/baseline-master` | MicroBench | `694 Marks`, `58,319,798 instr/s` |

The current strict JIT MicroBench score is about `8.16x` the same branch with
JIT disabled, `8.63x` the non-JIT performance branch, and `39.05x` the original
baseline by Marks. By guest instruction throughput, the current strict JIT is
about `49.05x` the original baseline. Compared with the exported non-strict
`6d946ee` numbers above, strict MicroBench is now slightly ahead in this local
run, while JITBench overall throughput remains a little lower; the memory loop
is faster than the non-strict comparison because guarded native memory and loop
chaining avoid the old helper fallback cost.

### Current JIT Performance Improvements

The current RISC-V32 JIT is faster mainly because the translated block now keeps
hot guest registers in host registers for common RV32I/RV32M instructions. In
the older JIT path, many instructions loaded guest registers from `cpu.gpr[]`,
performed one operation, and stored the result back immediately. That is simple
and correct, but it spends a lot of time moving values between memory and host
registers. The register cache avoids most of those repeated loads and stores
inside one translated block, then flushes only the dirty guest registers before
leaving the block or calling a helper that can observe full CPU state.

Loads, stores, and branches also use the register cache now. This matters for
MicroBench-style code because tight loops usually repeat the same induction
variables, addresses, and compare operands. Keeping those values live across
several emitted x86-64 instructions lets the host CPU execute the loop body with
less memory traffic and fewer helper calls.

RV32M multiply/divide/remainder instructions are emitted directly where the
RISC-V edge cases can be represented cheaply in native code. The JIT still
preserves the specified behaviour for divide-by-zero and signed overflow, but
ordinary `mul`, `mulh`, `div`, `divu`, `rem`, and `remu` no longer need to exit
to the generic complex-operation helper in the common case.

The memory fast path is deliberately guarded rather than disabled. Native loads
and stores first check alignment and ordinary-PMEM conditions before the guest
destination register or memory side effect can happen. If the access would hit
MMIO, paging edge cases, source bytes, page-table pages, or another
trap-sensitive condition, generated code takes a helper side exit. If a write
can modify translated code, the invalidation logic discards the affected JIT
blocks before stale code can run.

Hot loops that contain normal loads and stores can now stay chained inside
generated code. The accounting still includes previous chained laps when a cold
trap or helper side exit occurs, so device polling and exception reporting stay
bounded while memory-heavy loops avoid repeated dispatcher returns. Larger native
blocks also reduce dispatch overhead for straight-line code without changing the
strict trap-ordering rule.

For the normal pass/fail performance check, run:

```bash
scripts/check-rv32-jit-performance.sh
```

That script sets `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`, then checks
the current `JITBENCH_ALU_MAX_US` and `MICROBENCH_MIN_MARKS` thresholds. To
collect comparable raw numbers manually, use the same dummy-driver environment:

```bash
source scripts/setup-env.sh

# Current JIT path.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/jitbench ARCH=riscv32-nemu run

# Current branch with JIT disabled at runtime.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Non-JIT performance branch.
git checkout performance_improve
source scripts/setup-env.sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# Original baseline branch.
git checkout legacy/baseline-master
source scripts/setup-env.sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

The speed-up calculation is simple division:

```text
speed-up = faster instr/s / slower instr/s
```

Example:

```text
2,687,376,608 / 58,319,798 = 46.08x
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
