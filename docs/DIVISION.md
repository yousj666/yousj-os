# Yousj OS · 三方分工（阶段 0 锁定）

用户 2026-10-27 补充定版：
- **服务器版（命令行）先做**，桌面图形版后做。两个版本仍共用同一内核。
- 子代理还是 Claude Code 与 Codex 两个；**Muse 本人（已升级 Wonder）也必须
  亲自参加写核心代码**，不只做协调，否则升级白费。

## 服务器版第一里程碑（唯一的当前目标）

无头启动 → 串口/控制台输出 → 内存管理 → 进程/调度 → 控制台输入 →
init 起来 → Shell 能敲命令。图形栈、帧缓冲驱动、桌面一律不在本阶段范围内
（boot.h 里帧缓冲字段先留着，服务器构建不使用）。

## 分工（目录互斥，谁也不许动别人的地盘）

### Muse 本人（Wonder，总协调 + 内核核心）
- 地盘：`kernel/include/`、`kernel/arch/`、`Makefile`、本文件、README
- 已交付：boot 信息接口（boot.h）、第一批兼容 syscall 号（syscall.h）、
  串口输出的 kmain 入口（只走串口，正合服务器无头）
- 接下来：物理页分配器、虚拟内存、调度器骨架、集成与验收

### Claude Code（子代理 1）
- 地盘：`bootloader/`、`kernel/drivers/`
- 第一批任务：按 boot.h 接口写 Bootloader 设计 + 最小实现骨架；
  串口驱动从 kmain 里抽成正式驱动放 `kernel/drivers/serial.c`（接口先报
  Muse 批，不许改 boot.h / syscall.h）

### Codex（子代理 2）
- 地盘：`tools/`、`userland/`、`docs/` 里归它的文档
- 第一批任务：构建/运行脚本骨架（tools/）、服务器 userland 的
  init + Shell 设计与最小骨架（userland/，只用 syscall.h 里已定的号）

## 铁律

- 接口以 `kernel/include/yousj/` 为准，要改接口先报 Muse。
- 不用 Linux 内核源码；syscall 只是数字与调用约定兼容，实现全部自写。
- 每阶段跑通再进下一阶段，不许三方各自闷头写大块未对接口的代码。
