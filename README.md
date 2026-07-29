# NEMU Full System Emulator

## Overview

This project is built on top of the
[Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).

NEMU is a lightweight full-system emulator framework. The upstream project can
support several guest ISAs and provides a monitor, register and memory
inspection, expression evaluation, watchpoints, snapshotting, differential
testing, paging, interrupts, exceptions, and a small set of emulated devices.

This repository focuses on RV32IM, RV32IMF, RV32IMFD, RV64IM, and RV64IMFD
system-mode execution, the x86 Nanos-lite path, and the local performance
experiments around RISC-V and IA-32 execution. It adds practical work needed by
this local full-system emulation project:

- `nemu`: the emulator core, RISC-V32/RISC-V64 executors, device models, and
  optional RISC-V and x86 JIT acceleration.
- `abstract-machine`: the AM runtime and the RISC-V NEMU device abstraction.
- `nanos-lite`: the small OS used to run Navy applications on NEMU for RV32,
  RV64, and x86.
- `navy-apps`: user programs, libraries, file-system image generation, PAL game
  integration, and RV64 build fixes needed by the Nanos-lite app set.
- `am-kernels`: CPU tests and benchmarks, including MicroBench and JITBench.
- `fceux-am`: an AM port of the FCEUX NES emulator for native and RISC-V32 NEMU
  runs.
- `scripts` and `tools`: repeatable checks, environment setup, cleanup helpers,
  and local build tooling.

The current `master` branch is the RISC-V32 and RISC-V64 performance-improved
version. Older branches are kept as comparison points, so behaviour and speed
can be compared across the original baseline, disk/ONScripter work, non-JIT
performance work, the early JIT version, and the RV64 implementation work.

## RISC-V Execution Design

The RISC-V side is split into a small architectural reference path and optional
native acceleration paths. The reference path must stay correct first: it owns
trap ordering, CSR side effects, privilege changes, device-visible memory
behaviour, and DiffTest-visible CPU state. The JIT paths are fast paths on top
of that reference model. They only execute native code when the current build
and runtime state are simple enough to prove safe, and they fall back before an
instruction can partially commit in uncertain cases.

RV32 keeps the mature direct interpreter, fast executor, and x86-64 JIT used by
the performance branches. RV64 now follows the newer direct-interpreter style as
well. The old RV64 table/RTL split has been removed, so RV64 instruction
semantics now live in one direct decode file:

```text
nemu/src/isa/riscv64/inst.c
```

In the current tree, `nemu/src/isa/riscv64` is a symbolic link to the shared
`nemu/src/isa/riscv32` implementation. This came from `8dc8207 Share RISC-V
interpreter and sync upstream files`: the path above is still the RV64 entry
point from the build system's point of view, but the implementation is shared.

That file fetches one 32-bit base instruction, decodes operands from the raw
instruction word, and dispatches through direct `INSTPAT` patterns. Helper
functions around the patterns are deliberately small and architectural:

- immediate helpers build I/S/B/U/J/CSR operands with the same sign-extension
  rules used by hardware;
- alignment helpers raise visible instruction/load/store address-misaligned
  traps before register writeback or memory write side effects;
- CSR helpers perform implemented-CSR and privilege checks, normalise sensitive
  `mstatus` fields, and skip the reference model where the local DiffTest path
  cannot mirror the side effect exactly;
- trap helpers write `mepc`, `mcause`, `mtval`, `mstatus`, and privilege state
  before redirecting to `mtvec`;
- multiply/divide helpers encode the RISC-V divide-by-zero and signed-overflow
  results explicitly, which avoids relying on host undefined behaviour;
- `mret`, `wfi`, `sfence.vma`, `fence`, `fence.i`, CSR instructions,
  RV64I/RV64M, W-form integer instructions, jumps, branches, loads, stores,
  `ecall`, `ebreak`, and the private `nemu_trap` are handled in the same decode
  flow;
- `x0` is restored to zero after each executed instruction, so helper bugs
  cannot leak a write into the architectural zero register.

RV64 also has an optional x86-64 JIT:

```text
nemu/src/isa/riscv64/jit-rv64-*.c
```

`nemu/src/isa/riscv64` is a symlink to the shared RISC-V source directory; the
files are named `jit-rv64-*` because this is the RV64 guest JIT path.

