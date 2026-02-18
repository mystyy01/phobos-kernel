#include "pci.h"

#define PCI_CONFIG_ADDR 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t addr = pci_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDR, addr);
    uint32_t old = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    old &= ~(0xFFFFu << shift);
    old |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, old);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_config_read32(bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }

                uint32_t class_reg = pci_config_read32(bus, slot, func, 0x08);
                uint8_t cc  = (uint8_t)(class_reg >> 24);
                uint8_t sc  = (uint8_t)(class_reg >> 16);
                uint8_t pi  = (uint8_t)(class_reg >> 8);

                if (cc == class_code && sc == subclass && pi == prog_if) {
                    if (out) {
                        out->bus       = (uint8_t)bus;
                        out->slot      = (uint8_t)slot;
                        out->func      = (uint8_t)func;
                        out->vendor_id = vendor;
                        out->device_id = (uint16_t)(id >> 16);
                        out->bar4      = pci_config_read32(bus, slot, func, 0x20);
                        out->irq       = pci_config_read8(bus, slot, func, 0x3C);
                    }
                    return 1;
                }

                // Only scan func > 0 if multi-function
                if (func == 0) {
                    uint8_t header = pci_config_read8(bus, slot, func, 0x0E);
                    if ((header & 0x80) == 0) break;
                }
            }
        }
    }
    return 0;
}
