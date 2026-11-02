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
- ✅ 调度 M11（2026-11-02）：initramfs 根文件系统——根
  文件系统改为标准的 newc 格式 cpio 归档（`tools/
  mkinitramfs.py` 在构建时确定性生成：程序 ELF、rootfs/
  目录树），整包嵌入内核镜像，启动时由内核解包进 ramfs
  （目录 mkdir -p 式创建、普通文件落盘、其他类型跳过）。
  M9/M10 的逐程序 objcopy 嵌入（8 个 *_elf_bin.o）整体退
  役，kmain 启动 /init 也改走文件系统（PMM 暂存加载，与
  execve 同路）。Shell 启动会打印 /etc/motd（数据文件首
  次随根文件系统入驻）。两条启动链共用同一镜像、零引导
  层改动。验收：fs 验收新增 motd 检查（启动打印＋cat 读
  回），其余七链全绿不回归。诚实的边界：这是 initramfs
  ——镜像即根文件系统的源头，运行时改动仍不跨重启；真
  持久化需要块设备驱动＋磁盘文件系统（后续里程碑）。
- ✅ 调度 M12（2026-11-02）：真持久化——ATA PIO 块驱动
  （kernel/drivers/ata.c：双总线探测、IDENTIFY、LBA28 轮
  询读写、写后 FLUSH CACHE）＋磁盘文件系统 yfs（kernel/
  fs/yfs.c：固定几何——超级块/128 个 inode/位图/4KiB 数
  据块、12 个直接指针、目录即文件、**全写直通无缓存**，
  系统调用返回即落盘）。启动时探测磁盘：有 yfs 魔数则挂
  载、全零盘则格式化后挂载、都没有则 /data 不可用（其余
  功能不受影响）；挂载到 /data，fs 层按路径前缀分发后端
  （open/read/write/seek/getdents/mkdir/chdir/execve 全
  通，程序也能直接从 /data 执行）。新增 /bin/mkdir、
  /bin/ls。验收：`tools/qemu-disk-test.py` 两阶段——写文
  件后**硬杀 QEMU（模拟断电）**，重启后文件完好、追加落
  在原文之后、子目录与 ls 均正常；UEFI 链带数据盘（副总
  线）启动并正确跳过 FAT 格式的 ESP 盘完成格式化挂载；
  其余八链不回归。已知边界：单文件上限 48KiB、无删除/
  改名、无日志（断电恰逢元数据写一半可能损坏该次操作
  涉及的结构）、yfs 无并发锁、ATA 仅主盘 LBA28 PIO。
- ✅ 调度 M13（2026-11-02）：删除与改名＋yfs 大文件。
  unlink(87)/unlinkat(263)/rmdir(84)/rename(82) 两后端
  齐全；带 POSIX 语义的延迟释放——文件被删后，已打开的
  描述符仍可读到原内容，存储在最后一个 close 时才真正
  回收（ramfs 节点引用计数／yfs inode 的 rsvd 计数双路
  实现）；rename 同后端内任意目录间移动，跨后端 -EXDEV，
  覆盖目标按常规规则处理。yfs 升级为 11 个直接块＋1 个
  单级间接块（1024 项），单文件上限从 48KiB 提到约
  4MiB，格式版本升到 2（旧版磁盘会被当作异种盘跳过，
  不误挂）。新增 /bin/rm、/bin/rmdir、/bin/mv、
  /bin/bigfile（图案写/验工具）。验收：fstest 新增
  unlink/unlink-open/rename/rmdir 四场景全过；磁盘套件
  扩到三阶段——第二阶段写 100KiB 图案文件（跨直接/间接
  边界）、改名、删文件，硬杀后第三阶段逐字节验大文件、
  改名与删除结果均已落盘；其余八链＋UEFI 带盘链不回归。
  施工事故：yfs 的 inode 结构体数组维度写的是语义常量
  DIRECT，DIRECT 从 12 改 11 时结构体从 64 字节缩到
  60，而磁盘格式是 64——每次 inode 读都溢出 4 字节砸穿
  调用者栈帧（ls 打开 /data 必崩）。已写死为 12 并加
  _Static_assert 钉住 sizeof。
