/* Yousj OS - bigfile (M13): write or verify a pattern file.
 *   bigfile PATH SIZE          write SIZE bytes of pattern
 *   bigfile PATH SIZE check    verify them
 * Pattern: byte at offset i is (i * 31 + 7) & 0xFF. Exits 0
 * on success; used to prove files past yfs's old 48 KiB
 * direct-block limit (and across the power-cut test). */
#include <stdint.h>
#include <stddef.h>

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

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

static long parse_long(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static unsigned char pat(long i)
{
    return (unsigned char)((i * 31 + 7) & 0xFF);
}

int main(int argc, char **argv)
{
    static unsigned char buf[4096];
    long size, done = 0, fd;
    int check;

    if (argc < 3) {
        out_err("bigfile: PATH SIZE [check]\n");
        return 1;
    }
    size = parse_long(argv[2]);
    check = argc > 3 && argv[3][0] == 'c';
    if (check) {
        fd = sys_call3(SYS_open, (long)(uintptr_t)argv[1],
                       O_RDONLY, 0);
    } else {
        fd = sys_call3(SYS_open, (long)(uintptr_t)argv[1],
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        out_err("bigfile: cannot open\n");
        return 1;
    }
    while (done < size) {
        long chunk = size - done;
        long i, r;
        if (chunk > (long)sizeof(buf))
            chunk = (long)sizeof(buf);
        if (check) {
            r = sys_call3(SYS_read, fd, (long)(uintptr_t)buf, chunk);
            if (r != chunk) {
                out_err("bigfile: short read\n");
                return 1;
            }
            for (i = 0; i < chunk; i++) {
                if (buf[i] != pat(done + i)) {
                    out_err("bigfile: pattern mismatch\n");
                    return 1;
                }
            }
        } else {
            for (i = 0; i < chunk; i++)
                buf[i] = pat(done + i);
            r = sys_call3(SYS_write, fd, (long)(uintptr_t)buf,
                          chunk);
            if (r != chunk) {
                out_err("bigfile: short write\n");
                return 1;
            }
        }
        done += chunk;
    }
    sys_call3(SYS_close, fd, 0, 0);
    return 0;
}
