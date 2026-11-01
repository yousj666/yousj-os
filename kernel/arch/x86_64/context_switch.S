/* Yousj OS - context switch between kernel tasks.
 * Written by Muse (coordinator), per docs/SCHED_DESIGN.md sec. 5.
 *
 * void context_switch(struct task *prev, struct task *next)
 * (arguments arrive in rdi and rsi respectively)
 *
 * Only the callee-saved set is preserved: rbx, rbp, r12-r15 are
 * pushed on prev's stack, rsp is stored into prev->ctx.rsp (offset
 * 0 in struct task -- pinned by a static assert in sched.c), then
 * rsp is loaded from next->ctx.rsp, the same registers are popped
 * from next's stack, and `ret` lands either at next's saved return
 * address (a task switched out here before) or at its fabricated
 * first-run frame built by kthread_create() -- whose layout MUST
 * mirror the pop order below:
 *   pop order: r15, r14, r13, r12, rbx, rbp, then ret.
 * This function touches no CR3, no flags, and calls nothing.
 */

    .section .text
    .globl context_switch
context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)        /* prev->ctx.rsp */
    mov (%rsi), %rsp        /* next->ctx.rsp */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

    .section .note.GNU-stack, "", @progbits
