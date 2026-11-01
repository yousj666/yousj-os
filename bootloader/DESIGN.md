# Yousj OS · 最小 Bootloader 设计（阶段 1）

> 地盘：`bootloader/`（辅助旁聊 A）。本文只定 Bootloader 一侧的设计；
> 与内核的唯一接口是 `kernel/include/yousj/boot.h`，本文不改、也不能改那个接口。
> 服务器版先行：无头、无图形是常态，串口是第一调试通道。

## 1. 目标与非目标

目标（阶段 1 最小可用）：

1. 被固件加载并运行，拿到固件内存图（memory map）。
2. 把内核映像读进内存、放到约定位置。
3. 把固件内存图转换成一份 `struct yousj_boot_info`（见 boot.h）。
4. 退出固件服务，以 `RDI = &boot_info` 跳进内核入口 `kmain()`，此后不再回头。

非目标（本阶段不做）：

- 图形模式设置与帧缓冲填充（`boot_info.fb` 全零；桌面版阶段再用 GOP 填）。
- 多核拉起（AP 启动）、PCI 枚举、磁盘/文件系统驱动——那是内核的活。
- 网络启动（PXE/HTTP Boot）、Secure Boot 签名链——服务器版成型阶段再议。
- 通用 ELF 动态链接、多模块加载——内核映像只按静态布局加载（见 §4）。

## 2. 启动方式选型

### 2.1 候选对比

| 方案 | 从零原则 | 服务器契合度 | 调试友好度 | 工作量/风险 | 结论 |
|---|---|---|---|---|---|
| **UEFI 应用自写**（BOOTX64.EFI，自己调固件服务） | 完全自写，与架构文档分层图一致（UEFI 固件 → Yousj 自写 Bootloader） | 现代服务器与云主机固件均为 UEFI；无头机照样跑，不依赖显示 | QEMU 用 OVMF、真机用主板 UEFI，同一份二进制两边跑；串口日志可从第一条指令打 | 中：要自己处理 GetMemoryMap / ExitBootServices 时序与 ELF/平面映像加载，但协议面小、无外部依赖 | **选它（主线）** |
| GRUB2 + Multiboot2 | 借现成引导器，不算自写；且 Multiboot2 信息格式与 boot.h 不是一回事，还得在内核里再转一层 | 服务器常见，但等于长期背一个外部引导器配置体系 | QEMU/真机都方便 | 低（起步快），但接口主导权不在自己手里，与「boot.h 是唯一接口」的分工冲突 | 不选（仅可作临时对照工具） |
| Limine 等现成协议引导器 | 同上，外部协议引导器 | 能跑，但协议字段远超 boot.h，多余面大 | 方便 | 低，但等于把启动协议外包，后期换掉成本更高 | 不选 |
| Legacy BIOS（MBR + 实模式自写） | 自写 | 现代服务器固件多已纯 UEFI 或 CSM 默认关；>2TB 盘、GPT 都别扭 | QEMU 默认固件即是 BIOS 路线，但真机方向相反 | 高：实模式/保护模式切换、A20、E820 全套老包袱，与 x86_64 长模式目标绕远路 | 不选 |

选型理由小结：UEFI 自写是唯一同时满足「从零写、不借引导器」「服务器版先行」
「同一二进制可跑虚拟与真机调试」三条的方案。UEFI 在被调用时已经处于
x86_64 长模式，不用再写模式切换；内存图、ACPI 表、文件访问都有现成固件
服务，Bootloader 本体可以保持很小。

### 2.2 硬件起步：虚拟机先行（2026-11-01 用户已拍板，总协调回填）

本文写作时该项尚待用户拍板，故当时只保证设计对两个答案都不返工；
2026-11-01 用户已明确决定**虚拟机（QEMU）先行、真机后置**，以下按
决定执行，原真机分支留作后续阶段参考，不再是待定项：

- **QEMU 先行（执行）**：固件用 OVMF（UEFI），`BOOTX64.EFI` 与内核映像放在
  虚拟 FAT 盘（ESP）里挂载启动；串口直接连到宿主机终端/日志文件，
  `kernel/drivers/serial.c` 的输出第一时间可见；崩溃只关进程。
  （在 OVMF/UEFI 链路打通前，总协调另用 QEMU `-kernel` Multiboot
  直载做内核冒烟，那只是临时验证工具，不改本文的 UEFI 主线。）
