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
- ⏳ 下一步候选：lstat、mprotect、brk 按需化。

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
