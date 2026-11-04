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
static void tcp_input_seg(uint32_t src_ip, uint32_t dst_ip,
                          const uint8_t *seg, uint32_t seglen,
                          const uint8_t *src_mac); /* M75 */
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
    else if (ip[9] == 6 /* TCP: M75 */)
        tcp_input_seg(rd32(ip + 12), rd32(ip + 16), ip + 20,
                      tot - 20, frame + 6);
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

static void tcp_timer(void); /* M75, below */
void net_poll(void)
{
    if (g_up) {
        virtio_net_poll_rx(net_input);
        tcp_timer();
    }
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

/* ------------------------------------------------------------------ */
/* M75: TCP. A deliberately straightforward implementation:
 * the full connection state machine, in-order delivery only
 * (an out-of-order segment is dropped and answered with a
 * duplicate ACK, so a well-behaved peer's retransmit timer
 * heals the stream), one retransmit timer per connection
 * driven from the poll tick, and loopback delivered by
 * feeding the segment straight back into the input path.
 *
 * Send buffer bookkeeping uses absolute sequence numbers:
 * byte with sequence s lives at s % TCP_RING in sbuf. Bytes
 * in [snd_una, snd_nxt) are sent-unacknowledged, bytes in
 * [snd_nxt, snd_nxt + s_queued) wait for window. The receive
 * side is a plain byte queue fed strictly in order. */

#define PROTO_TCP 6
#define TCP_CONN_MAX 8
#define TCP_RING 4096
#define TCP_MSS 1360

enum {
    TS_CLOSED = 0,
    TS_LISTEN,
    TS_SYN_SENT,
    TS_SYN_RCVD,
    TS_ESTABLISHED,
    TS_FIN_WAIT_1,
    TS_FIN_WAIT_2,
    TS_CLOSE_WAIT,
    TS_LAST_ACK,
    TS_TIME_WAIT
};

#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

struct yousj_tcp {
    bool in_use;
    bool detached; /* file gone; stack finishes alone */
    uint8_t state;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t peer_ip;
    uint16_t peer_port;
    uint32_t iss, snd_una, snd_nxt, rcv_nxt;
    uint32_t peer_wnd;
    uint8_t sbuf[TCP_RING];
    uint32_t s_queued;  /* in ring, not yet sent */
    uint8_t rbuf[TCP_RING];
    uint32_t r_head, r_count;
    bool fin_queued;    /* our side asked to close-write */
    bool fin_sent;
    bool fin_seen;      /* peer's FIN consumed in order */
    bool rst_err;       /* connection reset by peer */
    bool refused;       /* connect() refused */
    struct yousj_tcp *pend[4]; /* listener: finished children */
    int pend_count;
    int backlog;
    struct yousj_tcp *listener; /* a SYN_RCVD child's parent */
    uint64_t last_tx_tick;
    uint64_t rto;       /* ticks */
    int rtx_count;
    uint64_t tw_deadline;
    uint64_t estab_seq_at; /* snapshot counter stamp */
};

static struct yousj_tcp g_tcps[TCP_CONN_MAX];
static uint32_t g_iss = 0x2A150000;
static uint64_t g_tcp_segs_in, g_tcp_segs_out, g_tcp_estab_total;
static uint8_t g_tcp_seg[20 + TCP_MSS];

static bool seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint16_t tcp_rcv_wnd(struct yousj_tcp *t)
{
    return (uint16_t)(TCP_RING - t->r_count);
}

static uint32_t csum_partial(uint32_t sum, const uint8_t *p,
                             uint32_t len)
{
    while (len >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Transmit one segment. `fbuf` is the caller's ethernet
 * scratch (interrupt context and process context each use
 * their own). Loopback destinations never touch the wire. */
static void tcp_input_seg(uint32_t src_ip, uint32_t dst_ip,
                          const uint8_t *seg, uint32_t seglen,
                          const uint8_t *src_mac);
static void tcp_xmit(uint8_t *fbuf, const uint8_t *dst_mac,
                     uint32_t src_ip, uint32_t dst_ip,
                     uint16_t sport, uint16_t dport, uint32_t seq,
                     uint32_t ack, uint8_t flags, uint16_t wnd,
                     const uint8_t *data, uint32_t dlen)
{
    uint8_t *seg = g_tcp_seg;
    uint32_t slen = 20 + dlen;

    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = 0x50; /* data offset 5 words */
    seg[13] = flags;
    wr16(seg + 14, wnd);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    if (dlen > 0)
        mem_copy(seg + 20, data, dlen);
    /* checksum over pseudo-header + segment */
    {
        uint8_t ph[12];
        uint32_t sum = 0;
        uint32_t i;
        wr32(ph, src_ip);
        wr32(ph + 4, dst_ip);
        ph[8] = 0;
        ph[9] = PROTO_TCP;
        wr16(ph + 10, (uint16_t)slen);
        for (i = 0; i < 12; i += 2)
            sum += ((uint32_t)ph[i] << 8) | ph[i + 1];
        for (i = 0; i + 1 < slen; i += 2)
            sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
        if (slen & 1)
            sum += (uint32_t)seg[slen - 1] << 8;
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        wr16(seg + 16, (uint16_t)~sum);
    }
    g_tcp_segs_out++;
    if (dst_ip == OUR_IP || dst_ip == 0x7F000001u) {
        tcp_input_seg(src_ip, dst_ip, seg, slen, NULL);
        return;
    }
    if (dst_mac != NULL)
        ip_send_to_mac(fbuf, dst_mac, dst_ip, PROTO_TCP, seg,
                       slen);
    else
        ip_send(fbuf, dst_ip, PROTO_TCP, seg, slen);
}

/* Send everything the peer's window allows (process
 * context). Also issues the queued FIN when the data is
 * gone. */
static void tcp_output(struct yousj_tcp *t)
{
    if (t->state != TS_ESTABLISHED && t->state != TS_CLOSE_WAIT)
        return;
    for (;;) {
        uint32_t inflight = t->snd_nxt - t->snd_una;
        uint32_t can, take, i;

        if (t->s_queued == 0)
            break;
        if (inflight >= t->peer_wnd)
            break;
        can = t->peer_wnd - inflight;
        take = t->s_queued < can ? t->s_queued : can;
        if (take > TCP_MSS)
            take = TCP_MSS;
        {
            uint8_t data[TCP_MSS];
            for (i = 0; i < take; i++)
                data[i] = t->sbuf[(t->snd_nxt + i) % TCP_RING];
            tcp_xmit(g_frame_out2, NULL, t->local_ip != 0 &&
                                       t->local_ip != 0x7F000001u
                                       ? t->local_ip
                                       : (t->peer_ip == 0x7F000001u
                                              ? 0x7F000001u
                                              : OUR_IP),
                     t->peer_ip, t->local_port, t->peer_port,
                     t->snd_nxt, t->rcv_nxt, TF_PSH | TF_ACK,
                     tcp_rcv_wnd(t), data, take);
        }
        t->snd_nxt += take;
        t->s_queued -= take;
        t->last_tx_tick = sched_uptime_ticks();
        if (t->rto == 0)
            t->rto = 100;
    }
    if (t->fin_queued && !t->fin_sent && t->s_queued == 0) {
        tcp_xmit(g_frame_out2, NULL,
                 t->local_ip != 0 && t->local_ip != 0x7F000001u
                     ? t->local_ip
                     : (t->peer_ip == 0x7F000001u ? 0x7F000001u
                                                 : OUR_IP),
                 t->peer_ip, t->local_port, t->peer_port, t->snd_nxt,
                 t->rcv_nxt, TF_FIN | TF_ACK, tcp_rcv_wnd(t), NULL,
                 0);
        t->snd_nxt++;
        t->fin_sent = true;
        t->last_tx_tick = sched_uptime_ticks();
        if (t->state == TS_ESTABLISHED)
            t->state = TS_FIN_WAIT_1;
        else if (t->state == TS_CLOSE_WAIT)
            t->state = TS_LAST_ACK;
    }
}

static struct yousj_tcp *tcp_alloc(void)
{
    int i;

    for (i = 0; i < TCP_CONN_MAX; i++)
        if (!g_tcps[i].in_use) {
            mem_set((uint8_t *)&g_tcps[i], 0, sizeof(g_tcps[i]));
            g_tcps[i].in_use = true;
            g_tcps[i].state = TS_CLOSED;
            g_tcps[i].peer_wnd = 65535;
            return &g_tcps[i];
        }
    return NULL;
}

static struct yousj_tcp *tcp_find(uint32_t sip, uint16_t sport,
                                  uint32_t dip, uint16_t dport,
                                  bool listeners)
{
    int i;

    for (i = 0; i < TCP_CONN_MAX; i++) {
        struct yousj_tcp *t = &g_tcps[i];
        if (!t->in_use || (t->state == TS_LISTEN) != listeners)
            continue;
        if (listeners) {
            if (t->local_port == dport &&
                (t->local_ip == 0 || t->local_ip == dip))
                return t;
        } else {
            if (t->local_port == dport && t->peer_port == sport &&
                t->peer_ip == sip &&
                (t->local_ip == 0 || t->local_ip == dip ||
                 dip == 0x7F000001u))
                return t;
        }
    }
    return NULL;
}

/* A reply segment from a connection (ACK/RST/SYN-ACK of the
 * moment): interrupt-context flavour, MAC known. */
static void tcp_reply(struct yousj_tcp *t, const uint8_t *mac,
                      uint8_t flags, uint32_t seq)
{
    tcp_xmit(g_frame_out, mac,
             t->local_ip != 0 && t->local_ip != 0x7F000001u
                 ? t->local_ip
                 : (t->peer_ip == 0x7F000001u ? 0x7F000001u
                                             : OUR_IP),
             t->peer_ip, t->local_port, t->peer_port, seq,
             t->rcv_nxt, flags, tcp_rcv_wnd(t), NULL, 0);
}

static void tcp_ack(struct yousj_tcp *t, const uint8_t *mac)
{
    tcp_reply(t, mac, TF_ACK, t->snd_nxt);
}

static void tcp_input_seg(uint32_t src_ip, uint32_t dst_ip,
                          const uint8_t *seg, uint32_t seglen,
                          const uint8_t *src_mac)
{
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t flags;
    uint32_t hlen, plen;
    const uint8_t *data;
    struct yousj_tcp *t;

    if (seglen < 20) {
        g_dropped++;
        return;
    }
    /* checksum: pseudo-header + segment */
    {
        uint8_t ph[12];
        uint32_t sum;
        wr32(ph, src_ip);
        wr32(ph + 4, dst_ip);
        ph[8] = 0;
        ph[9] = PROTO_TCP;
        wr16(ph + 10, (uint16_t)seglen);
        sum = csum_partial(0, ph, 12);
        sum = csum_partial(sum, seg, seglen);
        if (csum_fold(sum) != 0) {
            g_dropped++;
            return;
        }
    }
    sport = rd16(seg);
    dport = rd16(seg + 2);
    seq = rd32(seg + 4);
    ack = rd32(seg + 8);
    hlen = (uint32_t)(seg[12] >> 4) * 4;
    flags = seg[13];
    if (hlen < 20 || hlen > seglen) {
        g_dropped++;
        return;
    }
    data = seg + hlen;
    plen = seglen - hlen;
    g_tcp_segs_in++;

    t = tcp_find(src_ip, sport, dst_ip, dport, false);
    if (t == NULL) {
        struct yousj_tcp *lis =
            tcp_find(src_ip, sport, dst_ip, dport, true);
        if (lis != NULL && (flags & TF_SYN) && !(flags & TF_RST)) {
            /* A child is born: SYN_RCVD, parked until the
             * handshake completes. */
            struct yousj_tcp *c = tcp_alloc();
            if (c == NULL)
                return;
            c->local_ip = lis->local_ip;
            c->local_port = dport;
            c->peer_ip = src_ip;
            c->peer_port = sport;
            c->listener = lis;
            g_iss += 0x1111;
            c->iss = g_iss;
            c->snd_una = c->iss;
            c->snd_nxt = c->iss + 1;
            c->rcv_nxt = seq + 1;
            c->peer_wnd = rd16(seg + 14);
            c->state = TS_SYN_RCVD;
            /* SYN+ACK */
            tcp_xmit(g_frame_out, src_mac,
                     c->local_ip != 0 ? c->local_ip : dst_ip,
                     src_ip, dport, sport, c->iss, c->rcv_nxt,
                     TF_SYN | TF_ACK, tcp_rcv_wnd(c), NULL, 0);
            c->last_tx_tick = sched_uptime_ticks();
            c->rto = 100;
            return;
        }
        /* Nobody home: RST, formatted per the book. */
        if (!(flags & TF_RST)) {
            uint32_t rseq = 0, rack = 0;
            uint8_t rfl = TF_RST;
            if (flags & TF_ACK)
                rseq = ack;
            else {
                rack = seq + plen + ((flags & TF_SYN) ? 1 : 0) +
                       ((flags & TF_FIN) ? 1 : 0);
                rfl |= TF_ACK;
            }
            tcp_xmit(g_frame_out, src_mac, dst_ip, src_ip, dport,
                     sport, rseq, rack, rfl, 0, NULL, 0);
        }
        return;
    }

    if (flags & TF_RST) {
        if (t->state == TS_SYN_SENT)
            t->refused = true;
        else if (t->state != TS_LISTEN)
            t->rst_err = true;
        if (t->state != TS_LISTEN) {
            t->state = TS_CLOSED;
            if (t->detached)
                t->in_use = false;
        }
        return;
    }

    if (t->state == TS_SYN_SENT) {
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) &&
            ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->peer_wnd = rd16(seg + 14);
            t->state = TS_ESTABLISHED;
            g_tcp_estab_total++;
            tcp_ack(t, src_mac);
        }
        return;
    }

    if (t->state == TS_SYN_RCVD) {
        if ((flags & TF_SYN) && seq == t->rcv_nxt - 1) {
            /* Their SYN again: our SYN+ACK went missing. */
            tcp_xmit(g_frame_out, src_mac,
                     t->local_ip != 0 ? t->local_ip : dst_ip,
                     t->peer_ip, t->local_port, t->peer_port,
                     t->iss, t->rcv_nxt, TF_SYN | TF_ACK,
                     tcp_rcv_wnd(t), NULL, 0);
            return;
        }
        if ((flags & TF_ACK) && ack == t->snd_nxt &&
            seq == t->rcv_nxt) {
            t->snd_una = ack;
            t->state = TS_ESTABLISHED;
            g_tcp_estab_total++;
            if (t->listener != NULL &&
                t->listener->pend_count < 4 &&
                t->listener->pend_count < t->listener->backlog) {
                struct yousj_tcp *lis = t->listener;
                lis->pend[lis->pend_count++] = t;
                t->listener = NULL;
            }
            /* Fall through: this segment may already data. */
        } else
            return;
    }

    /* ACK bookkeeping for the connected states */
    if (flags & TF_ACK) {
        if (seq_before(t->snd_una, ack) &&
            !seq_before(t->snd_nxt, ack)) {
            bool fin_acked = t->fin_sent && ack == t->snd_nxt;
            t->snd_una = ack;
            t->rtx_count = 0;
            t->rto = 100;
            t->peer_wnd = rd16(seg + 14);
            if (fin_acked && t->state == TS_FIN_WAIT_1)
                t->state = TS_FIN_WAIT_2;
            else if (fin_acked && t->state == TS_LAST_ACK) {
                t->state = TS_CLOSED;
                if (t->detached)
                    t->in_use = false;
                return;
            }
        }
    }

    /* Payload, strictly in order */
    if (plen > 0 && (t->state == TS_ESTABLISHED ||
                     t->state == TS_FIN_WAIT_1 ||
                     t->state == TS_FIN_WAIT_2)) {
        if (seq == t->rcv_nxt) {
            uint32_t free = TCP_RING - t->r_count;
            uint32_t take = plen < free ? plen : free;
            uint32_t i;
            for (i = 0; i < take; i++) {
                t->rbuf[(t->r_head + t->r_count) % TCP_RING] =
                    data[i];
                t->r_count++;
            }
            t->rcv_nxt += take;
            tcp_ack(t, src_mac);
        } else
            tcp_ack(t, src_mac); /* duplicate ACK */
    }

    /* FIN, only once everything before it is consumed */
    if ((flags & TF_FIN) && seq + plen == t->rcv_nxt) {
        t->rcv_nxt++;
        t->fin_seen = true;
        if (t->state == TS_ESTABLISHED)
            t->state = TS_CLOSE_WAIT;
        else if (t->state == TS_FIN_WAIT_1 ||
                 t->state == TS_FIN_WAIT_2) {
            t->state = TS_TIME_WAIT;
            t->tw_deadline = sched_uptime_ticks() + 200;
        }
        tcp_ack(t, src_mac);
    }
}

/* Retransmission + TIME_WAIT reaping, from the poll tick. */
static void tcp_timer(void)
{
    uint64_t now = sched_uptime_ticks();
    int i;

    for (i = 0; i < TCP_CONN_MAX; i++) {
        struct yousj_tcp *t = &g_tcps[i];
        if (!t->in_use)
            continue;
        if (t->state == TS_TIME_WAIT) {
            if (now >= t->tw_deadline) {
                t->state = TS_CLOSED;
                if (t->detached)
                    t->in_use = false;
            }
            continue;
        }
        if ((t->state == TS_ESTABLISHED ||
             t->state == TS_FIN_WAIT_1 || t->state == TS_LAST_ACK ||
             t->state == TS_SYN_RCVD) &&
            t->snd_una != t->snd_nxt && t->rto > 0 &&
            now - t->last_tx_tick >= t->rto) {
            if (t->rtx_count++ > 6) {
                t->rst_err = true;
                t->state = TS_CLOSED;
                if (t->detached)
                    t->in_use = false;
                continue;
            }
            if (t->state == TS_SYN_RCVD) {
                tcp_xmit(g_frame_out, NULL,
                         t->local_ip != 0 ? t->local_ip : OUR_IP,
                         t->peer_ip, t->local_port, t->peer_port,
                         t->iss, t->rcv_nxt, TF_SYN | TF_ACK,
                         tcp_rcv_wnd(t), NULL, 0);
            } else {
                uint32_t avail = t->snd_nxt - t->snd_una;
                bool finbit = false;
                uint32_t dlen = avail;
                if (t->fin_sent)
                    dlen = avail - 1; /* last unit is FIN */
                if (dlen > TCP_MSS)
                    dlen = TCP_MSS;
                {
                    uint8_t data[TCP_MSS];
                    uint32_t k;
                    for (k = 0; k < dlen; k++)
                        data[k] =
                            t->sbuf[(t->snd_una + k) % TCP_RING];
                    finbit = t->fin_sent &&
                             t->snd_una + dlen == t->snd_nxt - 1;
                    tcp_xmit(g_frame_out, NULL,
                             t->local_ip != 0 &&
                                     t->local_ip != 0x7F000001u
                                 ? t->local_ip
                                 : (t->peer_ip == 0x7F000001u
                                        ? 0x7F000001u
                                        : OUR_IP),
                             t->peer_ip, t->local_port,
                             t->peer_port, t->snd_una, t->rcv_nxt,
                             (uint8_t)(TF_ACK |
                                       (dlen ? TF_PSH : 0) |
                                       (finbit ? TF_FIN : 0)),
                             tcp_rcv_wnd(t), data, dlen);
                }
            }
            t->rto = t->rto * 2 > 800 ? 800 : t->rto * 2;
            t->last_tx_tick = now;
        }
    }
}

/* -- API for the syscall layer (negative Linux errnos) ------- */

#define N_EADDRINUSE_T 98
#define N_EADDRNOTAVAIL_T 99
#define N_ECONNREFUSED 111
#define N_ECONNRESET 104
#define N_EAGAIN_T 11
#define N_ENFILE_T 23

long net_tcp_create(struct yousj_tcp **out)
{
    struct yousj_tcp *t = tcp_alloc();

    if (t == NULL)
        return -N_ENFILE_T;
    *out = t;
    return 0;
}

long net_tcp_bind(struct yousj_tcp *t, uint32_t ip, uint16_t port)
{
    int i;

    if (ip != 0 && ip != OUR_IP && ip != 0x7F000001u)
        return -N_EADDRNOTAVAIL_T;
    if (port != 0)
        for (i = 0; i < TCP_CONN_MAX; i++)
            if (g_tcps[i].in_use && &g_tcps[i] != t &&
                g_tcps[i].local_port == port)
                return -N_EADDRINUSE_T;
    t->local_ip = ip;
    t->local_port = port;
    return 0;
}

long net_tcp_listen(struct yousj_tcp *t, long backlog)
{
    if (t->local_port == 0)
        /* listen without bind: take from the shared ephemeral
         * counter (the same source UDP uses) */
        t->local_port = g_eph_port++;
    t->backlog = backlog < 1 ? 1 : (backlog > 4 ? 4 : (int)backlog);
    t->state = TS_LISTEN;
    return 0;
}

long net_tcp_connect_start(struct yousj_tcp *t, uint32_t ip,
                           uint16_t port)
{
    uint32_t local_ip;

    if (t->local_port == 0) {
        uint16_t p = g_eph_port;
        int i;
        for (;;) {
            bool taken = false;
            for (i = 0; i < TCP_CONN_MAX; i++)
                if (g_tcps[i].in_use && g_tcps[i].local_port == p)
                    taken = true;
            if (!taken)
                break;
            p++;
        }
        t->local_port = p;
        g_eph_port = (uint16_t)(p + 1);
    }
    local_ip = t->local_ip != 0 ? t->local_ip
                                : (ip == 0x7F000001u ? 0x7F000001u
                                                     : OUR_IP);
    t->local_ip = t->local_ip != 0 ? t->local_ip : local_ip;
    t->peer_ip = ip;
    t->peer_port = port;
    g_iss += 0x1111;
    t->iss = g_iss;
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->rcv_nxt = 0;
    t->state = TS_SYN_SENT;
    if (ip != OUR_IP && ip != 0x7F000001u) {
        uint32_t onward = (ip & 0xFFFFFF00u) ==
                                  (OUR_IP & 0xFFFFFF00u)
                              ? ip
                              : OUR_GATEWAY;
        if (!resolve_wait(onward)) {
            t->state = TS_CLOSED;
            return -N_EHOSTUNREACH;
        }
    }
    tcp_xmit(g_frame_out2, NULL, local_ip, ip, t->local_port,
             port, t->iss, 0, TF_SYN, tcp_rcv_wnd(t), NULL, 0);
    t->last_tx_tick = sched_uptime_ticks();
    t->rto = 100;
    return 0;
}

bool net_tcp_established(struct yousj_tcp *t)
{
    return t->state == TS_ESTABLISHED ||
           t->state == TS_FIN_WAIT_1 || t->state == TS_FIN_WAIT_2 ||
           t->state == TS_CLOSE_WAIT || t->state == TS_LAST_ACK ||
           t->state == TS_TIME_WAIT;
}

bool net_tcp_listening(struct yousj_tcp *t)
{
    return t->state == TS_LISTEN;
}

bool net_tcp_connecting(struct yousj_tcp *t)
{
    return t->state == TS_SYN_SENT;
}

long net_tcp_poll_err(struct yousj_tcp *t)
{
    if (t->refused)
        return -N_ECONNREFUSED;
    if (t->rst_err)
        return -N_ECONNRESET;
    if (t->state == TS_CLOSED && !t->in_use)
        return -N_ECONNRESET;
    return 0;
}

bool net_tcp_pending(struct yousj_tcp *t)
{
    return t->pend_count > 0;
}

long net_tcp_accept_try(struct yousj_tcp *t,
                        struct yousj_tcp **out)
{
    if (t->pend_count == 0)
        return -N_EAGAIN_T;
    *out = t->pend[0];
    t->pend_count--;
    {
        int i;
        for (i = 0; i < t->pend_count; i++)
            t->pend[i] = t->pend[i + 1];
    }
    return 0;
}

void net_tcp_local(struct yousj_tcp *t, uint32_t *ip,
                   uint16_t *port)
{
    *ip = t->local_ip != 0 ? t->local_ip : OUR_IP;
    *port = t->local_port;
}

bool net_tcp_peer(struct yousj_tcp *t, uint32_t *ip,
                  uint16_t *port)
{
    if (t->state == TS_LISTEN || t->state == TS_CLOSED ||
        t->peer_port == 0)
        return false;
    *ip = t->peer_ip;
    *port = t->peer_port;
    return true;
}

long net_tcp_send(struct yousj_tcp *t, const uint8_t *data,
                  uint32_t len)
{
    uint32_t used = (t->snd_nxt - t->snd_una) + t->s_queued;
    uint32_t free = TCP_RING - used;
    uint32_t take = len < free ? len : free;
    uint32_t i, wpos;

    if (t->rst_err)
        return -N_ECONNRESET;
    if (t->state != TS_ESTABLISHED && t->state != TS_CLOSE_WAIT)
        return -109; /* EPIPE-ish for not-yet/closed: syscall
                        layer distinguishes the common cases */
    if (take == 0)
        return -N_EAGAIN_T;
    wpos = t->snd_nxt + t->s_queued;
    for (i = 0; i < take; i++)
        t->sbuf[(wpos + i) % TCP_RING] = data[i];
    t->s_queued += take;
    tcp_output(t);
    return (long)take;
}

long net_tcp_recv_try(struct yousj_tcp *t, uint8_t *dst,
                      uint64_t cap, bool peek)
{
    uint64_t n, i;

    if (t->r_count > 0) {
        n = cap < t->r_count ? cap : t->r_count;
        if (peek) {
            for (i = 0; i < n; i++)
                dst[i] = t->rbuf[(t->r_head + i) % TCP_RING];
        } else {
            for (i = 0; i < n; i++) {
                dst[i] = t->rbuf[t->r_head];
                t->r_head = (t->r_head + 1) % TCP_RING;
                t->r_count--;
            }
        }
        return (long)n;
    }
    if (t->rst_err)
        return -N_ECONNRESET;
    if (t->fin_seen || t->state == TS_TIME_WAIT)
        return 0;
    if (t->state == TS_CLOSED)
        return 0;
    return -N_EAGAIN_T;
}

void net_tcp_shutdown_write(struct yousj_tcp *t)
{
    if ((t->state == TS_ESTABLISHED || t->state == TS_CLOSE_WAIT) &&
        !t->fin_queued) {
        t->fin_queued = true;
        tcp_output(t);
    }
}

void net_tcp_release(struct yousj_tcp *t)
{
    int i;

    if (t->state == TS_LISTEN) {
        /* Children of a dying listener go with it. */
        for (i = 0; i < TCP_CONN_MAX; i++)
            if (g_tcps[i].in_use && g_tcps[i].listener == t)
                g_tcps[i].in_use = false;
        for (i = 0; i < t->pend_count; i++)
            t->pend[i]->in_use = false;
        t->in_use = false;
        return;
    }
    if (t->state == TS_ESTABLISHED || t->state == TS_CLOSE_WAIT) {
        /* Graceful farewell: detach and let the stack finish
         * (the retransmit timer keeps the FIN honest). */
        t->detached = true;
        if (!t->fin_queued) {
            t->fin_queued = true;
            tcp_output(t);
        }
        return;
    }
    if (t->state == TS_SYN_SENT || t->state == TS_SYN_RCVD) {
        /* A half-open connection with no owner: RST it. */
        t->detached = true;
        tcp_xmit(g_frame_out2, NULL,
                 t->local_ip != 0 ? t->local_ip : OUR_IP,
                 t->peer_ip, t->local_port, t->peer_port,
                 t->snd_nxt, 0, TF_RST, 0, NULL, 0);
        t->in_use = false;
        return;
    }
    t->in_use = false;
}

uint64_t net_tcp_estab(void)
{
    return g_tcp_estab_total;
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
    at = snap_puts(buf, len, at, " tcp_in=");
    at = snap_put_u64(buf, len, at, g_tcp_segs_in);
    at = snap_puts(buf, len, at, " tcp_out=");
    at = snap_put_u64(buf, len, at, g_tcp_segs_out);
    at = snap_puts(buf, len, at, " tcp_estab=");
    at = snap_put_u64(buf, len, at, g_tcp_estab_total);
    at = snap_puts(buf, len, at, "\n");
    if (at < len)
        buf[at] = '\0';
    return at;
}
