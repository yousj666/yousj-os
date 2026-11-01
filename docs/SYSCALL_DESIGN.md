# Yousj OS · 系统调用接口设计

> 状态：设计稿，待总协调评审。阶段 1 里程碑 M3（Ring 3 冒烟）起、
> 贯穿阶段 2（Linux 兼容核心）。
> 既有事实：`kernel/include/yousj/syscall.h` 已定第一批号码（见
> §6 清单，与 Linux x86-64 同号）；号码之外尚无任何实现——没有
> syscall 入口、没有用户任务、GDT 仍借自启动 stub 且无 TSS。
> 本文所有新增结构、函数、常量与对 `syscall.h` 的增补均为
> **接口提案**，标注待批；未获总协调批准前不得据此改头文件。
> 前置设计：VMM_DESIGN.md（用户低半区、每进程 `vmm_space`）、
> SCHED_DESIGN.md §6（各进程类 syscall 的分阶段语义）、
> INTERRUPT_DESIGN.md §8（GDT/TSS 规划，本文 §3 给出段表终稿）。
> 北极星（用户定）：快、干净；目标是承载 Linux 程序生态且不学
> Ubuntu 的臃肿——对 syscall 层而言，就是入口薄、分发直、
> 语义不撒谎（未实现就 `-ENOSYS`，做了子集就明确拒绝子集之外）。

## 1. 兼容的定义：什么必须与 Linux 一模一样

「同号同语义」拆成三层，层层都是硬承诺，不许含糊：

| 层 | 内容 | 承诺 |
|---|---|---|
| 编号层 | syscall 号与 Linux x86-64 表一致（`syscall.h` 已锁定） | 完全一致，永不重排、永不复用已删号码 |
| 约定层 | 参数/返回值寄存器、错误编码方式（§4） | 完全一致——这是二进制兼容的本体，任何偏差都会让未改动的 Linux 程序在第一个 syscall 上翻车 |
| 语义层 | 每个已实现 syscall 的行为 | 分阶段做子集（§6）：子集内与 Linux 一致；子集外返回明确错误码，绝不静默近似、绝不假成功 |

不承诺的也要写明（§7 有完整边界表）：32 位兼容表（ia32）不做；
未列入 `syscall.h` 的号码一律 `-ENOSYS`，包括 Linux 程序可能
探到的新号——glibc/musl 对绝大多数可选 syscall 的 `-ENOSYS`
都有降级路径，诚实拒绝比拙劣仿造更兼容。

## 2. 入口机制取舍：`syscall`/`sysret`，不设 int 0x80

### 2.1 候选对比

| 机制 | 速度 | 与 Linux ABI 的关系 | 代价/问题 |
|---|---|---|---|
| `syscall`/`sysret` 指令对 | 最快（不走 IDT、无门描述符查表，专为此设计） | **就是** Linux x86-64 的原生入口，未改动的程序本来就用它 | 需配 MSR（EFER.SCE、STAR、LSTAR、SFMASK）；硬件不自动换栈、不自动存返回地址以外的现场（细节见 §3） |
| `int 0x80` | 慢一截（全程 IDT 门 + iret） | **是坑**：在 64 位 Linux 上 int 0x80 进的是 32 位兼容 syscall 表，号码与寄存器约定都不同。拿它承接 64 位号码，等于亲手制造「看起来像 Linux、其实不是」的私有 ABI，将来跑真 Linux 程序时必炸 | 唯一优点是复用 IDT 机制、实现短——但省下的代码要用永久的 ABI 混乱来还 |
| 自定义中断向量（如 int 0x81） | 同 int 0x80 | 与 Linux 无关，纯私有入口 | 只对自家 userland 有效；真 Linux 程序不会用它，等于白写 |

### 2.2 结论

**只做 `syscall`/`sysret` 一条入口，从第一天起，不做任何
int 式过渡。** 理由除了上表，还有一条工程纪律：过渡入口一旦
能跑通冒烟，就会沉淀成「先用着」的长期包袱（本项目已在别处
立过「过渡方案不许沉淀」的规矩，见 SCHED_DESIGN 的 init 选型）。
`syscall` 入口的难点（MSR 配置、手工换栈、GS 基址）都是
一次性的，且与 INTERRUPT_DESIGN §8 的 GDT/TSS 是同一批
前置件，合起来做正好一轮。

