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
 * The 9th push saves the user RSP on the kernel stack: it is the
 * one piece of per-task syscall state that must survive a
 * context switch taken inside a blocking syscall.
 */

    .section .text
    .globl syscall_entry
syscall_entry:
    swapgs
    mov %rsp, %gs:8
    mov %gs:0, %rsp



    push %r11               /* user RFLAGS */
    mov %gs:8, %r11         /* r11 is saved now: borrow it for the
                             * user RSP, which must live on THIS
                             * task's kernel stack -- the cpu_local
                             * slot alone does not survive a context
                             * switch inside a blocking syscall
                             * (another task's entry would overwrite
                             * it, and this task would resume on the
                             * other task's stack pointer) */
    push %rcx               /* user RIP */
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r11               /* user RSP (9th push, see header) */
    push %r9                /* a6: dispatcher's stack argument */
    mov %rsp, %gs:24        /* cpu_local.syscall_frame: the pushed
                             * user frame, clone's snapshot source.
                             * Slots from here: +0 r9, +8 user RSP,
                             * +16 r8, +24 r10, +32 rdx, +40 rsi,
                             * +48 rdi, +56 rcx (user RIP),
                             * +64 r11 (RFLAGS) */

    mov %r8, %r9            /* a5 */
    mov %r10, %r8           /* a4 */
    mov %rdx, %rcx          /* a3 */
    mov %rsi, %rdx          /* a2 */
    mov %rdi, %rsi          /* a1 */
    mov %rax, %rdi          /* syscall number */

    sti
    call syscall_dispatch_shim
    cli

    pop %r9                 /* restore the user's r9 -- it is a
                             * normal register (arg 6), NOT a
                             * scratch: user code may keep live
                             * state in it across a syscall */
    pop %r11                /* the user RSP, off this task's own
                             * kernel stack (see the entry) */
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rcx                /* user RIP */
    mov %r11, %gs:8         /* park the user RSP for the final
                             * load; no switch can intervene (IF
                             * is off on this whole unwind) */
    pop %r11                /* user RFLAGS */

    mov %gs:8, %rsp         /* back to the user stack */
    swapgs
    sysretq

/* The dispatcher shim (M7): yousj_syscall_dispatch() takes a
 * struct yousj_syscall_ctx (see <yousj/signal.h>) so the signal
 * machinery can see -- and, for rt_sigreturn, rewrite -- the
 * COMPLETE ring-3 register set. The argument registers are
 * already in place from the shuffle above; what is missing is
 * a6 (the topmost entry push, now one slot above this call's
 * return address), the entry frame pointer (cpu_local.
 * syscall_frame) and the user callee-saved registers, none of
 * which a plain C call could observe reliably. The ctx is
 * built on this task's kernel stack (128 bytes, so the 16-byte
 * call alignment the entry arranged is preserved); the callee-
 * saved set is reloaded FROM the ctx afterwards, which is how
 * rt_sigreturn's restored values take effect. */
syscall_dispatch_shim:
    sub $128, %rsp
    mov %rdi, 0(%rsp)       /* no */
    mov %rsi, 8(%rsp)       /* a1 */
    mov %rdx, 16(%rsp)      /* a2 */
    mov %rcx, 24(%rsp)      /* a3 */
    mov %r8, 32(%rsp)       /* a4 */
    mov %r9, 40(%rsp)       /* a5 */
    mov 136(%rsp), %rax     /* a6: [rsp+128]=ret, [+136]=a6 slot */
    mov %rax, 48(%rsp)      /* a6 */
    mov %gs:24, %rax        /* cpu_local.syscall_frame */
    mov %rax, 56(%rsp)      /* frame */
    mov %rbx, 64(%rsp)      /* callee[0..5] */
    mov %rbp, 72(%rsp)
    mov %r12, 80(%rsp)
    mov %r13, 88(%rsp)
    mov %r14, 96(%rsp)
    mov %r15, 104(%rsp)
    mov %rsp, %rdi
    call yousj_syscall_dispatch
    mov 64(%rsp), %rbx
    mov 72(%rsp), %rbp
    mov 80(%rsp), %r12
    mov 88(%rsp), %r13
    mov 96(%rsp), %r14
    mov 104(%rsp), %r15
    add $128, %rsp
    ret

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
    mov %rdi, %rax          /* base in rax: rbx itself is loaded
                             * from the snapshot (M7) */
    mov 0(%rax), %rdi
    mov 8(%rax), %rsi
    mov 16(%rax), %rdx
    mov 24(%rax), %rcx      /* user RIP (sysret loads it) */
    mov 32(%rax), %r10
    mov 40(%rax), %r8
    mov 48(%rax), %r9
    mov 56(%rax), %r11      /* user RFLAGS (sysret loads it) */
    mov 72(%rax), %rbx      /* callee-saved set (M7) */
    mov 80(%rax), %rbp
    mov 88(%rax), %r12
    mov 96(%rax), %r13
    mov 104(%rax), %r14
    mov 112(%rax), %r15
    mov 64(%rax), %rsp      /* user stack pointer (last: every
                             * load above reads through rax) */
    xor %eax, %eax          /* the child sees clone() == 0 */
    sysretq

    .section .note.GNU-stack, "", @progbits
