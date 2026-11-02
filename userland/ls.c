/* Yousj OS - ls (M12): list a directory (default "."). */
#include <stdint.h>
#include <stddef.h>

#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_getdents64 217
#define SYS_exit_group 231

#define O_RDONLY    0
#define O_DIRECTORY 0x10000
#define DT_DIR 4

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out_s(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    if (n > 0)
        sys_call3(SYS_write, 1, (long)(uintptr_t)s, n);
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
    const char *path = argc > 1 ? argv[1] : ".";
    static char buf[1024];
    long fd = sys_call3(SYS_open, (long)(uintptr_t)path,
                        O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        out_err("ls: cannot open ");
        out_err(path);
        out_err("\n");
        return 1;
    }
    for (;;) {
        long n = sys_call3(SYS_getdents64, fd, (long)(uintptr_t)buf,
                           (long)sizeof(buf));
        long off = 0;
        if (n < 0) {
            out_err("ls: read error\n");
            return 1;
        }
        if (n == 0)
            break;
        while (off < n) {
            unsigned short reclen =
                *(const unsigned short *)(buf + off + 16);
            unsigned char type =
                *(const unsigned char *)(buf + off + 18);
            out_s(buf + off + 19);
            if (type == DT_DIR)
                out_s("/");
            else if (type == 10) /* DT_LNK */
                out_s("@");
            out_s("\n");
            if (reclen == 0)
                return 1;
            off += reclen;
        }
    }
    sys_call3(SYS_close, fd, 0, 0);
    return 0;
}
