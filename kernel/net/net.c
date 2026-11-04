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
#define PROTO_UDP 17
static void udp_input(const uint8_t *frame, const uint8_t *ip,
                      uint32_t ip_tot); /* M74, below */
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
    else if (ip[9] == 17 /* UDP */)
        udp_input(frame, ip, tot);
    else
        g_dropped++; /* TCP: a later milestone */
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

/* ------------------------------------------------------------------ */
/* M74: UDP. An endpoint owns a packet queue (boundaries kept,
 * sender recorded), indexed in a small registry for delivery.
 * Sending resolves the neighbour (ARP waits as in ping),
 * loopback addresses are delivered internally without the
 * wire, and a packet for a port nobody bound earns an ICMP
 * port-unreachable, like a real stack. */

#define UDP_EP_MAX 8
#define UDP_QUEUE 8
#define UDP_MAX YOUSJ_UDP_MAX

struct udp_pkt {
    uint32_t len;
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t data[UDP_MAX];
};

struct yousj_udp_ep {
    bool in_use;
    uint32_t local_ip;   /* 0 = any local address */
    uint16_t local_port; /* 0 = unbound */
    uint32_t peer_ip;
    uint16_t peer_port;
    bool connected;
    struct udp_pkt rxq[UDP_QUEUE];
    int rx_head;
    int rx_count;
};

static struct yousj_udp_ep g_udp_eps[UDP_EP_MAX];
static uint16_t g_eph_port = 49152;
static uint64_t g_udp_dropped, g_udp_rx, g_udp_tx;
static uint8_t g_udp_seg[12 + 8 + UDP_MAX]; /* pseudo + segment */
static uint8_t g_udp_verify[12 + 1600];

#define N_EADDRINUSE 98
#define N_EADDRNOTAVAIL 99
#define N_EMSGSIZE 90
#define N_EHOSTUNREACH 113
#define N_ENFILE 23

long net_udp_create(struct yousj_udp_ep **out)
{
    int i;

    for (i = 0; i < UDP_EP_MAX; i++) {
        if (!g_udp_eps[i].in_use) {
            mem_set((uint8_t *)&g_udp_eps[i], 0,
                    sizeof(g_udp_eps[i]));
            g_udp_eps[i].in_use = true;
            *out = &g_udp_eps[i];
            return 0;
        }
    }
    return -N_ENFILE;
}

void net_udp_release(struct yousj_udp_ep *ep)
{
    ep->in_use = false;
    ep->local_port = 0;
    ep->rx_count = 0;
    ep->rx_head = 0;
    ep->connected = false;
}

static bool port_taken(uint16_t port, struct yousj_udp_ep *self)
{
    int i;

    for (i = 0; i < UDP_EP_MAX; i++)
        if (g_udp_eps[i].in_use && &g_udp_eps[i] != self &&
            g_udp_eps[i].local_port == port)
            return true;
    return false;
}

long net_udp_bind(struct yousj_udp_ep *ep, uint32_t ip,
                  uint16_t port)
{
    if (ip != 0 && ip != OUR_IP && ip != 0x7F000001u)
        return -N_EADDRNOTAVAIL;
    if (port != 0 && port_taken(port, ep))
        return -N_EADDRINUSE;
    ep->local_ip = ip;
    ep->local_port = port;
    return 0;
}

void net_udp_connect(struct yousj_udp_ep *ep, uint32_t ip,
                     uint16_t port)
{
    ep->peer_ip = ip;
    ep->peer_port = port;
    ep->connected = true;
}

void net_udp_local(struct yousj_udp_ep *ep, uint32_t *ip,
                   uint16_t *port)
{
    *ip = ep->local_ip != 0 ? ep->local_ip : OUR_IP;
    *port = ep->local_port;
}

bool net_udp_peer(struct yousj_udp_ep *ep, uint32_t *ip,
                  uint16_t *port)
{
    if (!ep->connected)
        return false;
    *ip = ep->peer_ip;
    *port = ep->peer_port;
    return true;
}

