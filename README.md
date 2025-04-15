# NEMU Full System Emulator

# Overview

This project is build on top of [Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).
I implemented the part of it that is used to emulate the RISC-V instruction set and filled the missing code in the output devices.

This version is based on the 2022 edition and incorporates parts of the updates from the 2023 and 2024 versions. Currently, the RISC-V32 instruction set has been implemented. Context switching is currently under development.

The nemu directory contains the core of the emulator, abstract-machine provides the bare-metal runtime (and supports native execution to allow debugging), and am-kernel offers some test programs.

# NEMU Supported devices

- Serial [✅]
- Clock [✅]
- Keyboard [✅]
- VGA [✅]
- Audio [✅]
- SD Card [❌]
- Disk [❌]

# Build and run

## Build

Run `init.sh` or set three environment variables. 

- AM_HOME=Path to abstract mechine folder.
- ISA=riscv32
- NEMU_HOME=Path to nemu folder

Also, you need some packages:

- It is recommended to build a RISC-V32 toolchain based on Newlib, using rv32im as the -march and ilp32 as the ABI.
- LLVM-15
- readline
- libncurses
- flex
- bison

Then, go to nemu folder to run `make menuconfig` then `make`. It will build the NEMU.
For other program, please go to other folder to check the README.MD.

## Run

You can read the Makefile to see what it can run. Basiclly, use `make run` to run the nemu.
For program, use `make ARCH=$ISA-nemu run` for nemu, `make ARCH=native run` for native.

