# NEMU Full System Emulator

# Overview

This project is build on top of [Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).
I implemented the part of it that is used to emulate the RISC-V instruction set and filled the missing code in the output devices.

# Build and run

# Build

Run `init.sh` or set three environment variables. 

- AM_HOME=Path to abstract mechine folder.
- ARCH=riscv32
- NEMU_HOME=Path to nemu folder

Also, you need some packages:

- LLVM-18 (You may need to build this by your own and patch some files)
- readline
- libncurses
- flex
- bison

Then, go to nemu folder to run `make menuconfig` then `make`. It will build the NEMU.
For other program, please go to other folder to check the README.MD.

## Run

You can read the Makefile to see what it can run. Basiclly, use `make run` to run the nemu.

# More

To run the program, you need the riscv toolchain, 
see the official riscv instructions for details.
