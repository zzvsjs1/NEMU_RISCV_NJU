# NEMU

NEMU(NJU Emulator) is a simple but complete full-system emulator designed for teaching purpose.
Currently it supports x86, mips32, riscv32 and riscv64.
To build programs run above NEMU, refer to the [AM project](https://github.com/NJU-ProjectN/abstract-machine).
See the [repository README](../README.md) for the current local RISC-V
configurations, floating-point standards boundaries, and verification commands.

The main features of NEMU include
* a small monitor with a simple debugger
  * single step
  * register/memory examination
  * expression evaluation without the support of symbols
  * watch point
  * differential testing with reference design (e.g. QEMU)
  * snapshot
* CPU core with support of most common used instructions
  * x86
    * real mode is not supported
    * x87 floating point instructions are not supported
  * mips32
    * CP1 floating point instructions are not supported
  * riscv32
    * RV32IM, RV32IMF, or RV32IMFD according to configuration
    * scalar F/D Version 2.2 uses Berkeley SoftFloat; D is optional and
      depends on F
    * the JIT calls the shared SoftFloat executor for all seven configured F/D
      major opcodes instead of using host floating-point arithmetic
  * riscv64
    * RV64IM or RV64IMFD according to configuration
    * successful non-memory F/D helper calls can continue in the current JIT
      block; floating-point loads and stores complete through architectural
      memory helpers and then end the block for fault, MMIO, page-table, and
      source-invalidation safety
* memory
* paging
  * TLB is optional (but necessary for mips32)
  * protection is not supported
* interrupt and exception
  * protection is not supported
* 5 devices
  * serial, timer, keyboard, VGA, audio
  * most of them are simplified and unprogrammable
* 2 types of I/O
  * port-mapped I/O and memory-mapped I/O
