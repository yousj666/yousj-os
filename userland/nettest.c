/*
 * nettest.c -- M72: drive the net layer from userland.
 *
 *   nettest        print the netinfo snapshot
 *   nettest arp    ARP for 10.0.2.2, wait, print again
 *
 * The frame-level assertions live in the harness: it plays
 * the peer on a QEMU socket netdev and checks the wire.
 */
typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef long int64_t;

#define SYS_write 1
#define SYS_exit_group 231
#define SYS_nanosleep 35
#define SYS_NET_INFO 0x1002
#define SYS_NET_ARP  0x1003

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
    uint64_t n = 0;
    while (s[n])
        n++;
    sys_call3(SYS_write, 1, (long)s, (long)n);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void print_info(const char *tag)
{
    char buf[256];
    long n, i;

    for (i = 0; i < 256; i++)
        buf[i] = 0;
    n = sys_call3(SYS_NET_INFO, (long)buf, 255, 0);
    out(tag);
    if (n < 0) {
        out(" <netinfo failed>\n");
        return;
    }
    sys_call3(SYS_write, 1, (long)buf, n);
}

int main(int argc, char **argv)
{
    if (argc > 1 && str_eq(argv[1], "arp")) {
        /* 10.0.2.2 in network byte order */
        uint32_t be = (uint32_t)10 | (0u << 8) | (2u << 16) |
                      (2u << 24);
        struct {
            int64_t sec;
            int64_t nsec;
        } ts;
        sys_call3(SYS_NET_ARP, (long)be, 0, 0);
        ts.sec = 1;
        ts.nsec = 0;
        sys_call3(SYS_nanosleep, (long)&ts, 0, 0);
        print_info("NETTEST after-arp ");
        out("NETTEST DONE\n");
        return 0;
    }
    print_info("NETTEST info ");
    out("NETTEST DONE\n");
    return 0;
}
