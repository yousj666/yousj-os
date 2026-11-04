/* FDPROBE: run under busybox nc -e. Reports what fd 0 is
 * (fstat mode), echoes bytes back, and prints every read
 * outcome to the console (fd 2), so the exact channel shape
 * and EOF behaviour nc's exec mode creates becomes visible
 * on the serial line. */
#define SYS_read 0
#define SYS_write 1
#define SYS_fstat 5

struct stat_k {
    unsigned long dev, ino;
    unsigned int mode, nlink, uid, gid, rdev;
    unsigned int pad0;
    long size, blksize, blocks;
    long atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    long unused[3];
};

static long sc3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out2(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sc3(SYS_write, 2, (long)s, n);
}

static void outn2(long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        out2("0");
        return;
    }
    if (v < 0) {
        out2("-");
        v = -v;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c[2];
        c[0] = tmp[--n];
        c[1] = 0;
        out2(c);
    }
}

int main(void)
{
    struct stat_k st;
    char buf[128];
    long r;

    r = sc3(SYS_fstat, 0, (long)&st, 0);
    out2("FDPROBE fstat=");
    outn2(r);
    out2(" mode=");
    outn2((long)st.mode);
    out2("\n");
    for (;;) {
        r = sc3(SYS_read, 0, (long)buf, sizeof(buf));
        out2("FDPROBE read=");
        outn2(r);
        out2("\n");
        if (r <= 0)
            break;
        sc3(SYS_write, 1, (long)buf, r);
    }
    out2("FDPROBE exiting\n");
    return 0;
}
