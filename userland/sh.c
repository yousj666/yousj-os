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
 *
 * M10: redirection and pipelines. Operators are standalone
 * tokens (type the spaces):  > >> < 2>  and a single | :
 *   hello > /h.txt      hello | cat      cat < /h.txt
 * echo supports > / >> redirection too; other built-ins do
 * not take redirections.
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
#define SYS_open       2
#define SYS_close      3
#define SYS_pipe       22
#define SYS_dup        32
#define SYS_dup2       33
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

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

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

static void err_out(const char *s2)
{
    sys_call3(SYS_write, 2, (long)(uintptr_t)s2, (long)slen(s2));
}

struct redir {
    const char *in_path;
    const char *out_path;
    const char *err_path;
    int append;
};

/* Pull the redirection operators out of argv (in place).
 * Returns the new argc, or -1 on a dangling operator. */
static int extract_redirs(char **argv, int argc, struct redir *r)
{
    int i, w = 0;

    r->in_path = NULL;
    r->out_path = NULL;
    r->err_path = NULL;
    r->append = 0;
    for (i = 0; i < argc; i++) {
        const char *t = argv[i];
        if (streq(t, ">") || streq(t, ">>") || streq(t, "<") ||
            streq(t, "2>")) {
            if (i + 1 >= argc)
                return -1;
            if (t[0] == '>')
                r->out_path = argv[++i];
            else if (t[0] == '<')
                r->in_path = argv[++i];
            else {
                r->err_path = argv[++i];
                continue;
            }
            if (t[1] == '>')
                r->append = 1;
            else if (t[0] == '>')
                r->append = 0;
            continue;
        }
        argv[w++] = argv[i];
    }
    argv[w] = NULL;
    return w;
}

static void child_fail(const char *what)
{
    err_out("sh: cannot open ");
    err_out(what);
    err_out("\n");
    sys_call3(SYS_exit_group, 127, 0, 0);
}

/* In the child, after any pipe wiring: point 0/1/2 at the
 * redirection targets. */
static void apply_redirs(const struct redir *r)
{
    long fd;

    if (r->in_path != NULL) {
        fd = sys_call3(SYS_open, (long)(uintptr_t)r->in_path,
                       O_RDONLY, 0);
        if (fd < 0)
            child_fail(r->in_path);
        if (fd != 0) {
            sys_call3(SYS_dup2, fd, 0, 0);
            sys_call3(SYS_close, fd, 0, 0);
        }
    }
    if (r->out_path != NULL) {
        fd = sys_call3(SYS_open, (long)(uintptr_t)r->out_path,
                       O_WRONLY | O_CREAT |
                           (r->append ? O_APPEND : O_TRUNC),
                       0644);
        if (fd < 0)
            child_fail(r->out_path);
        if (fd != 1) {
            sys_call3(SYS_dup2, fd, 1, 0);
            sys_call3(SYS_close, fd, 0, 0);
        }
    }
    if (r->err_path != NULL) {
        fd = sys_call3(SYS_open, (long)(uintptr_t)r->err_path,
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            child_fail(r->err_path);
        if (fd != 2) {
            sys_call3(SYS_dup2, fd, 2, 0);
            sys_call3(SYS_close, fd, 0, 0);
        }
    }
}

static void exec_or_die(char **argv)
{
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
    }
    sys_call3(SYS_execve, (long)(uintptr_t)p,
              (long)(uintptr_t)argv, (long)(uintptr_t)empty_env);
    err_out("sh: cannot exec ");
    err_out(p);
    err_out("\n");
    sys_call3(SYS_exit_group, 127, 0, 0);
}

/* Fork one command with its stdin/stdout optionally pre-wired
 * to pipe ends. Returns the child pid or a negative error. */
static long spawn(char **argv, const struct redir *r,
                  long stdin_from, long stdout_to, long also_close)
{
    long pid = sys_call3(SYS_clone, SIGCHLD, 0, 0);

    if (pid != 0)
        return pid;
    if (stdin_from >= 0) {
        if (stdin_from != 0)
            sys_call3(SYS_dup2, stdin_from, 0, 0);
    }
    if (stdout_to >= 0) {
        if (stdout_to != 1)
            sys_call3(SYS_dup2, stdout_to, 1, 0);
    }
    if (stdin_from > 2)
        sys_call3(SYS_close, stdin_from, 0, 0);
    if (stdout_to > 2)
        sys_call3(SYS_close, stdout_to, 0, 0);
    if (also_close > 2)
        sys_call3(SYS_close, also_close, 0, 0);
    apply_redirs(r);
    exec_or_die(argv);
    return 0; /* unreachable */
}

static int wait_status(long pid)
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

/* Run one external command and wait for it. Returns the exit
 * code (128+signal if killed, 127 on spawn/exec failure). */
