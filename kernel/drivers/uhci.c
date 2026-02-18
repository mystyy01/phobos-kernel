#include "uhci.h"
#include "pci.h"
#include "mouse.h"
#include "../pmm.h"
#include <stdint.h>

/* ---------- Port I/O helpers ---------- */

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* ---------- Debug helpers (QEMU debug port + framebuffer) ---------- */

/* Port 0xE9: QEMU debug console (shown on terminal with -debugcon stdio) */
static void dbg_char(char c) {
    outb(0xE9, (uint8_t)c);
}

static void dbg_str(const char *s) {
    while (*s) dbg_char(*s++);
}

static void dbg_hex8(uint8_t val) {
    const char *hex = "0123456789ABCDEF";
    dbg_char(hex[(val >> 4) & 0xF]);
    dbg_char(hex[val & 0xF]);
}

static void dbg_hex16(uint16_t val) {
    dbg_hex8((uint8_t)(val >> 8));
    dbg_hex8((uint8_t)val);
}

static void dbg_hex32(uint32_t val) {
    dbg_hex16((uint16_t)(val >> 16));
    dbg_hex16((uint16_t)val);
}

static void io_delay(void) {
    for (volatile int i = 0; i < 10000; i++) {}
}

static void delay_ms(int ms) {
    for (int i = 0; i < ms; i++) {
        for (volatile int j = 0; j < 5000; j++) {}
    }
}

/* ---------- UHCI register offsets ---------- */

#define UHCI_CMD        0x00
#define UHCI_STS        0x02
#define UHCI_INTR       0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

#define UHCI_CMD_RS     0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET 0x0004
#define UHCI_CMD_MAXP   0x0080

#define UHCI_PORTSC_CONNECT    0x0001
#define UHCI_PORTSC_CONNECT_CHG 0x0002
#define UHCI_PORTSC_ENABLE     0x0004
#define UHCI_PORTSC_ENABLE_CHG 0x0008
#define UHCI_PORTSC_LOWSPEED   0x0100
#define UHCI_PORTSC_RESET      0x0200

/* ---------- UHCI Transfer Descriptor ---------- */

struct uhci_td {
    uint32_t link;
    uint32_t ctrl_status;
    uint32_t token;
    uint32_t buffer;
    /* software fields (not read by hardware) */
    uint32_t _pad[4];
} __attribute__((aligned(16)));

#define TD_LINK_TERMINATE 0x01
#define TD_LINK_QH        0x02
#define TD_LINK_DEPTH     0x04

#define TD_STATUS_ACTIVE  (1u << 23)
#define TD_STATUS_STALL   (1u << 22)
#define TD_STATUS_DBERR   (1u << 21)
#define TD_STATUS_BABBLE  (1u << 20)
#define TD_STATUS_NAK     (1u << 19)
#define TD_STATUS_CRC     (1u << 18)
#define TD_STATUS_BITSTUF (1u << 17)
#define TD_STATUS_ANY_ERR (TD_STATUS_STALL | TD_STATUS_DBERR | TD_STATUS_BABBLE | TD_STATUS_CRC | TD_STATUS_BITSTUF)
#define TD_STATUS_IOC     (1u << 24)
#define TD_STATUS_LS      (1u << 26)
#define TD_STATUS_SPD     (1u << 29)
#define TD_CERR_SHIFT     27

#define TD_PID_SETUP 0x2D
#define TD_PID_IN    0x69
#define TD_PID_OUT   0xE1

#define TD_TOKEN(pid, addr, endp, toggle, maxlen) \
    ((uint32_t)(pid) | ((uint32_t)(addr) << 8) | ((uint32_t)(endp) << 15) | \
     ((uint32_t)(toggle) << 19) | ((uint32_t)((maxlen) - 1) << 21))

#define TD_TOKEN_ZLENGTH(pid, addr, endp, toggle) \
    ((uint32_t)(pid) | ((uint32_t)(addr) << 8) | ((uint32_t)(endp) << 15) | \
     ((uint32_t)(toggle) << 19) | (0x7FFu << 21))