- ✅ 调度 M14（2026-11-02）：文件 mmap——mmap(9) 支持
  文件映射（此前仅匿名）：每任务 16 个 VMA 记录区域与
  后备文件（持有文件对象引用）；映射建立时**急切填充**
  （内容拷入新页、超 EOF 部分为零，无缺页异常路径）；
  MAP_PRIVATE 写不回文件；MAP_SHARED 在 msync(26)、
  munmap、exec、进程退出时经 VMM 翻译＋定位写回后备
  文件，ramfs/yfs 双后端一致；munmap 只整区释放（部分
  重叠 -EINVAL，不做 VMA 分割）；fork 时 VMA 随地址空间
  复制（页是深拷贝，跨进程共享一致性不在此阶段）。模型
  的已知边界：映射是快照——映射后经 write() 改文件，映
  射里看不到；共享映射的脏页在上述写回点之前对其他读
  者不可见。验收：`tools/qemu-mmap-test.py` 在带盘启动
  下跑 mmtest 两轮（anon/私有读/私有写隔离/ramfs 共享
  写回/yfs 共享写回/非零偏移六场景）＋PMM 稳态，其余
  九链不回归。
- ✅ 调度 M15（2026-11-02）：poll/select 全家——poll(7)、
  ppoll(271)、select(23)、pselect6(270) 共用一条就绪探
  针：普通文件恒就绪；管道读端有数据或对端已关（HUP）
  时可读，写端有空间时可写、无读者时报错（ERR）；控制
  台输入用 UART 状态位探活（不消费字节，串口驱动新增
  serial_rx_ready）。等待沿用 2-tick 轮询＋-ERESTARTSYS
  协议，信号闸门自动产出 Linux 的 EINTR/自动重启二分。
  select 遵循 Linux 的坏 fd 规则（任一被查 fd 无效直接
  -EBADF，poll 则逐个报 POLLNVAL），超时后回写剩余时
  间。验收：`tools/qemu-poll-test.py` 跑 polltest 两轮
  （文件恒就绪/空管道超时/数据与 HUP/写端三态/select
  基本面/信号中断长等待出 EINTR 六场景）＋PMM 稳态；
  其余十链不回归。注：磁盘套件一处断言在三机并行时出
  现过缓冲快照竞态（第二行输出未到就断言），已改为显
  式等待该行后单跑复验全绿，与内核无关。
- ✅ 调度 M16（2026-11-02）：符号链接两后端齐全——
  symlink(88)/symlinkat(266)/readlink(89)；ramfs 节点加
  is_link 标志、yfs 新增链接型 inode（type 3），目标文
  本都存为文件内容。路径解析层 fs_resolve_links 织入所
  有带路径入口：逐分量跟随（目标可绝对、可相对链接所
  在目录）、"." 与 ".." 归一、链深上限 8（超出 ELOOP）；
  open/stat/exec 跟随末分量，mkdir/unlink/rmdir/rename/
  readlink 不跟随末分量（删链接不伤真身）。解析纯在路
  径空间进行，链接可跨 /data 边界。getdents 给链接报
  DT_LNK，ls 显示 @ 后缀，新增 /bin/ln 与 /bin/readlink。
  验收：fstest 七场景（相对/绝对目标、链、环、悬空、
  readlink 本身与非链接 EINVAL、中间分量是链接、删链
  留真身）两轮全过；磁盘套件在 /data 建相对链接、两次
  硬杀断电后仍可读穿；其余十一链不回归。
