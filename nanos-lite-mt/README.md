# nanos-lite-mt

`nanos-lite-mt` is the RV64 NEMU sibling of `nanos-lite`. It keeps one process
address space per application and schedules multiple user tasks inside that
address space. NEMU still emulates one RISC-V hart: timer interrupts provide
pre-emptive concurrency, rather than hardware SMP.

The target reuses unchanged loader, filesystem, memory, device, and syscall
services from `nanos-lite`. Its own `proc.c` adds:

- up to eight tasks per process, including the main task;
- independent 32 KiB kernel stacks and caller-supplied user stacks;
- timer-pre-emptive round-robin scheduling within the selected process;
- join, exit, kill, recursive mutex, and try-lock services;
- whole-process `execve()` replacement, including sibling-task cleanup; and
- the original process-based foreground framebuffer and audio ownership.

MiniSDL exposes the SDL 1.2 two-argument thread API with seven 64 KiB worker
stacks and 64 reusable mutex handles. Newlib selects a separate `_reent` record
for each worker, so `errno`, allocator state, and standard I/O locks remain safe
across pre-emption. The old single-thread runtime continues to use its original
global record and no-op locks until MiniSDL creates its first worker.

## Run the integration test

From the repository root:

```bash
source scripts/setup-env.sh
make -C nemu riscv64-am-headless_defconfig
make -C nemu -j4
make -C nanos-lite-mt ARCH=riscv64-nemu \
  NANOS_INIT=miniSDL-thread-test update run
```

Use `riscv64-am-headless-jit_defconfig` instead to exercise the RV64 JIT. A
successful run prints:

```text
miniSDL-thread-test PASS
```

The test covers timer pre-emption without cooperative syscalls, distinct task
IDs and stacks, join results, kill/reuse, recursive mutex contention, repeated
task-slot reuse, concurrent `malloc`, and thread-local `errno`.

For the normal Doom, ONScripter, and FCEUX image, omit
`NANOS_INIT=miniSDL-thread-test`.

## Limits

- Only `ARCH=riscv64-nemu` is accepted.
- There are at most eight live tasks per process and 128 active kernel mutex
  keys.
- MiniSDL has seven worker handles and 64 public mutex handles.
- The Navy RV64 ABI is soft-float. Task contexts save all integer registers,
  but do not save floating-point registers or `fcsr`; hard-float worker code is
  therefore unsupported.
- Audio writes are bounded in the MT kernel. NDL retries a full ring after a
  user-mode `sched_yield()`, while the original `nanos-lite` blocking behaviour
  remains unchanged.