- 真机（后续阶段）：同一 `BOOTX64.EFI` 拷进真机 ESP，串口走主板
  COM 口或串口转 USB；流程、映像格式、boot_info 内容全不变。

两条路线差异只在「介质与接线」，不在 Bootloader 代码。内核映像加载
地址、内存图转换、交棒方式（§4–§6）均与此无关。

## 3. 总体流程

```
固件 (UEFI)
  │ 加载 \EFI\YOUSJ\BOOTX64.EFI，传入 ImageHandle + SystemTable
  ▼
efi_main（Bootloader 入口，实现见 efi_main.c；UEFI 类型见 uefi.h）
  1. 存 ImageHandle/SystemTable；用固件 ConOut 打最早一行日志（退出服务后不可用）
  2. 经 LoadedImage + SimpleFileSystem 打开 ESP，读内核映像文件到临时缓冲
  3. 给内核映像分配物理页（AllocatePages），按 §4 布局放好，记下
     [kernel_base, kernel_end)
  4. 给 boot_info 本体 + 内核栈分配物理页（AllocatePages，类型自选
     EfiLoaderData），记下这两段范围——它们和内核映像一样不能被 PMM 发出去
  5. 查配置表拿 ACPI RSDP 地址 → boot_info.rsdp_addr（找不到填 0）
  6. 取 LoadOptions 作命令行（UTF-16 → ASCII）→ boot_info.cmdline
     （服务器版可为空串；boot.h 上限 256 字节含结尾 NUL）
  7. GetMemoryMap（两段式，见 §5.1）→ 原始固件内存图
  8. 纯函数转换（boot_info.c，可独立单测）：
       yousj_boot_info_build()  固件图 → boot_info.mem[]
       + 强制把内核映像段标成非 USABLE（§5.3）
       + 把 boot_info 本体段、栈段标成 YOUSJ_MEM_BOOTLOADER（§5.3）
  9. ExitBootServices（用第 7 步的 MapKey；失败按协议重取内存图再试一次）
     —— 此步之后：禁止一切固件调用、禁止再分配内存、禁止 ConOut 输出
 10. 关中断 (cli)，RDI = boot_info 物理地址（当前恒等映射下即指针值），
     RSP = 第 4 步栈顶（16 字节对齐），跳 (jmp) 到内核入口（§4.3）
  ▼
kmain(boot_info)   ← 内核接管，第一件事 serial_init() 后串口出字
```

顺序要点：第 7 步必须晚于第 2–6 步的一切分配动作——每次分配都可能改变
内存图与 MapKey，图取早了 ExitBootServices 会被固件以 MapKey 过期拒绝。
第 8 步的转换是纯函数、不碰固件，所以放在取图之后、退出之前在固件内存里
算完没有问题；也可以先退出再算，但没有必要给自己添风险。

## 4. 内核映像：放哪、怎么放

### 4.1 文件与介质

- ESP（FAT32）路径：`\EFI\YOUSJ\BOOTX64.EFI`（引导器）、
  `\EFI\YOUSJ\YOUSJ.KRN`（内核映像，文件名暂定，可在实现期改）。
- 读取走固件 SimpleFileSystem，只读、整文件读入，不做流式解析。

### 4.2 映像格式与放置（提案，待协调者确认后锁定）

内核由 gcc 链接，天然产物是 ELF64。两个子选项：

- **A（推荐）最小 ELF64 加载**：只认 ELF64 / x86_64 / 静态可执行，
  遍历 Program Header，把每个 `PT_LOAD` 段按 `p_paddr` 拷到物理内存，
  `p_memsz > p_filesz` 的尾部清零（即 BSS）。不认动态链接、不认重定位。
  好处：链接脚本说放哪就放哪，段级信息不丢；加载器约百行级。
- B 平面二进制（objcopy -O binary）：加载器最省事，但段布局信息全丢，
  BSS 起止要另立约定，后续加段就返工。不推荐，只作 A 卡住时的临时兜底。