/* ---------- UHCI Queue Head ---------- */

struct uhci_qh {
    uint32_t head_link;
    uint32_t element;
    uint32_t _pad[2];
} __attribute__((aligned(16)));

/* ---------- USB Setup Packet ---------- */

struct usb_setup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* ---------- USB Descriptor Types ---------- */

#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_INTERFACE     4
#define USB_DESC_ENDPOINT      5

/* ---------- Module State ---------- */

static uint16_t io_base;
static int uhci_active = 0;

/* Bump allocator for DMA-accessible structures (identity-mapped) */
static uint8_t *dma_pool;
static int dma_pool_offset;
static int dma_pool_size;

/* TD pool */
#define MAX_TDS 64
static struct uhci_td *td_pool;
static int td_pool_used;

/* Frame list and QHs */
static uint32_t *frame_list;
static struct uhci_qh *ctrl_qh;
static struct uhci_qh *intr_qh;

/* HID device type */
#define HID_TYPE_MOUSE  1   /* relative boot protocol mouse */
#define HID_TYPE_TABLET 2   /* absolute tablet (e.g. QEMU usb-tablet) */

/* Mouse/tablet state */
static int mouse_found = 0;
static int hid_type = 0;
static uint8_t mouse_addr;
static uint8_t mouse_endp;
static int mouse_low_speed;
static uint8_t mouse_max_pkt;
static uint8_t mouse_data_toggle;
static struct uhci_td *mouse_td;
static uint8_t *mouse_buf;

/* ---------- DMA Allocator ---------- */

static void dma_init(void) {
    void *page = pmm_alloc_page();
    if (!page) return;
    dma_pool = (uint8_t *)page;
    dma_pool_offset = 0;
    dma_pool_size = 4096;
}

static void *dma_alloc(int size, int align) {
    /* Grow pool if needed */
    while (dma_pool_offset + size + align > dma_pool_size) {
        void *page = pmm_alloc_page();
        if (!page) return (void *)0;
        /* Only works if pages are contiguous — just extend */
        dma_pool_size += 4096;
    }
    int off = dma_pool_offset;
    if (align > 1) {
        off = (off + align - 1) & ~(align - 1);
    }
    void *ptr = dma_pool + off;
    dma_pool_offset = off + size;
    return ptr;
}

/* ---------- TD helpers ---------- */

static struct uhci_td *alloc_td(void) {
    if (td_pool_used >= MAX_TDS) return (void *)0;
    struct uhci_td *td = &td_pool[td_pool_used++];
    td->link = TD_LINK_TERMINATE;
    td->ctrl_status = 0;
    td->token = 0;
    td->buffer = 0;
    return td;
}

static void reset_td_pool(void) {
    td_pool_used = 0;
}

static uint32_t td_phys(struct uhci_td *td) {
    return (uint32_t)(uint64_t)td;
}

static uint32_t qh_phys(struct uhci_qh *qh) {
    return (uint32_t)(uint64_t)qh;
}

/* ---------- UHCI register access ---------- */

static uint16_t uhci_read16(uint16_t reg) {
    return inw(io_base + reg);
}

static void uhci_write16(uint16_t reg, uint16_t val) {
    outw(io_base + reg, val);
}

static uint32_t uhci_read32(uint16_t reg) {
    return inl(io_base + reg);
}

static void uhci_write32(uint16_t reg, uint32_t val) {
    outl(io_base + reg, val);
}

/* ---------- USB Control Transfer ---------- */

