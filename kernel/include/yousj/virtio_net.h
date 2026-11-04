/*
 * virtio_net.h -- virtio-net (legacy PCI) driver surface
 * (M72). The driver speaks to a transitional virtio-net-pci
 * device through its I/O BAR, with two virtqueues (receive
 * and transmit), polling only -- no interrupt is wired yet.
 * The link layer above hands whole ethernet frames down and
 * receives whole frames up, virtio headers already stripped.
 */
#ifndef YOUSJ_VIRTIO_NET_H
#define YOUSJ_VIRTIO_NET_H

#include <yousj/types.h>

/* Probe, initialise and bring the link up. False when no
 * device is present (the common case for the other suites). */
bool virtio_net_init(void);
bool virtio_net_present(void);
const uint8_t *virtio_net_mac(void);

/* Send one ethernet frame (without any virtio header).
 * Returns 0, or -1 when the transmit queue never completes. */
int virtio_net_send(const uint8_t *frame, uint32_t len);

/* Drain completed receive descriptors, handing each frame to
 * input(). Called from the timer tick; cheap when idle. */
void virtio_net_poll_rx(void (*input)(const uint8_t *frame,
                                      uint32_t len));

uint64_t virtio_net_rx_count(void);
uint64_t virtio_net_tx_count(void);
/* M79: how many times the device's INTx line has fired. */
uint64_t virtio_net_irq_count(void);

/* M72 diagnostics: raw ring indices for the snapshot. */
uint32_t virtio_net_diag(void);

#endif /* YOUSJ_VIRTIO_NET_H */