The RV64 JIT is conservative by design. It is available only for supported
x86-64 native ELF builds with tracing, watchpoints, memory/function tracing and
DiffTest disabled. Its compile pipeline records the physical source bytes behind
guest fetches, records instruction-fetch page-table dependencies, emits a native
block for a bounded instruction budget, and publishes the block only after the
source/invalidation metadata is complete. Native blocks return the number of
retired guest instructions and leave `cpu.pc` at the next instruction, so the
generic CPU loop can keep device polling and interrupt checks bounded.

The RV64 JIT has three important safety mechanisms:

- Source invalidation tracks compiled instruction bytes by PMEM chunks. Stores
  or disk/DMA writes into those chunks discard affected blocks before stale code
  can run again.
- Sv39 data translation uses a small tagged data TLB only after checking `satp`,
  effective privilege, `SUM`, `MXR`, VPN, access type, page offset, A/D/U/R/W/X
  permissions, and PMEM range. Misses, MMIO, faults, cross-page accesses and
  uncertain PTEs fall back to C helpers.
- Direct links and loop chaining are guarded. A block can jump to another native
  block only if the target cache slot still matches PC, `satp`, fetch privilege,
  data privilege state when needed, and instruction-fetch generation. A miss
  returns to the C dispatcher for full validation.

### Floating-Point Execution

The shared RISC-V interpreter implements the mandatory scalar instruction
families in the RISC-V `F` and `D` extensions, Version 2.2. Arithmetic uses the
RISC-V specialisation of Berkeley SoftFloat Release 3e rather than host
floating-point operations, so guest rounding modes, accrued exception flags,
NaNs, infinities, subnormal values, and fused operations do not depend on the
host floating-point environment.

| Configuration | XLEN | FLEN | Advertised floating-point extensions |
|---------------|------|------|----------------------------------------|
| `CONFIG_RV32_FPU=y`, `CONFIG_RV32_D` unset | 32 | 32 | `F` |
| `CONFIG_RV32_D=y` | 32 | 64 | `F`, `D` |
| `CONFIG_RV64_FPU=y` | 64 | 64 | `F`, `D` |

`RV32_D` depends on `RV32_FPU`; it cannot create a D-without-F configuration.
The interpreter exposes the configured extensions through a fixed `misa` view,
implements `fflags`, `frm`, and `fcsr`, rejects floating-point instructions when
`mstatus.FS=Off`, and marks floating-point state Dirty after an architectural
write.

RV32D is deliberately modelled as XLEN=32 with FLEN=64. Single-precision values
written into its 64-bit floating-point registers are NaN-boxed, and a malformed
box is treated as canonical binary32 NaN by computational instructions. Raw
loads, stores, and move instructions preserve their payload bits. D-format
arithmetic, fused operations, comparisons, classification, conversions, and
`FLD`/`FSD` are available without incorrectly enabling operations that require
a 64-bit integer register.

The RV32 and RV64 x86-64 JITs do not lower floating-point instructions to host
floating-point code. They end the native block before an F/D instruction and
resume in the interpreter, preserving SoftFloat and trap semantics. Focused JIT
tests and counters check both a floating-point instruction at block entry and a
native integer prefix immediately followed by floating point.

## x86 JIT Design

The x86 side is a separate IA-32 guest path. It has an interpreter in
`nemu/src/isa/x86/inst.c` and an optional x86-64 native JIT in:

```text
nemu/src/isa/x86/jit.c
```

The x86 JIT was introduced by `f7769c2 Add x86 JIT`, expanded through the
`Improve X86 JIT performance` series, hardened for paged fast paths in
`3473e79 Harden x86 paged JIT fast paths`, and refactored in
`a77584f Refactor X86 JIT`. The current `440c0d0 Reformatting codes` commit is
mostly formatting and should not be treated as the source of the JIT design.

The design is deliberately conservative. The interpreter remains the
architectural reference; the JIT decodes a focused IA-32 subset into a local IR,
emits host x86-64 bytes only for cases that are cheap to guard, and falls back
before unsupported or fault-sensitive instructions can partially commit. Native
blocks run inside the common `cpu_exec()` budget, return the number of retired
guest instructions, and let the normal CPU loop keep device polling and
interrupt checks bounded.

The x86 JIT has these main pieces:

