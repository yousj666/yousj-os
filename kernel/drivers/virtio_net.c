/*
 * virtio_net.c -- virtio-net over the legacy (transitional)
 * PCI interface, M72.
 *
 * Deliberate simplifications, all revisited later:
 *  - Legacy interface only (QEMU: -device virtio-net-pci,
 *    disable-modern=on). I/O-port registers, no MMIO mapping
 *    needed anywhere.
 *  - No feature bits accepted: no checksum offload, no GSO,
 *    no mergeable buffers. The MAC still comes from the
 *    device configuration area, as QEMU always provides it.
 *  - Polling from the timer tick; the device interrupt line
 *    stays masked.
 *  - Each queue's descriptor table is used through a small
 *    window: 16 receive buffers in flight, one transmit
 *    buffer, however large the queue itself is.
 */
#include <yousj/types.h>
#include <yousj/pci.h>
#include <yousj/pmm.h>
#include <yousj/vmm.h>
#include <yousj/virtio_net.h>

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_NET_DEVICE 0x1000 /* transitional net id */

/* Legacy I/O register offsets */
#define R_HOST_FEATURES 0x00
#define R_GUEST_FEATURES 0x04
#define R_QUEUE_PFN 0x08
#define R_QUEUE_NUM 0x0C
#define R_QUEUE_SEL 0x0E
#define R_QUEUE_NOTIFY 0x10
#define R_STATUS 0x12 /* after the 16-bit notify at 0x10 */
#define R_ISR 0x13
#define R_CONFIG 0x14 /* net: MAC at +0..5 */

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4

#define DESC_F_NEXT 1
#define DESC_F_WRITE 2

#define VIRTIO_HDR_LEN 10
#define FRAME_BUF 2048
#define RX_WINDOW 16

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[];
};

struct vqueue {
    uint16_t io_sel;
    uint16_t num; /* device queue size (ring modulus) */
    struct vq_desc *desc;
    struct vq_avail *avail;
    struct vq_used *used;
    uint16_t last_used;
};

static uint16_t g_io;
static bool g_present;
static uint8_t g_mac[6];
static struct vqueue g_rxq, g_txq;
static uint64_t g_rx_count, g_tx_count;