附带决定：`sysret` 在「用户 RIP 非 canonical」等边角情形会
在 Ring 0 触发 #GP，Linux 的对策是改走 iretq 返回。本文
阶段内用户 RIP 恒为低半区 canonical 地址（由 ELF 加载器
保证），**提案：先只实现 sysretq 返回，iretq 兜底路径等
第一个需要它的情形出现时再加**，并在入口代码注释中点名
这一简化，避免后人以为已处理。

## 3. 入口的地基：GDT 终稿、TSS 与每 CPU 区

这是本设计对排期最硬的一条主张：**syscall 入口不能单独开工，
它与内核自有 GDT、TSS、每 CPU 数据区是同一个里程碑**
（INTERRUPT_DESIGN §8 已从中断侧提出同一结论，两文在此合流）。

### 3.1 为什么

- `syscall` 指令不切换栈：进入内核后 RSP 仍是用户栈。内核
  第一件事就是换到当前任务的内核栈，而「当前任务的内核栈
  在哪」必须在一个不依赖任何通用寄存器、且用户不可见的
  地方取到——标准解法是 `swapgs` 后从每 CPU 区读。
- Ring 3 被时钟中断打断时的栈来自 TSS.rsp0（INTERRUPT_
  DESIGN §5.5/§8）；TSS 必须挂在 GDT 里。
- STAR MSR 对段的相对位置有硬约束（§3.2），段表不是随便排的。

### 3.2 GDT 终稿（提案）

| 选择子 | 内容 | 备注 |
|---|---|---|
| 0x00 | 空描述符 | 架构要求 |
| 0x08 | 内核代码（Ring 0, 64 位） | 与现 stub GDT 及 IDT 现有常量一致，保 IDT 零改动 |
| 0x10 | 内核数据 | 与现 stub GDT 一致；STAR 约束：内核 SS = 内核 CS + 8 ✓ |
| 0x18 | 用户数据 | STAR 约束：`sysret` 的 SS = STAR.sysret 基址 + 8 |
| 0x20 | 用户代码（Ring 3, 64 位） | `sysret` 的 CS = 基址 + 16；用户 CS 实际用 0x20\|3=0x23，SS 用 0x18\|3=0x1B |
| 0x28 起 | TSS 描述符（占两槽） | 随本里程碑新增；IST1 给 #DF（INTERRUPT_DESIGN §8） |

STAR.sysret 基址由此定为 0x10：sysret 时 SS=0x10+8=0x18、
CS=0x10+16=0x20，与上表自洽。此表一经批准，`syscall` 入口、
iretq 落 Ring 3、IDT 三处共同引用，段序不许再单独调整。

### 3.3 每 CPU 区（单核先行，形状按多核预留）

提案一个最小 `struct cpu_local`（单核时就一份静态实例，GS
基址经 MSR_GS_BASE 指向它；`swapgs` 在入口/出口各执行一次）：

```c
/* 提案，待批准 */
struct cpu_local {
    uint64_t kernel_rsp;    /* 当前任务的内核栈顶（调度切换时更新，
                               与 TSS.rsp0 同值、同更新点） */
    uint64_t user_rsp_save; /* 入口换栈时暂存用户 RSP */
    uint64_t current_task;  /* struct task* 的镜像，避免入口汇编
                               追多级指针；单一真相仍在 sched 的
                               g_current，此处仅为入口缓存，
                               更新点与 kernel_rsp 相同 */
};
```

更新纪律：`kernel_rsp` 与 TSS.rsp0 在 `sched_schedule()` 完成
切换的同一点更新为新任务的 `kernel_stack_top`——两处、同一
时机、同一数值，评审时按此核对。禁止入口汇编之外的代码读
cpu_local（它不是全局变量的后门）。

### 3.4 MSR 配置清单（入口启用时一次性完成，提案）

| MSR | 设置 |
|---|---|
| EFER | 置 SCE 位（连同 VMM 侧已需的 NXE） |
| STAR | 高 16 位 sysret 基址 = 0x10；中 16 位 syscall 目标 CS = 0x08（SS 自动 +8=0x10） |
| LSTAR | 入口汇编标号地址 |
| SFMASK | 置 IF 位（进入内核即关中断，保证换栈完成前不被打断）+ DF 位（清方向标志，SysV 约定）+ TF 位 |
| GS_BASE / KERNEL_GS_BASE | 配合 swapgs：用户态时 GS 基址为 0（或用户值），内核态为 cpu_local 地址——具体哪侧放哪边按 swapgs 语义定稿时写死一处并注释 |