- ✅ 调度 M17（2026-11-02）：mmap 真按需分页——mmap 只
  登记 VMA 不再分配/填充任何页；#PF 换上专属可返回桩
  （pf_stub：存寄存器＋pf_info＋GS 纪律与 irq0 同款，进
  出时按 irq0 同规则把 sysret 伪造的 CS/SS 修正为规范对，
  否则 iretq #GP），缺页时按页物化：文件页读当时的文件
  内容（超 EOF 部分为零）、匿名页为零页；PRIVATE 文件
  页只读进入、写时"写时复制"（帧从不共享/别名，COW 退
  化为权限翻转）；SHARED 回写只写硬件脏位为 1 的页并清
  脏（新增 vmm_query_raw 读原始 PTE、vmm_flush_tlb_va）。
  无 VMA 覆盖或权限不许的用户缺页走唯一退出路径以
  SIGSEGV 终结任务；内核缺页保持旧 dump 挂起。fork 只
  拷已物化页（窗口克隆天然如此）。验收：mmtest 新增
  lazy（mmap 后空闲页不变、首触才掉页、munmap 后全
  归还）、cow-flip、segv-wild（野指针）、segv-ro（写只
  读共享页）四场景与原六场景两轮全过；其余十一链不回
  归。边界：已物化页与 write() 的一致性仍不保证（无页
  缓存）；整页越过 EOF 只给零页、不发 SIGBUS。