static int uhci_control_transfer(uint8_t addr, struct usb_setup *setup,
                                  void *data, int data_len, int dir_in, int low_speed) {
    reset_td_pool();

    uint8_t *setup_buf = (uint8_t *)dma_alloc(8, 16);
    if (!setup_buf) return -1;
    for (int i = 0; i < 8; i++)
        setup_buf[i] = ((uint8_t *)setup)[i];

    uint8_t *data_buf = (void *)0;
    if (data_len > 0) {
        data_buf = (uint8_t *)dma_alloc(data_len, 16);
        if (!data_buf) return -1;
        if (!dir_in) {
            for (int i = 0; i < data_len; i++)
                data_buf[i] = ((uint8_t *)data)[i];
        }
    }

    uint32_t ls_bit = low_speed ? TD_STATUS_LS : 0;

    /* SETUP TD */
    struct uhci_td *setup_td = alloc_td();
    if (!setup_td) return -1;
    setup_td->ctrl_status = TD_STATUS_ACTIVE | ls_bit | (3u << TD_CERR_SHIFT);
    setup_td->token = TD_TOKEN(TD_PID_SETUP, addr, 0, 0, 8);
    setup_td->buffer = (uint32_t)(uint64_t)setup_buf;

    /* DATA TDs */
    struct uhci_td *prev = setup_td;
    int toggle = 1;
    int remaining = data_len;
    int offset = 0;
    uint8_t data_pid = dir_in ? TD_PID_IN : TD_PID_OUT;

    while (remaining > 0) {
        int pkt_len = remaining > 8 ? 8 : remaining;
        struct uhci_td *dtd = alloc_td();
        if (!dtd) return -1;
        dtd->ctrl_status = TD_STATUS_ACTIVE | ls_bit | (3u << TD_CERR_SHIFT);
        if (dir_in) dtd->ctrl_status |= TD_STATUS_SPD;
        dtd->token = TD_TOKEN(data_pid, addr, 0, toggle, pkt_len);
        dtd->buffer = (uint32_t)(uint64_t)(data_buf + offset);

        prev->link = td_phys(dtd) | TD_LINK_DEPTH;
        prev = dtd;
        toggle ^= 1;
        offset += pkt_len;
        remaining -= pkt_len;
    }

    /* STATUS TD */
    struct uhci_td *status_td = alloc_td();
    if (!status_td) return -1;
    uint8_t status_pid = dir_in ? TD_PID_OUT : TD_PID_IN;
    status_td->ctrl_status = TD_STATUS_ACTIVE | ls_bit | TD_STATUS_IOC | (3u << TD_CERR_SHIFT);
    status_td->token = TD_TOKEN_ZLENGTH(status_pid, addr, 0, 1);
    status_td->buffer = 0;

    prev->link = td_phys(status_td) | TD_LINK_DEPTH;

    /* Insert into control QH */
    ctrl_qh->element = td_phys(setup_td);

    /* Poll for completion */
    for (int i = 0; i < 50000; i++) {
        io_delay();
        uint32_t st = status_td->ctrl_status;
        if (!(st & TD_STATUS_ACTIVE)) {
            if (st & TD_STATUS_ANY_ERR) {
                ctrl_qh->element = TD_LINK_TERMINATE;
                return -1;
            }
            /* Copy data back */
            if (dir_in && data_len > 0 && data_buf) {
                for (int j = 0; j < data_len; j++)
                    ((uint8_t *)data)[j] = data_buf[j];
            }
            ctrl_qh->element = TD_LINK_TERMINATE;
            return data_len;
        }
        /* Check for stalled/errored earlier TDs */
        for (int t = 0; t < td_pool_used - 1; t++) {
            if (td_pool[t].ctrl_status & TD_STATUS_ANY_ERR) {
                ctrl_qh->element = TD_LINK_TERMINATE;
                return -1;
            }
        }
    }

    ctrl_qh->element = TD_LINK_TERMINATE;
    return -1; /* timeout */
}

/* ---------- USB Requests ---------- */

static int usb_get_descriptor(uint8_t addr, uint8_t type, uint8_t index,
                               void *buf, int len, int low_speed) {
    struct usb_setup setup;
    setup.bmRequestType = 0x80; /* device-to-host, standard, device */
    setup.bRequest = 6; /* GET_DESCRIPTOR */
    setup.wValue = (uint16_t)((type << 8) | index);
    setup.wIndex = 0;
    setup.wLength = (uint16_t)len;
    return uhci_control_transfer(addr, &setup, buf, len, 1, low_speed);
}