物理放置：内核映像占据的物理范围记为
`[kernel_base, kernel_end) = [最低 PT_LOAD 的 p_paddr, 最高段尾按页上取整)`。
链接地址具体定多少（如 1 MiB / 2 MiB 起）属内核侧链接脚本事项
（协调者地盘），Bootloader 不写死数值、只按映像头里的地址放——这样地址
调整不用改 Bootloader。

栈：Bootloader 另分配若干页（如 16 KiB，可调）作内核初始栈，栈顶地址
在交棒时写入 RSP。内核 VMM 建好自己的映射后应尽早换到自有栈
（内核侧事项，本文只提醒）。

### 4.3 交棒约定（与 boot.h 的对齐）

boot.h 是唯一合同，交棒必须逐字满足它：

- `boot_info.magic = YOUSJ_BOOT_MAGIC (0x594F5553)`、`version = 1`；
- `RDI = boot_info 的地址`（SysV 第一参数），kmain 签名即
  `void kmain(struct yousj_boot_info *boot_info)`；
- 用 `jmp` 不用 `call`：kmain 不返回（最终 `cli; hlt`），不需要返回地址；
- 交棒时中断关闭（`cli`），分页沿用 UEFI 的恒等映射现状——阶段 1 内核
  靠物理地址直接读 boot_info 与内存图，其 VMM 硬件步再切自有页表
  （内核事项，UEFI 链实测已通过该步）；
- **GDT 契约（2026-11-01 OVMF 实测踩出，重要）**：内核在开中断前不加载
  自己的 GDT——其 IDT 表项写死选择子 `0x08` 为「当前 GDT 中的 code64
  描述符」（kernel/arch/x86_64/interrupts.c），数据段按 `0x10`。EDK2
  固件 GDT 的 0x08 不是 code64，沿用固件 GDT 时第一个时钟中断即三级
  fault。Multiboot 桩的 GDT 恰好是这个布局。故交棒前 Bootloader 必须
  自带并 `lgdt` 一份三项 GDT（null / 0x08 code64 / 0x10 data，描述符
  与 stub.S 逐位一致），far return 重载 CS、0x10 重载数据段，然后才
  跳。**启动契约由此明确：谁引导谁提供这份 GDT 布局。**
- **入口衔接（已落地）**：Bootloader 按 §4.2 方案 A 加载（BSS 清零、
  栈另行备好），并解析 ELF 符号表直跳 `kmain` 符号本体（boot.h 字面
  合同）。现行 `_kentry64` 是 Multiboot 桩专属入口（从物理 0x9008 读
  boot_info、假设桩 GDT），UEFI 链不用它。若日后链接剥离符号表，
  本加载器会 fail closed 停住——届时请内核侧立一个正式 UEFI 入口
  符号（见 §8 提案 1）。

## 5. 内存图：获取与转换

### 5.1 从 UEFI 拿原始图

`GetMemoryMap` 两段式（UEFI 常规打法）：

1. 先以空缓冲调用一次，固件回填所需字节数（并返回 BUFFER_TOO_SMALL）。
2. 在所需量上**额外加富余**（至少 2 个描述符的量）：第 3–6 步若还有分配，
   以及 ExitBootServices 本身，都可能让图变长；本设计把一切分配放在取图
   之前，富余只是保险。
3. 再调一次，拿到 `MemoryMapSize / DescriptorSize / MapKey /
   DescriptorVersion`。遍历时**步长必须用固件给的 DescriptorSize**，
   不能用 `sizeof(EFI_MEMORY_DESCRIPTOR)`——描述符结构以后只会变长。
4. `DescriptorVersion` 不是 1 就停（未见过的格式不猜）。

### 5.2 类型映射（UEFI MemoryType → yousj_mem_type）

原则：**只有固件明说能用的才是 USABLE，其余一律保守**。PMM
（kernel/mm/pmm.c）的语义是「图里没标 USABLE 的帧永远不发」，转换侧
宁可少报可用、不可错报可用。