- ✅ 调度 M18（2026-11-02）：epoll——epoll_create1(291)、
  epoll_ctl(233)、epoll_wait(232)、epoll_pwait(281，sigmask
  忽略）。实例是 fs 层对象（池 8 个、每实例至多 32 条兴
  趣），条目持有目标文件对象的引用（关 fd 不掉兴趣，与
  Linux 一致），close 实例时逐条放引用。就绪判定直接复
  用 M15 的探针（fd_ready 抽出 file 版），等待沿用
  2-tick 轮询＋-ERESTARTSYS，EINTR/重启由信号闸门分。
  电平触发；EPOLLET/EPOLLONESHOT 接受但按电平触发处理；
  嵌套 epoll（把 epoll fd 加入另一个 epoll）拒绝。
  验收：epolltest 六场景（基本就绪＋data 回传、超时、
  双 fd 甄别、DEL 静默与 MOD 换 data、HUP、信号中断长
  等待出 EINTR）两轮全过；其余十二链不回归。附带修复
  fstest 的 dents 辅助函数：/bin 程序数涨过单次
  getdents 缓冲后改为循环读尽（测试侧问题，内核无恙）。
- ✅ 一致性 M19（2026-11-02）：yfs 预写日志（格式版本
  3）——元数据操作（mknod/symlink/mkdir/unlink/rmdir/
  rename/truncate/延迟释放）全程事务化：操作期间所有扇
  区写先进内存暂存（读己之写），提交时暂存镜像按 LBA
  排序后整批写入日志区（扇区 25 起 1 头＋48 载荷），
  写头为提交点，随后镜像按连续段写回家、清头；失败的
  操作直接丢弃暂存，磁盘分毫不动。挂载时校验头与载荷
  的 FNV 校验和并回放已提交事务（全扇区重做、幂等）。
  语义边界（如实记档）：文件内容写（pwrite）不入日志
  （顺序模式：结构原子、内容断电时可为部分写）；打开
  计数 rsvd[0] 的增减不入日志；与 yfs 其余部分一样，
  事务不跨任务加锁。验收：disk 套件扩至五段 46 项——
  含日志头清零断言、churn 程序（建/删目录文件死循环）
  跑满 3 轮后在事务中途硬杀、第五段验哨兵文件完好、
  目录可列、探针文件可建可删、100KiB 大文件仍逐字节
  校验通过；其余十二链不回归。
- ✅ 接口 M20（2026-11-02）：stat 家族补齐——此前
  fs_path_stat 只供 execve/chdir 内部探询，用户态没有
  任何 stat 系统调用。现新增 stat(4)、fstat(5)、
  lstat(6)、newfstatat(262)：fs 层 fs_path_stat2 带
  跟随开关（lstat 与 AT_SYMLINK_NOFOLLOW 名副其实地
  返回链接自身：S_IFLNK＋目标长度为 size），fs_file_stat
  覆盖打开文件（控制台报字符设备、管道报 FIFO、epoll
  报普通文件）。newfstatat 支持 AT_FDCWD 与
  AT_EMPTY_PATH；相对 dirfd 暂回 ENOSYS（如实记档）。
  struct stat 按 x86_64 Linux 布局填写（mode/ino/size/
  nlink/blksize/blocks，时间为零——尚无时钟来源）。
  验收：stattest 六场景（文件 stat 与 fstat 一致、链接
  本体与目标对照、fstatat 双向、新建/缺失/悬空链接、
  控制台字符设备、AT_EMPTY_PATH）在 ramfs 与 yfs 两后
  端各全过（fs 套件两轮＋disk 套件 p2），其余各链不
  回归。
- ✅ 内存 M21（2026-11-02）：mprotect(10)——覆盖检查
  （区间每页必须属于某个 VMA，否则 ENOMEM；ELF 映像与
  栈不是 VMA，对其 mprotect 报 ENOMEM）、边缘 VMA 分裂
  （文件映射的 file_off 随分裂平移、文件引用逐片持有；
  VMA 总数仍上限 16，分裂无槽位时 ENOMEM）、已物化页
  同步改 PTE 并逐页刷 TLB。PROT_NONE 不丢内容：页帧保
  留、仅清 USER 位成主管态，访问照样触发段错误，日后
  恢复保护时内容完好。munmap 本就支持整段跨多 VMA 释
  放，分裂后一次 munmap 原始区间照常工作。相邻同保护
  VMA 不合并（如实记档）。验收：mmtest 新增三场景
  （整段改只读后子进程写触发 SEGV＋改回可写、中间页
  NONE 隔离＋内容保全＋跨片 munmap、间隙/未对齐/零长
  错误码）两轮全过，其余十二链不回归。
  排查中挖出一个既有隐患（如实记档，留待异常表恢复机
  制专项）：内核向用户内存写数据是 user_range_ok 后直
  接解引用，若目标页被 mprotect 成只读（或 M17 的只读
  文件页），内核态写会触发保护故障并停机——Linux 靠
  copy_to_user 异常恢复返回 EFAULT，我们尚无此机制；
  规范调用方（rusage 传 NULL 等）不受影响，本轮已把
  测试程序里经三参 helper 调 wait4（rusage 寄存器为垃
  圾值）的旧写法全部改为四参显式传 0。
- ✅ 内存 M22（2026-11-02）：brk 按需化——此前 brk 增长
  是急切的（当场把零页全映射好）。现增长只改记账
  （brk_end)，堆页由缺页路径按需物化：缺页处理在 VMA
  未命中时多一条堆分支——[brk_start, 按页取整的
  brk_end）内的地址按匿名读写页补上。收缩照旧回收已
  物化页（未触摸的页本就不存在）。另给堆顶加了硬顶：
  不得越过 MMAP_BASE，堆与 mmap 区从此井水不犯河水。
  顺带修了一个更老的真 bug（M22 验收逼出来的）：
  unmap_free_page 解映射后不刷 TLB，同任务内收缩再扩
  张（或 munmap 后重映射）会经陈旧翻译读到已释放帧的
  旧内容——现解映射必刷该 VA 的 TLB 条目，brk 与
  munmap 两条路同时受益。验收：新套件 brktest 四场景
  （增长零帧耗＋触摸逐页物化、收缩还帧＋重扩清零、
  越界请求原样返回旧 break、页对齐 break 处最后一字节
  可用/越界一字节 SEGV）两轮全过，其余十三链不回归
  （disk 套件首轮因与 UEFI 验收并行抢 CPU 出现 harness
  超时假失败，单独重跑 ALL OK）。
- ✅ 时间 M23（2026-11-02）：内核有了真实时钟。启动
  时读 CMOS RTC（等 UIP 落下、双读一致才采信；按
  Register B 解 BCD/二进制与 12/24 小时制）播种墙钟，
  之后由 100 Hz 节拍推进（RTC 按 UTC 对待，与 QEMU
  默认一致）。新系统调用：time(201)、
  gettimeofday(96)、clock_gettime(228；REALTIME 墙钟、
  MONOTONIC 开机节拍、其他 clock id 回 EINVAL)。
  stat 的时间戳不再恒为零：fs 层只跟踪一个 mtime
  （atime/ctime 暂同值）——ramfs 节点带内存字段，创建/
  写入/截断时打戳；yfs 把 mtime 存进 inode 一直闲置
  的 rsvd[1] 字段（借位不改格式：pwrite/截断/建 inode
  时打戳，随 inode 落盘、断电不丢）。验收：新套件
  timetest 六场景（epoch 与宿主机实测相差 3 秒内、
  time 指针式、gettimeofday 一致性、双时钟语义+坏
  clock id、mtime 创建即当下且写入 1.2 秒后前进）在
  ramfs 两轮、yfs（disk 套件第二段）各全过，其余
  十四链不回归。
- ✅ 健壮性 M24（2026-11-02）：copy_to_user 异常恢复
  ——M21 记档的那颗雷拆了。此前内核对用户指针只有
  范围检查：指向只读页或未映射窗口的出参（wait4 的
  rusage 是实测案例）会让内核在主管态缺页、当场停机，
  任何程序一个坏参数就能打死整机。现在任务结构里
  挂了一块恢复垫：调度器全程布防（__builtin_setjmp
  垫在调度帧里），缺页处理在「内核态、用户窗口地址、
  按需分页救不了、已布防」时 __builtin_longjmp 跳回
  垫子，系统调用干干净净返回 -EFAULT。用户态缺页不
  走这条路，真段错误照旧 SIGSEGV；rt_sigreturn 绕过
  调度器，它自带一块垫（帧指针同样先查范围再解引用，
  是同类可达的停机点）。信号投递时往用户栈写帧一步
  未纳入：栈页由 exec 预映射且不可被 mprotect/munmap
  触及（非 VMA），构造上不可缺页。覆盖面是整个调度
  器——所有处理函数的用户内存解引用（路径串、数据
  缓冲、出参）一并受益，包括 wait4 的「EFAULT 不回收
  僵尸」既有语义（写出在回收之前，跳回即保住僵尸）。
  验收：新套件 faulttest 八场景两轮全过——rusage 指
  只读页（M21 原案）得 EFAULT 且僵尸保留可重试、往
  只读页 read、往未映射窗口 read/stat/nanosleep/
  gettimeofday/子进程内野 rsp 的 sigreturn 全部
  -EFAULT、事后系统照常工作；其余十五链不回归。
- ✅ 文件系统 M25（2026-11-02）：/proc 雏形。fs 层加
  第三个后端：路径以 /proc 为前缀的 open/stat/
  getdents 在解析层之前被截走，交给一组生成式只读
  文件——uptime（开机秒数与 idle 秒数，厘秒精度）、
  meminfo（MemTotal/MemFree/MemAvailable，按 PMM 页
  数×4KiB）、version（版本横幅）。内容在 open 时一次
  快照进文件对象，read/lseek 按偏移供给，stat 给
  0444（目录 0555；根目录经 ramfs 挂载点让 ls / 能
  看见 proc/）；write 与指向 /proc 的 mkdir/unlink/
  rmdir/rename/symlink 一律拒绝。idle 数字是真的：
  调度器新计 idle 节拍，M2 自测那两个永不退出的
  spin 线程改为考后自退——此前机器从未真正 idle，
  uptime 第二个数永远贴着 0。本轮还刨出一个潜伏多
  时的大 bug：调度器的 CR3 缓存拿 struct vmm_space
  指针作比较键，而 exit/execve 绕开调度器直接装
  CR3、空间池槽位又按先空闲先用回收——一个任务
  exec 之后，旧槽位被 clone 新空间占上时指针相等，
  调度器误判「空间没变」跳过 CR3 装载，子进程便跑
  进父进程的页表里（两边栈互踩、子进程 RIP=0 崩
  死，fx 套件每轮漏 4 页且 PMM 不平衡）。修复：缓
  存键改成 PML4 物理地址，exit/execve 的直接装载
  补 sched_note_cr3() 回报缓存；clone 的子 RSP 也
  改从入口帧槽位取（入口 shim 本为此而存），不再读
  会被后入系统调用覆盖的 per-CPU 全局槽。验收：新
  套件 proctest 九场景两轮全过，fx 套件 PMM 平衡项
  恢复 PASS，其余十六链不回归。顺带记档：被收养
  的孤儿僵尸要等 shell 退出才被 init 清扫（init 主
  循环阻塞在对 shell 的 wait4 上），已列入下一步。
- ✅ 时间戳 M26（2026-11-02）：atime/ctime/mtime 独立
  跟踪＋utimensat(280)/futimens(261)。ramfs 节点新带
  三个独立字段：创建时三者同值，读（含 getdents）刷
  atime（严格模式，读到字节才算），写/截断刷 mtime
  ＋ctime，rename 刷 ctime，utimens 成功必刷 ctime。
  yfs 的 64 字节 inode 没有余量存三份时间戳
  （rsvd[1] 已被 mtime 用满），维持单戳：stat 三者
  同值，utimens 以 mtime 操作元为准（省略时才轮到
  atime 操作元）改写那一个戳——三戳独立要等 yfs 格
  式升版。系统调用面：times 为 NULL 即双 NOW；
  UTIME_NOW/UTIME_OMIT 解码、nsec 范围与负秒校验
  回 EINVAL；AT_SYMLINK_NOFOLLOW、AT_EMPTY_PATH
  （语义与 newfstatat 对齐）、相对 dirfd 仍 ENOSYS；
  /proc 回 EACCES，futimens 打在管道/epoll/控制台
  上回 EINVAL。stat 的 atime/ctime 自此是真值（时间
  戳粒度仍为秒，显式 nsec 只做校验后截断）。验收：
  新套件 utimetest 九场景（显式设时、OMIT、NULL-
  now、futimens、NOFOLLOW 只打链本身、读刷 atime、
  写刷 mtime+ctime、错误面、空路径）在 ramfs 两轮
  全过，disk 套件 p2 段以别名模式在 yfs 上同套全
  过，其余十七链不回归。
- ✅ 进程卫生 M27（2026-11-02）：孤儿僵尸及时清扫。
  内核的收养机制（M5）一直在：父进程先死、孩子过
  继给 init。但 init 的监管循环堵在 wait4(shell)
  上，过继来的孩子死了叫不醒它，要等 shell 退出才
  被 WNOHANG 顺带扫掉——会话中 ps 常年挂着 Z。改
  法全在用户态：init 改 wait4(-1) 阻塞监管，任一
  子进程（shell 或过继孤儿）退出都唤醒它，按 pid
  只汇报 shell 的死、其余静默收尸；1 秒 respawn
  间隔里死的孤儿在下一轮监管入口即被收走。内核未
  动。验收：新套件 orphantest（子生孙后立即死、孙
  被过继、睡 1.5 秒后退出）两轮后 ps 里孙任务彻底
  消失、PMM 两轮持平；负对照（旧 init）下同套件
  如期 FAIL——孙以僵尸残留且跨轮漏页。其余十八链
  不回归（disk 套件首轮紧接十五链回归、宿主机高
  负载下 harness 超时假失败一簇，闲时单独重跑
  ALL OK，与 M22 同款）。
- ✅ /proc/<pid> M28（2026-11-03）：每个存活任务
  一个目录。根目录动态列出全部 pid（僵尸被收尸
  前照列），<pid>/status 给 Name/Pid/PPid/State、
  <pid>/stat 按 Linux 字段序（前四字段真实、其余
  零占位）；不存在的 pid 处处 ENOENT、与不存在的
  名字无区别。生成沿用 open 时快照，任务中途死去
  不影响已打开的读者。顺手修了 clone 命名：子进
  程改继承父 comm（Linux 语义、exec 再改名），不
  再人人顶着创建占位名 "clone"——未 exec 的孩子
  此前在 ps 与 /proc 里永远叫 clone。验收：新套
  件 pidtest 九场景两轮；其余十九链不回归。
- ✅ yfs 格式升版 M29（2026-11-03）：格式 v4。
  inode 64B 扩到 128B：atime/mtime/ctime 三个真
  字段（M26 的单戳别名退役）、flags 独立收纳原
  rsvd[0] 的引用计数与 unlinked 位、留 56B 空白
  给未来字段；布局平移：inode 表 1..32、位图
  33..40、日志 41..89、数据自 90。utimens 三戳
  各走各的；写/截断刷 mtime+ctime、创建三戳齐
  设、rename 补刷被移动者 ctime（此前 yfs 漏刷）；
  读不刷 atime（省日志流量，明示的取舍）。旧盘
  策略：超级块版本号 3→4，v3 盘拒挂并打印说明、
  绝不自动格式化毁数据。验收途中修掉一个真 bug：
  stat 回填曾把 u64 字段地址当 u32* 写，只落低
  半、高半是未初始化栈垃圾（调试打印全按 u32 截
  断、一度"处处正确"）；timetest 的创建三戳相等
  断言也改为一秒容差（独立戳下跨秒边界本就合
  法）。disk 五段（含 v4 模式 utimetest 九场景）
  全绿，其余二十链不回归。
- ✅ 相对 dirfd M30（2026-11-03）：openat /
  newfstatat / utimensat / unlinkat / symlinkat
  的 dirfd 全部生效（此前分别回 EBADF/ENOSYS）。
  做法：打开文件时把其绝对路径记进 file 对象，
  dirfd 解析以该路径为基准拼接——三后端统一受
  益；绝对路径忽略 dirfd、坏 fd EBADF、非目录
  ENOTDIR 与 Linux 一致。明示的偏离：目录在打开
  后被改名时按名字而非 inode 解析（fs 层是路径
  制的）。utimens 的旧 ENOSYS 期望改 EBADF。验
  收：新套件 dirfdtest 十场景 ramfs 两轮＋disk
  p2 在 yfs 上同跑；其余二十一链不回归。
- ✅ /proc/self 与 cmdline M31（2026-11-03）：
  self 由系统调用层在路径解析时按调用者改写成
  /proc/<tid>（fs 层不感知调用者，改写点在
  resolve_path/resolve_at 汇合处）；任务新增
  cmdline 存储——execve 提交时把 argv 以 NUL 连
  接存入（上限 128B）、clone 继承，/proc/<pid>/
  cmdline 按原始字节生成（内核线程为空）。验收：
  pidtest 扩 self-link/cmdline-self/cmdline-
  child 三场景两轮；其余二十二链不回归。
- ✅ sigaltstack M32（2026-11-03）：131 号从
  ENOSYS 转正。任务存备用栈（sp/size/flags）：
  clone 继承、exec 清除（Linux 语义）；设置须
  ≥MINSIGSTKSZ 2048（小了回 ENOMEM）、查询以
  SS_DISABLE 报告未配置；投递帧构造点统一处理
  SA_ONSTACK——已在备用栈上时嵌套投递不换栈，
  ucontext 同步回填 uc_stack。未追踪 SS_ONSTACK
  查询位（明示）。验收：sigtest 加 altstack 场
  景（查询/过小/往返/ONSTACK 落栈/无标志回主栈/
  禁用后回主栈）；其余二十三链不回归。
- ✅ *at 家族补齐 M33（2026-11-03）：mkdirat、
  readlinkat 为 resolve_at 薄封装；renameat 双
  dirfd 独立解析，renameat2 仅支持
  RENAME_NOREPLACE（目标预检 lstat，存在即
  EEXIST，非原子已明示）；access/faccessat/
  faccessat2 新增（无 uid 模型，请求位按所有者
  权限三联字面校验，AT_EACCESS 空转，
  AT_EMPTY_PATH 可查 dirfd 本身）。link/linkat
  明确回 -EPERM：ramfs 名字在节点内（单父单
  名），硬链接需目录项重构、另立里程碑。
  验收：dirfdtest 增六场景（mkdirat/renameat/
  noreplace/readlinkat/faccessat/linkat-eperm）
  两轮全过，disk 套件在 yfs 上同样复跑。
- ✅ /proc/<pid>/environ M34（2026-11-03）：
  execve 本就把 envp 拷进新镜像初始栈，本轮在
  提交点把 envp 留存进任务（512 字节，execve
  上限 4×128 故无损），clone 同样继承；
  /proc/<pid>/environ 按原始字节生成（pid 项
  inode 步长 4→8，生成物不落盘故无兼容问题）；
  proc 快照缓冲 288→640 以容纳完整 environ。
  验收：pidtest 加 environ-self（shell 以空 env
  启动故自读为 0 字节）与 environ-child（子进
  程 exec 带 K1=V1/K2=V2、父逐字节核对）。
- ✅ /proc/<pid>/cwd 与 exe M35（2026-11-03）：
  execve 提交点留存解析后的绝对路径
  （exe_path）；两项为魔法链接——readlink 返回
  目标路径、lstat 报链接自身（0120777）、stat
  跟随到目标、open 打开目标本身；其余 /proc
  项 readlink 改回 EINVAL（此前误报 ENOENT）。
  从未 exec 的任务 exe 为空、链接不存在。顺手
  修 stat 尺寸 scratch 288→640（M34 遗漏、
  environ 大于 288 时 st_size 会被截断）。
  验收：pidtest 加 cwd-exe 场景（exe/cwd 读
  链、lstat/stat 分工、open cwd 列目录、生成
  文件 readlink 应 EINVAL、dents 类型 DT_LNK）。
- ✅ chmod/chown 家族 M36（2026-11-03）：此前
  mode 全是按类型写死的常数，本轮落存储——
  ramfs 节点加 mode 字段（创建时按类型设默认
  值）；yfs 复用 inode 的 future[0] 存 mode、
  flags 加 MODE_SET 位，旧盘无此位时按类型默
  认值报告（格式不变、无需升版）。chmod/
  fchmod/fchmodat 生效并刷 ctime，faccessat 的
  校验随之真实；chown/fchown/lchown/fchownat
  无 uid 模型——校验存在性后成功（明示占位）。
  open(O_CREAT) 的 mode 参数仍未贯通（创建恒
  默认值），列入候选。验收：dirfdtest 加
  chmod-family 场景，ramfs 与 yfs（disk 套件）
  各跑一遍。
- ✅ umask 与创建 mode 贯通 M37（2026-11-03）：
  umask(95) 转正——任务出生默认 0022（kthread
  创建处统一赋值）、clone 继承、exec 保留；
  open(O_CREAT) 与 mkdir/mkdirat 的 mode 参数
  经调用者 umask 过滤后贯通 fs_open_path/
  fs_mkdir_path 落到两后端。偏离明示：mode 传
  0 仍取默认 0644/0755（自家程序普遍传 0，
  不引入 Linux 的 000 文件陷阱）。验收：
  dirfdtest 加 creat-modes（umask 0027 下 0666
  →0640、umask 0 下 mkdir 0777 全保留、恢复
  0022 后 0755），ramfs/yfs 各一遍。
- ✅ getdents 语义收口 M38（2026-11-03）：
  d_off 本就是「下一条目」cookie（三后端一
  致、与目录 lseek 同一坐标系），本轮修三个
  边界：①缓冲装不下一条记录时返回 EINVAL
  而非 0——此前会被调用方误读成 EOF、静默
  截断目录清单；②目录 SEEK_END 统一 EINVAL
  （ramfs 版此前落在 size=0 上等于静默
  rewind、yfs 版落在字节尺寸上无意义）；
  ③getdents 作用于管道/控制台等非目录对象
  改回 ENOTDIR（原 EBADF）。验收：dirfdtest
  加 seekdir——全量清单、按 f.txt 的 cookie
  seek 后从下一条续读、16 字节小缓冲 EINVAL
  且位置不乱、SEEK_END 被拒，ramfs/yfs 各
  一遍。
- ⏳ 下一步候选：硬链接（目录项重构）、
  /proc/<pid>/maps 示意、sendfile/splice
  等零拷贝家族盘点。

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