uint64_t net_udp_dropped(void)
{
    return g_udp_dropped;
}

/* Resolve a neighbour, ARP-begging on ticks as ping does. */
static bool resolve_wait(uint32_t onward)
{
    int tries;

    if (neigh_lookup(onward) != NULL)
        return true;
    for (tries = 0; tries < 20; tries++) {
        arp_send(g_frame_out2, 1, NULL, onward);
        sched_sleep_ticks(5);
        if (neigh_lookup(onward) != NULL)
            return true;
    }
    return false;
}

static void udp_deliver(struct yousj_udp_ep *ep, uint32_t src_ip,
                        uint16_t src_port, const uint8_t *data,
                        uint32_t len)
{
    struct udp_pkt *p;

    if (ep->connected && (ep->peer_ip != src_ip ||
                          ep->peer_port != src_port))
        return; /* the connected filter */
    if (ep->rx_count >= UDP_QUEUE) {
        g_udp_dropped++;
        return;
    }
    p = &ep->rxq[(ep->rx_head + ep->rx_count) % UDP_QUEUE];
    p->len = len;
    p->src_ip = src_ip;
    p->src_port = src_port;
    mem_copy(p->data, data, len);
    ep->rx_count++;
    g_udp_rx++;
}

static struct yousj_udp_ep *udp_find(uint32_t dst_ip,
                                     uint16_t dst_port)
{
    int i;

    for (i = 0; i < UDP_EP_MAX; i++) {
        struct yousj_udp_ep *ep = &g_udp_eps[i];
        if (!ep->in_use || ep->local_port != dst_port)
            continue;
        if (ep->local_ip == 0 || ep->local_ip == dst_ip ||
            dst_ip == 0x0A0002FFu || dst_ip == 0xFFFFFFFFu ||
            dst_ip == 0x7F000001u)
            return ep;
    }
    return NULL;
}

/* ICMP type 3 code 3 for a packet with no home. The reply
 * quotes the offending IP header + 8 bytes, as required. */
static void icmp_unreachable(const uint8_t *frame,
                             const uint8_t *ip, uint32_t ip_tot)
{
    uint8_t out[8 + 20 + 8];
    uint32_t src = rd32(ip + 12);
    uint32_t q = ip_tot < 28 ? ip_tot : 28;

    mem_set(out, 0, sizeof(out));
    out[0] = 3;
    out[1] = 3;
    mem_copy(out + 8, ip, q);
    wr16(out + 2, csum16(out, 8 + q));
    ip_send_to_mac(g_frame_out, frame + 6, src, PROTO_ICMP, out,
                   8 + q);
}

static void udp_input(const uint8_t *frame, const uint8_t *ip,
                      uint32_t ip_tot)
{
    const uint8_t *u = ip + 20;
    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
    uint16_t sport, dport, ulen;
    struct yousj_udp_ep *ep;

    if (ip_tot < 28) {
        g_dropped++;
        return;
    }
    ulen = rd16(u + 4);
    if (ulen < 8 || 20u + ulen > ip_tot) {
        g_dropped++;
        return;
    }
    if (rd16(u + 6) != 0) {
        /* Verify against the pseudo-header. */
        uint8_t *v = g_udp_verify;
        wr32(v, src);
        wr32(v + 4, dst);
        v[8] = 0;
        v[9] = PROTO_UDP;
        wr16(v + 10, ulen);
        mem_copy(v + 12, u, ulen);
        if (csum16(v, 12 + ulen) != 0) {
            g_dropped++;
            return;
        }
    }
    sport = rd16(u);
    dport = rd16(u + 2);
    ep = udp_find(dst, dport);
    if (ep == NULL) {
        if (dst == OUR_IP) /* no ICMP for broadcasts */
            icmp_unreachable(frame, ip, ip_tot);
        g_udp_dropped++;
        return;
    }
    udp_deliver(ep, src, sport, u + 8, (uint32_t)(ulen - 8));
}

