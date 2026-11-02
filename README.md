# Yousj OS

从零写的独立操作系统。不基于 Linux 内核，不用一行 Linux 内核源码；
为了能跑 Linux 程序，内核实现一套与 Linux 兼容的系统调用接口
（接口兼容，代码独立，就像 Wine 跑 Windows 程序不等于 Windows）。
Windows EXE 通过 Wine 式用户态兼容层运行，目标是「常见 EXE 能跑」。

- 服务器版：无头，命令行 + 服务（**先做，2026-10-27 用户定版**）
- 桌面版：图形界面 + 终端（后做）
- 两个版本共用同一个内核
- 到时候开源（协议发布时定，发布需用户单次确认）

## 方针（2026-11-01 用户定版）

- **内核优先**：先把内核做好，完整系统（GUI、userland 大铺开）
  放后面；开源也先开源内核。
- **北极星**：内核设计求**快**与**干净**——精简、低开销、不堆
  臃肿抽象。长期目标是承载 Linux 程序生态（包括 AI/视频工具链
  这类负载），并且比 Ubuntu 更快、更干净。这是设计原则，
  不是当前阶段的功能清单。

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
- ✅ 调度器 M0+M1（2026-11-01）：协作式轮转内核线程；逻辑层宿主机
  单测全过，QEMU 实测两线程严格交替 10 轮并干净退出（串口自检 PASS）。
- ✅ 调度器 M2（2026-11-01）：IDT+PIC+PIT 100Hz 抢占跑通——从不让出
  的两个忙等线程被时钟轮转（1 秒内各推进约 6000 万/5200 万次计数），
  睡眠线程 3 次准时醒来；异常有串口转储不再静默三重故障。一键冒烟：
  tools/qemu-smoke.sh。
- ✅ VMM 硬件步 1（2026-11-01）：CR3 已从引导桩页表切换到内核自有
  主空间（全 RAM 恒等映射＋映像区可执行，NXE/WP 已开）；切换后在
  2GiB 处映射/写入/翻译/撤销冒烟通过，M1/M2 自检在新页表上照常 PASS。
- ✅ UEFI 启动链端到端打通（2026-11-01）：自写 BOOTX64.EFI 在
  QEMU/OVMF 下把内核送入 kmain，全部自检（PMM/VMM/CR3 切换/调度
  M1+M2）在 UEFI 链上与 Multiboot 冒烟链上双双 PASS。内核已自建
  GDT，不再借用启动环境的表（bootloader/Makefile 一键出 ESP 镜像）。
- ✅ 调度 M3（2026-11-01）：第一个 Ring 3 程序跑通——GDT 用户段＋
  TSS 落地，`syscall`/`sysret` 入口＋分发器（write/getpid/exit）
  上线，调度器按任务切换 CR3；扁平二进制用户程序在自有地址空间
  里打印出自己的 pid 并以状态 42 退出，Multiboot 与 UEFI 两条
  启动链均实测 PASS。
- ✅ 调度 M4（2026-11-02）：静态 ELF64 加载器上线，init 以真
  ELF 形象正式上岗（过渡方案：映像内嵌进内核）——banner＋
  `yousj-init: INIT_SMOKE_OK` 后进入串口命令循环（help/ps/
  uname/kill/exit）。syscall 批次 1 落地：read（串口控制台）/
  nanosleep/uname（sysname "Yousj"）/brk/匿名 mmap/munmap/
  arch_prctl（FS）/kill；exit 掉 init 触发内核 panic
  （"Attempted to kill init"）。验收：`make check`/`make test`、
  Multiboot 冒烟、串口交互实测（`tools/qemu-init-test.py`：
  help→ps→kill 演示线程→ps 验消失→exit 验 panic）与 UEFI 链
  全部 PASS。
- ✅ 调度 M5（2026-11-02）：进程模型打通——clone（fork 式，
  子空间全页拷贝、rax=0 复活）、execve（内嵌映像表 /init、
  /bin/sh、/bin/hello；先建新空间、成功才切换）、wait4
  （阻塞/WNOHANG、回收内核栈与地址空间、孤儿由 init 收养）。
  init 改任监管者：循环拉起独立 Shell 进程、回收、1 秒后
  重启；Shell（yousj-sh）有 help/ps/free/uname/echo/exit 内建
  命令与外部命令执行。验收：`tools/qemu-proc-test.py` 全项
  PASS，含多轮命令后 PMM 空闲页数对账归零（无泄漏），UEFI
  链到 Shell 提示符。
