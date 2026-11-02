/* Yousj OS - legacy ATA PIO driver (M12). See ata.h.
 *
 * Status polling is bounded-spin (QEMU answers instantly);
 * every wait gives up after ~10M polls so a missing or wedged
 * drive degrades to "not present" instead of hanging boot.
 */
#include <yousj/ata.h>
#include <yousj/io.h>

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#define REG_DATA     0
#define REG_ERROR    1
#define REG_COUNT    2
#define REG_LBA_LO   3
#define REG_LBA_MID  4
#define REG_LBA_HI   5
#define REG_DRIVE    6
#define REG_STATUS   7
#define REG_COMMAND  7

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ   0x20
#define CMD_WRITE  0x30
#define CMD_FLUSH  0xEA
#define CMD_IDENT  0xEC

static uint16_t bus_base(int bus)
{
    return bus == 0 ? 0x1F0 : 0x170;
}

/* 0 when BSY clears in time, -1 on timeout. */
static int wait_bsy(uint16_t base)
{
    uint32_t i;
    for (i = 0; i < 10000000u; i++) {
        if (!(inb(base + REG_STATUS) & ST_BSY))
            return 0;
    }
    return -1;
}

/* 0 when DRQ sets (and no ERR), -1 otherwise. */
static int wait_drq(uint16_t base)
{
    uint32_t i;
    for (i = 0; i < 10000000u; i++) {
        uint8_t st = inb(base + REG_STATUS);
        if (st & ST_ERR)
            return -1;
        if (st & ST_DRQ)
            return 0;
        if (!(st & ST_BSY))
            return -1;
    }
    return -1;
}

int ata_probe(int bus, struct ata_drive *out)
{
    uint16_t base = bus_base(bus);
    uint16_t id[256];
    int i;

    out->present = false;
    out->bus = bus;
    out->sectors = 0;
    /* Floating bus: the status port reads all-ones. */
    if (inb(base + REG_STATUS) == 0xFF)
        return -1;
    outb(base + REG_DRIVE, 0xA0); /* master */
    io_wait();
    if (wait_bsy(base) != 0)
        return -1;
    outb(base + REG_COUNT, 0);
    outb(base + REG_LBA_LO, 0);
    outb(base + REG_LBA_MID, 0);
    outb(base + REG_LBA_HI, 0);
    outb(base + REG_COMMAND, CMD_IDENT);
    io_wait();
    /* ATAPI devices bail here (they set PACKET signature bytes;
     * QEMU's cdrom would need the packet protocol -- skipped). */
    if (inb(base + REG_LBA_MID) != 0 || inb(base + REG_LBA_HI) != 0)
        return -1;
    if (wait_drq(base) != 0)
        return -1;
    for (i = 0; i < 256; i++)
        id[i] = inw(base + REG_DATA);
    out->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (out->sectors == 0)
        return -1;
    out->present = true;
    return 0;
}

static int select_and_cmd(const struct ata_drive *d, uint32_t lba,
                          uint32_t count, uint8_t cmd)
{
    uint16_t base = bus_base(d->bus);

    if (wait_bsy(base) != 0)
        return -1;
    outb(base + REG_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    io_wait();
    if (wait_bsy(base) != 0)
        return -1;
    outb(base + REG_COUNT, (uint8_t)(count == 256 ? 0 : count));
    outb(base + REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(base + REG_COMMAND, cmd);
    io_wait();
    return 0;
}

int ata_read(const struct ata_drive *d, uint32_t lba,
             uint32_t count, void *buf)
{
    uint16_t base = bus_base(d->bus);
    uint16_t *p = (uint16_t *)buf;
    uint32_t s;
    int i;

    if (!d->present || count == 0 || count > 256)
        return -1;
    if (select_and_cmd(d, lba, count, CMD_READ) != 0)
        return -1;
    for (s = 0; s < count; s++) {
        if (wait_drq(base) != 0)
            return -1;
        for (i = 0; i < 256; i++)
            *p++ = inw(base + REG_DATA);
    }
    return 0;
}

int ata_write(const struct ata_drive *d, uint32_t lba,
              uint32_t count, const void *buf)
{
    uint16_t base = bus_base(d->bus);
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t s;
    int i;

    if (!d->present || count == 0 || count > 256)
        return -1;
    if (select_and_cmd(d, lba, count, CMD_WRITE) != 0)
        return -1;
    for (s = 0; s < count; s++) {
        if (wait_drq(base) != 0)
            return -1;
        for (i = 0; i < 256; i++)
            outw(base + REG_DATA, *p++);
        io_wait();
    }
    if (wait_bsy(base) != 0)
        return -1;
    if (inb(base + REG_STATUS) & ST_ERR)
        return -1;
    /* Push the drive's write cache out before we call it done:
     * persistence claims start here. */
    outb(base + REG_COMMAND, CMD_FLUSH);
    io_wait();
    if (wait_bsy(base) != 0)
        return -1;
    return 0;
}