| UEFI 类型 | 值 | → yousj 类型 | 理由 |
|---|---|---|---|
| EfiConventionalMemory | 7 | `YOUSJ_MEM_USABLE` | 唯一无条件可发给 PMM 的 |
| EfiLoaderCode / EfiLoaderData | 1 / 2 | `YOUSJ_MEM_BOOTLOADER` | 引导器自身与 boot_info、栈所在页；退出后其实可回收，阶段 1 先不回收，防 boot_info 被 PMM 发出去覆盖（见 §5.3) |
| EfiBootServicesCode / Data | 3 / 4 | `YOUSJ_MEM_BOOTLOADER` | 退出后规范允许回收；阶段 1 保守，回收优化留待 PMM 久经考验后再开 |
| EfiRuntimeServicesCode / Data | 5 / 6 | `YOUSJ_MEM_RESERVED` | 退出后仍是固件运行时服务的地盘，永不发 |
| EfiACPIReclaimMemory | 9 | `YOUSJ_MEM_ACPI` | 内核读完 ACPI 表后可回收，先按 ACPI 交 |
| EfiACPIMemoryNVS | 10 | `YOUSJ_MEM_ACPI` | NVS 要长期保留；boot.h 无 NVS 专类，归 ACPI 类且永不由 PMM 发出，语义安全 |
| EfiMemoryMappedIO / IOPortSpace | 11 / 12 | `YOUSJ_MEM_MMIO` | 设备窗口，发给 PMM 就是灾难 |
| EfiReservedMemoryType、EfiUnusableMemory、EfiPalCode、EfiPersistentMemory 及其余/未知值 | 0 / 8 / 13 / 14 / 其他 | `YOUSJ_MEM_RESERVED` | 不认识就当保留——fail closed |

实现对应 `boot_info.c` 的 `yousj_boot_map_uefi_type()`，纯函数。

### 5.3 三条转换铁律（boot_info.c 的纯函数负责）

1. **内核映像段必须非 USABLE**：转换完成后，用
   `yousj_boot_info_mark_range(info, kernel_base, kernel_len, YOUSJ_MEM_RESERVED)`
   把 `[kernel_base, kernel_end)` 与内存图的交叠部分逐段改型：与 USABLE 段
   交叠时先按边界切分（段前 USABLE / 段内 RESERVED / 段后 USABLE），
   已经是非 USABLE 的段不用动。双保险在于：就算某段固件图压根没描述
   内核所在范围，PMM 也不会发它（未标 USABLE 即不发）——但图里**写明**
   RESERVED 才能让 PMM 的 `max_phys`/统计口径正确，且防后续维护者误判，
   所以这一步不许省。
2. **boot_info 本体与内核栈同样非 USABLE**：它们由 AllocatePages 拿，
   所在页在固件图里多半是 EfiLoaderData → 已映射为 BOOTLOADER（非 USABLE)，
   再用 `mark_range(..., YOUSJ_MEM_BOOTLOADER)` 显式标一遍，同理双保险。
   boot_info 在 kmain 读完、PMM 初始化完之前必须原样活着。
3. **条目数上限 128**（`YOUSJ_MAX_MEM_ENTRIES`）：转换时相邻、同型、首尾
   相接的固件条目先合并再写入（UEFI 图条目常碎，合并不丢信息）；合并后
   仍超 128 则**整图构建失败、停住报错，绝不截断着往下跳**——截断可能恰好
   砍掉内核段的 RESERVED 标记，是拿正确性换启动成功率，不做。

其他细节：长度为 0 的条目跳过；`base + length` 溢出回绕的条目按
「截到地址空间顶」处理且强制 RESERVED（坏固件图不许污染 USABLE）；
输入固件图要求按 base 升序（UEFI 实践中即如此），合并逻辑依赖此序，
efi_main 侧落地时若遇到乱序需先排序（boot_info.c 里已注 TODO）。

### 5.4 转换结果示例（帮助对图）

固件图（节选）：`[0, 0x9FC00) Conventional`、`[0x100000, 0x8000000) Conventional`，
内核映像放在 `[0x100000, 0x124000)`。转换 + 标内核段后：

```
[0,       0x9FC00)   USABLE      （PMM 还会另把首 1MiB 保留，见 pmm.c）
[0x100000,0x124000)  RESERVED    ← 内核映像，自 USABLE 中切出
[0x124000,0x8000000) USABLE
```

## 6. 失败处理（全部 fail closed）

