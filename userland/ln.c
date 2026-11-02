/* ln - create a symbolic link: ln -s TARGET LINK.
 * Hard links are not supported. Freestanding, Linux ABI. */
#include <stdint.h>

#define SYS_write      1
#define SYS_symlink    88
#define SYS_exit_group 231

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static unsigned long str_len_(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)str_len_(s));
}

int main(int argc, char **argv)
{
    if (argc != 4 || argv[1][0] != '-' || argv[1][1] != 's' ||
        argv[1][2] != '\0') {
        out("usage: ln -s TARGET LINK\n");
        sys_call3(SYS_exit_group, 2, 0, 0);
    }
    if (sys_call3(SYS_symlink, (long)(uintptr_t)argv[2],
                  (long)(uintptr_t)argv[3], 0) != 0) {
        out("ln: cannot create link\n");
        sys_call3(SYS_exit_group, 1, 0, 0);
    }
    sys_call3(SYS_exit_group, 0, 0, 0);
}