static int usb_set_address(uint8_t new_addr, int low_speed) {
    struct usb_setup setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = 5; /* SET_ADDRESS */
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    return uhci_control_transfer(0, &setup, (void *)0, 0, 0, low_speed);
}

static int usb_set_configuration(uint8_t addr, uint8_t config, int low_speed) {
    struct usb_setup setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = 9; /* SET_CONFIGURATION */
    setup.wValue = config;
    setup.wIndex = 0;
    setup.wLength = 0;
    return uhci_control_transfer(addr, &setup, (void *)0, 0, 0, low_speed);
}

static int usb_set_protocol(uint8_t addr, uint16_t iface, uint16_t protocol, int low_speed) {
    struct usb_setup setup;
    setup.bmRequestType = 0x21; /* host-to-device, class, interface */
    setup.bRequest = 0x0B; /* SET_PROTOCOL */
    setup.wValue = protocol;
    setup.wIndex = iface;
    setup.wLength = 0;
    return uhci_control_transfer(addr, &setup, (void *)0, 0, 0, low_speed);
}

static int usb_set_idle(uint8_t addr, uint16_t iface, int low_speed) {
    struct usb_setup setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = 0x0A; /* SET_IDLE */
    setup.wValue = 0;
    setup.wIndex = iface;
    setup.wLength = 0;
    return uhci_control_transfer(addr, &setup, (void *)0, 0, 0, low_speed);
}

/* ---------- Port Reset ---------- */

static int uhci_port_reset(uint16_t port_reg) {
    /* Assert reset */
    uhci_write16(port_reg, UHCI_PORTSC_RESET);
    delay_ms(50);

    /* Clear reset */
    uhci_write16(port_reg, 0);
    delay_ms(10);

    /* Wait for connect and enable */
    for (int i = 0; i < 10; i++) {
        uint16_t st = uhci_read16(port_reg);
        if (st & UHCI_PORTSC_CONNECT) {
            /* Clear change bits by writing 1 to them, enable port */
            uhci_write16(port_reg, UHCI_PORTSC_ENABLE | UHCI_PORTSC_CONNECT_CHG | UHCI_PORTSC_ENABLE_CHG);
            delay_ms(10);
            st = uhci_read16(port_reg);
            if (st & UHCI_PORTSC_ENABLE) {
                return 1;
            }
        }
        delay_ms(10);
    }
    return 0;
}

/* ---------- Device Enumeration ---------- */

static uint8_t next_usb_addr = 1;

