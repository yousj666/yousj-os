/*
 * net.h -- the link/network layer surface (M72).
 *
 * M72 scope: an ethernet layer over the virtio-net driver, a
 * fixed IPv4 configuration (the QEMU user-net defaults:
 * 10.0.2.15/24, gateway 10.0.2.2) and ARP -- answering
 * requests for our address and resolving on demand. IP
 * itself arrives in M73.
 */
#ifndef YOUSJ_NET_H
#define YOUSJ_NET_H

#include <yousj/types.h>

/* Bring the stack up (driver probe included). Safe to call
 * on a machine with no NIC: it just reports not-present. */
void net_init(void);
bool net_present(void);

/* Timer-tick hook: drains the receive queue. */
void net_poll(void);

const uint8_t *net_mac(void);
uint32_t net_ip(void); /* host byte order */

/* Send an ARP request asking who has `who` (host order). */
void net_arp_request(uint32_t who);

/* M73: resolve `who` if needed (ARP with retries, process
 * context: it sleeps on ticks) and send one ICMP echo
 * request. Returns 0 when the request went out. */
long net_ping(uint32_t who);

/* M74: UDP endpoints for the AF_INET datagram socket face.
 * The endpoint object lives in the net layer; the file layer
 * holds only this opaque pointer. Addresses are host order.
 * Error returns are negative Linux errnos. */
struct yousj_udp_ep;
#define YOUSJ_UDP_MAX 1400 /* single-datagram payload cap */
long net_udp_create(struct yousj_udp_ep **out);
void net_udp_release(struct yousj_udp_ep *ep);
long net_udp_bind(struct yousj_udp_ep *ep, uint32_t ip,
                  uint16_t port);
void net_udp_connect(struct yousj_udp_ep *ep, uint32_t ip,
                     uint16_t port);
void net_udp_local(struct yousj_udp_ep *ep, uint32_t *ip,
                   uint16_t *port);
bool net_udp_peer(struct yousj_udp_ep *ep, uint32_t *ip,
                  uint16_t *port);
long net_udp_send(struct yousj_udp_ep *ep, uint32_t dst_ip,
                  uint16_t dst_port, const uint8_t *data,
                  uint32_t len);
long net_udp_recv_try(struct yousj_udp_ep *ep, uint8_t *dst,
                      uint64_t cap, bool peek,
                      uint32_t *real_len_out,
                      uint32_t *src_ip_out,
                      uint16_t *src_port_out);
uint64_t net_udp_dropped(void);

/* Text snapshot for the native netinfo syscall. Returns the
 * number of bytes written (excluding the NUL). */
uint64_t net_snapshot(char *buf, uint64_t len);

#endif /* YOUSJ_NET_H */
