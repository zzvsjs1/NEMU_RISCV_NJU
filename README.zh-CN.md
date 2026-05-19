# NEMU 全系统模拟器

## 概览

本项目基于
[南京大学 NEMU 全系统模拟器](https://github.com/NJU-ProjectN/nemu)。

NEMU 是一个轻量级全系统模拟器框架。上游项目支持多种客户机 ISA，并提供
Monitor、寄存器和内存查看、表达式求值、监视点、快照、DiffTest、分页、
中断、异常以及一组简化设备模型。

本仓库主要面向 RV32IM system mode 和当前 RISC-V64 Nanos-lite bring-up，
并加入了这个本地 RISC-V 项目需要的实际功能：

- `nemu`：模拟器核心、RISC-V32/RISC-V64 执行器、设备模型和可选 RISC-V JIT
  加速。
- `abstract-machine`：AM 运行时，以及 RISC-V NEMU 平台设备抽象。
- `nanos-lite`：用于在 NEMU 上运行 Navy 应用的小型 OS，同时服务 RV32 和当前
  RV64 bring-up 路径。
- `navy-apps`：用户程序、运行库、文件系统镜像生成、PAL 游戏集成，以及
  Nanos-lite 应用集在 RV64 上构建所需的修正。
- `am-kernels`：CPU 测试和 benchmark，包括 MicroBench 和 JITBench。
- `fceux-am`：FCEUX NES 模拟器的 AM 移植，可用于 native 和 RISC-V32 NEMU
  运行。

当前 `master` 分支是 RISC-V32/RISC-V64 性能改进版本。旧分支会保留下来作为
对比点，方便比较原始 baseline、磁盘/ONScripter 改动、非 JIT 性能改动、早期
JIT 版本和 RV64 bring-up 工作之间的行为与性能差异。

## RISC-V 执行设计

RISC-V 部分分成一个小而严格的架构参考路径，以及可选的 native 加速路径。参考
路径必须先保证正确：trap 顺序、CSR 副作用、特权级切换、设备可见的内存行为和
DiffTest 可见的 CPU 状态都以它为准。JIT 是建立在参考路径上的 fast path，只在
当前构建和运行状态足够简单、可以证明安全时执行 native code；遇到不确定情况时
会在指令产生部分提交之前退回解释器或 helper。

RV32 保留成熟的 direct interpreter、fast executor 和 x86-64 JIT。RV64 现在也
跟随新的 direct-interpreter 风格。旧的 RV64 table/RTL 拆分已经移除，RV64 指令
语义集中在一个直接解码文件中：

```text
nemu/src/isa/riscv64/inst.c
```

这个文件每次取一条 32 位 base instruction，从原始 instruction word 中解码
operand，并用直接 `INSTPAT` pattern 分发。pattern 周围的 helper 有明确的架构
职责：

- immediate helper 负责 I/S/B/U/J/CSR operand，并使用和硬件一致的符号扩展规则；
- alignment helper 会在寄存器写回或内存写副作用之前，先产生可见的
  instruction/load/store address-misaligned trap；
- CSR helper 做 implemented-CSR 检查、特权级检查、敏感 `mstatus` 字段规范化，
  并在本地 DiffTest reference 不能精确镜像副作用时跳过 reference；
- trap helper 会写入 `mepc`、`mcause`、`mtval`、`mstatus` 和特权级，再跳转到
  `mtvec`；
- multiply/divide helper 显式实现 RISC-V 对除零和有符号溢出的规定结果，避免依赖
  host 的未定义行为；
- `mret`、`wfi`、`sfence.vma`、`fence`、`fence.i`、CSR 指令、RV64I/RV64M、
  W-form integer 指令、jump、branch、load、store、`ecall`、`ebreak` 和私有
  `nemu_trap` 都在同一套 decode flow 中处理；
- 每条指令执行后都会把 `x0` 恢复为 0，避免 helper bug 把写入泄漏到架构零寄存器。

RV64 还有一个可选的 x86-64 JIT：

```text
nemu/src/isa/riscv64/jit.c
```

RV64 JIT 是保守设计。它只在受支持的 x86-64 native ELF 构建中可用，并且需要关闭
trace、watchpoint、memory/function trace 和 DiffTest。它的编译流程会记录 guest
取指背后的物理 source byte，记录 instruction-fetch page-table 依赖，在有限的
instruction budget 内生成 native block，并且只在 source/invalidation metadata
全部完成后发布该 block。Native block 返回已退休 guest instruction 数，并把
`cpu.pc` 留在下一条要执行的指令上，这样通用 CPU loop 仍能有界地检查设备和中断。

RV64 JIT 的三个关键安全机制是：

- Source invalidation 按 PMEM chunk 跟踪已编译的指令字节。普通 store 或 disk/DMA
  写入这些 chunk 时，会在旧代码再次运行前丢弃受影响的 block。
- Sv39 数据访问只有在检查 `satp`、effective privilege、`SUM`、`MXR`、VPN、
  access type、page offset、A/D/U/R/W/X 权限和 PMEM 范围之后，才允许命中小型
  tagged data TLB。TLB miss、MMIO、fault、跨页访问和不确定 PTE 都回到 C helper。
- Direct link 和 loop chaining 都带 guard。一个 block 只有在目标 cache slot 仍
  匹配 PC、`satp`、fetch privilege、必要时的数据 privilege state，以及
  instruction-fetch generation 时，才能跳到另一个 native block。Guard miss 会
  回到 C dispatcher，由它做完整验证。

## 分支角色

| 分支 | 作用 |
|------|------|
| `master` | 当前 RISC-V32/RISC-V64 性能版本 |
| `legacy/baseline-master` | 原始 baseline，早于磁盘、ONScripter、性能和 JIT 改动 |
| `legacy/onscripter-disk` | 旧的 Navy/ONScripter 磁盘支持分支 |
| `performance_improve` | 非 JIT 性能 baseline |
| `performance_improve_jit` | 当前 JIT 工作迁移到 `master` 前的旧名字 |

## 环境变量

在仓库根目录执行以下命令，初始化构建和运行环境：

```bash
source scripts/setup-env.sh
```

这个脚本会根据当前 checkout 自动导出路径：

```bash
export AM_HOME="$PWD/abstract-machine"
export NEMU_HOME="$PWD/nemu"
export NAVY_HOME="$PWD/navy-apps"
export ISA=riscv32
export ARCH=riscv32-nemu
```

如果手动设置变量，请把 `$PWD` 换成你自己的仓库根目录。这个脚本仍然默认使用
RV32；下面的 RV64 命令会显式覆盖 `ARCH`，或选择 RV64 NEMU defconfig。

## 支持的设备

| 设备 | 状态 | 说明 |
|------|------|------|
| 串口 | 支持 | 客户机输出通过 NEMU 串口设备写出。 |
| 时钟 / RTC | 支持 | 64 位微秒计时器，支持 AM uptime。 |
| 键盘 | 支持 | SDL scancode 队列映射到 AM 按键事件。 |
| 鼠标 | 支持 | SDL 移动、左/中/右键和滚轮事件会通过 AM 与 `/dev/events` 暴露。 |
| VGA / framebuffer | 支持 | ARGB8888 framebuffer，并通过 SDL 显示。 |
| 音频 | 支持 | SDL 音频后端，也支持 headless benchmark 用的 dummy 后端。 |
| 磁盘 | 支持 | Navy ramdisk 镜像作为磁盘，Nanos-lite 仍保留内嵌 ramdisk fallback。 |
| SD 卡 | 实验性 / 默认关闭 | 代码存在，但常规 workflow 使用 disk 设备。 |

## 设备实现细节

RISC-V NEMU 设备通过 NEMU 的 MMIO 区域暴露给客户机。AM 平台头文件
`abstract-machine/am/src/platform/nemu/include/nemu.h` 定义了客户机可见地址：

| 区域 | 地址 |
|------|------|
| 设备基址 | `0xa0000000` |
| 串口 | `0xa00003f8` |
| 键盘 | `0xa0000060` |
| RTC | `0xa0000048` |
| 鼠标 | `0xa0000070` |
| VGA 控制寄存器 | `0xa0000100` |
| 音频控制寄存器 | `0xa0000200` |
| 磁盘控制寄存器 | `0xa0000300` |
| Framebuffer | `0xa1000000` |
| 音频流缓冲区 | `0xa1400000` |

同一套 AM 代码仍然可以支持 x86 风格 NEMU 的 port I/O，但正常
`riscv32-nemu` 和 `riscv64-nemu` 路径使用 MMIO。

### 串口

客户机向 `SERIAL_PORT` 写字节。AM 通过 `putch()` 使用该设备，Nanos-lite
把它映射为 `/dev/serial`。在 native NEMU 运行时，字节会写到 host 的标准
错误输出，因此日志和客户机 console 输出不需要单独 UART 窗口也能看到。

### Timer 和 RTC

RTC 设备把 64 位微秒计数器拆成两个 32 位寄存器。AM 在启动时记录初始值，
之后用差值实现 `AM_TIMER_UPTIME`。NEMU 平台路径里的 `AM_TIMER_RTC` 返回固定
日期，因为本项目主要需要单调时间来支持调度、SDL timer 和 benchmark 计时。

NEMU system mode 会安装 alarm callback 来产生设备中断。CPU 执行循环会为了
性能批量检查设备更新，但批量大小是有上限的，避免 timer 和输入处理长期得不
到执行。

### 键盘和事件

NEMU 把 SDL scancode 转换成 AM key code，并存入一个小 FIFO。AM 读取
`KBD_ADDR`，按下事件会在 key code 上设置 `0x8000`。Nanos-lite 通过
`/dev/events` 暴露字符串事件，例如：

```text
kd A
ku A
```

F1、F2、F3 会被 Nanos-lite 拦截，用于切换前台应用。这些热键不会继续传给
用户程序。

### 鼠标

鼠标设备是 `MOUSE_ADDR` 上的 MMIO 事件 FIFO。Host SDL 的移动、按键和滚轮事件
会被记录成 AM 每次读取一个的事件。AM 通过 `AM_INPUT_MOUSE` 暴露该记录，
Nanos-lite 再输出 `/dev/events` 文本：

```text
mm X Y BUTTONS
md BUTTON X Y BUTTONS
mu BUTTON X Y BUTTONS
mw DX DY X Y BUTTONS
```

`NEMU_MOUSE_SCRIPT` 可以在 dummy driver 测试中注入确定性的鼠标事件，适合没有
真实 host 鼠标指针的自动化运行。

### VGA 和 Framebuffer

VGA 控制寄存器报告屏幕大小，取决于 `nemu/menuconfig` 中选择的 `400x300` 或
`800x600`。Framebuffer 是 `FB_ADDR` 上的线性 ARGB8888 MMIO 区域。AM/NEMU
地址映射为 `1024x768` ARGB8888 图像预留了足够的 framebuffer 空间，因此
`/dev/fb` 和 `AM_GPU_CONFIG.vmemsz` 可以覆盖更大的客户机侧缓冲区，并且不会
和音频流缓冲区重叠。AM 的 `AM_GPU_FBDRAW` 会把像素复制到 framebuffer，并在
需要 flush 时写 sync 寄存器。

`AM_GPU_MEMCPY` 和 Nanos-lite 的 framebuffer restore 路径都是软件实现的批量
复制，目标是这个映射 framebuffer。它们有用，是因为能减少客户机侧重复绘制
循环，但它们不是单独的硬件 GPU 加速器。

Nanos-lite 暴露：

- `/proc/dispinfo`：屏幕宽高；
- `/dev/fb`：framebuffer 写入。

为了支持前台应用切换，Nanos-lite 给每个可切换前台进程保存一份全屏 shadow
buffer。切回某个应用时，会恢复该应用上次的 framebuffer 状态，避免其他程序
留下的旧像素残留在屏幕上。

### 音频

音频设备包含频率、声道数、callback sample 数、流缓冲区大小、init 和已使用
字节数等控制寄存器。音频样本缓冲区是映射在 `AUDIO_SBUF_ADDR` 的 ring buffer。

AM 把样本字节写入 ring buffer 后，会向 count 寄存器写入一个 delta，也就是
本次新增的字节数。NEMU 在内部维护真实占用量，并用 SDL audio lock 和 host
callback 串行化更新，避免 host 正在消费音频时 guest 写回旧 count 导致状态
回退。

Nanos-lite 暴露：

- `/dev/sb`：写音频样本；
- `/dev/sbctl`：设置音频格式和查询剩余缓冲区。

由于 NEMU 进程只有一个 host SDL 音频设备，Nanos-lite 会记住每个前台应用的
音频格式。切回 PAL 这类应用前，会先恢复它自己的音频配置，再让它继续运行。

`CONFIG_AUDIO_DUMMY` 会保留 guest 可见的音频 MMIO 接口，但立即丢弃排队样本。
这适合无图形、无声卡或 benchmark 运行。

### Headless SDL、Dummy VGA 和 Dummy Audio

这里有两层不同的 dummy 机制，适合测试和 benchmark：

- Host SDL dummy driver：运行 NEMU 前设置 `SDL_VIDEODRIVER=dummy` 和
  `SDL_AUDIODRIVER=dummy`。这样 SDL 不需要 host 上真实的显示器或音频设备。
- NEMU 设备选项：如果不需要显示 SDL 窗口，可以关闭 `CONFIG_VGA_SHOW_SCREEN`；
  如果 guest 音频接口需要存在、但样本可以直接丢弃，可以打开
  `CONFIG_AUDIO_DUMMY`。

Benchmark 时使用 dummy video/audio，可以避免把 host 开窗口或音频设备初始化的
开销算进去。调试 GUI 正确性时，应保留真实 SDL video 路径，方便检查
framebuffer 画面。

### 磁盘和 Ramdisk

常规磁盘镜像路径是：

```text
$NAVY_HOME/build/ramdisk.img
```

NEMU 会优先打开这个镜像。如果不可用，可以 fallback 到 `nemu/menuconfig` 中的
`CONFIG_DISK_IMG_PATH`。磁盘控制器暴露 512 字节块，并使用客户机物理内存作为
DMA buffer。对于大小不是 512 字节对齐的镜像，最后一个块的填充区域会返回 0，
避免读到旧的客户机内存内容。

Nanos-lite 使用 `nanos-lite/src/disk.c` 作为高层 block 接口。如果没有 disk
设备，则 fallback 到内嵌 ramdisk 符号。对于大块对齐读取，它会批量读取多个
块，减少 MMIO 访问和 host 文件操作次数。

当磁盘读操作写入客户机内存时，RISC-V JIT invalidation 路径会让受影响的翻译
代码缓存失效。这样自加载代码或从磁盘加载的代码能和解释器路径保持一致。

### JIT 和设备正确性

JIT 只对普通 RAM 场景走 fast path。MMIO、PIO、trap、复杂虚拟内存情况和设备
副作用都会回到普通 NEMU memory / execution helper。这个约束很重要：JIT 变快
不能以跳过客户机可见设备行为为代价。RV64 也遵守同一规则：direct PMEM
load/store 会先经过 guard；translated/Sv39 场景要么命中完整 tagged data TLB，
要么回到 helper path。

## 构建和运行

配置 NEMU：

```bash
source scripts/setup-env.sh
make -C nemu ISA=riscv32 menuconfig
```

构建 NEMU：

```bash
source scripts/setup-env.sh
make -B -C nemu ISA=riscv32
```

常用依赖包括能用 `-march=rv32im_zicsr -mabi=ilp32` 生成 RV32IM+Zicsr 代码，
并能用 `-march=rv64im_zicsr_zifencei -mabi=lp64` 生成 RV64IM+Zicsr+Zifencei
代码的 RISC-V 工具链，以及 readline、ncurses、flex、bison。LLVM 只在打开
指令 trace/disassembly 相关构建时需要；本树会在 `CONFIG_ITRACE` 打开时使用
`llvm-config`，而 `nemu/llvm.sh` 当前默认安装 LLVM 18，也支持显式指定脚本中
列出的其他版本。

### RISC-V64 Nanos-lite Bring-up

RV64 路径当前目标是 `RV64IM_Zicsr_Zifencei`，ABI 是 `lp64`，用户态库使用
soft-float。RV64 构建会带上 `compiler-rt`，因为即使没有使用硬件浮点 ABI，
工具链仍可能生成一些 helper routine 调用。

在当前 RV64 路径上，先配置并构建 NEMU，再重建 Nanos-lite 磁盘镜像，并用
`riscv64-nemu` 运行：

```bash
source scripts/setup-env.sh
make -C nemu riscv64-am-sdl_defconfig
make -C nemu -j4
make -C nanos-lite ARCH=riscv64-nemu update
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C nanos-lite ARCH=riscv64-nemu run
```

当前 Nanos-lite RV64 的进程顺序会先启动 ONScripter，然后是 FCEUX，最后是 PAL。
这个顺序能让 framebuffer、disk、audio 和大型应用 loader 问题更早暴露，方便
bring-up 调试。常规 RV64 bring-up 配置使用 direct interpreter；需要单独测试
native acceleration 时，使用下面的 RV64 JIT defconfig。

## JIT 配置

RV32 和 RV64 JIT 菜单位于 `nemu/menuconfig`。这些菜单只在受支持的 x86-64 host
上的 native ELF interpreter 构建中可见，并且需要关闭 trace、watchpoint、
memory/function trace 和 DiffTest，因为这些功能依赖解释器的逐条指令 hook。

```text
RISC-V32 JIT
  [*] Enable RISC-V32 x86-64 JIT
  [ ] Collect RISC-V32 JIT statistics

RISC-V64 execution acceleration
  [*] Enable RISC-V64 x86-64 JIT
  [ ] Collect RISC-V64 JIT statistics
```

正常性能测试建议打开 JIT、关闭 JIT statistics。统计信息适合诊断，但额外计数
会带来开销。

RV64 常用 defconfig 如下：

```bash
# RV64 headless direct interpreter。
make -C nemu riscv64-am-headless_defconfig

# RV64 JIT，不带计数器。
make -C nemu riscv64-am-headless-jit_defconfig

# RV64 JIT，打开诊断计数器。
make -C nemu riscv64-am-headless-jit-stats_defconfig
```

运行时控制：

```bash
# 强制使用解释器 / 非 JIT 路径做对比。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 同一个开关可以在 RV64 JIT 构建中关闭 RV64 JIT。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run

# 保留 RV64 JIT，但关闭跨 block direct link。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  NEMU_DISABLE_RV64_JIT_DIRECT_LINK=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run

# 在 CONFIG_RV32_JIT_STATS 或 CONFIG_RV64_JIT_STATS 打开时打印 JIT 统计。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_JIT_STATS=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_JIT_STATS=1 \
  make -C am-kernels/benchmarks/coremark ARCH=riscv64-nemu run
```

`NEMU_DISABLE_JIT=1` 只会在 `CONFIG_RV32_JIT` 或 `CONFIG_RV64_JIT` 构建中关闭
JIT。它不是通用的“纯解释器”开关。如果 RV32 二进制是用
`CONFIG_RV32_FAST_EXEC` 构建的，CPU loop 仍然可以走 fast executor。要保证 RV32
纯解释器运行，需要重新构建 NEMU，并把 acceleration mode 设为
`CONFIG_RV32_ACCEL_NONE`。RV64 纯解释器运行可以使用
`riscv64-am-headless_defconfig`，或者关闭 `CONFIG_RV64_JIT`。

常用正确性检查：

```bash
scripts/check-nemu-arch-config-selection.sh
scripts/check-nemu-upstream-isa-selection.sh
scripts/check-riscv-difftest-state.sh
scripts/check-rv64-new-interpreter.sh
scripts/check-rv64-jit-correctness.sh
scripts/check-rv64-jit-io.sh
scripts/check-rv64-jit-wop.sh
scripts/check-rv64-jit-store-invalidation.sh
scripts/check-rv64-jit-performance.sh
```

## RISC-V 异常模型和限制

旧的教学版 NEMU + Nanos-lite 路径经常把 trap 当成模拟器或 AM 的约定处理。
这样 `ecall`、`yield()`、syscall dispatch 和私有 `nemu_trap` stop instruction
看起来像 emulator、Abstract Machine、Nanos-lite 之间的直接跳转。这个模型适合
小项目，但边界和真实 RISC-V 硬件不同：软件不一定能按架构观察到 `mepc`、
`mcause`、`mtval`、`mstatus`、`mtvec`、特权级检查和 `mret` 顺序。

当前 RISC-V 路径对普通异常使用架构 trap delivery。NEMU 写入 machine CSR、更新
特权级，然后跳到 `mtvec`。AM 的 RISC-V trap shim 保存 `Context`，调用 C trap
handler，再通过 `mret` 返回。Nanos-lite 的 syscall、yield、timer interrupt 和
user/kernel context switch 因此走同一套 CSR-backed trap frame。Syscall number
仍在 ABI 寄存器 `a7`，`mcause` 记录 trap 类型，例如 user ecall、machine ecall、
illegal instruction、breakpoint 或 misaligned address。

正确性的核心是 faulting instruction 不能部分提交。例如 misaligned load 必须在
写目的寄存器之前产生 load-address-misaligned trap，misaligned store 必须在改变
内存之前 trap。解释器、fast executor 和 JIT 都按这个顺序组织：hot native path
先做 guard，cold side exit 再产生和解释器一致的 CSR 可见 trap。

当前实现不是完整的 RISC-V unprivileged/privileged specification：

- RV32 interpreter 覆盖 RV32IM、CSR、`ecall`、`ebreak`、`mret` 和私有
  `nemu_trap`，但 base `FENCE`、Zifencei `FENCE.I`、`WFI`、`SRET`、
  `SFENCE.VMA` 仍走 illegal-instruction 路径。
- RV64 direct interpreter 覆盖 RV64IM、W-form integer operation、CSR、
  `ecall`、`ebreak`、`mret`、`wfi`、`sfence.vma`、`fence`、`fence.i` 和私有
  `nemu_trap`。它不实现 compressed、floating-point、vector、atomic、
  supervisor-return 或 hypervisor 指令。
- CSR model 是小型 machine-level subset，重点是 `satp`、`mstatus`、`mtvec`、
  `mscratch`、`mepc`、`mcause` 和 `mtval`。很多标准 CSR 还没有建模。
- Trap delegation 和 supervisor trap entry 还没有完整实现；普通 trap 进入
  M-mode，并写 machine CSR。
- RV32 Sv32 walker 仍是局部实现，很多 page-fault 情况还不是完整 guest-visible
  trap。RV64 的 Sv39 检查独立存在于 RV64 interpreter/JIT 路径，但也不是完整
  supervisor/hypervisor 平台模型。
- JIT 只适合关闭 trace、watchpoint、memory/function trace 和 DiffTest 的
  native ELF 运行。需要逐条指令调试或 reference 对比时，使用解释器路径。

## 性能测量

第一个表格是当前 RV32 JIT 分支的本地参考数据，测于 2026-05-16，并使用 dummy
SDL video/audio driver。它们适合看趋势，但你应该在自己的 CPU 上重新测量，
因为 host 频率调节、系统负载、温度限制，以及大小核调度都会影响结果。

| 分支 / 模式 | Benchmark | 结果 |
|-------------|-----------|------|
| `master`，strict exceptions，JIT 开启 | MicroBench | `27098 Marks`, `2,860,733,499 instr/s` |
| `master`，strict exceptions，JIT 开启 | JITBench | `ALU 8.436 ms`, `Memory 3.528 ms`, `4,049,658,568 instr/s` |
| `master`，strict exceptions，`NEMU_DISABLE_JIT=1` | MicroBench | `3322 Marks`, `275,864,060 instr/s` |
| `master`，strict exceptions，`NEMU_DISABLE_JIT=1` | JITBench | `ALU 157.041 ms`, `Memory 77.442 ms`, `302,722,837 instr/s` |
| exported non-strict `6d946ee`，JIT 开启 | MicroBench | `25041 Marks`, `2,480,000,000 instr/s` |
| exported non-strict `6d946ee`，JIT 开启 | JITBench | `ALU 7.304 ms`, `Memory 4.352 ms`, `4,520,000,000 instr/s` |
| `performance_improve` | MicroBench | `3141 Marks`, `271,000,633 instr/s` |
| `legacy/baseline-master` | MicroBench | `694 Marks`, `58,319,798 instr/s` |

下面是当前 RV32/RV64 CoreMark 解释器对比数据，测于 2026-05-17，并使用 dummy
SDL video/audio driver：

| 分支 / 模式 | Benchmark | 结果 |
|-------------|-----------|------|
| `riscv32`，`CONFIG_RV32_ACCEL_NONE` | CoreMark | `586 Marks`, `4980 ms`, `62,912,314 instr/s` |
| `riscv64`，解释器 | CoreMark | `1259 Marks`, `2319 ms`, `154,708,727 instr/s` |

这是干净的解释器对解释器比较，上面的 `NEMU_DISABLE_JIT=1` 行不能完整表达这一
点。在这次 CoreMark 运行中，RV64 的 Marks 大约是 RV32 纯解释器的 `2.15x`，
guest instruction throughput 大约是 RV32 纯解释器的 `2.46x`。RV64 在这个
workload 中执行的 guest instruction 仍然更多（`358,947,764` 对
`313,363,093`），但在这台 host 上 RV64 解释器路径更早完成。这个结论应理解为
本地 CoreMark 结果，不代表每个 RV64 应用都会更快。

如果和 RV32 JIT 行比较，RV64 解释器仍然慢很多：大约是 RV32 JIT MicroBench
throughput 的 `1/18.5`，也是 RV32 JITBench throughput 的 `1/26.2`。这个比较
有意是 interpreter versus JIT。当前 RV64 JIT 性能请在自己的 host 上运行
`scripts/check-rv64-jit-performance.sh` 获取。

按 Marks 计算，当前 strict JIT MicroBench 分数约为同分支关闭 JIT 的 `8.16x`、
非 JIT 性能分支的 `8.63x`、原始 baseline 的 `39.05x`。按 guest instruction
throughput 计算，当前 strict JIT 约为原始 baseline 的 `49.05x`。与上表中的
exported non-strict `6d946ee` 数据相比，本地 strict MicroBench 略高；JITBench
整体 throughput 略低，但 memory loop 更快，因为 guarded native memory 和 loop
chaining 避免了旧 helper fallback 的成本。

### 当前 JIT 性能改进

当前 RISC-V32 JIT 变快，主要因为翻译块现在会把常用 RV32I/RV32M 指令涉及的
hot guest register 保存在 host register 里。旧 JIT 路径里，很多指令都要从
`cpu.gpr[]` 读取 guest register，执行一次运算，再立即写回。这样实现简单且
正确，但会把大量时间花在 guest register memory 和 host register 之间搬运。
新的 register cache 会在同一个翻译块内部尽量复用这些值，只在离开翻译块或调
用可能观察完整 CPU 状态的 helper 前写回 dirty guest register。

Load、store 和 branch 现在也使用 register cache。MicroBench 这类 tight loop
经常重复使用相同的循环变量、地址和比较操作数，把这些值跨多条 x86-64 指令保
持在 host register 中，可以减少内存访问和 helper 调用。

RV32M 的 multiply/divide/remainder 指令在常见情况下也会直接生成 native
代码。JIT 仍然保留 RISC-V 对除零和有符号溢出的规定行为，但普通 `mul`、
`mulh`、`div`、`divu`、`rem` 和 `remu` 不再总是退出到通用复杂操作 helper。

Memory fast path 仍然有意保持很窄。只有地址是普通 guest RAM，并且访问不会
命中 MMIO、设备状态、trap 或复杂地址转换时，才直接访问 PMEM。如果写入可能改
变已经翻译过的代码，失效逻辑会丢弃受影响的 JIT block。这样同一个二进制仍能
运行 bare-metal AM 测试、Nanos-lite、Navy 应用和从磁盘加载的代码，同时让普
通 RAM-heavy loop 得到加速。

常规 pass/fail 性能检查使用：

```bash
scripts/check-rv32-jit-performance.sh
```

该脚本会设置 `SDL_VIDEODRIVER=dummy` 和 `SDL_AUDIODRIVER=dummy`，然后检查当前
`JITBENCH_ALU_MAX_US` 和 `MICROBENCH_MIN_MARKS` 阈值。手动收集可比原始数据时，
也建议使用同样的 dummy driver 环境：

```bash
source scripts/setup-env.sh

# 当前 JIT 路径。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/jitbench ARCH=riscv32-nemu run

# 当前分支运行时关闭 JIT。
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEMU_DISABLE_JIT=1 \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 非 JIT 性能分支。
git checkout performance_improve
source scripts/setup-env.sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 原始 baseline 分支。
git checkout legacy/baseline-master
source scripts/setup-env.sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

性能提升用简单除法计算：

```text
speed-up = 更快版本 instr/s / 更慢版本 instr/s
```

例如：

```text
2,687,376,608 / 58,319,798 = 46.08x
```

## Nanos-lite GUI 流程

常规 RISC-V32 Nanos-lite GUI workflow：

```bash
source scripts/setup-env.sh
cd nanos-lite
make ARCH=riscv32-nemu update
make ARCH=riscv32-nemu run
```

`make ARCH=riscv32-nemu update` 会重新构建 Navy apps、生成 ramdisk，并更新
Nanos-lite 的生成链接。`make ARCH=riscv32-nemu run` 会在 NEMU 下启动 GUI
程序。如果 GUI 窗口打开，交互后关闭窗口，终端正常结束即可视为运行成功。

PAL 游戏数据位于：

```text
navy-apps/fsimg/share/games/pal
```

清理或创建新 checkout 时，不要删除或重新生成这些数据。

## 清理

保守清理生成文件：

```bash
scripts/clean-build.sh
```

这个脚本会保留 NEMU menuconfig/autoconfig 状态、PAL 数据和源码改动。如果还想
清理 NEMU build 目录：

```bash
scripts/clean-build.sh --nemu
```
