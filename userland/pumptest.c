/* PUMPTEST: the busybox nc -e shape, in miniature.
 * Two pipes and a child running /bin/cat with its stdin and
 * stdout rewired onto them. The parent writes a line into
 * the child's stdin, closes that end (the EOF signal a pump
 * sends when its network side dies), reads the echo back,
 * and waits for the child to exit. Every step is printed;
 * a hang anywhere in the close/EOF bookkeeping shows as a
 * missing later marker. */
#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_pipe 22
#define SYS_dup2 33
#define SYS_clone 56
#define SYS_execve 59
#define SYS_exit_group 231
#define SYS_wait4 61
#define SYS_nanosleep 35
#define SIGCHLD 17

static long sc3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sc4(long nr, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sc3(SYS_write, 1, (long)s, n);
}

static void outn(long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        out("0");
        return;
    }
    if (v < 0) {
        out("-");
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
        out(c);
    }
}

struct ts { long sec, nsec; };

#define SYS_socketpair 53
#define SYS_shutdown 48

/* The socketpair flavour: one full-duplex channel, the
 * child on sv[1] as both stdin and stdout, the parent on
 * sv[0]. The pump's "no more input" signal here is
 * shutdown(SHUT_WR) on the parent's end, not close. */
static int spair_main(void)
{
    int sv[2];
    long pid, r;
    char buf[64];
    int status = 0;

    r = sc4(SYS_socketpair, 1, 1, 0, (long)sv);
    if (r != 0) { out("SPAIR create FAIL\n"); return 1; }

    pid = sc3(SYS_clone, SIGCHLD, 0, 0);
    if (pid == 0) {
        static const char *const argv[] = { "/bin/cat", 0 };
        static const char *const envp[] = { 0 };
        sc3(SYS_dup2, sv[1], 0, 0);
        sc3(SYS_dup2, sv[1], 1, 0);
        sc3(SYS_close, sv[0], 0, 0);
        sc3(SYS_close, sv[1], 0, 0);
        sc3(SYS_execve, (long)"/bin/cat", (long)argv,
            (long)envp);
        sc3(SYS_exit_group, 127, 0, 0);
    }
    if (pid < 0) { out("SPAIR clone FAIL\n"); return 1; }
    out("SPAIR child=");
    outn(pid);
    out("\n");
    sc3(SYS_close, sv[1], 0, 0);

    r = sc3(SYS_write, sv[0], (long)"abc-spair\n", 10);
    out("SPAIR wrote=");
    outn(r);
    out("\n");
    r = sc3(SYS_shutdown, sv[0], 1, 0);
    out("SPAIR shutdown=");
    outn(r);
    out("\n");
    {
        struct ts t;
        t.sec = 0; t.nsec = 300000000L;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    r = sc3(SYS_read, sv[0], (long)buf, 40);
    out("SPAIR read1=");
    outn(r);
    out("\n");
    if (r > 0) {
        buf[r] = 0;
        out("SPAIR data: ");
        out(buf);
        if (buf[r - 1] != '\n')
            out("\n");
    }
    r = sc3(SYS_read, sv[0], (long)buf, 40);
    out("SPAIR read2=");
    outn(r);
    out("\n");
    r = sc4(SYS_wait4, pid, (long)&status, 0, 0);
    out("SPAIR wait=");
    outn(r);
    out(" status=");
    outn(status);
    out("\n");
    out("SPAIR DONE\n");
    return 0;
}

#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_bind 49
#define SYS_listen 50

struct in_k {
    unsigned short fam;
    unsigned char port[2];
    unsigned char addr[4];
    unsigned char zero[8];
};

/* The inetd flavour: the accepted connection fd itself
 * becomes the child's stdin and stdout (what busybox nc -e
 * does). The parent writes through a second connection,
 * shuts down, and waits. */
static int inetd_main(void)
{
    struct in_k in;
    long ls, cs, as, pid, r;
    char buf[64];
    int status = 0;

    in.fam = 2;
    in.port[0] = 0x27; in.port[1] = 0x0F; /* 9999 */
    in.addr[0] = 127; in.addr[1] = 0; in.addr[2] = 0; in.addr[3] = 1;
    for (r = 0; r < 8; r++) in.zero[r] = 0;

    ls = sc3(SYS_socket, 2, 1, 0);
    r = sc3(SYS_bind, ls, (long)&in, 16);
    if (r != 0) { out("INETD bind FAIL\n"); return 1; }
    r = sc3(SYS_listen, ls, 4, 0);
    if (r != 0) { out("INETD listen FAIL\n"); return 1; }
    cs = sc3(SYS_socket, 2, 1, 0);
    r = sc3(SYS_connect, cs, (long)&in, 16);
    if (r != 0) { out("INETD connect FAIL\n"); return 1; }
    as = sc4(SYS_accept, ls, 0, 0, 0);
    if (as < 0) { out("INETD accept FAIL\n"); return 1; }
    out("INETD linked\n");

    pid = sc3(SYS_clone, SIGCHLD, 0, 0);
    if (pid == 0) {
        static const char *const argv[] = { "/bin/cat", 0 };
        static const char *const envp[] = { 0 };
        sc3(SYS_dup2, as, 0, 0);
        sc3(SYS_dup2, as, 1, 0);
        sc3(SYS_close, as, 0, 0);
        sc3(SYS_close, ls, 0, 0);
        sc3(SYS_close, cs, 0, 0);
        sc3(SYS_execve, (long)"/bin/cat", (long)argv,
            (long)envp);
        sc3(SYS_exit_group, 127, 0, 0);
    }
    if (pid < 0) { out("INETD clone FAIL\n"); return 1; }
    sc3(SYS_close, as, 0, 0);
    out("INETD child=");
    outn(pid);
    out("\n");

    r = sc3(SYS_write, cs, (long)"abc-inetd\n", 10);
    out("INETD wrote=");
    outn(r);
    out("\n");
    {
        struct ts t;
        t.sec = 0; t.nsec = 300000000L;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    r = sc3(SYS_read, cs, (long)buf, 40);
    out("INETD read1=");
    outn(r);
    out("\n");
    if (r > 0) {
        buf[r] = 0;
        out("INETD data: ");
        out(buf);
        if (buf[r - 1] != '\n')
            out("\n");
    }
    /* the network side dies: shutdown the write side, then
     * close entirely (an external peer closing looks the
     * same to the reader) */
    r = sc3(SYS_shutdown, cs, 1, 0);
    out("INETD shutdown=");
    outn(r);
    out("\n");
    {
        struct ts t;
        t.sec = 0; t.nsec = 300000000L;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    /* the child should be gone now: a wait with WNOHANG(1)
     * tells us without blocking the probe itself */
    r = sc4(SYS_wait4, pid, (long)&status, 1, 0);
    out("INETD wait-nohang=");
    outn(r);
    out("\n");
    if (r == 0) {
        out("INETD child STILL RUNNING\n");
        /* don't leave the suite hanging: full close + block */
        sc3(SYS_close, cs, 0, 0);
        r = sc4(SYS_wait4, pid, (long)&status, 0, 0);
        out("INETD wait-after-close=");
        outn(r);
        out("\n");
    }
    out("INETD DONE\n");
    return 0;
}

#define SYS_sendfile 40

/* The sendfile flavour: what busybox cat does when its
 * stdin and stdout are one TCP connection. A loopback peer
 * sends a line and shuts down; this side runs sendfile
 * calls exactly as copyfd does (huge count, repeating) and
 * prints every return. */
static int sf_main(void)
{
    struct in_k in;
    long ls, cs, as, pid, r;
    char buf[64];
    int status = 0;

    in.fam = 2;
    in.port[0] = 0x27; in.port[1] = 0x0E; /* 9998 */
    in.addr[0] = 127; in.addr[1] = 0; in.addr[2] = 0; in.addr[3] = 1;
    for (r = 0; r < 8; r++) in.zero[r] = 0;

    ls = sc3(SYS_socket, 2, 1, 0);
    r = sc3(SYS_bind, ls, (long)&in, 16);
    if (r != 0) { out("SF bind FAIL\n"); return 1; }
    r = sc3(SYS_listen, ls, 4, 0);
    if (r != 0) { out("SF listen FAIL\n"); return 1; }
    cs = sc3(SYS_socket, 2, 1, 0);
    r = sc3(SYS_connect, cs, (long)&in, 16);
    if (r != 0) { out("SF connect FAIL\n"); return 1; }
    as = sc4(SYS_accept, ls, 0, 0, 0);
    if (as < 0) { out("SF accept FAIL\n"); return 1; }

    pid = sc3(SYS_clone, SIGCHLD, 0, 0);
    if (pid == 0) {
        /* child plays cat, faithfully: one sendfile attempt
         * (copyfd tries it first), then the read/write
         * fallback loop to EOF */
        long rr = sc4(SYS_sendfile, as, as, 0, 0x7fff0000L);
        out("SF child sendfile=");
        outn(rr);
        out("\n");
        for (;;) {
            rr = sc3(SYS_read, as, (long)buf, 40);
            out("SF child read=");
            outn(rr);
            out("\n");
            if (rr <= 0)
                break;
            sc3(SYS_write, as, (long)buf, rr);
        }
        out("SF child exiting\n");
        sc3(SYS_exit_group, 0, 0, 0);
    }
    if (pid < 0) { out("SF clone FAIL\n"); return 1; }
    sc3(SYS_close, as, 0, 0);

    r = sc3(SYS_write, cs, (long)"abc-sf\n", 7);
    out("SF wrote=");
    outn(r);
    out("\n");
    {
        struct ts t;
        t.sec = 0; t.nsec = 300000000L;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    r = sc3(SYS_read, cs, (long)buf, 40);
    out("SF read-back=");
    outn(r);
    out("\n");
    if (r > 0) {
        buf[r] = 0;
        out("SF data: ");
        out(buf);
        if (buf[r - 1] != '\n')
            out("\n");
    }
    r = sc3(SYS_shutdown, cs, 1, 0);
    out("SF shutdown=");
    outn(r);
    out("\n");
    r = sc4(SYS_wait4, pid, (long)&status, 1, 0);
    out("SF wait-nohang=");
    outn(r);
    out("\n");
    if (r == 0) {
        struct ts t;
        t.sec = 1; t.nsec = 0;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
        r = sc4(SYS_wait4, pid, (long)&status, 1, 0);
        out("SF wait-nohang2=");
        outn(r);
        out("\n");
    }
    out("SF DONE\n");
    return 0;
}

/* The exit flavour: hold a listener AND an established
 * connection when exiting, exactly like nc -l -e does after
 * exec (it never closes the listener). The parent tells us
 * whether the child's exit ever lands. */
static int exit_main(void)
{
    struct in_k in;
    long ls, cs, as, pid, r;
    int status = 0;

    in.fam = 2;
    in.port[0] = 0x27; in.port[1] = 0x0D; /* 9997 */
    in.addr[0] = 127; in.addr[1] = 0; in.addr[2] = 0; in.addr[3] = 1;
    for (r = 0; r < 8; r++) in.zero[r] = 0;

    pid = sc3(SYS_clone, SIGCHLD, 0, 0);
    if (pid == 0) {
        ls = sc3(SYS_socket, 2, 1, 0);
        sc3(SYS_bind, ls, (long)&in, 16);
        sc3(SYS_listen, ls, 4, 0);
        cs = sc3(SYS_socket, 2, 1, 0);
        sc3(SYS_connect, cs, (long)&in, 16);
        as = sc4(SYS_accept, ls, 0, 0, 0);
        (void)as;
        out("EXIT child armed (listener+conn open)\n");
        /* write, read, then just exit with everything open */
        sc3(SYS_write, cs, (long)"zz\n", 3);
        {
            char b[16];
            sc3(SYS_read, as, (long)b, 3);
        }
        out("EXIT child exiting now\n");
        sc3(SYS_exit_group, 0, 0, 0);
    }
    if (pid < 0) { out("EXIT clone FAIL\n"); return 1; }
    {
        struct ts t;
        t.sec = 1; t.nsec = 0;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    r = sc4(SYS_wait4, pid, (long)&status, 1, 0);
    out("EXIT wait-nohang=");
    outn(r);
    out("\n");
    if (r == 0) {
        struct ts t;
        t.sec = 2; t.nsec = 0;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
        r = sc4(SYS_wait4, pid, (long)&status, 1, 0);
        out("EXIT wait-nohang2=");
        outn(r);
        out("\n");
    }
    out("EXIT DONE\n");
    return 0;
}

int main(int argc, char **argv)
{
    int pin[2], pout[2];
    if (argc > 1 && argv[1][0] == 's' && argv[1][1] == 'p')
        return spair_main();
    if (argc > 1 && argv[1][0] == 's')
        return sf_main();
    if (argc > 1 && argv[1][0] == 'i')
        return inetd_main();
    if (argc > 1 && argv[1][0] == 'e')
        return exit_main();
    long pid, r;
    char buf[64];
    int status = 0;

    r = sc3(SYS_pipe, (long)pin, 0, 0);
    if (r != 0) { out("PUMP pipe-in FAIL\n"); return 1; }
    r = sc3(SYS_pipe, (long)pout, 0, 0);
    if (r != 0) { out("PUMP pipe-out FAIL\n"); return 1; }

    pid = sc3(SYS_clone, SIGCHLD, 0, 0);
    if (pid == 0) {
        static const char *const argv[] = { "/bin/cat", 0 };
        static const char *const envp[] = { 0 };
        /* child: stdin = pin[0], stdout = pout[1]; close all
         * four originals (dup2 first, exactly the pump way) */
        sc3(SYS_dup2, pin[0], 0, 0);
        sc3(SYS_dup2, pout[1], 1, 0);
        sc3(SYS_close, pin[0], 0, 0);
        sc3(SYS_close, pin[1], 0, 0);
        sc3(SYS_close, pout[0], 0, 0);
        sc3(SYS_close, pout[1], 0, 0);
        sc3(SYS_execve, (long)"/bin/cat", (long)argv,
            (long)envp);
        sc3(SYS_exit_group, 127, 0, 0);
    }
    if (pid < 0) { out("PUMP clone FAIL\n"); return 1; }
    out("PUMP child=");
    outn(pid);
    out("\n");
    /* parent keeps pin[1] (to child stdin) and pout[0]
     * (from child stdout); close the other two ends */
    sc3(SYS_close, pin[0], 0, 0);
    sc3(SYS_close, pout[1], 0, 0);

    r = sc3(SYS_write, pin[1], (long)"abc-pump\n", 9);
    out("PUMP wrote=");
    outn(r);
    out("\n");
    /* EOF signal: close the write end */
    sc3(SYS_close, pin[1], 0, 0);
    out("PUMP write-end closed\n");
    /* small grace so cat can print before we count time */
    {
        struct ts t;
        t.sec = 0; t.nsec = 300000000L;
        sc3(SYS_nanosleep, (long)&t, 0, 0);
    }
    r = sc3(SYS_read, pout[0], (long)buf, 40);
    out("PUMP read1=");
    outn(r);
    out("\n");
    if (r > 0) {
        buf[r] = 0;
        out("PUMP data: ");
        out(buf);
        if (buf[r - 1] != '\n')
            out("\n");
    }
    r = sc3(SYS_read, pout[0], (long)buf, 40);
    out("PUMP read2=");
    outn(r);
    out("\n");
    r = sc4(SYS_wait4, pid, (long)&status, 0, 0);
    out("PUMP wait=");
    outn(r);
    out(" status=");
    outn(status);
    out("\n");
    out("PUMP DONE\n");
    return 0;
}
