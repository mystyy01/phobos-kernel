#ifndef UHCI_H
#define UHCI_H

// Initialize UHCI host controller and enumerate USB devices.
// If no UHCI controller is found, returns silently (PS/2 fallback).
void uhci_init(void);

// Poll UHCI for completed transfers. Call from timer IRQ.
void uhci_poll(void);

#endif
