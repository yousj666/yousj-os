/* Yousj OS - system call numbers, first batch.
 *
 * These NUMBERS intentionally match the Linux x86-64 syscall ABI so that
 * unmodified Linux ELF programs can run on Yousj later (stage 2).
 * Only the numbers and calling convention are compatible; every line of
 * the implementation behind them is written from scratch for Yousj.
 * Do not copy Linux kernel source to implement any of these.
 *
 * Server-first scope (user decision, 2026-10-27): the first milestone only
 * needs console I/O, process exit, memory, and basic file access.
 * The rest are reserved here so numbers never get reassigned.
 */
#ifndef YOUSJ_SYSCALL_H
#define YOUSJ_SYSCALL_H

#define YOUSJ_SYS_read          0
#define YOUSJ_SYS_write         1
#define YOUSJ_SYS_open          2
#define YOUSJ_SYS_close         3
#define YOUSJ_SYS_lseek         8
#define YOUSJ_SYS_mmap          9
#define YOUSJ_SYS_munmap       11
#define YOUSJ_SYS_brk          12
#define YOUSJ_SYS_ioctl        16
#define YOUSJ_SYS_pipe         22
#define YOUSJ_SYS_dup          32
#define YOUSJ_SYS_nanosleep    35
#define YOUSJ_SYS_getpid       39
#define YOUSJ_SYS_clone        56   /* fork/exec come in stage 2 */
#define YOUSJ_SYS_execve       59
#define YOUSJ_SYS_exit         60
#define YOUSJ_SYS_exit_group   231 /* approved 2026-11-01 (coordinator):
                                    proposed by both SCHED_DESIGN sec. 6.1
                                    and SYSCALL_DESIGN batch 0 -- glibc's
                                    exit() really issues exit_group, so the
                                    number is required for real programs;
                                    same handler as exit */
#define YOUSJ_SYS_wait4        61
#define YOUSJ_SYS_kill         62
#define YOUSJ_SYS_uname        63
#define YOUSJ_SYS_getcwd      79
#define YOUSJ_SYS_chdir        80
#define YOUSJ_SYS_mkdir        83
#define YOUSJ_SYS_getdents64  217
#define YOUSJ_SYS_arch_prctl  158

/* Yousj-native calls live far above the Linux-compatible range so the
 * two namespaces can never collide. None are in the server-first scope. */
#define YOUSJ_SYS_NATIVE_BASE 0x1000

#endif /* YOUSJ_SYSCALL_H */