static int run_external(char **argv, const struct redir *r)
{
    long pid = spawn(argv, r, -1, -1, -1);

    if (pid < 0) {
        out("sh: clone failed\n");
        return 127;
    }
    return wait_status(pid);
}

/* left | right : the pipeline's status is the right side's. */
static int run_pipeline(char **largv, const struct redir *lr,
                        char **rargv, const struct redir *rr)
{
    int pfd[2];
    long p1, p2;
    int rc;

    if (sys_call3(SYS_pipe, (long)(uintptr_t)pfd, 0, 0) != 0) {
        out("sh: pipe failed\n");
        return 127;
    }
    p1 = spawn(largv, lr, -1, pfd[1], pfd[0]);
    if (p1 < 0) {
        sys_call3(SYS_close, pfd[0], 0, 0);
        sys_call3(SYS_close, pfd[1], 0, 0);
        out("sh: clone failed\n");
        return 127;
    }
    p2 = spawn(rargv, rr, pfd[0], -1, pfd[1]);
    sys_call3(SYS_close, pfd[0], 0, 0);
    sys_call3(SYS_close, pfd[1], 0, 0);
    if (p2 < 0) {
        wait_status(p1);
        out("sh: clone failed\n");
        return 127;
    }
    wait_status(p1);
    rc = wait_status(p2);
    return rc;
}

static void do_echo(char **argv, int argc)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1)
            out(" ");
        out(argv[i]);
    }
    out("\n");
}

/* echo with > / >> : swap stdout for the target file, print,
 * swap back. */
static int echo_redirected(char **argv, int argc,
                           const struct redir *r)
{
    long saved, fd;

    if (r->in_path != NULL || r->err_path != NULL ||
        r->out_path == NULL) {
        out("sh: echo takes only > or >> file\n");
        return 0;
    }
    saved = sys_call3(SYS_dup, 1, 0, 0);
    fd = sys_call3(SYS_open, (long)(uintptr_t)r->out_path,
                   O_WRONLY | O_CREAT |
                       (r->append ? O_APPEND : O_TRUNC),
                   0644);
    if (saved < 0 || fd < 0) {
        out("sh: echo redirect failed\n");
        if (fd >= 0)
            sys_call3(SYS_close, fd, 0, 0);
        if (saved >= 0)
            sys_call3(SYS_close, saved, 0, 0);
        return 0;
    }
    sys_call3(SYS_dup2, fd, 1, 0);
    sys_call3(SYS_close, fd, 0, 0);
    do_echo(argv, argc);
    sys_call3(SYS_dup2, saved, 1, 0);
    sys_call3(SYS_close, saved, 0, 0);
    return 0;
}

static int run_line(char *line)
{
    char *argv[16];
    int argc = 0;
    char *p = line;
    int i, pipe_at = -1;
    struct redir lr, rr;
    int larc, rarc;

    while (*p != '\0' && argc < 15) {
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

    for (i = 0; i < argc; i++) {
        if (streq(argv[i], "|")) {
            if (pipe_at >= 0) {
                out("sh: only one | per line\n");
                return 0;
            }
            pipe_at = i;
        }
    }
    if (pipe_at == 0 || pipe_at == argc - 1) {
        out("sh: bad pipeline\n");
        return 0;
    }

    if (pipe_at > 0) {
        int rc;
        argv[pipe_at] = NULL;
        larc = extract_redirs(argv, pipe_at, &lr);
        rarc = extract_redirs(argv + pipe_at + 1,
                              argc - pipe_at - 1, &rr);
        if (larc <= 0 || rarc <= 0) {
            out("sh: bad redirection\n");
            return 0;
        }
        rc = run_pipeline(argv, &lr, argv + pipe_at + 1, &rr);
        if (rc != 0) {
            out("[exit ");
            out_long(rc);
            out("]\n");
        }
        return 0;
    }

    argc = extract_redirs(argv, argc, &lr);
    if (argc < 0) {
        out("sh: bad redirection\n");
        return 0;
    }
    if (argc == 0)
        return 0;

    if (streq(argv[0], "exit"))
        return -1;
    if (streq(argv[0], "echo")) {
        if (lr.in_path != NULL || lr.out_path != NULL ||
            lr.err_path != NULL)
            return echo_redirected(argv, argc, &lr);
        do_echo(argv, argc);
        return 0;
    }
    if (streq(argv[0], "help") || streq(argv[0], "free") ||
        streq(argv[0], "ps") || streq(argv[0], "uname")) {
        if (lr.in_path != NULL || lr.out_path != NULL ||
            lr.err_path != NULL) {
            out("sh: redirection works on external commands "
                "(and echo)\n");
            return 0;
        }
    }
    if (streq(argv[0], "help")) {
        out("built-ins: help ps free uname echo exit\n");
        out("external: /bin programs, e.g. hello cat fstest; "
            "> >> < 2> and one | work\n");
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
    {
        int rc = run_external(argv, &lr);
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
