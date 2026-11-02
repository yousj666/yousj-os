/* Yousj OS - init, the supervisor (milestones M4 -> M5).
 *
 * A freestanding static ELF on raw syscalls. M4 ran a built-in
 * command loop here; with clone/execve/wait4 landed (M5), init
 * does its real job, following B's supervisor design
 * (userland/init.c prototype, DESIGN.md sec. 2):
 *
 *   print the banner, then forever: clone a child that execve's
 *   the shell (/bin/sh), supervise with wait4(-1) -- reaping
 *   adopted orphans the moment they die (M27) and reporting
 *   only the shell's exit -- print the smoke marker after a
 *   clean cycle, throttle 1s, respawn.
 *
 * init never exits: the kernel panics if it does ("Attempted
 * to kill init"), by design.
 */
#include <stddef.h>
#include <stdint.h>

static long sys_call3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static long sys_call4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_nanosleep  35
#define SYS_getpid     39
#define SYS_clone      56
#define SYS_execve     59
#define SYS_wait4      61
#define SYS_uname      63
#define SYS_exit_group 231

#define SIGCHLD 17
#define WNOHANG 1

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int v, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = (uint8_t)v;
    return dst;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)slen(s));
}

static void out_long(long v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long u;

    buf[i] = '\0';
    if (v < 0) {
        out("-");
        u = (unsigned long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0)
        buf[--i] = '0';
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    out(&buf[i]);
}

static void print_banner(void)
{
    struct utsname u;

    out("yousj-init: ");
    if (sys_call3(SYS_uname, (long)(uintptr_t)&u, 0, 0) == 0) {
        out(u.sysname);
        out(" ");
        out(u.release);
        out(" ");
        out(u.machine);
    } else {
        out("(uname failed)");
    }
    out(" (pid ");
    out_long(sys_call3(SYS_getpid, 0, 0, 0));
    out(")\n");
}

/* Spawn the shell and supervise it. Returns its exit code
 * (128+signal if killed, 127 on spawn/exec failure).
 *
 * M27: the wait is wait4(-1), not wait4(shell_pid). Any child
 * exit wakes it -- the shell's, or an adopted orphan's -- and
 * the loop reaps whatever died, reporting only the shell.
 * Orphans adopted mid-session are therefore collected the
 * moment they die; the old shape (block on the shell, then
 * WNOHANG-sweep) left them as zombies for the whole session,
 * sometimes forever in practice. An orphan that dies during
 * the 1 s respawn throttle below is reaped on the next pass. */
static int spawn_and_reap(void)
{
    static const char *const shell_argv[] = { "/bin/sh", NULL };
    static const char *const empty_env[] = { NULL };
    long pid = sys_call3(SYS_clone, SIGCHLD, 0, 0);

    if (pid < 0) {
        out("yousj-init: clone failed\n");
        return 127;
    }
    if (pid == 0) {
        sys_call3(SYS_execve, (long)(uintptr_t)"/bin/sh",
                  (long)(uintptr_t)shell_argv,
                  (long)(uintptr_t)empty_env);
        out("yousj-init: execve of /bin/sh failed\n");
        sys_call3(SYS_exit_group, 127, 0, 0);
    }
    for (;;) {
        int status = 0;
        long got = sys_call4(SYS_wait4, -1, (long)(uintptr_t)&status,
                             0, 0);

        if (got < 0) {
            out("yousj-init: wait4 failed\n");
            return 127;
        }
        if (got != pid)
            continue; /* an adopted orphan, reaped: quiet duty */
        if ((status & 0x7F) == 0) {
            out("yousj-init: child exited code=");
            out_long((status >> 8) & 0xFF);
            out("\n");
            return (status >> 8) & 0xFF;
        }
        out("yousj-init: child killed signal=");
        out_long(status & 0x7F);
        out("\n");
        return 128 + (status & 0x7F);
    }
}

int main(void)
{
    print_banner();

    for (;;) {
        int rc = spawn_and_reap();

        if (rc == 0)
            out("yousj-init: INIT_SMOKE_OK\n");
        else {
            out("yousj-init: INIT_SMOKE_FAIL rc=");
            out_long(rc);
            out("\n");
        }

        /* Respawn throttle: PID 1 parks here between shells. */
        {
            struct timespec ts;
            ts.tv_sec = 1;
            ts.tv_nsec = 0;
            sys_call3(SYS_nanosleep, (long)(uintptr_t)&ts, 0, 0);
        }
    }
}
