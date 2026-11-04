/* Yousj OS - gterm: a graphical terminal (M88).
 *
 * Opens a PTY pair (M87) and a window (M88), forks the
 * shell onto the slave end, then pumps: PTY output goes
 * to the window, window keystrokes go to the PTY. The
 * shell does its own echo and line editing on the raw
 * stream, exactly as it does on the console.
 *
 * The window is focused when it opens, so typing lands
 * here until the user clicks the console window or the
 * desktop. Closing the window with the mouse (gui_getc
 * reports -2) ends the terminal: the child is killed,
 * the pair closed, the window closed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_poll       7
#define SYS_dup2       33
#define SYS_clone      56
#define SYS_execve     59
#define SYS_exit       60
#define SYS_wait4      61
#define SYS_kill       62
#define SYS_exit_group 231
#define SYS_pty_slave  0x1005
#define SYS_gui_open   0x1006
#define SYS_gui_write  0x1007
#define SYS_gui_getc   0x1008
#define SYS_gui_close  0x1009

#define O_RDWR 2
#define POLLIN 0x1
#define WNOHANG 1
#define SIGKILL 9

static long sys1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static uint64_t slen(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void emit(const char *s)
{
    sys3(SYS_write, 1, (long)s, (long)slen(s));
}

struct pollfd {
    int fd;
    short events;
    short revents;
};

void main(void)
{
    long m, s, win, pid;

    m = sys3(SYS_open, (long)"/dev/ptmx", O_RDWR, 0);
    if (m < 0) {
        emit("gterm: cannot open /dev/ptmx\n");
        sys1(SYS_exit_group, 1);
    }
    s = sys1(SYS_pty_slave, m);
    if (s < 0) {
        emit("gterm: pty_slave failed\n");
        sys1(SYS_exit_group, 1);
    }
    win = sys3(SYS_gui_open, (long)"TERMINAL", 880, 560);
    if (win < 0) {
        emit("gterm: gui_open failed\n");
        sys1(SYS_exit_group, 1);
    }

    pid = sys3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
    if (pid == 0) {
        static const char *const argv[] = { "sh", NULL };
        sys3(SYS_dup2, s, 0, 0);
        sys3(SYS_dup2, s, 1, 0);
        sys3(SYS_dup2, s, 2, 0);
        sys1(SYS_close, m);
        sys1(SYS_close, s);
        sys3(SYS_execve, (long)"/bin/sh", (long)argv, 0);
        sys1(SYS_exit, 127);
    }
    sys1(SYS_close, s);

    for (;;) {
        struct pollfd p;
        int status = 0;
        long c;
        int child_gone = 0;

        if (sys4(SYS_wait4, pid, (long)&status, WNOHANG, 0) == pid)
            child_gone = 1;

        p.fd = (int)m;
        p.events = POLLIN;
        p.revents = 0;
        if (sys3(SYS_poll, (long)&p, 1, 10) > 0 &&
            (p.revents & POLLIN)) {
            char buf[256];
            long n = sys3(SYS_read, m, (long)buf, sizeof(buf));
            if (n > 0)
                sys3(SYS_gui_write, win, (long)buf, n);
            else if (n == 0)
                child_gone = 1; /* slave side fully closed */
        }

        while ((c = sys1(SYS_gui_getc, win)) >= 0) {
            char ch = (char)c;
            sys3(SYS_write, m, (long)&ch, 1);
        }
        if (c == -2)
            break; /* the mouse closed the window */
        if (child_gone)
            break;
    }

    /* If the child is still alive (window closed under it),
     * do not leave it spinning on a dead PTY. */
    {
        int status = 0;
        if (sys4(SYS_wait4, pid, (long)&status, WNOHANG, 0) != pid) {
            sys2(SYS_kill, pid, SIGKILL);
            sys4(SYS_wait4, pid, (long)&status, 0, 0);
        }
    }
    sys1(SYS_gui_close, win);
    sys1(SYS_close, m);
    sys1(SYS_exit_group, 0);
}
