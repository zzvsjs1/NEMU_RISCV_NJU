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
