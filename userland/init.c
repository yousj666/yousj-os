/*
 * Yousj OS - minimal init scaffold (kernel-milestone support ONLY).
 *
 * Purpose (kernel-first phase): give the kernel the smallest possible
 * PID 1 to smoke-test the moment the scheduler + syscall entry +
 * execve path land. It validates exactly one thing end to end:
 *
 *     first user process runs -> write() reaches the console
 *     -> getpid() says 1 -> clone() makes a child -> execve() loads it
 *     -> child exits -> wait4() reaps it -> marker line printed
 *
 * That marker line is the contract the QEMU smoke test will grep for:
 *
 *     yousj-init: INIT_SMOKE_OK
 *
 * This is a HOST-SIDE PROTOTYPE (same status as minishell.c): it
 * compiles and runs on the Linux host today so the logic is verified
 * before Yousj can run it. It does NOT use any libc stdio; output goes
 * through write() directly, so what you see is what the syscalls do.
 *
 * Syscall subset used - and nothing else. All numbers already defined
 * in kernel/include/yousj/syscall.h (proposal-free by design):
 *
 *     write       1   all output (fd 1, errors to fd 2)
 *     nanosleep  35   respawn throttle / PID 1 idle park
 *     getpid     39   PID 1 check
 *     clone      56   via fork() here; Yousj side = fork-style clone
 *     execve     59   called directly, empty environment
 *     exit       60   _exit() in the child if execve fails; exit() is
 *                     used ONLY on the not-PID-1 host/test path -
 *                     a real PID 1 must never exit (see DESIGN.md 2.3)
 *     wait4      61   called directly (glibc wait4), reaps the child
 *                     plus any other zombie that shows up
 *
 * Behaviour depends on the pid, deliberately:
 *   - PID 1 (real Yousj boot): supervise loop forever - spawn child,
 *     reap it, print the marker once per cycle, sleep 1s, respawn.
 *     Never returns, never exits.
 *   - not PID 1 (host test): run ONE spawn/reap cycle, print the
 *     marker on success, and exit with the child's status (127 if
 *     execve failed), so a developer workstation is never parked.
 *
 * Usage:
 *   host build:  gcc -Wall -Wextra -o /tmp/yousj-init userland/init.c
 *   host test:   /tmp/yousj-init /bin/echo hello-from-child
 *   Yousj boot:  no arguments; default child is /bin/minishell
 *                (override at build time: -DYOUSJ_INIT_CHILD='"/bin/sh"')
 */

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#ifndef YOUSJ_INIT_CHILD
#define YOUSJ_INIT_CHILD "/bin/minishell"
#endif

/* --- tiny write()-only output helpers (no stdio, no formatting lib) --- */

static unsigned long slen(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static void out(int fd, const char *s)
{
    unsigned long left = slen(s);

    while (left > 0) {
        ssize_t done = write(fd, s, left);

        if (done < 0) {
            if (errno == EINTR)
                continue;
            return; /* console gone: nothing useful left to do */
        }
        s += (unsigned long)done;
        left -= (unsigned long)done;
    }
}

static void out_long(int fd, long v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long u;

    buf[i] = '\0';
    if (v < 0) {
        out(fd, "-");
        u = (unsigned long)(-(v + 1)) + 1; /* LONG_MIN-safe enough here */
    } else {
        u = (unsigned long)v;
    }
    if (u == 0)
        buf[--i] = '0';
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    out(fd, &buf[i]);
}

static void sleep_1s(void)
{
    struct timespec ts;

    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* Spawn target (argv = its full argument vector) and reap it.
 * Returns the child's exit code, 128+signal if it was killed, or
 * 127 if execve failed in the child / fork itself failed. */
static int spawn_and_reap(char *const target_argv[])
{
    pid_t pid = fork(); /* Yousj: clone(56), fork-style flags */

    if (pid < 0) {
        out(STDERR_FILENO, "yousj-init: fork/clone failed\n");
        return 127;
    }
    if (pid == 0) {
        char *const empty_env[] = { NULL };

        execve(target_argv[0], target_argv, empty_env); /* execve(59) */
        out(STDERR_FILENO, "yousj-init: execve failed for ");
        out(STDERR_FILENO, target_argv[0]);
        out(STDERR_FILENO, "\n");
        _exit(127); /* exit(60) */
    }

    /* Parent: wait for this child with wait4(61) directly. */
    {
        int status = 0;
        pid_t got;

        do {
            got = wait4(pid, &status, 0, NULL);
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            out(STDERR_FILENO, "yousj-init: wait4 failed\n");
            return 127;
        }

        /* Sweep: reap any other zombie already waiting (orphans get
         * re-parented to PID 1; collecting them is init's hard duty). */
        while (wait4(-1, NULL, WNOHANG, NULL) > 0)
            ;

        if (WIFEXITED(status)) {
            out(STDOUT_FILENO, "yousj-init: child exited code=");
            out_long(STDOUT_FILENO, WEXITSTATUS(status));
            out(STDOUT_FILENO, "\n");
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            out(STDOUT_FILENO, "yousj-init: child killed signal=");
            out_long(STDOUT_FILENO, WTERMSIG(status));
            out(STDOUT_FILENO, "\n");
            return 128 + WTERMSIG(status);
        }
        return 127;
    }
}

int main(int argc, char *argv[])
{
    static char *default_argv[] = { YOUSJ_INIT_CHILD, NULL };
    char **target_argv;
    long my_pid;
    int am_pid1;

    (void)argc;
    target_argv = (argv[1] != NULL) ? &argv[1] : default_argv;

    my_pid = (long)getpid(); /* getpid(39) */
    am_pid1 = (my_pid == 1);

    out(STDOUT_FILENO, "yousj-init: start pid=");
    out_long(STDOUT_FILENO, my_pid);
    out(STDOUT_FILENO, " child=");
    out(STDOUT_FILENO, target_argv[0]);
    out(STDOUT_FILENO, "\n");
    if (!am_pid1)
        out(STDOUT_FILENO,
            "yousj-init: not PID 1 - host/test mode, single pass\n");

    for (;;) {
        int rc = spawn_and_reap(target_argv);

        if (rc == 0) {
            out(STDOUT_FILENO, "yousj-init: INIT_SMOKE_OK\n");
        } else {
            out(STDOUT_FILENO, "yousj-init: INIT_SMOKE_FAIL rc=");
            out_long(STDOUT_FILENO, rc);
            out(STDOUT_FILENO, "\n");
        }

        if (!am_pid1)
            return rc; /* host/test: exit(60) with the child's status */

        /* Real PID 1: never exit. Throttle, then respawn the child
         * (minimal supervision; the full policy is DESIGN.md sec. 2). */
        sleep_1s(); /* nanosleep(35) */
    }
}