## 4. 参数与返回值约定（与 Linux x86-64 逐项对齐）

### 4.1 寄存器表

| 角色 | 寄存器 | 备注 |
|---|---|---|
| syscall 号 | `rax` | 入口时 |
| 参数 1–6 | `rdi, rsi, rdx, r10, r8, r9` | **第 4 参数是 r10 不是 rcx**——rcx 被 `syscall` 硬件占用存返回 RIP，这是 Linux 约定里最容易抄错的一处，分发层注释必须点名 |
| 返回值/错误 | `rax` | 成功为结果；失败为 `-errno`（见 §4.2） |
| 被硬件改写 | `rcx`（=返回 RIP）、`r11`（=用户 RFLAGS） | 程序不得指望二者跨 syscall 保持；内核出口前由 sysret 语义处理，入口桩负责把它们存入陷入帧供 clone 等需要 |
| 其余寄存器 | 全部保持 | 入口桩除工作寄存器外不得破坏用户现场；调度若在 syscall 内发生，靠各任务自己的内核栈与既有 context_switch 分工保存（见 §5.3） |

### 4.2 错误语义

- 内核内统一约定：handler 返回 `long`，负值即 `-errno`，
  与 VMM 已定的负 errno 风格（vmm.h 的 `YOUSJ_E*` 宏）一脉。
- 出口判定：`rax` 落在 `-4095..-1` 区间即错误，用户态 libc
  包装据此置 `errno` 并返回 -1——内核只管返回负值本身，
  不替 libc 做任何转换。
- errno 数值与 Linux 完全一致。已在 vmm.h 落地的有
  ENOENT(2)、ENOMEM(12)、EEXIST(17)、EINVAL(22)；本文
  **提案**随各批 syscall 落地逐步补齐所需 errno 到同一处
  定义（反对一次抄进整张 errno 表——「快、干净」也包括
  头文件只含用得着的），首批预计需要：EPERM(1)、ESRCH(3)、
  EINTR(4)、EIO(5)、EBADF(9)、ECHILD(10)、EAGAIN(11)、
  EFAULT(14)、ENODEV(19)、EPIPE(32)、ENOSYS(38)、
  ENAMETOOLONG(36)、ERANGE(34)（提案清单，批准时定稿）。
- 内部专用码不得漏给用户：将来若出现「被信号打断需重启」
  类内部状态（如 Linux 的 ERESTARTSYS 一族，数值 512+），
  必须在出口前转成 `-EINTR` 或自动重启，提案阶段内睡眠类
  syscall 一律用 `-EINTR` 收口，不引入重启机制。

### 4.3 用户指针规则（安全与正确性的分界线）

任何来自用户态的指针，内核**永不直接解引用**，只许经下述
通道（提案 `copy_from_user / copy_to_user / strncpy_from_user`
一组小函数，归属 syscall 公共层）：

1. 范围检查：地址与长度须落在用户低半区内（`< YOUSJ_USER_END`），
   且加法不溢出、不跨入内核半区；
2. 逐页验证：对覆盖的每一页用 `vmm_translate()` 查当前任务
   的 `vmm_space`，要求 present 且带 USER 位（读/写方向再
   对应要求 WRITE 位），任一页不过即 `-EFAULT`；
3. 验证后才逐页拷贝（经物理页或映射窗口访问），拷贝期间
   不再相信任何用户数据（长度、字符串终止符都要在内核侧
   复核上限，如路径名上限提案 4096 与 Linux PATH_MAX 对齐）。

坦白记录简化与债务：逐页预验证存在「验证后映射被改」的
理论竞态。本阶段用户进程单线程、且能改映射的只有它自己
在另一条 syscall 里，竞态不成立，故预验证是 sound 的；
但阶段 2 引入多线程（CLONE_VM）或共享映射**之前**，必须
升级为「拷贝时捕获 #PF 并转 -EFAULT」的兜底机制（与
INTERRUPT_DESIGN §7.3 的 #PF 分化绑定排期）。这条债务
写在此处，防遗忘、防后人误以为预验证是终局方案。

另：`write` 冒烟阶段允许一个收窄的例外——fd 只认控制台、
缓冲区仍走上述通道，不许因为「只是打印」就直接解引用
用户指针。规矩从第一个 syscall 起立，后面才不用补票。

## 5. 入口与分发结构

### 5.1 入口汇编的动作序列（提案，要点级）