- Build and runtime gates: it is available only on x86-64 native ELF
  interpreter builds with tracing, DiffTest, watchpoints, memory tracing, and
  function tracing disabled.
- A bounded decode/compile pipeline: cache lookup, block decode, optional trace
  compilation, native byte emission, source-byte recording, and cache publish.
- Helper semantics: decoded but awkward operations use helper calls for exact
  8/16/32-bit ALU, stack, branch, multiply/divide, shift, SETcc, MOVZX/MOVSX,
  and PIO behaviour.
- Source invalidation: translated blocks record physical source pages and source
  bytes. PMEM writes use reverse indexes to discard stale blocks, with a broad
  scan fallback when a fixed index overflows.
- Paged fast paths: normal non-PAE 4 KiB paging can use a private direct-mapped
  DTLB after guard checks. DTLB misses, MMIO, large pages, PAE, uncertain
  permission cases, cross-page accesses, and self-modifying writes leave native
  code or call the normal MMU/memory path.
- Chaining and traces: direct exits can be patched to compiled successors, and
  hot straight-line paths can become traces when runtime flags allow it.

Known x86 JIT design limitations:

- It is an IA-32 JIT only. It does not implement IA-32e, PAE, NX, x87, SSE,
  MMX, string instructions, non-flat segmentation, or a complete exception
  delivery model inside generated code.
- Opcode coverage is workload-driven. A missing opcode is expected to fall back;
  only a decoded/emitted opcode with wrong semantics is a correctness bug.
- The private DTLB is an optimisation, not an authority. Correctness depends on
  generation counters, flushes, guards, and fallback paths.
- Runtime feature flags are read once because generated code may bake ABI and
  guard layout decisions into native code. Changing environment variables after
  NEMU starts has no effect.
- Some performance features remain opt-in because their correctness/performance
  trade-off is still being validated. In particular,
  `NEMU_X86_JIT_NATIVE_IDIV` is not a default-on path.

## Branch Roles

| Branch | Role |
|--------|------|
| `master` | Current RISC-V32/RISC-V64 performance version |
| `legacy/baseline-master` | Original baseline before disk, ONScripter, performance, and JIT work |
| `legacy/onscripter-disk` | Legacy disk-backed Navy/ONScripter branch |
| `performance_improve` | Non-JIT performance baseline |

Older local histories or discussions may mention `performance_improve_jit`, but
that branch is not present in the current local/remote branch list. Use `master`
for the current JIT work unless you have an old checkout that still contains
that branch name.

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
own checkout. The setup script still defaults to RV32; the RV64 commands below
override `ARCH` or select the RV64 NEMU defconfig explicitly.

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

RISC-V NEMU devices are exposed through the NEMU MMIO area. The AM platform
header defines the guest-visible addresses in
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
| Audio stream buffer | `0xa1400000` |

The same AM code can still support port I/O for x86-style NEMU builds, but the
normal `riscv32-nemu` and `riscv64-nemu` paths use MMIO.

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
MMIO region at `FB_ADDR`. The AM/NEMU address map reserves enough framebuffer
space for a `1024x768` ARGB8888 image, so `/dev/fb` and `AM_GPU_CONFIG.vmemsz`
can cover larger guest-side buffers without overlapping the audio stream buffer.
AM implements `AM_GPU_FBDRAW` by copying pixels into that framebuffer and then
writing the sync register when a flush is requested.

`AM_GPU_MEMCPY` and the Nanos-lite framebuffer restore path are software-backed
bulk copies into that mapped framebuffer. They are useful because they avoid
extra guest-side drawing loops, but they are not a separate hardware GPU
accelerator.

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

When a disk read writes into guest memory, the RISC-V JIT invalidation path
discards affected translated code-cache entries. This keeps self-loading or
disk-loaded code consistent with the interpreter path.

### JIT and Device Correctness

The JIT only fast-paths ordinary RAM cases. MMIO, PIO, traps, unusual virtual
memory cases, and device side effects fall back to the normal NEMU memory and
execution helpers. This is important: a faster JIT must not skip guest-visible
device behaviour. RV64 follows the same rule: direct PMEM loads/stores are
guarded first, and translated/Sv39 cases either hit a fully tagged data-TLB
entry or return to the helper path.

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
using `-march=rv32im_zicsr -mabi=ilp32` and RV64IM with Zicsr/Zifencei using
`-march=rv64im_zicsr_zifencei -mabi=lp64`. The assembler must also accept
per-file `.option arch, +f` and `.option arch, +d` directives for the focused
floating-point tests. Other dependencies include readline, ncurses, flex, and
bison. Instruction tracing and instruction-queue disassembly now use the
Capstone path under `nemu/tools/capstone`; the old LLVM disassembler path was
removed when `8dc8207 Share RISC-V interpreter and sync upstream files`
replaced `nemu/src/utils/disasm.cc` with the current C implementation.

