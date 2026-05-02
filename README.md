# NEMU Full System Emulator

## Overview

This project is built on top of the [Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).

NEMU is a lightweight, full-system emulator framework that supports x86 (without real mode or x87 FPU), MIPS32 (without CP1 FPU), RV32IM, and RV64IM. It provides a built-in monitor with features such as single-step execution, register and memory inspection, symbol-free expression evaluation, watchpoints, snapshotting, and differential testing against Spike or QEMU. Optional features include paging, a TLB, basic interrupt/exception handling, and simplified support for serial, timer, keyboard, VGA, and audio devices via port-mapped or memory-mapped I/O.

This project implements the RV32IM architecture.

It is based on the 2021 version of NEMU and includes selected updates from the 2023 and 2024 versions. The full RV32IM instruction set is currently implemented. Context switching is under active development.

- The `nemu` directory contains the core of the emulator.
- The `abstract-machine` directory provides a bare-metal runtime (and supports native execution for debugging).
- The `am-kernel` directory contains several test programs.

## Supported Devices

| Device    | Status   |
|-----------|----------|
| Serial    | ✅        |
| Clock     | ✅ (Simple) |
| Keyboard  | ✅        |
| VGA       | ✅        |
| Audio     | ✅        |
| SD Card   | ❌        |
| Disk      | ❌        |

## Build and Run

### Build

First, set the following environment variables:

- `AM_HOME`: Path to the abstract-machine folder  
- `ISA`: `riscv32`  
- `NEMU_HOME`: Path to the nemu folder  
- `NAVY_HOME`: Path to the navy-apps folder  

For local Codex sessions, the repository includes a sourceable helper:

```bash
source scripts/codex-env.sh
```

This exports `AM_HOME`, `NEMU_HOME`, `NAVY_HOME`, `ISA=riscv32`, and
`ARCH=riscv32-nemu` based on the current checkout path. It only affects the
current shell.

#### Dependencies

Make sure the following packages are installed:

- A RISC-V32 toolchain based on **Newlib**, using `-march=rv32im` and `-mabi=ilp32`. This is required because other toolchains may not include the software multiplication and division library.
- `LLVM-15`
- `readline`
- `libncurses`
- `flex`
- `bison`

#### Building NEMU

Navigate to the `nemu` directory and run:

```bash
make menuconfig
make
```

This will build the NEMU emulator.

To build and run other programs, refer to the `README.md` files in their respective directories.

### Run

You can check the `Makefile` for available commands. In general:

- To run NEMU:  
  ```bash
  make run
  ```

- To run a program on NEMU:  
  ```bash
  make ARCH=$ISA-nemu run
  ```

- To run natively:  
  ```bash
  make ARCH=native run
  ```

You can also debug using GDB.

### Nanos-lite GUI Flow

The usual riscv32 Nanos-lite GUI workflow is:

```bash
source scripts/codex-env.sh
cd nanos-lite
make ARCH=riscv32-nemu update
make ARCH=riscv32-nemu run
```

`make ARCH=riscv32-nemu update` rebuilds the Navy ramdisk and updates the
generated Nanos-lite links to `ramdisk.img`, `files.h`, and `syscall.h`.
`make ARCH=riscv32-nemu run` builds and runs Nanos-lite under NEMU.

The PAL application needs its game data under
`navy-apps/fsimg/share/games/pal`. Keep that data when creating isolated
checkouts or worktrees; the cleanup helper intentionally does not remove it.

### Cleanup

For a conservative cleanup of generated artifacts:

```bash
scripts/codex-clean.sh
```

This cleans Navy application build output, AbstractMachine build output, and
Nanos-lite build output while preserving NEMU menuconfig/autoconfig state,
PAL data, and source changes. To also clean the NEMU build directory:

```bash
scripts/codex-clean.sh --nemu
```

### RISC-V32 VME Notes

This project uses the AM VME abstraction for virtual memory:

- `protect()` creates a process address space.
- `map()` fills virtual-page to physical-page mappings.
- `vme_init()` builds the kernel address space and enables paging through
  `satp`.
- `ucontext()` creates a user context and records the process page-table
  pointer.
- `__am_irq_handle()` saves the current address-space pointer and switches to
  the scheduled context's address space before returning.

For Sv32, a virtual address is split into `VPN[1]`, `VPN[0]`, and page offset.
The root page-table PTE address is:

```text
root_page_table + VPN[1] * 4
```

For example, a user stack address near `0x7ffffxxx` has `VPN[1] == 511`.
If NEMU tries to translate that address through the kernel root page table
(`kas`), it will read `kas + 511 * 4`. That entry should be invalid, because
the kernel address space does not own the process user-stack mapping. This is
a strong sign that the wrong page table is active, not necessarily that
`map()` forgot to map the stack.

Important invariants for this framework:

- User process code, heap, and stack virtual addresses must be translated
  through that process's page table.
- User page tables must include the copied kernel mapping so trap handling and
  kernel code remain reachable after entering the kernel.
- Kernel threads do not own a user address space. Their saved context should
  use a null page-table pointer, and address-space switching should leave the
  current page table unchanged for them.
- A scheduler-visible `Context *` should point to kernel-accessible memory.
  The saved user stack pointer belongs in `Context.gpr[2]`; the `Context`
  object itself should not depend on the current user page table.
- Page-table memory must not be shared with unrelated heap users such as
  debug-print buffers. Corrupted PTEs often appear as recognizable text or
  ANSI escape bytes when logging overwrites page-table pages.