- ✅ 内存 M6（2026-11-02）：高半区＋HHDM 布局落地——内核改链
  接到 0xFFFFFFFF80000000（物理仍 1 MiB 装载，`-mcmodel=kernel`），
  主空间只含 HHDM 全 RAM 直映＋映像分段权限（.text RX、.rodata R、
  .data/.bss RW，全部 NX 按位），不再有任何恒等映射；
  `vmm_share_kernel_identity()` 退役，用户空间靠 create 时拷贝的
  主空间高半区运行内核代码。全部物理帧访问（页表后端、ELF
  装载、clone 拷贝、内核栈）统一走 HHDM 指针。两条启动链同步
  改造：Multiboot 桩早期表加 4 KiB 粒度映像映射＋64 位跳板
  （映像 1 MiB 基址非 2 MiB 对齐，巨页表项会落保留位）；UEFI
  加载器改为统一 V2P 差值校验＋交接页表（恒等 1 GiB＋映像
  高半区），经新增入口 `_kentry64_rdi` 进入。迁移中修复一处
  既有启发式：定时器帧选择子归正只认用户窗口
  [0x8000000000, 0x8003000000)，不再把高半区内核帧误判为用户
  帧。验收：`make check`/`make test`、Multiboot 冒烟、
  `tools/qemu-proc-test.py`（三轮 hello 后 PMM 对账归零）、
  UEFI 链到 Shell 提示符，全部 PASS。
- ✅ 调度 M7（2026-11-02）：信号机制落地——标准信号 1–31
  全套默认处置（终止/终止+core 位/忽略/停止/继续）＋捕获型
  投递。投递只发生在两个回用户态闸门：syscall 出口与定时器
  中断返回（后者让纯自旋、不做 syscall 的程序也能收到处理
  器信号）。rt_sigaction/rt_sigprocmask/rt_sigpending/
  rt_sigreturn 按 Linux 布局实现（rt_sigframe 含 ucontext＋
  siginfo，restorer 契约）；kill(62) 改为真投递（sig=0 探活、
  内核线程保留阶段一语义）；子进程退出向父进程发 SIGCHLD
  （带 si_pid/si_status）；阻塞型 read/nanosleep/wait4 支持
  -EINTR 与 SA_RESTART（经 -ERESTARTSYS 内部标记＋sigreturn
  回绕 syscall 指令实现）；SIGSTOP/SIGCONT 停启、掩码与处置
  的 clone/exec 继承规则齐备。验收：`tools/qemu-signal-test.py`
  驱动 shell 跑 /bin/sigtest 十一场景全 PASS（handler/mask/
  default-term/stop-cont/sigkill/eintr/restart/sigchld/
  irq-deliver/clone-inherit/exec-reset），跑完 PMM 对账归零。
  施工中连带修复两个 M5 遗留的真 bug：①clone 子进程寄存器
  快照缺被调用者保存寄存器（rbx/rbp/r12–r15）——继续执行的
  子进程（而非立即 exec）会带着垃圾寄存器恢复，M5 验收只测
  了 fork 后立即 exec 而漏检，现经入口 shim 的 syscall 上下
  文补齐；②定时器中断不做 swapgs——从 Ring 3 帧调度到正
  阻塞在 syscall 里的任务时，其出口路径会踩到被遗落的用户
  GS 基址而 #PF，现 irq0_stub 对 Ring 3 帧进出各 swapgs
  一次，GS 纪律与调度顺序彻底解耦。