### RISC-V Floating-Point Support

The first floating-point build prepares a pinned Berkeley SoftFloat Release 3e
checkout with the RISC-V specialisation:

```bash
make -C nemu/tools/softfloat prepare
```

The normal AM and Nanos-lite ABIs remain `ilp32`/`lp64` soft float. The focused
tests place F/D instructions in local assembly blocks, so enabling the emulator
extension does not require a hard-float C library or change the process ABI.
Full `ilp32d` user-space libraries and floating-point process-context switching
are outside the current integration.

### RISC-V64 Nanos-lite

The RV64 emulator can expose `RV64IMFD_Zicsr_Zifencei`, while the current
Nanos-lite path retains the `lp64` soft-float userspace ABI. `compiler-rt` is
part of the RV64 build because some toolchain-generated helper routines are
needed even when no floating-point hardware ABI is used.

For the current RV64 path, configure and build NEMU, then rebuild the
Nanos-lite disk image and run it under `riscv64-nemu`:

```bash
source scripts/setup-env.sh
make -C nemu riscv64-am-sdl_defconfig
make -C nemu -j4
make -C nanos-lite ARCH=riscv64-nemu update
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C nanos-lite ARCH=riscv64-nemu run
```

The current Nanos-lite RV64 process order starts ONScripter first, then FCEUX,
then PAL. That order makes framebuffer, disk, audio, and large-app loader issues
show up quickly. The normal RV64 configuration uses the direct interpreter. Use
the RV64 JIT defconfigs below when testing native acceleration separately.

## JIT Configuration

The x86, RV32, and RV64 JIT menus live in `nemu/menuconfig`. They are visible
only for native ELF interpreter builds on supported x86-64 hosts when tracing,
watchpoints, memory/function tracing, and DiffTest are disabled, because those
features require interpreter per-instruction hooks.

```text
x86 JIT acceleration
  [*] Enable x86 x86-64 JIT
  [ ] Collect x86 JIT statistics

RISC-V32 JIT
  [*] Enable RISC-V32 x86-64 JIT
  [ ] Collect RISC-V32 JIT statistics

RISC-V64 execution acceleration
  [*] Enable RISC-V64 x86-64 JIT
  [ ] Collect RISC-V64 JIT statistics
```

Normal performance runs should keep JIT enabled and JIT statistics disabled.
Statistics are useful for diagnosis, but the extra counters add overhead.

Runtime controls:

```bash
# Force RV32 interpreter / non-JIT execution for comparison.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# The same switch disables the RV64 JIT in an RV64 JIT build.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run

# The same global switch also disables x86 JIT. x86 additionally accepts
# NEMU_X86_JIT=0, which disables only the x86 JIT runtime gate.
NEMU_DISABLE_JIT=1 make -C am-kernels/tests/cpu-tests ARCH=x86-nemu run
NEMU_X86_JIT=0 make -C am-kernels/tests/cpu-tests ARCH=x86-nemu run

# Disable RV64 direct cross-block links while keeping the rest of the JIT.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  NEMU_DISABLE_RV64_JIT_DIRECT_LINK=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run

# Print JIT counters when the matching CONFIG_*_JIT_STATS option is enabled.
NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH=x86-nemu run
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_JIT_STATS=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_JIT_STATS=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run
```

`NEMU_DISABLE_JIT=1` only disables JIT in a binary built with `CONFIG_X86_JIT`,
`CONFIG_RV32_JIT`, or `CONFIG_RV64_JIT`. It is not a general "pure interpreter"
switch. If an RV32 binary is built with `CONFIG_RV32_FAST_EXEC`, the CPU loop can
still use the fast executor. For a guaranteed pure RV32 interpreter run, rebuild
NEMU with the acceleration mode set to `CONFIG_RV32_ACCEL_NONE`. For RV64, use
`riscv64-am-headless_defconfig` or unset `CONFIG_RV64_JIT`.

