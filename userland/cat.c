/* Yousj OS - cat (M10): copy a file (or stdin) to stdout.
 * Exists to exercise the fd layer and shell redirection:
 *   cat /path        cat < /path        hello | cat
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_exit_group 231

#define O_RDONLY 0

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out_err(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sys_call3(SYS_write, 2, (long)(uintptr_t)s, n);
}

int main(int argc, char **argv)
{
    long fd = 0;
    char buf[512];

    if (argc > 1) {
        fd = sys_call3(SYS_open, (long)(uintptr_t)argv[1], O_RDONLY, 0);
        if (fd < 0) {
            out_err("cat: cannot open ");
            out_err(argv[1]);
            out_err("\n");
            return 1;
        }
    }
    for (;;) {
        long n = sys_call3(SYS_read, fd, (long)(uintptr_t)buf,
                           (long)sizeof(buf));
        long off = 0;
        if (n < 0) {
            out_err("cat: read error\n");
            return 1;
        }
        if (n == 0)
            break;
        while (off < n) {
            long w = sys_call3(SYS_write, 1,
                               (long)(uintptr_t)(buf + off), n - off);
            if (w <= 0)
                return 1;
            off += w;
        }
    }
    if (fd > 2)
        sys_call3(SYS_close, fd, 0, 0);
    return 0;
}
