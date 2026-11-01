/* Yousj OS - first ring-3 program (M3 smoke).
 * Assembled to a flat binary and linked at YOUSJ_USER_CODE_VA
 * (0x8000000000); position-independent within its page: all data
 * is reached through %rip-relative addressing.
 *
 * The program proves, in order:
 *   1. `syscall` reaches the kernel (getpid, 39) and returns.
 *   2. The kernel can read user memory (write, 1, of a string and
 *      of a pid rendered in decimal, using the stack as scratch).
 *   3. exit(60) kills the task with a checkable status (42).
 *
 * Calling convention note: the kernel's syscall entry preserves
 * all registers except rax/rcx/r11 (as Linux requires), so %r12
 * is used for the pid across calls.
 */
    .text
    .globl _start
_start:
    mov $39, %rax               /* getpid */
    syscall
    mov %rax, %r12              /* keep the pid */

    mov $1, %rdi                /* write(1, msg, msg_len) */
    lea msg(%rip), %rsi
    mov $msg_len, %rdx
    mov $1, %rax
    syscall

    /* Render the pid as decimal onto the stack (pid < 10000 is
     * more than enough for this boot, but the loop is general). */
    sub $32, %rsp
    lea 31(%rsp), %rsi          /* write digits backwards */
    mov %r12, %rax
    mov $10, %rcx
.Ldigit:
    xor %rdx, %rdx
    div %rcx
    add $'0', %dl
    mov %dl, (%rsi)
    dec %rsi
    test %rax, %rax
    jne .Ldigit
    inc %rsi                    /* first digit */
    lea 32(%rsp), %rdx          /* one past the last digit */
    sub %rsi, %rdx              /* digit count */
    mov $1, %rdi
    /* rsi = first digit, rdx = count */
    mov $1, %rax
    syscall

    lea nl(%rip), %rsi          /* write "\n" */
    mov $1, %rdi
    mov $1, %rdx
    mov $1, %rax
    syscall
    add $32, %rsp

    mov $42, %rdi               /* exit(42) */
    mov $60, %rax
    syscall
    /* Should never reach here; spin defensively. */
1:  jmp 1b

msg:
    .ascii "Yousj OS: ring3 alive, getpid="
msg_len = . - msg
nl:
    .ascii "\n"

    .section .note.GNU-stack, "", @progbits