x86-only runtime switches are useful when bisecting JIT behaviour:

```text
NEMU_X86_JIT=0                    disable x86 JIT after build
NEMU_X86_JIT_HELPERS=0            keep only the tiny native-only subset
NEMU_X86_JIT_VERIFY_SOURCE=1      re-check stored source bytes
NEMU_X86_JIT_TRACE=1              enable hot trace compilation
NEMU_X86_JIT_REGCACHE=1           enable the optional register cache path
NEMU_X86_JIT_PAGED_AGGRESSIVE=1   enable the grouped paged experimental paths
NEMU_X86_JIT_NATIVE_IDIV=1        enable native IDIV emission
NEMU_X86_JIT_BLOCK_LIMIT=<n>      clamp translated block length
NEMU_X86_JIT_TRACE_THRESHOLD=<n>  set the hot-trace threshold
```

Most default-on x86 knobs can be disabled with the explicit `=0` spelling, for
example `NEMU_X86_JIT_BATCH=0`, `NEMU_X86_JIT_CHAIN=0`,
`NEMU_X86_JIT_FAST_CHAIN=0`, `NEMU_X86_JIT_L0_CACHE=0`,
`NEMU_X86_JIT_PAGED_BATCH=0`, or `NEMU_X86_JIT_HIGH_BYTE_TEST=0`.

## RISC-V Exception Model

Older NEMU + Nanos-lite paths often treated traps as an emulator
or AM convention. In that model, `ecall`, `yield()`, syscall dispatch, and the
private `nemu_trap` stop instruction could look like direct control transfers
between the emulator, Abstract Machine, and Nanos-lite. That is practical for a
small project, but it is not the same boundary as real RISC-V hardware: software
does not necessarily observe architectural `mepc`, `mcause`, `mtval`,
`mstatus`, `mtvec`, privilege checks, or `mret` ordering for every faulting
instruction.

The current RISC-V paths use real architectural trap delivery for normal
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

- The RISC-V support is focused on the RV32IM, RV32IMF, RV32IMFD, RV64IM, and
  RV64IMFD NEMU/Nanos-lite workflows used by this tree. Scalar F/D support does
  not make this a complete privileged-platform model for every RISC-V
  extension.
- RV64 support is implemented for the local NEMU/Nanos-lite app workflow. It
  uses the direct interpreter, including scalar F/D when configured, as the
  reference path and can optionally use the RV64 x86-64 JIT for supported
  native ELF runs. Floating-point instructions leave the JIT before execution.
  The detailed Sv32 limitations below are RV32-specific unless they say
  otherwise; RV64 translated memory uses separate Sv39 checks in the RV64
  interpreter and JIT.
- Machine-mode trap handling is the main target. U-mode entry, U-mode ecall,
  CSR privilege checks, `mret`, and Sv32 paths are implemented for the local
  Nanos-lite workflow, but this is not a full supervisor-mode or hypervisor
  implementation.
- This EEI chooses visible traps for naturally misaligned scalar loads, stores,
  branches, and jumps. Code that expects invisible host-style misaligned memory
  emulation should not assume that behaviour here.
- The private `nemu_trap` instruction remains as the AM/NEMU test-exit
  convention. It is not a standard RISC-V exception.
- The JIT is available only for supported x86-64 native ELF RISC-V32/RISC-V64
  builds with tracing, watchpoints, memory/function tracing, and DiffTest
  disabled. Those debugging features require per-instruction interpreter hooks.
- The JIT fast path is intentionally conservative. MMIO, unsupported
  instructions, unusual translation cases, source-code writes, page-table
  writes, and trap-sensitive paths fall back to helpers or leave native code.
- Device timing is bounded by instruction-count polling, but it is not a
  cycle-accurate hardware timing model.

### Current RISC-V Spec Boundaries and Differences

The current RISC-V32/RISC-V64 interpreters and JITs do not implement every
extension and platform facility described by the RISC-V unprivileged and
privileged specifications. The important boundaries and differences are listed
here so tests and workloads can choose the right execution path deliberately.

- The shared direct interpreter decodes RV32IM or RV64IM, the RV64 W-form
  operations, configured scalar F/D instructions, CSR operations, `ecall`,
  `ebreak`, `mret`, `wfi`, `sfence.vma`, `fence`, `fence.i`, and the private
  `nemu_trap` stop instruction. It does not implement compressed, atomic,
  vector, supervisor-return, or hypervisor instructions.