```
syscall_entry:
    swapgs                      ; GS -> cpu_local
    mov [gs:user_rsp_save], rsp ; 暂存用户 RSP
    mov rsp, [gs:kernel_rsp]    ; 换到当前任务内核栈
    在内核栈上构建陷入帧：依次压 用户 RSP、用户 RIP(rcx)、
        用户 RFLAGS(r11)、rax(号) 与六个参数寄存器中 C 分发
        需要的现场（帧布局定为 struct syscall_frame，提案；
        clone 的子任务现场复制以此帧为源，见 §6）
    swapgs 前后顺序与 sti 时机：换栈与建帧全程 IF=0（SFMASK
        保证）；建帧完成后再 sti，允许长 syscall 被抢占/阻塞
    call sys_dispatch(frame)    ; C 分发，返回值写入帧中的 rax 槽
    出口：cli -> 按帧恢复寄存器 -> 把结果移入 rax ->
        swapgs -> sysretq       ; rcx/r11 由帧中的 RIP/RFLAGS 恢复
```

纪律：入口汇编不做任何判断（不查号码、不查指针），全部
判断在 C 侧；汇编只做「换栈、搬运、恢复」三件事，这样它
能被逐行评审，也使「入口帧布局」成为 syscall 层与 clone/
信号等后续机制的唯一现场格式。

### 5.2 分发器（C 层）

- 一张函数指针表，按号码下标直接索引（O(1)，无字符串、
  无哈希）：表项用 `YOUSJ_SYS_*` 常量做指定初始化，未填
  的槽位统一指向 `sys_enosys`（返回 `-ENOSYS`），表长即
  当前支持的最大号；越界号码（含负值形态的超大无符号值）
  在索引前拦截，返回 `-ENOSYS`。
- 原生扩展区：号码 `>= YOUSJ_SYS_NATIVE_BASE (0x1000)`
  走第二张小表（本阶段为空，全 `-ENOSYS`），与 Linux 区
  在分发第一步就分流，两个命名空间互不污染——这也是
  `syscall.h` 当初把原生基址定在 0x1000 的用意，本文确认
  沿用，不另设机制。
- handler 签名统一（提案）：`long sys_<name>(struct
  syscall_frame *f)` 或等价的六个 `uint64_t` 参数形式，
  二选一在定稿时定死；推荐帧指针形式，因为 clone 需要
  完整的帧、且将来信号返回要改帧里的 RIP/RAX，散参数
  形式届时要返工。
- 调试钩子（提案，默认关）：编译开关开启时，分发前后经
  串口打印「tid、号、返回值」一行。bring-up 阶段定位
  「程序死在哪个 syscall」全靠它；默认关闭因为它在热
  路径上，且串口打印慢到会改变时序、掩盖竞态。

### 5.3 syscall 内的阻塞与抢占

- handler 在当前任务的内核栈上以普通 C 调用深度运行，
  需要等待（读控制台无数据、wait4 无僵尸子进程、
  nanosleep 未到期）时按 SCHED_DESIGN 的既有机制置
  BLOCKED 并 `sched_schedule()` 让出；被唤醒后从 handler
  内部继续，仿佛只是慢了一拍——这要求入口帧与内核栈
  都属于任务私有（§3 的每任务内核栈正是为此），也是
  §2 反对「固定全局入口栈」的根本原因：那种栈过不了
  第一个会阻塞的 read。
- 时钟抢占在 syscall 执行中照常发生（sti 后）：被换出时
  现场在自己内核栈上，回來时由 `sched_schedule()` 的
  返回点续行，与内核线程被抢占同构，无特殊处理。
- 不可阻塞的临界段（操作调度队列、页表时）沿用既有关
  中断纪律，handler 不得在关中断状态下睡眠——此条与
  INTERRUPT_DESIGN §5.4 是同一纪律的两面。

## 6. 落地顺序：按依赖分四批

与 `syscall.h` 现有号码逐一对表（号码 / 批次 / 子集边界 /
依赖）。未到批次的号码保持 `-ENOSYS`。

### 批次 0：Ring 3 冒烟（随 M3）

| 号 | 名称 | 子集与说明 |
|---|---|---|
| 39 | getpid | 返回 current 的 tid；入口打通的验收用例 |
| 1 | write | 仅 fd 1/2 → 串口；其它 fd `-EBADF`；缓冲走 §4.3 通道 |
| 60 | exit | 按 SCHED_DESIGN §6.1：置僵尸、唤醒父、永不返回 |
| 231 | exit_group | **提案新增号码**（现 syscall.h 未定）：glibc 的 exit 实际调它，与 60 同 handler，先提案待批，未批前自研 userland 用 60 亦可 |

