/* Yousj OS - yousj-sh: the standalone shell (milestone M5).
 *
 * A freestanding static ELF on raw syscalls, ported from the
 * logic of B's host-side minishell.c prototype (same scope:
 * whitespace tokenizing only; no quotes, escapes, globbing,
 * pipes or redirection). It runs as its own process under the
 * supervisor init: external commands are clone + execve +
 * wait4, exactly the process family M5 lands.
 *
 * Built-ins: help, ps, uname, echo, exit. Anything else is
 * treated as a program path; a bare name is searched as
 * /bin/<name> first, then as given.
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
#define SYS_getpid     39
#define SYS_clone      56
#define SYS_execve     59
#define SYS_exit       60
#define SYS_wait4      61
#define SYS_uname      63
#define SYS_exit_group 231
#define SYS_PS_NATIVE  0x1000
#define SYS_PMM_FREE   0x1001

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

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void cmd_ps(void)
{
    static char buf[4096];
    long n = sys_call3(SYS_PS_NATIVE, (long)(uintptr_t)buf,
                       (long)sizeof(buf), 0);
    if (n < 0) {
        out("ps failed\n");
        return;
    }
    out_n(buf, (size_t)n);
}

/* Run one external command and wait for it. Returns the exit
 * code (128+signal if killed, 127 on spawn/exec failure). */
static int run_external(char **argv)
{
    long pid = sys_call3(SYS_clone, SIGCHLD, 0, 0);

    if (pid < 0) {
        out("sh: clone failed\n");
        return 127;
    }
    if (pid == 0) {
        static const char *const empty_env[] = { NULL };
        char pathbuf[128];
        const char *p = argv[0];
        int has_slash = 0;
        size_t i;

        for (i = 0; p[i] != '\0'; i++)
            if (p[i] == '/')
                has_slash = 1;
        if (!has_slash) {
            const char *prefix = "/bin/";
            size_t j = 0;
            for (i = 0; prefix[i] != '\0'; i++)
                pathbuf[j++] = prefix[i];
            for (i = 0; p[i] != '\0' && j + 1 < sizeof(pathbuf); i++)
                pathbuf[j++] = p[i];
            pathbuf[j] = '\0';
            sys_call3(SYS_execve, (long)(uintptr_t)pathbuf,
                      (long)(uintptr_t)argv, (long)(uintptr_t)empty_env);
            /* fall through: try the bare name as the path */
        }
        sys_call3(SYS_execve, (long)(uintptr_t)p,
                  (long)(uintptr_t)argv, (long)(uintptr_t)empty_env);
        out("sh: cannot exec ");
        out(p);
        out("\n");
        sys_call3(SYS_exit_group, 127, 0, 0);
    }
    {
        int status = 0;
        long got = sys_call4(SYS_wait4, pid, (long)(uintptr_t)&status,
                             0, 0);
        if (got < 0) {
            out("sh: wait4 failed\n");
            return 127;
        }
        if ((status & 0x7F) == 0)
            return (status >> 8) & 0xFF;
        return 128 + (status & 0x7F);
    }
}

static int run_line(char *line)
{
    char *argv[9];
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < 8) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0)
        return 0;

    if (streq(argv[0], "exit"))
        return -1;
    if (streq(argv[0], "help")) {
        out("built-ins: help ps free uname echo exit\n");
        out("external: any program in the image registry, "
            "e.g. hello\n");
        return 0;
    }
    if (streq(argv[0], "free")) {
        out("free pages: ");
        out_long(sys_call3(SYS_PMM_FREE, 0, 0, 0));
        out("\n");
        return 0;
    }
    if (streq(argv[0], "ps")) {
        cmd_ps();
        return 0;
    }
    if (streq(argv[0], "uname")) {
        struct utsname u;
        if (sys_call3(SYS_uname, (long)(uintptr_t)&u, 0, 0) == 0) {
            out(u.sysname);
            out(" ");
            out(u.release);
            out(" ");
            out(u.machine);
            out("\n");
        }
        return 0;
    }
    if (streq(argv[0], "echo")) {
        int i;
        for (i = 1; i < argc; i++) {
            if (i > 1)
                out(" ");
            out(argv[i]);
        }
        out("\n");
        return 0;
    }
    {
        int rc = run_external(argv);
        if (rc != 0) {
            out("[exit ");
            out_long(rc);
            out("]\n");
        }
        return 0;
    }
}

int main(void)
{
    static char line[256];

    out("yousj-sh: shell ready (pid ");
    out_long(sys_call3(SYS_getpid, 0, 0, 0));
    out(")\n");

    for (;;) {
        size_t n = 0;

        out("sh> ");
        for (;;) {
            char ch;
            long r = sys_call3(SYS_read, 0, (long)(uintptr_t)&ch, 1);

            if (r <= 0)
                continue;
            if (ch == '\r')
                ch = '\n';
            if (ch == '\n') {
                out("\n");
                break;
            }
            if (ch == 0x7F || ch == 0x08) {
                if (n > 0) {
                    n--;
                    out("\b \b");
                }
                continue;
            }
            if (n < sizeof(line) - 1) {
                line[n++] = ch;
                out_n(&ch, 1);
            }
        }
        line[n] = '\0';
        if (run_line(line) < 0)
            break;
    }
    sys_call3(SYS_exit_group, 0, 0, 0);
    return 0;
}
