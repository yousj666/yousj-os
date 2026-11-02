/* Yousj OS - process entry stub for freestanding user programs.
 * The kernel's ELF loader lands here (ELF entry point) with the
 * initial stack laid out per the Linux convention:
 *   (%rsp) = argc, 8(%rsp) = argv[0], ..., NULL, envp NULL,
 *   auxv pairs (AT_PAGESZ, AT_NULL) -- see the M4 launcher.
 * main() gets (argc, argv); if it ever returns, its status goes
 * to exit_group (for init, the kernel turns that into the
 * "Attempted to kill init" panic, by design).
 */
    .text
    .globl _start
_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi            /* argc */
    lea 8(%rsp), %rsi           /* argv */
    and $-16, %rsp
    call main
    mov %rax, %rdi
    mov $231, %rax              /* exit_group */
    syscall
1:  jmp 1b

    .section .note.GNU-stack, "", @progbits
