/* Yousj OS - legacy ATA (IDE) PIO block driver (M12).
 *
 * Polling PIO on the two legacy buses (primary 0x1F0,
 * secondary 0x170), master drive only, LBA28. No IRQs, no
 * DMA: slow but small, deterministic, and identical under
 * QEMU and period hardware. All transfers go through
 * 512-byte sectors; callers in yfs batch into 4 KiB blocks.
 */
#ifndef YOUSJ_ATA_H
#define YOUSJ_ATA_H

#include <yousj/types.h>

#define ATA_SECTOR_SIZE 512

struct ata_drive {
    int bus;              /* 0 primary, 1 secondary */
    uint32_t sectors;     /* LBA28 capacity */
    bool present;
};

/* Probe one bus's master. Returns 0 and fills *out when a
 * drive answers IDENTIFY; negative when the bus is empty. */
int ata_probe(int bus, struct ata_drive *out);

/* Read/write count sectors at lba. 0 on success. The drive
 * must have been probed. Writes end with a FLUSH CACHE. */
int ata_read(const struct ata_drive *d, uint32_t lba,
             uint32_t count, void *buf);
int ata_write(const struct ata_drive *d, uint32_t lba,
              uint32_t count, const void *buf);

#endif /* YOUSJ_ATA_H */