- The configured scalar F and D instruction sets follow Version 2.2, including
  all mandatory arithmetic, fused, sign-injection, minimum/maximum, comparison,
  classification, conversion, load/store, move, rounding-mode, and accrued
  exception-flag behaviour. D depends on F. Compressed floating point, Q, Zfa,
  vector floating point, hard-float user-space ABIs, and native JIT
  floating-point lowering are separate extensions or integration work and are
  not provided.
- RV32 correctly rejects the XLEN=64-only `FCVT.L[U].S`, `FCVT.S.L[U]`,
  `FCVT.L[U].D`, `FCVT.D.L[U]`, `FMV.X.D`, and `FMV.D.X` instructions. The
  `FMVH.X.D` and `FMVP.D.X` pair moves belong to optional Zfa and are not part
  of base RV32D. RV64F/D retains the XLEN=64 conversions and whole-register
  doubleword moves.
- RV32D implements `FLD` and `FSD` as two little-endian 32-bit transfers after
  checking the architectural eight-byte alignment. Base D does not guarantee
  an atomic 64-bit memory operation when XLEN is less than 64, so software must
  not use an RV32D load or store as an inter-hart atomic primitive.
- This EEI raises load/store-address-misaligned exceptions for misaligned
  `FLW`, `FSW`, `FLD`, and `FSD`; no register or memory result is committed.
- The CSR model is a small machine-level subset: `satp`, `mstatus`, `mtvec`,
  `mscratch`, `mepc`, `mcause`, and `mtval`, plus `misa`, `fflags`, `frm`, and
  `fcsr` in floating-point builds. `misa` is a fixed WARL view of the configured
  extensions; writes leave that set unchanged. Standard CSRs such as `mie`,
  `mip`, `medeleg`, `mideleg`, `sstatus`, `stvec`, `sepc`, `scause`, and
  `stval` are not modelled.
- CSR writes mostly store raw values after implemented/writeable/privilege
  checks. Full WARL/WPRI behaviour is not applied for fields such as
  `mstatus.MPP`, `mtvec.MODE`, `mepc[1:0]`, or unsupported `satp` encodings.
  The fast executor avoids fast raw writes to the more sensitive CSRs, but the
  final architectural behaviour still follows the interpreter when it falls
  back.
- Timer interrupt delivery uses NEMU's internal pending-interrupt flag plus
  `mstatus.MIE`. Guest-visible `mie`/`mip` enable and pending bits are not
  implemented, so machine timer interrupt control is not the same as a standard
  privileged machine.
- Trap delegation and supervisor trap entry are not implemented. Normal traps
  enter M-mode through `mtvec` and write machine CSRs; there is no
  `medeleg`/`mideleg` routing to `stvec`, `sepc`, `scause`, or `stval`.
- The shared Sv32 walker in `nemu/src/isa/riscv32/system/mmu.c` handles only
  ordinary 4 KiB leaves with basic `V/R/W/X` checks. It does not complete the
  privileged page-walk rules for `U`, `SUM`, `MXR`, `A`, `D`, reserved
  `W=1,R=0` PTEs, non-leaf reserved bits, ASIDs, or 4 MiB megapages.
- Many Sv32 fault cases currently become NEMU assertions or panics instead of
  guest-visible instruction/load/store page-fault traps with `mtval`. This
  includes invalid PTEs, bad permissions, unsupported superpages, and translated
  accesses that cross a page boundary.
- The fast executor has stricter local Sv32 checks than the shared interpreter
  path for effective privilege, `U`, `SUM`, `MXR`, `A`, and `D`, but rejected
  cases still fall back to the shared memory path. Therefore it inherits the
  same incomplete architectural page-fault delivery described above.
- The JIT has a separate, simpler Sv32 fast path. Its local translation cache is
  keyed mainly by `satp` and VPN and its paged guards mostly check `V` plus
  `R/W/X`. It does not fully encode effective privilege, `MPRV`, `SUM`, `MXR`,
  `PTE.U`, `A`, or `D`, so JIT execution should not be used as the reference for
  full privileged/Sv32 conformance.