long net_udp_send(struct yousj_udp_ep *ep, uint32_t dst_ip,
                  uint16_t dst_port, const uint8_t *data,
                  uint32_t len)
{
    uint32_t src_ip;
    uint16_t ulen;
    uint8_t *seg = g_udp_seg + 12; /* UDP header starts here */

    if (len > UDP_MAX)
        return -N_EMSGSIZE;
    if (ep->local_port == 0) {
        /* First send binds an ephemeral port, as usual. */
        uint16_t p = g_eph_port;
        while (port_taken(p, ep))
            p = (uint16_t)(p + 1);
        g_eph_port = (uint16_t)(p + 1);
        ep->local_port = p;
    }
    src_ip = ep->local_ip != 0 && ep->local_ip != 0x7F000001u
                 ? ep->local_ip
                 : OUR_IP;
    ulen = (uint16_t)(8 + len);
    wr16(seg, ep->local_port);
    wr16(seg + 2, dst_port);
    wr16(seg + 4, ulen);
    wr16(seg + 6, 0);
    mem_copy(seg + 8, data, len);
    /* Pseudo-header sits in front of the segment. */
    wr32(g_udp_seg, src_ip);
    wr32(g_udp_seg + 4, dst_ip);
    g_udp_seg[8] = 0;
    g_udp_seg[9] = PROTO_UDP;
    wr16(g_udp_seg + 10, ulen);
    wr16(seg + 6, csum16(g_udp_seg, 12 + ulen));

    if (dst_ip == OUR_IP || dst_ip == 0x7F000001u) {
        struct yousj_udp_ep *dst_ep = udp_find(dst_ip, dst_port);
        if (dst_ep != NULL)
            udp_deliver(dst_ep, src_ip, ep->local_port, data,
                        len);
        else
            g_udp_dropped++;
        g_udp_tx++;
        return (long)len;
    }
    {
        uint32_t onward = (dst_ip & 0xFFFFFF00u) ==
                                  (OUR_IP & 0xFFFFFF00u)
                              ? dst_ip
                              : OUR_GATEWAY;
        if (!resolve_wait(onward))
            return -N_EHOSTUNREACH;
    }
    if (!ip_send(g_frame_out2, dst_ip, PROTO_UDP, seg, ulen))
        return -N_EHOSTUNREACH;
    g_udp_tx++;
    return (long)len;
}

long net_udp_recv_try(struct yousj_udp_ep *ep, uint8_t *dst,
                      uint64_t cap, bool peek,
                      uint32_t *real_len_out,
                      uint32_t *src_ip_out,
                      uint16_t *src_port_out)
{
    struct udp_pkt *p;
    uint64_t n, i;

    if (ep->rx_count == 0)
        return -11; /* EAGAIN */
    p = &ep->rxq[ep->rx_head];
    *real_len_out = p->len;
    *src_ip_out = p->src_ip;
    *src_port_out = p->src_port;
    n = cap < p->len ? cap : p->len;
    for (i = 0; i < n; i++)
        dst[i] = p->data[i];
    if (!peek) {
        ep->rx_head = (ep->rx_head + 1) % UDP_QUEUE;
        ep->rx_count--;
    }
    return (long)n;
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
    at = snap_puts(buf, len, at, " udp_rx=");
    at = snap_put_u64(buf, len, at, g_udp_rx);
    at = snap_puts(buf, len, at, " udp_tx=");
    at = snap_put_u64(buf, len, at, g_udp_tx);
    at = snap_puts(buf, len, at, " udp_dropped=");
    at = snap_put_u64(buf, len, at, g_udp_dropped);
    at = snap_puts(buf, len, at, "\n");
    if (at < len)
        buf[at] = '\0';
    return at;
}