### 批次 1：init/Shell 可用（随 M4）

| 号 | 名称 | 子集与说明 |
|---|---|---|
| 0 | read | 仅 fd 0 ← 控制台（依赖 INTERRUPT_DESIGN §4.3 的 IRQ1 键盘或轮询输入，先到者为准）；无数据则阻塞 |
| 35 | nanosleep | 建在 `sched_sleep_ticks()` 上：timespec 换算 tick（向上取整），被终止信号打断回 `-EINTR` 并回填 rem |
| 63 | uname | 填静态结构：sysname 故意填 "Yousj"（见 §7 的诚实边界）、machine "x86_64"、release/version 为本系统版本串、nodename 固定串 |
| 12 | brk | 进程数据段末尾的伸缩，基于其 vmm_space 的堆区记录；非法回缩 `-EINVAL` |
| 9 / 11 | mmap / munmap | **仅**匿名映射：flags 只收 `MAP_PRIVATE\|MAP_ANONYMOUS`（可叠 MAP_FIXED 的受限语义另议），文件映射回 `-ENODEV`；prot 与 VMM 抽象标志对应，`PROT_EXEC` 在用户页上按 VMM 规则落位 |
| 158 | arch_prctl | 仅 `ARCH_SET_FS(0x1002)` / `ARCH_GET_FS(0x1003)`（TLS 是静态 libc 启动的硬需求）；其余 code `-EINVAL` |

### 批次 2：进程家族（随 M5，语义以 SCHED_DESIGN §6 为准）

| 号 | 名称 | 子集与说明 |
|---|---|---|
| 56 | clone | 仅 fork 式（flags 只收 SIGCHLD/0，非空 child_stack `-EINVAL`）；从入口帧复制出现场，子进程 rax=0 处「复活」 |
| 59 | execve | 依赖 ELF 加载器与 argv/envp 的用户栈构建（§4.3 通道反向使用）；失败不毁当前映像的顺序须在实现时保证（先建新空间、成功才切换） |
| 61 | wait4 | pid>0 / pid=-1、options 仅 0 与 WNOHANG、rusage 非空填零（SCHED_DESIGN §6.3） |
| 62 | kill | 仅 SIGKILL/SIGTERM 的延迟终止语义（SCHED_DESIGN §6.4）；完整信号机制不在本批 |

### 批次 3：文件家族（随 VFS 设计落地，本文只登记归属）

`open(2) close(3) lseek(8) ioctl(16) pipe(22) dup(32)
getcwd(79) chdir(80) mkdir(83) getdents64(217)`——全部依赖
尚未设计的 VFS/fd 表。其号码已在 `syscall.h` 锁定，但在
VFS 设计评审通过前不逐个拟义，避免 syscall 层替 VFS 做主。
fd 表挂在任务结构上的位置已在 SCHED_DESIGN 预留（`files`
指针），本批开启时先定 fd 表与「文件描述」对象的设计，
再逐个点亮。

### 每批的验收方式（统一）

宿主机可测的部分只有分发逻辑（号码边界、原生区分离、
ENOSYS 兜底），做成假 handler 表驱动的单测（提案）；真正的
验收在 QEMU：每批配一个静态链接的裸 syscall 小程序（不用
libc 包装，避免把 libc 的额外 syscall 噪声混进来），串口
输出逐项 PASS。批次 1 起增加负向用例：坏号码 → -ENOSYS、
坏指针 → -EFAULT 且内核存活续跑、坏 fd → -EBADF。

## 7. 兼容边界总表（诚实清单）

