/*
 * net.c -- ethernet + ARP, M72. See net.h for the scope.
 *
 * Everything incoming lands in net_input() from the driver's
 * receive poll (timer-tick context): ARP requests for our
 * address get an immediate reply, ARP replies fill the small
 * neighbour cache, everything else is counted and dropped
 * until the IP layer exists.
 */
#include <yousj/types.h>
#include <yousj/net.h>
#include <yousj/virtio_net.h>
#include <drivers/serial.h>

/* The QEMU user-net (slirp) defaults; a configuration
 * surface comes later. */
#define OUR_IP 0x0A00020Fu   /* 10.0.2.15 */
#define OUR_GATEWAY 0x0A000202u /* 10.0.2.2 */

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP 0x0800

static bool g_up;
static uint64_t g_arp_req_seen, g_arp_replies_sent;
static uint64_t g_dropped;
static uint8_t g_frame_out[1600]; /* one frame being built */

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void mem_set(uint8_t *dst, uint8_t v, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Build and send an ARP packet (ethernet header included).
 * oper 1 = request (broadcast, target MAC zeroed),
 * oper 2 = reply to the given peer. */
static void arp_send(uint16_t oper, const uint8_t *peer_mac,
                     uint32_t peer_ip)
{
    uint8_t *f = g_frame_out;
    const uint8_t *mac = virtio_net_mac();
    bool bcast = oper == 1;

    mem_set(f, bcast ? 0xFF : 0, 6);
    if (!bcast)
        mem_copy(f, peer_mac, 6);
    mem_copy(f + 6, mac, 6);
    wr16(f + 12, ETHERTYPE_ARP);
    wr16(f + 14, 1);              /* htype: ethernet */
    wr16(f + 16, ETHERTYPE_IP);   /* ptype */
    f[18] = 6;                    /* hlen */
    f[19] = 4;                    /* plen */
    wr16(f + 20, oper);
    mem_copy(f + 22, mac, 6);     /* sender hw */
    wr32(f + 28, OUR_IP);         /* sender proto */
    if (bcast)
        mem_set(f + 32, 0, 6);
    else
        mem_copy(f + 32, peer_mac, 6);
    wr32(f + 38, peer_ip);
    virtio_net_send(f, 42);
}

static void net_input(const uint8_t *frame, uint32_t len)
{
    uint16_t type;

    if (len < 14) {
        g_dropped++;
        return;
    }
    type = rd16(frame + 12);
    if (type == ETHERTYPE_ARP) {
        const uint8_t *a = frame + 14;
        uint16_t oper;
        uint32_t tpa;

        if (len < 42 || rd16(a) != 1 || rd16(a + 2) != ETHERTYPE_IP ||
            a[4] != 6 || a[5] != 4) {
            g_dropped++;
            return;
        }
        oper = rd16(a + 6);
        tpa = rd32(a + 24);
        if (oper == 1) {
            g_arp_req_seen++;
            if (tpa == OUR_IP) {
                /* Answer: sender becomes the target. */
                arp_send(2, a + 8, rd32(a + 14));
                g_arp_replies_sent++;
            }
            return;
        }
        /* Replies need no action until the IP layer keeps a
         * neighbour cache; they are simply consumed. */
        return;
    }
    g_dropped++;
}

void net_init(void)
{
    if (!virtio_net_init()) {
        serial_puts("Yousj OS: net: no virtio-net device\n");
        return;
    }
    g_up = true;
    serial_puts("Yousj OS: net: link up mac=");
    {
        static const char hexd[] = "0123456789abcdef";
        const uint8_t *m = virtio_net_mac();
        int i;
        for (i = 0; i < 6; i++) {
            if (i)
                serial_putc(':');
            serial_putc(hexd[m[i] >> 4]);
            serial_putc(hexd[m[i] & 15]);
        }
    }
    serial_puts(" ip=10.0.2.15\n");
}

bool net_present(void)
{
    return g_up;
}

void net_poll(void)
{
    if (g_up)
        virtio_net_poll_rx(net_input);
}

const uint8_t *net_mac(void)
{
    return virtio_net_mac();
}

uint32_t net_ip(void)
{
    return OUR_IP;
}

void net_arp_request(uint32_t who)
{
    if (g_up)
        arp_send(1, NULL, who);
}

/* -- the snapshot: deliberately textual, like ps ------------ */

static uint64_t snap_puts(char *buf, uint64_t len, uint64_t at,
                          const char *s)
{
    while (*s != '\0' && at < len) {
        buf[at++] = *s++;
    }
    return at;
}

static uint64_t snap_put_u64(char *buf, uint64_t len, uint64_t at,
                             uint64_t v)
{
    char tmp[24];
    int n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && at < len)
        buf[at++] = tmp[--n];
    return at;
}

static uint64_t snap_put_mac(char *buf, uint64_t len, uint64_t at,
                             const uint8_t *m)
{
    static const char hexd[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 6; i++) {
        if (i > 0 && at < len)
            buf[at++] = ':';
        if (at < len)
            buf[at++] = hexd[m[i] >> 4];
        if (at < len)
            buf[at++] = hexd[m[i] & 15];
    }
    return at;
}

uint64_t net_snapshot(char *buf, uint64_t len)
{
    uint64_t at = 0;

    if (len == 0)
        return 0;
    at = snap_puts(buf, len, at, g_up ? "up mac=" : "down mac=");
    at = snap_put_mac(buf, len, at, virtio_net_mac());
    at = snap_puts(buf, len, at, " ip=10.0.2.15 rx=");
    at = snap_put_u64(buf, len, at, virtio_net_rx_count());
    at = snap_puts(buf, len, at, " tx=");
    at = snap_put_u64(buf, len, at, virtio_net_tx_count());
    at = snap_puts(buf, len, at, " arp_seen=");
    at = snap_put_u64(buf, len, at, g_arp_req_seen);
    at = snap_puts(buf, len, at, " arp_replied=");
    at = snap_put_u64(buf, len, at, g_arp_replies_sent);
    at = snap_puts(buf, len, at, " dropped=");
    at = snap_put_u64(buf, len, at, g_dropped);
    at = snap_puts(buf, len, at, " uidx=");
    at = snap_put_u64(buf, len, at, virtio_net_diag() & 0xFFFF);
    at = snap_puts(buf, len, at, " aidx=");
    at = snap_put_u64(buf, len, at, virtio_net_diag() >> 16);
    at = snap_puts(buf, len, at, "\n");
    if (at < len)
        buf[at] = '\0';
    return at;
}
