#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar4;
    uint8_t  irq;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

// Find a PCI device by class/subclass/prog_if. Returns 1 if found, 0 if not.
int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out);

#endif