| 事项 | 立场 |
|---|---|
| 64 位 Linux 程序（二进制） | 目标对象；批次内子集语义与 Linux 一致 |
| 32 位程序 | 不支持：无 ia32 兼容表，exec 时以 ELFCLASS 检查拒绝（错误码提案 `-ENOEXEC`） |
| 动态链接程序 | 批次 1–2 阶段不支持：需要 ELF 解释器（PT_INTERP）加载路径与更多启动期 syscall；先以静态程序（musl 静态优先，自研 userland 本来就静态）为靶，动态支持随 ELF 加载器增强单独排期 |
| glibc 静态程序 | 会探若干未定号 syscall（TLS/健壮互斥等）：`arch_prctl` 已在批次 1，其余探到 `-ENOSYS` 时 glibc 大多能降级，但**不保证**每个 glibc 版本都跑得通；bring-up 记录实际缺号清单，按需提案增补，不预先全抄 Linux 表 |
| uname 的 sysname | 填 "Yousj" 不冒充 "Linux"：靠 uname 判断平台的程序可能走错分支，这是刻意的诚实代价；绝大多数程序只看 machine 与 syscall 行为本身 |
| 信号 | 批次 2 前无信号机制；kill 只是终止请求（SCHED_DESIGN §6.4 已注明）。`rt_sigaction` 等号码尚未在 syscall.h 中预定，待信号设计单独立项时提案 |
| Yousj 原生调用（0x1000+） | 命名空间保留，本阶段零实现；Linux 程序永不感知，符合「原生扩展不污染兼容层」的原定意图 |

## 8. 与其它设计的对接口

- INTERRUPT_DESIGN §8 / 本文 §3：GDT/TSS/cpu_local 是
  同一个里程碑的两面，段表以本文 §3.2 为终稿来源；
  TSS.rsp0 与 cpu_local.kernel_rsp 的更新点都在
  `sched_schedule()`。
- INTERRUPT_DESIGN §7.3：用户态 #PF 的终止路径与本文
  §4.3 的指针验证债务共用「返回用户态前检查点」机制，
  两边排期绑定（M3 之后、开放多线程/共享映射之前）。
- SCHED_DESIGN §6：进程类 syscall 的语义以那边为准；
  本文新增的只有两点：入口帧（`syscall_frame`）是 clone
  复制现场的唯一来源、exit_group(231) 的号码增补提案。
- VMM_DESIGN：用户指针验证与 mmap/brk 全部经既有
  `vmm_map/unmap/translate/protect` 接口，不要求 VMM
  为 syscall 新增接口；若 execve 落地时发现需要「整份
  空间替换」的原子语义，再走提案流程，不先行开口。

## 9. 风险清单

1. **最大风险：地基里程碑被冒烟压力拆散**。最诱人的捷径
   是「先用固定全局栈 + int 0x80 把 getpid/write 跑通给
   大家看」——但固定栈过不了第一个阻塞型 syscall、int
   0x80 是错误的 ABI 表，两者都会沉淀成返工债，且返工点
   恰在最难调试的入口汇编。§2/§3 的「一次做对」排期
   （GDT+TSS+cpu_local+MSR 与入口同批）是本文最想守住的
   结论；若总协调决定拆批，请把「全局栈/int 过渡不得
   合入主线、只许活在一次性实验分支」写进验收条件。
2. **sysret 的 #GP 暗坑**：用户 RIP 若被 execve/信号路径
   设成非 canonical 地址，sysret 会在 Ring 0 炸出 #GP
   而非把错误还给用户。本文以「加载器保证 canonical」
   规避，但任何将来能影响用户 RIP 的机制（ptrace、信号
   返回、解释器）出现时，§2.2 预告的 iretq 兜底必须
   同批补上，不许滞后。
3. **glibc 的长尾探号**：真实 glibc 程序启动期会发出
   一串我们未定号的 syscall，其降级行为随版本而异，
   可能出现「musl 静态跑得通、glibc 静态卡在启动」的
   落差。缓解：验收靶按 §7 分层声明（先 musl 与自研
   userland），缺号清单用 §5.2 的调试钩子实测收集，
   按数据提案增补，不靠猜补全表——这也守住「干净」。
4. **指针验证的竞态债务**（§4.3）：多线程/共享映射
   开放前若忘了升级为 #PF 兜底，预验证会被绕过且症状
   是内核被用户映射牵着鼻子走。此条已与 INTERRUPT
   设计双向登记，评审阶段 2 的 CLONE_VM 时以此为
   检查项。
5. **SFMASK/GS 配置的单点脆弱性**：SFMASK 漏掉 IF 位
   或 swapgs 方向写反，症状分别是「换栈中途被 tick
   打断踩栈」与「用户 GS 污染内核」，都表现为随机
   时序崩溃而非稳定复现。缓解：MSR 配置集中在一个
   初始化函数内、逐项注释依据（§3.4 表），且入口
   落地后的第一项验收固定为「开 tick 压力下反复
   syscall 十万次无异常」，把时序问题在冒烟期逼出来。