static void uhci_enumerate_port(uint16_t port_reg) {
    uint16_t st = uhci_read16(port_reg);
    if (!(st & UHCI_PORTSC_CONNECT)) return;

    if (!uhci_port_reset(port_reg)) return;

    st = uhci_read16(port_reg);
    int low_speed = (st & UHCI_PORTSC_LOWSPEED) ? 1 : 0;

    /* Get first 8 bytes of device descriptor to learn max packet size */
    uint8_t dev_desc[18];
    for (int i = 0; i < 18; i++) dev_desc[i] = 0;

    if (usb_get_descriptor(0, USB_DESC_DEVICE, 0, dev_desc, 8, low_speed) < 0) {
        return;
    }
    uint8_t max_pkt = dev_desc[7];
    if (max_pkt == 0) max_pkt = 8;

    /* Assign address */
    uint8_t addr = next_usb_addr++;
    if (usb_set_address(addr, low_speed) < 0) {
        return;
    }
    delay_ms(10);

    /* Full device descriptor */
    if (usb_get_descriptor(addr, USB_DESC_DEVICE, 0, dev_desc, 18, low_speed) < 0) {
        return;
    }

    /* Get configuration descriptor (first 64 bytes should be enough) */
    uint8_t conf_buf[128];
    for (int i = 0; i < 128; i++) conf_buf[i] = 0;

    if (usb_get_descriptor(addr, USB_DESC_CONFIGURATION, 0, conf_buf, 9, low_speed) < 0) {
        return;
    }
    uint16_t total_len = (uint16_t)(conf_buf[2] | (conf_buf[3] << 8));
    if (total_len > 128) total_len = 128;
    if (usb_get_descriptor(addr, USB_DESC_CONFIGURATION, 0, conf_buf, total_len, low_speed) < 0) {
        return;
    }

    /* Parse descriptors looking for HID pointing device */
    uint8_t config_val = conf_buf[5];
    int found_hid = 0;
    int found_type = 0;
    uint8_t ep_addr = 0;
    uint16_t iface_num = 0;

    int pos = 0;
    while (pos + 1 < total_len) {
        uint8_t dlen = conf_buf[pos];
        uint8_t dtype = conf_buf[pos + 1];
        if (dlen == 0) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9 && pos + 8 < total_len) {
            uint8_t bClass    = conf_buf[pos + 5];
            uint8_t bSubClass = conf_buf[pos + 6];
            uint8_t bProtocol = conf_buf[pos + 7];
            iface_num = conf_buf[pos + 2];

            found_hid = 0;
            found_type = 0;

            if (bClass == 3) {
                /* HID boot mouse: subclass=1 protocol=2 */
                if (bSubClass == 1 && bProtocol == 2) {
                    found_hid = 1;
                    found_type = HID_TYPE_MOUSE;
                }
                /* Generic HID pointing device (e.g. QEMU usb-tablet) */
                else if (bProtocol == 0 && bSubClass == 0) {
                    found_hid = 1;
                    found_type = HID_TYPE_TABLET;
                }
            }

            dbg_str("  iface class=0x");
            dbg_hex8(bClass);
            dbg_str(" sub=0x");
            dbg_hex8(bSubClass);
            dbg_str(" proto=0x");
            dbg_hex8(bProtocol);
            dbg_str(found_hid ? " -> MATCH\n" : "\n");
        }

        if (dtype == USB_DESC_ENDPOINT && dlen >= 7 && found_hid) {
            ep_addr = conf_buf[pos + 2];
            if (ep_addr & 0x80) { /* IN endpoint */
                usb_set_configuration(addr, config_val, low_speed);
                delay_ms(10);
                if (found_type == HID_TYPE_MOUSE) {
                    usb_set_protocol(addr, iface_num, 0, low_speed); /* boot protocol */
                    delay_ms(10);
                }
                usb_set_idle(addr, iface_num, low_speed);

                mouse_found = 1;
                hid_type = found_type;
                mouse_addr = addr;
                mouse_endp = ep_addr & 0x0F;
                mouse_low_speed = low_speed;
                mouse_max_pkt = max_pkt;
                mouse_data_toggle = 0;

                dbg_str("UHCI: HID device type=");
                dbg_hex8((uint8_t)hid_type);
                dbg_str(" addr=0x");
                dbg_hex8(addr);
                dbg_str(" ep=0x");
                dbg_hex8(mouse_endp);
                dbg_char('\n');
                return;
            }
        }
        pos += dlen;
    }
}

/* ---------- Mouse Interrupt Polling Setup ---------- */

static void setup_mouse_polling(void) {
    mouse_buf = (uint8_t *)dma_alloc(8, 16);
    if (!mouse_buf) {
        mouse_found = 0;
        return;
    }
    for (int i = 0; i < 8; i++) mouse_buf[i] = 0;

    /* Allocate a permanent TD for interrupt IN transfers */
    mouse_td = (struct uhci_td *)dma_alloc(sizeof(struct uhci_td), 16);
    if (!mouse_td) {
        mouse_found = 0;
        return;
    }

    uint32_t ls_bit = mouse_low_speed ? TD_STATUS_LS : 0;
    mouse_td->link = TD_LINK_TERMINATE;
    mouse_td->ctrl_status = TD_STATUS_ACTIVE | ls_bit | (3u << TD_CERR_SHIFT);
    mouse_td->token = TD_TOKEN(TD_PID_IN, mouse_addr, mouse_endp, mouse_data_toggle,
                               mouse_max_pkt > 8 ? 8 : mouse_max_pkt);
    mouse_td->buffer = (uint32_t)(uint64_t)mouse_buf;

    /* Insert into interrupt QH */
    intr_qh->element = td_phys(mouse_td);
}

