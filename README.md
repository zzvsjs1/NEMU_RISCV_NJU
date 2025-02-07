# NEMU Full System Emulator

# Overview

This project is build on top of [Nanjing University NEMU Full System Emulator](https://github.com/NJU-ProjectN/nemu).
I implemented the part of it that is used to emulate the RISC-V instruction set and the output devices.

# Build and run

Firstly, you need to set three environment variables. 

- AM_HOME=Path to abstract mechine folder.
- ARCH=riscv32
- NEMU_HOME=Path to nemu folder

Then, go to each subproject to subprojects to run `make menuconfig` then `make`. It will build the NEMU or image which can be run on NEMU.

# More

To run the program, you need the riscv toolchain, 
see the official riscv instructions for details.
