/*
 * pci.c -- PCI configuration space via the 0xCF8/0xCFC ports
 * (M72). See pci.h. All offsets are dword-aligned by the
 * mechanism itself; 8/16-bit reads extract from the dword.
 */
#include <yousj/types.h>
#include <yousj/pci.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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

static uint32_t config_addr(uint8_t bus, uint8_t slot,
                            uint8_t func, uint8_t offset)
{
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
           (offset & 0xFCu);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func,
                    uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, config_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func,
                 uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDR, config_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func,
                    uint8_t offset)
{
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint16_t)(v >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func,
                  uint8_t offset)
{
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint8_t)(v >> ((offset & 3) * 8));
}

bool pci_find(uint16_t vendor, uint16_t device, uint8_t *slot_out,
              uint8_t *func_out)
{
    uint8_t slot, func;

    for (slot = 0; slot < 32; slot++) {
        for (func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0);
            if ((uint16_t)id == 0xFFFF)
                continue; /* nothing here */
            if ((uint16_t)id == vendor &&
                (uint16_t)(id >> 16) == device) {
                *slot_out = slot;
                *func_out = func;
                return true;
            }
            if (func == 0) {
                /* Not multifunction: skip the other funcs. */
                uint8_t hdr = pci_read8(0, slot, 0, 0x0E);
                if ((hdr & 0x80) == 0)
                    break;
            }
        }
    }
    return false;
}
