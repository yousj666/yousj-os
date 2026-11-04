/*
 * pci.h -- minimal PCI configuration-space access (M72).
 *
 * Type-1 configuration cycles through 0xCF8/0xCFC, the same
 * mechanism every PC has had since PCI 2.0. Only what the
 * virtio-net driver needs: full-dword reads and writes over
 * the single bus 0 a QEMU machine exposes, and a scan helper
 * that finds the first device with a given vendor/device id.
 */
#ifndef YOUSJ_PCI_H
#define YOUSJ_PCI_H

#include <yousj/types.h>

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func,
                    uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func,
                 uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func,
                    uint8_t offset);
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func,
                  uint8_t offset);

/* Find the first device with this vendor/device pair.
 * Returns true and fills slot/func (bus is always 0 here). */
bool pci_find(uint16_t vendor, uint16_t device, uint8_t *slot_out,
              uint8_t *func_out);

#endif /* YOUSJ_PCI_H */