- In particular, JIT memory paths still use `satp.MODE` as the main signal for
  translation. Standard RISC-V treats `satp` as active only when the effective
  privilege mode is S or U; normal M-mode instruction fetches and M-mode
  loads/stores with `MPRV=0` are physical even if `satp.MODE=Sv32`.

## Performance Measurements

The current matrix is script-driven. The numbers below are local samples from
this checkout on 2026-05-27 with dummy SDL video/audio drivers. They are useful
for checking trend direction, but re-measure on your own CPU because host
frequency scaling, scheduler load, thermal limits, and performance-core /
efficiency-core placement can change the result.

| Scope | Benchmark | Mode | Result |
|-------|-----------|------|--------|
| RV32 JIT | MicroBench | JIT enabled | `25374 Marks`, `2,615,873,637 instr/s` |
| RV32 JIT | MicroBench | `NEMU_DISABLE_JIT=1` | `1531 Marks`, `130,571,528 instr/s` |
| RV32 JIT | JITBench | JIT enabled | `ALU 7.052 ms`, `Memory 3.559 ms` |
| RV32 JIT | JITBench | `NEMU_DISABLE_JIT=1` | `ALU 367.962 ms`, `Memory 164.339 ms` |
| RV64 JIT | BranchMark | interpreter | `guest_us=99327`, `checksum=0x67c146ec` |
| RV64 JIT | BranchMark | JIT enabled | `guest_us=1037`, `avg_jit_entry_insns=20413.95`, `speedup=95.78x` |
| x86 JIT | MicroBench | JIT enabled | `18246 Marks`, `2,123,334,697 instr/s` |
| x86 JIT | MicroBench | `NEMU_DISABLE_JIT=1` | `336 Marks`, `31,123,524 instr/s` |

Current performance gates:

```bash
scripts/check-rv32-jit-performance.sh
scripts/check-rv64-jit-performance.sh
scripts/check-x86-jit-smoke.sh
scripts/check-x86-jit-helper-profile.sh
scripts/check-x86-jit-paged-memory-fastpath.sh
```

Historical local reference data is kept only as fixed comparison points. Do not
read these rows as current `master` measurements.

| Scope | Source / date | Benchmark | Mode | Result |
|-------|---------------|-----------|------|--------|
| RV32 JIT reference | `71c4588`, measured 2026-05-16 | MicroBench | JIT enabled | `27098 Marks`, `2,860,733,499 instr/s` |
| RV32 JIT reference | `71c4588`, measured 2026-05-16 | JITBench | JIT enabled | `ALU 8.436 ms`, `Memory 3.528 ms`, `4,049,658,568 instr/s` |
| RV32 JIT reference | `71c4588`, measured 2026-05-16 | MicroBench | `NEMU_DISABLE_JIT=1` | `3322 Marks`, `275,864,060 instr/s` |
| RV32 JIT reference | `71c4588`, measured 2026-05-16 | JITBench | `NEMU_DISABLE_JIT=1` | `ALU 157.041 ms`, `Memory 77.442 ms`, `302,722,837 instr/s` |
| RV32 non-strict export | `6d946ee`, measured 2026-05-16 | MicroBench | JIT enabled | `25041 Marks`, `2,480,000,000 instr/s` |
| RV32 non-strict export | `6d946ee`, measured 2026-05-16 | JITBench | JIT enabled | `ALU 7.304 ms`, `Memory 4.352 ms`, `4,520,000,000 instr/s` |
| RV32 non-JIT reference | `performance_improve`, measured 2026-05-16 | MicroBench | non-JIT | `3141 Marks`, `271,000,633 instr/s` |
| RV32 baseline reference | `legacy/baseline-master`, measured 2026-05-16 | MicroBench | interpreter | `694 Marks`, `58,319,798 instr/s` |
| RV32/RV64 interpreter reference | `c0d33ae`, measured 2026-05-17 | CoreMark | RV32 pure interpreter | `586 Marks`, `4980 ms`, `62,912,314 instr/s` |
| RV32/RV64 interpreter reference | `c0d33ae`, measured 2026-05-17 | CoreMark | RV64 direct interpreter | `1259 Marks`, `2319 ms`, `154,708,727 instr/s` |

The speed-up calculation is simple division:

```text
speed-up = faster result / slower result
```

For example, the current local x86 MicroBench sample is:

```text
18246 / 336 = 54.30x by Marks
```

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
