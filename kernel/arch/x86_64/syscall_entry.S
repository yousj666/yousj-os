/* Yousj OS - `syscall` instruction entry and the ring-3 drop.
 * Written by Muse (coordinator), per docs/SYSCALL_DESIGN.md
 * sec. 2/5: syscall/sysret only, no int 0x80 path.
 *
 * Entry state (hardware-defined): rax = syscall number,
 * rdi/rsi/rdx/r10/r8/r9 = args 1..6 (Linux x86-64 order; note r10
 * instead of rcx), rcx = user RIP, r11 = user RFLAGS, interrupts
 * masked by SFMASK, CPL 0, but RSP is still the USER stack and GS
 * still the user's -- the first three instructions fix both via
 * the cpu_local area (kernel/syscall/syscall.c):
 *
 *   swapgs                  GS <-> kernel GS base (= &cpu_local)
 *   mov %rsp, %gs:8         save user RSP   (cpu_local.user_rsp_save)
 *   mov %gs:0, %rsp         load kernel RSP (cpu_local.kernel_rsp,
 *                           kept equal to the current task's
 *                           kernel stack top by arch_on_task_switch)
 *
 * Then every register the dispatcher or the return path cares
 * about is pushed, arguments are shuffled into the SysV order the
 * C dispatcher expects (its 7th argument, a6, is the topmost push
 * so it lands exactly in the stack-arg slot), and the dispatcher
 * runs with interrupts on. On return, rax holds the result and the
 * pushes unwind; user registers other than rax/rcx/r11 survive, as
 * Linux requires. sysretq reloads RIP from rcx and RFLAGS from
 * r11.
 *
 * Push count is 9, which with kernel_rsp 8-mod-16 aligned leaves
 * the stack 16-byte aligned at the `call`, as the SysV ABI wants.
 */

    .section .text
    .globl syscall_entry
syscall_entry:
    swapgs
    mov %rsp, %gs:8
    mov %gs:0, %rsp



    push %r11               /* user RFLAGS */
    push %rcx               /* user RIP */
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push $0                 /* alignment pad (9th push, see header) */
    push %r9                /* a6: dispatcher's stack argument */
    mov %rsp, %gs:24        /* cpu_local.syscall_frame: the pushed
                             * user frame, clone's snapshot source.
                             * Slots from here: +0 r9, +8 pad, +16 r8,
                             * +24 r10, +32 rdx, +40 rsi, +48 rdi,
                             * +56 rcx (user RIP), +64 r11 (RFLAGS) */

    mov %r8, %r9            /* a5 */
    mov %r10, %r8           /* a4 */
    mov %rdx, %rcx          /* a3 */
    mov %rsi, %rdx          /* a2 */
    mov %rdi, %rsi          /* a1 */
    mov %rax, %rdi          /* syscall number */

    sti
    call yousj_syscall_dispatch
    cli

    pop %r9                 /* restore the user's r9 -- it is a
                             * normal register (arg 6), NOT a
                             * scratch: user code may keep live
                             * state in it across a syscall */
    add $8, %rsp            /* drop the alignment pad */
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rcx                /* user RIP */
    pop %r11                /* user RFLAGS */

    mov %gs:8, %rsp         /* back to the user stack */
    swapgs
    sysretq

/* void enter_user_mode(uint64_t ip, uint64_t sp)
 * Build the iretq frame for the first ring-3 landing: user SS,
 * user RSP, RFLAGS with IF set, user CS, entry IP. Data segments
 * are switched to the user data selector first (the loads happen
 * here, at CPL 0, where they are legal). Never returns. */
    .globl enter_user_mode
enter_user_mode:
    mov $0x1B, %ax          /* user data selector | RPL 3 */
    mov %ax, %ds
    mov %ax, %es
    pushq $0x1B             /* SS */
    push %rsi               /* RSP */
    pushfq                  /* RFLAGS ... */
    pop %rax
    or $0x200, %rax         /* ... with IF set */
    push %rax
    pushq $0x23             /* CS: user code selector | RPL 3 */
    push %rdi               /* RIP */
    iretq

/* void user_frame_resume(uint64_t *resume_fields)
 * Clone landing (M5): load a cloned child's saved ring-3
 * registers (the resume_* fields of its task, in sched.h order)
 * and sysret into it with rax = 0 -- the child returns from its
 * clone call exactly where the parent called it. The caller has
 * already normalized the GS pair (see user_gs_normalize()).
 * Never returns. */
    .globl user_frame_resume
user_frame_resume:
    mov $0x1B, %ax          /* user data segments, as in ring 3 */
    mov %ax, %ds
    mov %ax, %es
    mov %rdi, %rbx
    mov 0(%rbx), %rdi
    mov 8(%rbx), %rsi
    mov 16(%rbx), %rdx
    mov 24(%rbx), %rcx      /* user RIP (sysret loads it) */
    mov 32(%rbx), %r10
    mov 40(%rbx), %r8
    mov 48(%rbx), %r9
    mov 56(%rbx), %r11      /* user RFLAGS (sysret loads it) */
    mov 64(%rbx), %rsp      /* user stack pointer */
    xor %eax, %eax          /* the child sees clone() == 0 */
    sysretq

    .section .note.GNU-stack, "", @progbits