| 失败点 | 动作 |
|---|---|
| 内核文件打不开/读不全 | ConOut 打印一行原因，死循环停住（`cli; hlt`)，不跳内核 |
| 映像格式不对（非 ELF64/非 x86_64，方案 A 时） | 同上 |
| AllocatePages 失败 | 同上 |
| GetMemoryMap 两次仍失败 / DescriptorVersion 不认识 | 同上 |
| boot_info 构建失败（条目超限等） | 同上（退出固件服务之前，还能用 ConOut) |
| ExitBootServices 首次失败（MapKey 过期） | 重取内存图 → **重跑转换** → 再退一次；第二次仍失败则停住（此时已无 ConOut，只能串口直写 COM1 端口或死等——实现期在引导器里留一个极简端口直写日志，只作兜底） |

总原则：宁可停在引导器报错，也不带一份不确定的 boot_info 跳内核——
kmain 只校验 magic，图错了 PMM 会把错图当真理。

## 7. 本目录交付与待落地清单

已交付：

- `DESIGN.md`：本文。
- `boot_info.h` / `boot_info.c`：boot_info 纯转换（类型映射、图转换、
  区间改型、命令行拷贝），完整可读、可单独编译自检。
- `uefi.h`：自写最小 UEFI 类型子集（ms_abi 已标注），零外部依赖。
- `efi_main.c`：efi_main 全流程真实现（读 ESP 内核文件 → ELF64
  PT_LOAD 加载并按址申页 → boot_info/栈申页 → RSDP/命令行 →
  取图+转换（转换数组在取图前预分配，取图后零分配，保 MapKey
  有效）→ ExitBootServices 一次重试 → 关中断、串口面包屑、
  RDI=boot_info 直跳 kmain 符号）。总协调验收时修了两处：
  kmain 符号名比较越界（精确名永远匹配不上）、转换数组分配
  顺序（原顺序可能使 MapKey 失效且重试无法自愈）；符号查找已
  用真实内核 ELF 对照 nm 验证（0x100245 一致）。

待落地（实现期，按序）：

- TODO-1～TODO-5：**已完成**（见上 efi_main.c；TODO-1 按倾向
  采用自写最小子集）。
- TODO-6：**已完成（2026-11-01，总协调联调）**。环境按 §2.2 拍板
  走虚拟机：沙箱实有 mingw-w64 + OVMF + mtools，本目录 Makefile
  产出 BOOTX64.EFI 与 ESP 镜像，QEMU/OVMF 端到端实测：固件 →
  本引导器（串口面包屑 YOUSJ-UEFI）→ kmain → PMM/VMM 自检 →
  VMM 硬件步切 CR3 → 调度 M1/M2 全 PASS。联调中定位并修复了
  内核借用固件 GDT 的隐患（OVMF 的 0x08 不是 64 位代码段，
  首个定时器中断即三重故障）：内核已自建 GDT（kernel gdt.c，
  kmain 最早调用），两条启动链从此共用同一张表。
- 补记（辅助旁聊 A 独立复验 + 一处待验收加固）：本旁聊亦在自有
  沙箱（apt 装 ovmf/mingw-w64/mtools）对内核 7dacdb6（尚无 gdt.c
  的版本）跑过同一端到端：无引导侧 GDT 时复现 M1 后三重故障、
  定位同上；随后在 `efi_main.c` 的 jump_to_kernel 里加了**引导侧
  GDT**（三项描述符与 stub.S 逐位一致，lgdt + far return 重载 CS
  后再跳），同一内核即全套 PASS（PMM/VMM/CR3/M1/M2，timeout 正常
  收尾）。该改动已由总协调验收提交（d4eb06e）：与 kernel gdt.c
  构成双保险——交棒瞬间起 §4.3 契约即成立，内核随后换上自己的
  同布局表；两者描述符一致，不冲突。

## 8. 给协调者的接口提案（如需批复）

1. **内核入口桩二选一**（§4.3）：BSS 由加载器清零 + 栈由加载器备好、
   直跳 kmain；还是内核侧加汇编入口桩？请随链接脚本一并定。
2. **内核链接基址**：PT_LOAD 的 p_paddr 由链接脚本决定，Bootloader
   不写死；请协调者在链接脚本落地后把基址/上限回填给本目录备案。
3. boot.h 本身**无需改动**：内存图、RSDP、命令行、帧缓冲（留空）四项
   都够阶段 1 用；fb 字段服务器版按约定全零。
