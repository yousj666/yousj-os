# Yousj OS

从零写的独立操作系统。不基于 Linux 内核，不用一行 Linux 内核源码；
为了能跑 Linux 程序，内核实现一套与 Linux 兼容的系统调用接口
（接口兼容，代码独立，就像 Wine 跑 Windows 程序不等于 Windows）。
Windows EXE 通过 Wine 式用户态兼容层运行，目标是「常见 EXE 能跑」。

- 服务器版：无头，命令行 + 服务（**先做，2026-10-27 用户定版**）
- 桌面版：图形界面 + 终端（后做）
- 两个版本共用同一个内核
- 到时候开源（协议发布时定，发布需用户单次确认）

## 当前阶段

阶段 0（已完成）：仓库骨架、构建系统、核心接口锁定、三方分工。
阶段 1（进行中）：Bootloader + 最小内核（串口输出、内存管理、进程/调度、Shell 能跑）。
- ✅ 物理页分配器 PMM（2026-11-01）：`kernel/mm/pmm.c`，双位图
  （可用/已用分开，保留洞不会被误释放），`make test` 宿主机单元测试全过；
  kmain 启动后初始化 PMM 并自检一块页的分配/释放。
- ✅ 虚拟内存 VMM 逻辑（2026-11-01）：四级页表 map/unmap/translate/
  protect，宿主机单测全过；CR3 切换待 Bootloader 分页契约。
- ✅ **首次在 QEMU 虚拟机中真启动（2026-11-01，用户拍板虚拟机先行）**：
  `make run` 经 32 位 Multiboot 桩 + ELF64 内核模块链路启动，串口实测
  输出 kmain 到达、PMM 自检 PASS、VMM 自检 PASS。Multiboot 桩只是
  冒烟工具，正式 Bootloader 为自写 UEFI 应用（见 bootloader/DESIGN.md）。
- ⏳ 下一步：调度器（内核线程先行，设计见 docs/SCHED_DESIGN.md）、
  UEFI Bootloader 主线、init/Shell 上机。

## 开发分工（三方并行 + Muse 本人下场）

- **Muse（本人，总协调 + 内核核心）**：架构、核心接口定义
  （`kernel/include/`）、内核入口、内存管理与集成、进度汇报。
  不只协调，也写核心代码。
- **辅助旁聊（2026-11-01 用户授权）**：A 管 Bootloader 与驱动
  （只动 `bootloader/`、`kernel/drivers/`）；B 管构建工具与用户态
  （只动 `tools/`、`userland/`）；C 只做 VMM/调度设计文档。
  详见 `docs/DIVISION.md`。

接口以 `kernel/include/yousj/` 下的头文件为准，三方不得自行改接口，
要改先报 Muse 协调。

## 技术起点（阶段 0 定，可由用户否决）

- 内核核心：C（x86_64）+ 少量汇编；工具链先用沙箱现有的 gcc 13。
- 理由：gcc 现成、裸机代码资料最全、与后续 ELF/Linux 兼容层衔接最直接。
- 硬件起步方式待用户拍板：QEMU 先行还是直接真机。阶段 0 不依赖这项。
