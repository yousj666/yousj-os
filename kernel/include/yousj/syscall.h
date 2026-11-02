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
#define YOUSJ_SYS_stat          4  /* M20 */
#define YOUSJ_SYS_fstat         5  /* M20 */
#define YOUSJ_SYS_lstat         6  /* M20 */
#define YOUSJ_SYS_close         3
#define YOUSJ_SYS_lseek         8
#define YOUSJ_SYS_poll          7   /* M15 */
#define YOUSJ_SYS_mmap          9
#define YOUSJ_SYS_mprotect     10  /* M21 */
#define YOUSJ_SYS_munmap       11
#define YOUSJ_SYS_msync        26   /* M14 */
#define YOUSJ_SYS_select       23   /* M15 */
#define YOUSJ_SYS_pselect6     270  /* M15 */
#define YOUSJ_SYS_epoll_wait   232  /* M18 */
#define YOUSJ_SYS_epoll_ctl    233  /* M18 */
#define YOUSJ_SYS_epoll_pwait  281  /* M18 */
#define YOUSJ_SYS_epoll_create1 291 /* M18 */
#define YOUSJ_SYS_ppoll        271  /* M15 */
#define YOUSJ_SYS_brk          12
#define YOUSJ_SYS_rt_sigaction 13   /* M7: signal batch */
#define YOUSJ_SYS_rt_sigprocmask 14
#define YOUSJ_SYS_rt_sigreturn 15
#define YOUSJ_SYS_ioctl        16
#define YOUSJ_SYS_pipe         22
#define YOUSJ_SYS_dup          32
#define YOUSJ_SYS_dup2         33   /* M10 */
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
#define YOUSJ_SYS_rename       82   /* M13 */
#define YOUSJ_SYS_mkdir        83
#define YOUSJ_SYS_rmdir        84   /* M13 */
#define YOUSJ_SYS_unlink       87   /* M13 */
#define YOUSJ_SYS_getdents64  217
#define YOUSJ_SYS_openat      257
#define YOUSJ_SYS_newfstatat  262  /* M20 */  /* M9 */
#define YOUSJ_SYS_unlinkat    263  /* M13 */
#define YOUSJ_SYS_symlink      88   /* M16 */
#define YOUSJ_SYS_readlink     89   /* M16 */
#define YOUSJ_SYS_symlinkat   266   /* M16 */
#define YOUSJ_SYS_dup3        292  /* M10 */
#define YOUSJ_SYS_pipe2       293  /* M10 */
#define YOUSJ_SYS_time        201  /* M23 */
#define YOUSJ_SYS_gettimeofday 96  /* M23 */
#define YOUSJ_SYS_clock_gettime 228 /* M23 */
#define YOUSJ_SYS_arch_prctl  158
#define YOUSJ_SYS_rt_sigpending 127 /* M7 */
#define YOUSJ_SYS_sigaltstack  131  /* M7: -ENOSYS for now */

/* Yousj-native calls live far above the Linux-compatible range so the
 * two namespaces can never collide. */
#define YOUSJ_SYS_NATIVE_BASE 0x1000

/* ps_snapshot(buf, len) -> bytes written: one "<tid> <state> <name>"
 * line per task (state: R running, r ready, S sleeping, Z zombie,
 * T stopped by a signal -- M7).
 * Linux has no such syscall (the information lives in /proc there);
 * until Yousj has a VFS, this native call is how init's `ps`
 * built-in sees the task table. Deliberately text, deliberately
 * simple; it goes away when /proc arrives. */
#define YOUSJ_SYS_NATIVE_PS_SNAPSHOT (YOUSJ_SYS_NATIVE_BASE + 0)
#define YOUSJ_SYS_NATIVE_PMM_FREE    (YOUSJ_SYS_NATIVE_BASE + 1)

#endif /* YOUSJ_SYSCALL_H */
