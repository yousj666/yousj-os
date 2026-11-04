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
#include <yousj/sched.h>
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
static uint8_t g_frame_out[1600];  /* built in irq context */
static uint8_t g_frame_out2[1600]; /* built in proc context */

/* M73: neighbour cache (ARP results) and IP/ICMP state */
#define NEIGH_MAX 4
static struct {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
} g_neigh[NEIGH_MAX];
static uint16_t g_ip_id;
static uint64_t g_ip_rx, g_ip_tx, g_icmp_req_seen;
static uint64_t g_ping_sent, g_ping_replied;
#define PING_ID 0x5973 /* "Ys" */
static uint16_t g_ping_seq;

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
 * oper 2 = reply to the given peer. The scratch frame buffer
 * is the caller's: interrupt context and process context
 * never share one. */
static void arp_send(uint8_t *f, uint16_t oper,
                     const uint8_t *peer_mac, uint32_t peer_ip)
{
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

static void neigh_learn(uint32_t ip, const uint8_t *mac)
{
    int i, slot = 0;

    for (i = 0; i < NEIGH_MAX; i++) {
        if (g_neigh[i].valid && g_neigh[i].ip == ip) {
            slot = i;
            break;
        }
        if (!g_neigh[i].valid) {
            slot = i;
            break;
        }
    }
    g_neigh[slot].ip = ip;
    mem_copy(g_neigh[slot].mac, mac, 6);
    g_neigh[slot].valid = true;
}

static const uint8_t *neigh_lookup(uint32_t ip)
{
    int i;

    for (i = 0; i < NEIGH_MAX; i++)
        if (g_neigh[i].valid && g_neigh[i].ip == ip)
            return g_neigh[i].mac;
    return NULL;
}

/* The internet checksum over a buffer (host-order result). */
static uint16_t csum16(const uint8_t *p, uint32_t len)
{
    uint32_t sum = 0;

    while (len >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Send one IPv4 packet to a known ethernet address. */
static void ip_send_to_mac(uint8_t *f, const uint8_t *dst_mac,
                           uint32_t dst_ip, uint8_t proto,
                           const uint8_t *payload, uint32_t plen)
{
    uint16_t tot = (uint16_t)(20 + plen);

    mem_copy(f, dst_mac, 6);
    mem_copy(f + 6, virtio_net_mac(), 6);
    wr16(f + 12, ETHERTYPE_IP);
    f[14] = 0x45; /* v4, IHL 5 */
    f[15] = 0;
    wr16(f + 16, tot);
    wr16(f + 18, g_ip_id++);
    wr16(f + 20, 0); /* flags/fragment offset */
    f[22] = 64;      /* TTL */
    f[23] = proto;
    wr16(f + 24, 0);
    wr32(f + 26, OUR_IP);
    wr32(f + 30, dst_ip);
    wr16(f + 24, csum16(f + 14, 20));
    mem_copy(f + 34, payload, plen);
    virtio_net_send(f, (uint32_t)(14 + tot));
    g_ip_tx++;
}

/* Send one IPv4 packet by address: same-subnet hosts are
 * addressed directly, everything else goes to the gateway.
 * Returns false when the neighbour is unresolved (the caller
 * arranges ARP first). */
static bool ip_send(uint8_t *f, uint32_t dst_ip, uint8_t proto,
                    const uint8_t *payload, uint32_t plen)
{
    uint32_t onward = (dst_ip & 0xFFFFFF00u) == (OUR_IP & 0xFFFFFF00u)
                          ? dst_ip
                          : OUR_GATEWAY;
    const uint8_t *mac = neigh_lookup(onward);

    if (mac == NULL)
        return false;
    ip_send_to_mac(f, mac, dst_ip, proto, payload, plen);
    return true;
}

#define PROTO_ICMP 1
static uint8_t g_icmp_out[1600];

static void icmp_input(const uint8_t *frame, const uint8_t *ip,
                       uint32_t ip_tot)
{
    const uint8_t *ic = ip + 20;
    uint32_t ilen = ip_tot - 20;

    if (ilen < 8 || csum16(ic, ilen) != 0) {
        g_dropped++;
        return;
    }
    if (ic[0] == 8) {
        /* Echo request: answer the sender directly. */
        uint32_t src = rd32(ip + 12);
        g_icmp_req_seen++;
        mem_copy(g_icmp_out, ic, ilen);
        g_icmp_out[0] = 0; /* echo reply */
        g_icmp_out[1] = 0;
        wr16(g_icmp_out + 2, 0);
        wr16(g_icmp_out + 2, csum16(g_icmp_out, ilen));
        ip_send_to_mac(g_frame_out, frame + 6, src, PROTO_ICMP,
                       g_icmp_out, ilen);
        return;
    }
    if (ic[0] == 0 && rd16(ic + 4) == PING_ID)
        g_ping_replied++;
    /* Other ICMP types are consumed silently. */
}

static void ip_input(const uint8_t *frame, uint32_t len)
{
    const uint8_t *ip = frame + 14;
    uint32_t dst;
    uint16_t tot;

    if (len < 34 || ip[0] != 0x45 || csum16(ip, 20) != 0) {
        g_dropped++;
        return;
    }
    tot = rd16(ip + 2);
    dst = rd32(ip + 16);
    if (tot < 20 || 14u + tot > len ||
        (dst != OUR_IP && dst != 0x0A0002FFu &&
         dst != 0xFFFFFFFFu)) {
        g_dropped++;
        return;
    }
    g_ip_rx++;
    if (ip[9] == PROTO_ICMP)
        icmp_input(frame, ip, tot);
    else
        g_dropped++; /* UDP/TCP: later milestones */
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
        /* Both requests and replies teach us the sender. */
        neigh_learn(rd32(a + 14), a + 8);
        if (oper == 1) {
            g_arp_req_seen++;
            if (tpa == OUR_IP) {
                /* Answer: sender becomes the target. */
                arp_send(g_frame_out, 2, a + 8, rd32(a + 14));
                g_arp_replies_sent++;
            }
            return;
        }
        /* Replies only feed the cache. */
        return;
    }
    if (type == ETHERTYPE_IP)
        ip_input(frame, len);
    else
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
        arp_send(g_frame_out2, 1, NULL, who);
}

long net_ping(uint32_t who)
{
    static const char payload[] = "yousj-ping";
    uint8_t ic[64];
    uint32_t ilen = 8 + sizeof(payload) - 1;
    int tries;

    if (!g_up)
        return -1;
    /* Resolve first: ARP, then wait on ticks for the reply
     * to land in the neighbour cache (the reply arrives in
     * interrupt context through net_poll). */
    if (neigh_lookup((who & 0xFFFFFF00u) == (OUR_IP & 0xFFFFFF00u)
                         ? who
                         : OUR_GATEWAY) == NULL) {
        uint32_t onward = (who & 0xFFFFFF00u) ==
                                  (OUR_IP & 0xFFFFFF00u)
                              ? who
                              : OUR_GATEWAY;
        for (tries = 0; tries < 20; tries++) {
            arp_send(g_frame_out2, 1, NULL, onward);
            sched_sleep_ticks(5);
            if (neigh_lookup(onward) != NULL)
                break;
        }
        if (neigh_lookup(onward) == NULL)
            return -2;
    }
    mem_set(ic, 0, ilen);
    ic[0] = 8; /* echo request */
    wr16(ic + 4, PING_ID);
    wr16(ic + 6, g_ping_seq++);
    mem_copy(ic + 8, (const uint8_t *)payload, sizeof(payload) - 1);
    wr16(ic + 2, csum16(ic, ilen));
    if (!ip_send(g_frame_out2, who, PROTO_ICMP, ic, ilen))
        return -3;
    g_ping_sent++;
    return 0;
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
    at = snap_puts(buf, len, at, " ip_rx=");
    at = snap_put_u64(buf, len, at, g_ip_rx);
    at = snap_puts(buf, len, at, " ip_tx=");
    at = snap_put_u64(buf, len, at, g_ip_tx);
    at = snap_puts(buf, len, at, " icmp_seen=");
    at = snap_put_u64(buf, len, at, g_icmp_req_seen);
    at = snap_puts(buf, len, at, " ping_sent=");
    at = snap_put_u64(buf, len, at, g_ping_sent);
    at = snap_puts(buf, len, at, " ping_replied=");
    at = snap_put_u64(buf, len, at, g_ping_replied);
    at = snap_puts(buf, len, at, "\n");
    if (at < len)
        buf[at] = '\0';
    return at;
}
