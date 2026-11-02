/* churn - M19 journal acceptance driver: loop directory
 * metadata operations on /data forever (create dirs and
 * files, delete them) so the test harness can hard-kill
 * QEMU mid-transaction and verify the filesystem survives.
 * Errors are ignored: after a crash, leftovers from an
 * interrupted cycle are expected and cleaned up by later
 * cycles. Freestanding, Linux ABI. */
#include <stdint.h>

#define SYS_write   1
#define SYS_close   3
#define SYS_open    2
#define SYS_mkdir   83
#define SYS_unlink  87
#define SYS_rmdir   84
#define SYS_exit_group 231

#define O_WRONLY_CREAT 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    sys_call3(SYS_write, 1, (long)(uintptr_t)s, (long)n);
}

static void out_num(long v)
{
    char buf[24];
    int i = 24;
    if (v == 0) {
        out("0");
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    sys_call3(SYS_write, 1, (long)(uintptr_t)(buf + i),
              (long)(24 - i));
}

int main(void)
{
    static const char payload[] =
        "churn-payload: journal me if you can, 64 bytes of fun......"
        "..";
    long cycle = 0;
    for (;;) {
        int i;
        char dir[32], file[40];
        for (i = 0; i < 6; i++) {
            int j = 0;
            const char *pre = "/data/cyc";
            while (*pre)
                dir[j++] = *pre++;
            dir[j++] = (char)('0' + i);
            dir[j] = '\0';
            sys_call3(SYS_mkdir, (long)(uintptr_t)dir, 0, 0);
            j = 0;
            while (dir[j]) {
                file[j] = dir[j];
                j++;
            }
            {
                const char *suf = "/f.txt";
                while (*suf)
                    file[j++] = *suf++;
            }
            file[j] = '\0';
            {
                long fd = sys_call3(SYS_open,
                                    (long)(uintptr_t)file,
                                    O_WRONLY_CREAT, 0);
                if (fd >= 0) {
                    sys_call3(SYS_write, fd,
                              (long)(uintptr_t)payload,
                              (long)sizeof(payload));
                    sys_call3(SYS_close, fd, 0, 0);
                }
            }
        }
        for (i = 0; i < 6; i++) {
            int j = 0;
            const char *pre = "/data/cyc";
            while (*pre)
                dir[j++] = *pre++;
            dir[j++] = (char)('0' + i);
            dir[j] = '\0';
            j = 0;
            while (dir[j]) {
                file[j] = dir[j];
                j++;
            }
            {
                const char *suf = "/f.txt";
                while (*suf)
                    file[j++] = *suf++;
            }
            file[j] = '\0';
            sys_call3(SYS_unlink, (long)(uintptr_t)file, 0, 0);
            sys_call3(SYS_rmdir, (long)(uintptr_t)dir, 0, 0);
        }
        cycle++;
        out("churn cycle ");
        out_num(cycle);
        out("\n");
    }
    return 0;
}
