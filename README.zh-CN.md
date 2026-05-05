# NEMU 全系统模拟器

## 概览

本项目基于
[南京大学 NEMU 全系统模拟器](https://github.com/NJU-ProjectN/nemu)。

NEMU 是一个轻量级全系统模拟器框架。上游项目支持多种客户机 ISA，并提供
Monitor、寄存器和内存查看、表达式求值、监视点、快照、DiffTest、分页、
中断、异常以及一组简化设备模型。

本仓库主要面向 RV32IM system mode。目录结构仍然保持 NJU 项目风格，同时
加入了这个 RISC-V32 项目需要的实际功能：

- `nemu`：模拟器核心、RISC-V32 执行器、设备模型和可选 JIT。
- `abstract-machine`：AM 运行时，以及 RISC-V32 NEMU 平台设备抽象。
- `nanos-lite`：用于在 NEMU 上运行 Navy 应用的小型 OS。
- `navy-apps`：用户程序、运行库、文件系统镜像生成，以及 PAL 游戏集成。
- `am-kernels`：CPU 测试和 benchmark，包括 MicroBench 和 JITBench。

当前 `master` 分支是 JIT 性能改进版本。旧分支会保留下来作为对比点，方便
比较原始 baseline、磁盘/ONScripter 改动、非 JIT 性能改动和 JIT 版本之间的
行为与性能差异。

## 分支角色

| 分支 | 作用 |
|------|------|
| `master` | 当前 RISC-V32 JIT 性能版本 |
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

如果手动设置变量，请把 `$PWD` 换成你自己的仓库根目录。

## 支持的设备

| 设备 | 状态 | 说明 |
|------|------|------|
| 串口 | 支持 | 客户机输出通过 NEMU 串口设备写出。 |
| 时钟 / RTC | 支持 | 64 位微秒计时器，支持 AM uptime。 |
| 键盘 | 支持 | SDL scancode 队列映射到 AM 按键事件。 |
| VGA / framebuffer | 支持 | ARGB8888 framebuffer，并通过 SDL 显示。 |
| 音频 | 支持 | SDL 音频后端，也支持 headless benchmark 用的 dummy 后端。 |
| 磁盘 | 支持 | Navy ramdisk 镜像作为磁盘，Nanos-lite 仍保留内嵌 ramdisk fallback。 |
| SD 卡 | 实验性 / 默认关闭 | 代码存在，但常规 workflow 使用 disk 设备。 |

## 设备实现细节

RISC-V32 设备通过 NEMU 的 MMIO 区域暴露给客户机。AM 平台头文件
`abstract-machine/am/src/platform/nemu/include/nemu.h` 定义了客户机可见地址：

| 区域 | 地址 |
|------|------|
| 设备基址 | `0xa0000000` |
| 串口 | `0xa00003f8` |
| 键盘 | `0xa0000060` |
| RTC | `0xa0000048` |
| VGA 控制寄存器 | `0xa0000100` |
| 音频控制寄存器 | `0xa0000200` |
| 磁盘控制寄存器 | `0xa0000300` |
| Framebuffer | `0xa1000000` |
| 音频流缓冲区 | `0xa1200000` |

同一套 AM 代码仍然可以支持 x86 风格 NEMU 的 port I/O，但正常
`riscv32-nemu` 路径使用 MMIO。

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

### VGA 和 Framebuffer

VGA 控制寄存器报告屏幕大小，取决于 `nemu/menuconfig` 中选择的 `400x300` 或
`800x600`。Framebuffer 是 `FB_ADDR` 上的线性 ARGB8888 MMIO 区域。AM 的
`AM_GPU_FBDRAW` 会把像素复制到 framebuffer，并在需要 flush 时写 sync
寄存器。

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

当磁盘读操作写入客户机内存时，RISC-V32 JIT 会让受影响的翻译代码缓存失效。
这样自加载代码或从磁盘加载的代码能和解释器路径保持一致。

### JIT 和设备正确性

JIT 只对普通 RAM 场景走 fast path。MMIO、PIO、trap、复杂虚拟内存情况和设备
副作用都会回到普通 NEMU memory / execution helper。这个约束很重要：JIT 变快
不能以跳过客户机可见设备行为为代价。

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

常用依赖包括 RISC-V32 Newlib 工具链，目标参数为
`-march=rv32im -mabi=ilp32`，以及 LLVM 15、readline、ncurses、flex、bison。

## JIT 配置

当前分支在 `nemu/menuconfig` 中加入了 RISC-V32 JIT 选项：

```text
Build Options
  [*] Enable RISC-V32 JIT
  [ ] Enable RISC-V32 JIT statistics
```

正常性能测试建议打开 JIT、关闭 JIT statistics。统计信息适合诊断，但额外计数
会带来开销。

运行时控制：

```bash
# 强制使用解释器 / 非 JIT 路径做对比。
NEMU_DISABLE_JIT=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 在 CONFIG_RV32_JIT_STATS 打开时打印 JIT 统计。
NEMU_JIT_STATS=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

## 性能测量

下面是当前 JIT 分支的本地参考数据。它们适合看趋势，但你应该在自己的 CPU 上
重新测量，因为 host 频率调节、系统负载、温度限制，以及大小核调度都会影响
结果。

| 分支 / 模式 | Benchmark | 结果 |
|-------------|-----------|------|
| `master`，JIT 开启 | MicroBench | `5069 Marks`, `494,788,583 instr/s` |
| `master`，JIT 开启 | JITBench | `183,037,705 instr/s` |

收集可比数据的命令：

```bash
source scripts/setup-env.sh

# 当前 JIT 路径。
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
make -C am-kernels/benchmarks/jitbench ARCH=riscv32-nemu run

# 当前分支运行时关闭 JIT。
NEMU_DISABLE_JIT=1 make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 非 JIT 性能分支。
git checkout performance_improve
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run

# 原始 baseline 分支。
git checkout legacy/baseline-master
source scripts/setup-env.sh
make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run
```

性能提升用简单除法计算：

```text
speed-up = 更快版本 instr/s / 更慢版本 instr/s
```

例如：

```text
494,788,583 / 200,000,000 = 2.47x
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