/* ---------- Public API ---------- */

void uhci_init(void) {
    struct pci_device pci;
    if (!pci_find_device(0x0C, 0x03, 0x00, &pci)) {
        dbg_str("UHCI: no controller found\n");
        return; /* No UHCI controller — PS/2 fallback */
    }

    io_base = (uint16_t)(pci.bar4 & ~0x3u);
    dbg_str("UHCI: found! io=0x");
    dbg_hex16(io_base);
    dbg_str(" vid=0x");
    dbg_hex16(pci.vendor_id);
    dbg_str(" did=0x");
    dbg_hex16(pci.device_id);
    dbg_char('\n');
    if (io_base == 0) return;

    /* Enable PCI bus mastering + I/O space */
    uint16_t cmd = pci_config_read16(pci.bus, pci.slot, pci.func, 0x04);
    cmd |= 0x05; /* I/O space + bus master */
    pci_config_write16(pci.bus, pci.slot, pci.func, 0x04, cmd);

    /* Global reset */
    uhci_write16(UHCI_CMD, UHCI_CMD_GRESET);
    delay_ms(50);
    uhci_write16(UHCI_CMD, 0);
    delay_ms(10);

    /* Host controller reset */
    uhci_write16(UHCI_CMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(uhci_read16(UHCI_CMD) & UHCI_CMD_HCRESET)) break;
        delay_ms(1);
    }

    /* Init DMA structures */
    dma_init();

    td_pool = (struct uhci_td *)dma_alloc(sizeof(struct uhci_td) * MAX_TDS, 16);
    if (!td_pool) return;
    td_pool_used = 0;

    /* Allocate frame list (4KB aligned, 1024 entries) */
    frame_list = (uint32_t *)pmm_alloc_page();
    if (!frame_list) return;

    /* Allocate QHs */
    ctrl_qh = (struct uhci_qh *)dma_alloc(sizeof(struct uhci_qh), 16);
    intr_qh = (struct uhci_qh *)dma_alloc(sizeof(struct uhci_qh), 16);
    if (!ctrl_qh || !intr_qh) return;

    /* Setup QH chain: frame_list -> intr_qh -> ctrl_qh -> terminate */
    ctrl_qh->head_link = TD_LINK_TERMINATE;
    ctrl_qh->element = TD_LINK_TERMINATE;

    intr_qh->head_link = qh_phys(ctrl_qh) | TD_LINK_QH;
    intr_qh->element = TD_LINK_TERMINATE;

    for (int i = 0; i < 1024; i++) {
        frame_list[i] = qh_phys(intr_qh) | TD_LINK_QH;
    }

    /* Program HC registers */
    uhci_write32(UHCI_FLBASEADD, (uint32_t)(uint64_t)frame_list);
    uhci_write16(UHCI_FRNUM, 0);
    uhci_write16(UHCI_INTR, 0); /* No interrupts — we poll */
    uhci_write16(UHCI_STS, 0xFFFF); /* Clear any pending status */

    /* Start the controller */
    uhci_write16(UHCI_CMD, UHCI_CMD_RS | UHCI_CMD_MAXP);

    uhci_active = 1;

    /* Enumerate both root hub ports */
    uhci_enumerate_port(UHCI_PORTSC1);
    if (!mouse_found) {
        uhci_enumerate_port(UHCI_PORTSC2);
    }

    if (mouse_found) {
        dbg_str("UHCI: HID ready type=");
        dbg_str(hid_type == HID_TYPE_TABLET ? "TABLET" : "MOUSE");
        dbg_str(" addr=0x");
        dbg_hex8(mouse_addr);
        dbg_str(" ep=0x");
        dbg_hex8(mouse_endp);
        dbg_str(" ls=");
        dbg_hex8((uint8_t)mouse_low_speed);
        dbg_char('\n');
        setup_mouse_polling();
        mouse_set_ps2_enabled(0);
    } else {
        mouse_set_ps2_enabled(1);
        dbg_str("UHCI: no mouse found");
        uint16_t p1 = uhci_read16(UHCI_PORTSC1);
        uint16_t p2 = uhci_read16(UHCI_PORTSC2);
        dbg_str(" P1=0x");
        dbg_hex16(p1);
        dbg_str(" P2=0x");
        dbg_hex16(p2);
        dbg_char('\n');
    }
}