- ✅ 调度 M8（2026-11-02）：FPU/SSE 状态落地——启动时开启
  CR4.OSFXSR/OSXMMEXCPT 并由 CPU 自身状态捕获 pristine 镜像
  （x87 复位态＋MXCSR 0x1F80，异常全屏蔽）；每任务内嵌 512B
  FXSAVE 区，创建即 pristine（调度层弱钩子播种，杜绝首调
  度装入全零镜像）、clone 继承父进程实时状态、execve 重置；
  任务切换在 arch_on_task_switch 里对用户任务急切 save/
  restore（内核自身 -mno-sse 从不碰 FPU，内核线程不参与）。
  信号帧带真 fpstate：投递时 fxsave 入帧，sigreturn 时
  fxrstor 恢复——处理函数可以任意踩 FP 寄存器。用户态自
  此可自由使用 SSE（fxtest 专门以启用 SSE 的构建验证）。
  验收：`tools/qemu-fx-test.py` 驱动 /bin/fxtest 五场景全
  PASS（基础双精度位精确、双进程 xmm 跨切换隔离、MXCSR
  屏蔽下除零得 +inf 不触发 #XM、信号帧 fpstate 真实性与
  sigreturn 恢复、exec 重置），PMM 对账归零。
- ✅ 调度 M9（2026-11-02）：文件系统落地——内存 ramfs
  （目录/普通文件树、文件内容存 PMM 页、单文件上限 256KiB）
  ＋每任务 16 项 fd 表＋open-file 对象层：open/openat/close/
  lseek/getdents64/mkdir/chdir/getcwd 按 Linux 语义与错误码
  实现，read/write 改为按 fd 路由（控制台仍是串口，含原有
  阻塞/EINTR 语义）。fork 复制 fd 表并共享 open-file 对象
  （偏移共享，与 Linux 一致）；进程退出关闭全部 fd；启动时
  内嵌程序映像装入 ramfs（/init 与 /bin/{sh,hello,sigtest,
  fxtest,fstest}），**execve 改为从文件系统加载**（经 PMM
  连续页暂存——HHDM 让连续物理页天然连续虚拟），M5 的内嵌
  映像注册表彻底退役。验收：`tools/qemu-fs-test.py` 连跑
  两遍 /bin/fstest 十场景全 PASS（跨运行持久化、写读回、
  append、截断、seek、getdents、错误码、fork 共享偏移、
  mkdir+cwd、openat），两遍之间 PMM 计数稳态相等；冒烟/
  进程/信号/FX/UEFI 五链不回归。已知边界：无持久化存储
  （关机即失）、无 dup/fcntl、O_CLOEXEC 未实现（fd 跨 exec
  保留）、路径上限 255、无符号/硬链接。
- ✅ 调度 M10（2026-11-02）：管道与描述符复制——匿名管道
  （4KiB 环形缓冲、端计数按 open-file 对象计，阻塞读写走
  2-tick 轮询睡眠＋信号 -ERESTARTSYS 协议，写无读者发
  SIGPIPE 并返回 -EPIPE，非阻塞走 -EAGAIN）；pipe(22)/
  pipe2(293)、dup(32)/dup2(33)/dup3(292)；close-on-exec
  全套（open/pipe2/dup3 设置、execve 提交点关闭、fork 继
  承掩码）。fd 分配跳过未关闭的隐含控制台槽。Shell 支持
  重定向（> >> < 2>）与单管道 |（状态取右侧命令），echo
  内建支持 > / >>（dup 保存/恢复 stdout）；新增 /bin/cat。
  验收：`tools/qemu-pipe-test.py` 中 /bin/pipetest 六场景
  （基础读写、阻塞 EOF、无读者 EPIPE、非阻塞填满 4096、
  dup 共享偏移、cloexec 跨 exec 消失）连跑两遍全 PASS＋
  PMM 稳态；shell 级九项检查（重定向落盘、cat 读回、管
  道、stdin 重定向、echo 落盘与追加）全 PASS；七链不回归。
- ⏳ 下一步候选：mmap 文件映射、持久化后端（initramfs/磁
  盘，让 ramfs 重启不丢）、poll/select、符号链接。

## 从零构建

依赖只有三样：`gcc`、`make`、`qemu-system-x86_64`（UEFI 链另需
`x86_64-w64-mingw32-gcc`、`mtools` 与 OVMF 固件）。

```sh
make check            # 内核核心 freestanding 编译检查
make test             # 宿主机单元测试（PMM / VMM / 调度逻辑）
make kernel           # 构建 ELF64 内核 + Multiboot 冒烟桩
tools/qemu-smoke.sh   # 一键冒烟：构建 + QEMU 启动 + 串口断言
make -C bootloader esp  # 构建 UEFI 链：BOOTX64.EFI + ESP 镜像
```

许可证：待定（开源发布前由用户确认；确认前本仓库不对外发布）。

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
