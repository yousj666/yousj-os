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
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef long int64_t;

#define SYS_write 1
#define SYS_exit_group 231
#define SYS_nanosleep 35
#define SYS_NET_INFO 0x1002
#define SYS_NET_ARP  0x1003
#define SYS_NET_PING 0x1004

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
    char buf[384];
    long n, i;

    for (i = 0; i < 384; i++)
        buf[i] = 0;
    n = sys_call3(SYS_NET_INFO, (long)buf, 383, 0);
    out(tag);
    if (n < 0) {
        out(" <netinfo failed>\n");
        return;
    }
    sys_call3(SYS_write, 1, (long)buf, n);
}

/* --- M74: UDP over the real socket calls ----------------- */
#define SYS_socket 41
#define SYS_bind 49
#define SYS_connect 42
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_getsockname 51
#define SYS_close 3

struct sockaddr_in_k {
    uint16_t family;
    uint16_t port_be;
    uint32_t addr_be;
    uint8_t zero[8];
};

static void fill_in(struct sockaddr_in_k *a, uint32_t ip,
                    uint16_t port)
{
    int i;
    a->family = 2;
    a->port_be = (uint16_t)((port >> 8) | (port << 8));
    a->addr_be = ((ip & 0xFFu) << 24) | ((ip & 0xFF00u) << 8) |
                 ((ip >> 8) & 0xFF00u) | ((ip >> 24) & 0xFFu);
    for (i = 0; i < 8; i++)
        a->zero[i] = 0;
}

static long sys_call6(long nr, long a1, long a2, long a3, long a4,
                      long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10),
                       "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static int buf_eq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int udp_main(void)
{
    struct sockaddr_in_k in, from;
    uint32_t flen;
    char buf[64];
    int ok = 1;
    long r, a, b, c;

    /* loopback: two endpoints, one machine */
    a = sys_call3(SYS_socket, 2, 2, 0);
    b = sys_call3(SYS_socket, 2, 2, 0);
    ok = ok && a >= 0 && b >= 0;
    fill_in(&in, 0x7F000001u, 7778);
    r = sys_call3(SYS_bind, b, (long)&in, 16);
    ok = ok && r == 0;
    {
        long b2 = sys_call3(SYS_socket, 2, 2, 0);
        r = sys_call3(SYS_bind, b2, (long)&in, 16);
        ok = ok && r == -98; /* the port is taken: EADDRINUSE */
        sys_call3(SYS_close, b2, 0, 0);
    }
    fill_in(&in, 0x7F000001u, 7778);
    r = sys_call6(SYS_sendto, a, (long)"loop", 4, 0, (long)&in,
                  16);
    ok = ok && r == 4;
    flen = sizeof(from);
    r = sys_call6(SYS_recvfrom, b, (long)buf, 64, 0, (long)&from,
                  (long)&flen);
    ok = ok && r == 4 && buf_eq(buf, "loop", 4) && flen == 16 &&
         from.family == 2;
    /* the sender got an ephemeral port; it must be >= 49152 */
    {
        uint16_t p = (uint16_t)((from.port_be >> 8) |
                                (from.port_be << 8));
        ok = ok && p >= 49152;
    }
    /* sending to a port nobody bound still succeeds */
    fill_in(&in, 0x7F000001u, 7999);
    r = sys_call6(SYS_sendto, a, (long)"void", 4, 0, (long)&in,
                  16);
    ok = ok && r == 4;
    sys_call3(SYS_close, a, 0, 0);
    sys_call3(SYS_close, b, 0, 0);
    out(ok ? "NETTEST udp-loop PASS\n" : "NETTEST udp-loop FAIL\n");
    if (!ok)
        return 1;

    /* wire side: bind 7777, wait for the harness's packet,
     * answer it, and let the wire checks speak */
    c = sys_call3(SYS_socket, 2, 2, 0);
    fill_in(&in, 0, 7777);
    r = sys_call3(SYS_bind, c, (long)&in, 16);
    if (r != 0) {
        out("NETTEST udp-bind FAIL\n");
        return 1;
    }
    out("NETTEST udp-ready\n");
    flen = sizeof(from);
    r = sys_call6(SYS_recvfrom, c, (long)buf, 64, 0, (long)&from,
                  (long)&flen);
    if (r != 10 || !buf_eq(buf, "wire-hello", 10)) {
        out("NETTEST udp-recv FAIL\n");
        return 1;
    }
    out("NETTEST udp-got\n");
    r = sys_call6(SYS_sendto, c, (long)"yousj-udp-pong", 14, 0,
                  (long)&from, 16);
    if (r != 14) {
        out("NETTEST udp-send FAIL\n");
        return 1;
    }
    print_info("NETTEST after-udp ");
    out("NETTEST DONE\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && str_eq(argv[1], "udp"))
        return udp_main();
    if (argc > 1 && str_eq(argv[1], "ping")) {
        /* ping the gateway 10.0.2.2, give the reply a second
         * to come back, then show the counters */
        uint32_t be = (uint32_t)10 | (0u << 8) | (2u << 16) |
                      (2u << 24);
        struct {
            int64_t sec;
            int64_t nsec;
        } ts;
        long rc = sys_call3(SYS_NET_PING, (long)be, 0, 0);
        out(rc == 0 ? "NETTEST ping-sent\n"
                    : "NETTEST ping-failed\n");
        ts.sec = 1;
        ts.nsec = 0;
        sys_call3(SYS_nanosleep, (long)&ts, 0, 0);
        print_info("NETTEST after-ping ");
        out("NETTEST DONE\n");
        return 0;
    }
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