void uhci_poll(void) {
    if (!uhci_active || !mouse_found || !mouse_td) return;

    uint32_t status = mouse_td->ctrl_status;

    /* Still active — nothing to do */
    if (status & TD_STATUS_ACTIVE) return;

    /* Check for errors */
    if (status & TD_STATUS_ANY_ERR) {
        dbg_str("UHCI ERR st=0x");
        dbg_hex32(status);
        dbg_char('\n');
        /* Resubmit without toggling */
        for (int i = 0; i < 8; i++) mouse_buf[i] = 0;
        uint32_t ls_bit = mouse_low_speed ? TD_STATUS_LS : 0;
        mouse_td->ctrl_status = TD_STATUS_ACTIVE | ls_bit | (3u << TD_CERR_SHIFT);
        mouse_td->token = TD_TOKEN(TD_PID_IN, mouse_addr, mouse_endp, mouse_data_toggle,
                                   mouse_max_pkt > 8 ? 8 : mouse_max_pkt);
        intr_qh->element = td_phys(mouse_td);
        return;
    }

    /* Completed — extract actual length (ActLen field is n-1, 0x7FF = zero) */
    int actual_len = (int)((status + 1) & 0x7FF);
    int toggled = 0;

    if (hid_type == HID_TYPE_TABLET && actual_len >= 6) {
        /* USB tablet: byte0=buttons, byte1-2=X(LE), byte3-4=Y(LE), byte5=wheel */
        toggled = 1;
        uint8_t buttons = mouse_buf[0] & 0x07;
        int abs_x = (int)(mouse_buf[1] | (mouse_buf[2] << 8));
        int abs_y = (int)(mouse_buf[3] | (mouse_buf[4] << 8));

        mouse_update_absolute(abs_x, abs_y, 32767, 32767, buttons);
    } else if (hid_type == HID_TYPE_MOUSE && actual_len >= 3) {
        /* Boot protocol mouse: byte0=buttons, byte1=dx, byte2=dy */
        toggled = 1;
        uint8_t buttons = mouse_buf[0] & 0x07;
        int8_t dx = (int8_t)mouse_buf[1];
        int8_t dy = (int8_t)mouse_buf[2];

        mouse_update_relative((int)dx, (int)dy, buttons);
    } else if (actual_len > 0) {
        toggled = 1;
    }

    if (toggled) {
        mouse_data_toggle ^= 1;
    }

    /* Resubmit TD for next poll */
    for (int i = 0; i < 8; i++) mouse_buf[i] = 0;
    uint32_t ls_bit = mouse_low_speed ? TD_STATUS_LS : 0;
    mouse_td->ctrl_status = TD_STATUS_ACTIVE | ls_bit | (3u << TD_CERR_SHIFT);
    mouse_td->token = TD_TOKEN(TD_PID_IN, mouse_addr, mouse_endp, mouse_data_toggle,
                               mouse_max_pkt > 8 ? 8 : mouse_max_pkt);
    mouse_td->link = TD_LINK_TERMINATE;
    intr_qh->element = td_phys(mouse_td);
}