/* Receive buffers (FRAME_BUF each) and their physical twins */
static uint8_t *g_rx_bufs[RX_WINDOW];
static uint64_t g_rx_phys[RX_WINDOW];
/* One transmit staging buffer (virtio hdr + frame) */
static uint8_t *g_tx_buf;
static uint64_t g_tx_phys;
static uint16_t g_tx_next_desc; /* rotating tx head slot */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void mem_set(uint8_t *dst, uint8_t v, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = v;
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Bytes before the used ring (descriptors + avail ring). */
static uint64_t vq_prefix_bytes(uint16_t num)
{
    return sizeof(struct vq_desc) * num + 4 + 2ull * num;
}

static bool vq_setup(struct vqueue *q, uint16_t sel)
{
    uint64_t pages, phys, prefix;
    uint8_t *base;

    outw((uint16_t)(g_io + R_QUEUE_SEL), sel);
    q->num = inw((uint16_t)(g_io + R_QUEUE_NUM));
    if (q->num == 0)
        return false;
    prefix = vq_prefix_bytes(q->num);
    pages = (align_up(prefix, 4096) +
             align_up(4 + 8ull * q->num, 4096)) /
            4096;
    phys = pmm_alloc_pages(pages);
    if (phys == 0)
        return false;
    base = (uint8_t *)yousj_phys_to_virt(phys);
    mem_set(base, 0, pages * 4096);
    q->desc = (struct vq_desc *)base;
    q->avail = (struct vq_avail *)(base + sizeof(struct vq_desc) *
                                   q->num);
    q->used = (struct vq_used *)(base + align_up(prefix, 4096));
    q->avail->flags = 1; /* no interrupts, thank you */
    q->last_used = 0;
    q->io_sel = sel;
    outl((uint16_t)(g_io + R_QUEUE_PFN), (uint32_t)(phys >> 12));
    return true;
}

static void vq_kick(uint16_t sel)
{
    outw((uint16_t)(g_io + R_QUEUE_NOTIFY), sel);
}

/* Post receive buffer i (descriptor i) to the avail ring. */
static void rx_post(int i)
{
    struct vq_desc *d = &g_rxq.desc[i];
    uint16_t pos;

    d->addr = g_rx_phys[i];
    d->len = FRAME_BUF;
    d->flags = DESC_F_WRITE;
    d->next = 0;
    pos = (uint16_t)(g_rxq.avail->idx % g_rxq.num);
    g_rxq.avail->ring[pos] = (uint16_t)i;
    __asm__ volatile("mfence" ::: "memory");
    g_rxq.avail->idx = (uint16_t)(g_rxq.avail->idx + 1);
}

bool virtio_net_init(void)
{
    uint8_t slot, func;
    uint32_t bar;
    int i;

    if (!pci_find(VIRTIO_VENDOR, VIRTIO_NET_DEVICE, &slot, &func))
        return false;
    bar = pci_read32(0, slot, func, 0x10); /* BAR0 */
    if ((bar & 1) == 0)
        return false; /* not an I/O BAR: a modern-only device */
    g_io = (uint16_t)(bar & ~3u);

    /* Enable I/O space + bus mastering in the command reg. */
    {
        uint32_t cmd = pci_read32(0, slot, func, 0x04);
        pci_write32(0, slot, func, 0x04, cmd | 0x5);
    }

    outb((uint16_t)(g_io + R_STATUS), 0); /* reset */
    for (i = 0; i < 1000; i++) {
        if (inb((uint16_t)(g_io + R_STATUS)) == 0)
            break;
    }
    outb((uint16_t)(g_io + R_STATUS), ST_ACK);
    outb((uint16_t)(g_io + R_STATUS), ST_ACK | ST_DRIVER);

    /* Accept no optional features (see the file header). */
    (void)inl((uint16_t)(g_io + R_HOST_FEATURES));
    outl((uint16_t)(g_io + R_GUEST_FEATURES), 0);

    if (!vq_setup(&g_rxq, 0) || !vq_setup(&g_txq, 1))
        return false;

    /* Transmit staging: one page holds hdr + a full frame. */
    g_tx_phys = pmm_alloc_page();
    if (g_tx_phys == 0)
        return false;
    g_tx_buf = (uint8_t *)yousj_phys_to_virt(g_tx_phys);
    g_tx_next_desc = 0;

    for (i = 0; i < RX_WINDOW; i++) {
        g_rx_phys[i] = pmm_alloc_page();
        if (g_rx_phys[i] == 0)
            return false;
        g_rx_bufs[i] = (uint8_t *)yousj_phys_to_virt(g_rx_phys[i]);
        rx_post(i);
    }

    outb((uint16_t)(g_io + R_STATUS),
         ST_ACK | ST_DRIVER | ST_DRIVER_OK);

    for (i = 0; i < 6; i++)
        g_mac[i] = inb((uint16_t)(g_io + R_CONFIG + i));
    g_present = true;
    vq_kick(0);
    return true;
}

bool virtio_net_present(void)
{
    return g_present;
}

const uint8_t *virtio_net_mac(void)
{
    return g_mac;
}

uint64_t virtio_net_rx_count(void)
{
    return g_rx_count;
}

uint64_t virtio_net_tx_count(void)
{
    return g_tx_count;
}

uint32_t virtio_net_diag(void)
{
    /* rx_used_idx | rx_avail_idx<<16 */
    if (!g_present)
        return 0;
    return (uint32_t)g_rxq.used->idx |
           ((uint32_t)g_rxq.avail->idx << 16);
}

int virtio_net_send(const uint8_t *frame, uint32_t len)
{
    struct vq_desc *d0, *d1;
    uint16_t head, pos;
    uint32_t spins;
    uint16_t want_used;

    if (!g_present || len > FRAME_BUF - VIRTIO_HDR_LEN)
        return -1;
    mem_set(g_tx_buf, 0, VIRTIO_HDR_LEN);
    mem_copy(g_tx_buf + VIRTIO_HDR_LEN, frame, len);

    head = g_tx_next_desc;
    d0 = &g_txq.desc[head];
    d1 = &g_txq.desc[(uint16_t)((head + 1) % g_txq.num)];
    d0->addr = g_tx_phys;
    d0->len = VIRTIO_HDR_LEN;
    d0->flags = DESC_F_NEXT;
    d0->next = (uint16_t)((head + 1) % g_txq.num);
    d1->addr = g_tx_phys + VIRTIO_HDR_LEN;
    d1->len = len;
    d1->flags = 0;
    d1->next = 0;
    g_tx_next_desc = (uint16_t)((head + 2) % g_txq.num);

    pos = (uint16_t)(g_txq.avail->idx % g_txq.num);
    g_txq.avail->ring[pos] = head;
    __asm__ volatile("mfence" ::: "memory");
    want_used = (uint16_t)(g_txq.used->idx + 1);
    g_txq.avail->idx = (uint16_t)(g_txq.avail->idx + 1);
    vq_kick(1);
    /* The device completes promptly; poll the used ring with
     * a generous bound instead of sleeping (send is called
     * from the receive poll, itself in interrupt context). */
    for (spins = 0; spins < 2000000; spins++) {
        if (g_txq.used->idx == want_used) {
            g_tx_count++;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

void virtio_net_poll_rx(void (*input)(const uint8_t *frame,
                                      uint32_t len))
{
    if (!g_present)
        return;
    while (g_rxq.last_used != g_rxq.used->idx) {
        struct vq_used_elem *e =
            &g_rxq.used->ring[g_rxq.last_used % g_rxq.num];
        uint32_t id = e->id, total = e->len;
        g_rxq.last_used = (uint16_t)(g_rxq.last_used + 1);
        if (id < RX_WINDOW && total > VIRTIO_HDR_LEN &&
            total <= FRAME_BUF) {
            g_rx_count++;
            input(g_rx_bufs[id] + VIRTIO_HDR_LEN,
                  total - VIRTIO_HDR_LEN);
        }
        rx_post((int)id);
    }
    /* A refill only matters when something was consumed; the
     * kick is harmless either way. */
    vq_kick(0);
}
