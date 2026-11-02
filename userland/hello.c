/* Yousj OS - hello: the smallest execve target (milestone M5).
 *
 * A freestanding static ELF on raw syscalls: prints a greeting
 * with its pid and its argv, then exits with status 7 so a
 * parent can verify the whole clone/exec/wait chain by the exit
 * code it reaps. Also used from init's exec builtin to prove
 * execve end to end.
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

#define SYS_write      1
#define SYS_getpid     39
#define SYS_exit_group 231

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
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1
                            : (unsigned long)v;

    buf[i] = '\0';
    if (u == 0)
        buf[--i] = '0';
    while (u > 0) {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}

int main(int argc, char **argv)
{
    int i;

    out("hello from Yousj userland (pid ");
    out_long(sys_call3(SYS_getpid, 0, 0, 0));
    out(")\n");
    for (i = 0; i < argc; i++) {
        out("argv[");
        out_long(i);
        out("] = ");
        out(argv[i]);
        out("\n");
    }
    sys_call3(SYS_exit_group, 7, 0, 0);
    return 7;
}
