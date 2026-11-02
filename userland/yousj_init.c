/* Yousj OS - init, the first real user process (milestone M4).
 *
 * A freestanding static ELF (no libc): every service comes from a
 * raw syscall. Per SCHED_DESIGN sec. 7.3 step 6, until clone/execve
 * land (M5) and a standalone shell can be spawned, init carries a
 * minimal built-in command loop itself:
 *
 *   help          list built-ins
 *   ps            task table (native ps_snapshot syscall)
 *   uname         system identity
 *   kill <tid>    retire a ready/sleeping task (stage-1 kill)
 *   exit          leave init -- the kernel panics by design
 *
 * Input is read from fd 0 (the serial console) one blocking read
 * at a time; init does its own echo and line editing.
 */
#include <stddef.h>
#include <stdint.h>

/* --- raw syscalls (numbers match kernel/include/yousj/syscall.h) --- */

static long sys_call3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static long sys_call1(long n, long a)
{
    return sys_call3(n, a, 0, 0);
}

static long sys_call2(long n, long a, long b)
{
    return sys_call3(n, a, b, 0);
}

#define SYS_read       0
#define SYS_write      1
#define SYS_nanosleep  35
#define SYS_getpid     39
#define SYS_kill       62
#define SYS_clone      56
#define SYS_execve     59
#define SYS_wait4      61
#define SYS_uname      63
#define SYS_exit_group 231
#define SYS_PS_NATIVE  0x1000

/* --- freestanding basics ---------------------------------------------- */

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

static void out_n(const char *s, size_t n)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)n);
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

static int starts_with(const char *line, const char *word)
{
    while (*word != '\0') {
        if (*line++ != *word++)
            return 0;
    }
    return *line == '\0' || *line == ' ';
}

static long parse_long(const char *s)
{
    long v = 0;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* --- the command loop --------------------------------------------------- */

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static void print_banner(void)
{
    struct utsname u;

    if (sys_call1(SYS_uname, (long)(uintptr_t)&u) == 0) {
        out("yousj-init: ");
        out(u.sysname);
        out(" ");
        out(u.release);
        out(" ");
        out(u.machine);
    } else {
        out("yousj-init: (uname failed)");
    }
    out(" (pid ");
    out_long(sys_call1(SYS_getpid, 0));
    out(")\n");
    /* The marker contract carried over from the scaffold init:
     * first user process up, console write path alive. */
    out("yousj-init: INIT_SMOKE_OK\n");
}

static void cmd_ps(void)
{
    static char buf[4096];
    long n = sys_call2(SYS_PS_NATIVE, (long)(uintptr_t)buf,
                       (long)sizeof(buf));
    if (n < 0) {
        out("ps failed\n");
        return;
    }
    out_n(buf, (size_t)n);
}

static void cmd_kill(const char *line)
{
    long tid = parse_long(line + 4);
    long r;

    if (tid <= 0) {
        out("usage: kill <tid>\n");
        return;
    }
    r = sys_call2(SYS_kill, tid, 9);
    if (r == 0) {
        out("killed ");
        out_long(tid);
        out("\n");
    } else {
        out("kill failed (");
        out_long(r);
        out(")\n");
    }
}

static void run_command(const char *line)
{
    if (line[0] == '\0')
        return;
    if (starts_with(line, "help")) {
        out("built-ins: help ps uname exec <path> kill <tid> exit\n");
        out("ps states: R running, r ready, S sleeping, Z zombie, D dead\n");
    } else if (starts_with(line, "ps")) {
        cmd_ps();
    } else if (starts_with(line, "uname")) {
        struct utsname u;
        if (sys_call1(SYS_uname, (long)(uintptr_t)&u) == 0) {
            out(u.sysname);
            out(" ");
            out(u.release);
            out(" ");
            out(u.machine);
            out("\n");
        }
    } else if (starts_with(line, "exec")) {
        /* exec <path>: replace this process image (M5). On
         * failure the syscall returns and we report it; on
         * success this code never runs again. */
        const char *path = line + 4;
        const char *argv[2];
        while (*path == ' ')
            path++;
        if (*path == '\0') {
            out("usage: exec <path>\n");
            return;
        }
        argv[0] = path;
        argv[1] = NULL;
        {
            long r = sys_call3(SYS_execve, (long)(uintptr_t)path,
                               (long)(uintptr_t)argv, 0);
            out("exec failed (");
            out_long(r);
            out(")\n");
        }
    } else if (starts_with(line, "kill")) {
        cmd_kill(line);
    } else if (starts_with(line, "exit")) {
        out("yousj-init: exiting -- the kernel panics when init "
            "exits, by design\n");
        sys_call1(SYS_exit_group, 0);
    } else {
        out("unknown command: ");
        out(line);
        out("\n");
    }
}

int main(int argc, char **argv)
{
    static char line[128];

    (void)argc;
    (void)argv;
    print_banner();

    for (;;) {
        size_t n = 0;

        out("init> ");
        for (;;) {
            char ch;
            long r = sys_call3(SYS_read, 0, (long)(uintptr_t)&ch, 1);

            if (r <= 0)
                continue; /* console hiccup: keep waiting */
            if (ch == '\r')
                ch = '\n';
            if (ch == '\n') {
                out("\n");
                break;
            }
            if (ch == 0x7F || ch == 0x08) { /* backspace */
                if (n > 0) {
                    n--;
                    out("\b \b");
                }
                continue;
            }
            if (n < sizeof(line) - 1) {
                line[n++] = ch;
                out_n(&ch, 1); /* echo */
            }
        }
        line[n] = '\0';
        run_command(line);
    }
}
