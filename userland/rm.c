/* Yousj OS - rm (M13). */
#include <stdint.h>
#include <stddef.h>

#define SYS_write 1
#define SYS_unlink 87

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out_err(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    sys_call3(SYS_write, 2, (long)(uintptr_t)s, n);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        out_err("rm: missing operand\n");
        return 1;
    }
    if (sys_call3(SYS_unlink, (long)(uintptr_t)argv[1], 0, 0) != 0) {
        out_err("rm: cannot remove ");
        out_err(argv[1]);
        out_err("\n");
        return 1;
    }
    return 0;
}
