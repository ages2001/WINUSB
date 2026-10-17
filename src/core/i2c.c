/* i2c.c - HID over I2C: touchpads and touch screens of notebooks on the DesignWare I2C
   controllers of the Intel LPSS (PCI functions or ACPI devices) and of the AMD FCH (dwi2c.c).
   The devices come from the ACPI tables: every PNP0C50 / ACPI0C50 device with its
   I2cSerialBus address, the controller that resource names, the HID descriptor register that
   _DSM function 1 returns and its GpioInt line. Without such a device the usual touchpad
   addresses and descriptor registers are tried on the LPSS PCI functions. When the GpioInt
   pad can be read (gpio.c) and follows the device's interrupt at its reset, the input
   register is read only while the line is asserted; otherwise it is polled. Touchpads go to
   ptp.c in touchpad mode (two-finger scrolling, taps), other pointers to the HID mouse code.
   /NOI2C turns it off. */
#include "usb.h"
#include "i2cbus.h"

#define MAXHI2C     4
#define MAXACPI     8

struct hi2c {
    struct i2cbus *bus;
    u8 addr;
    u16 descreg;
    u8 desc[30];
    u16 inreg, inlen, cmdreg, datareg;
    u8 *buf;
    struct ptp *ptp;
    struct hiddev *mouse;
    u32 last, idle;
    u32 errors;
    char gctl[32];              /* GpioInt controller, "" when the line is not used */
    u16 gpin;
    u8 gact;                    /* pad level while the interrupt is asserted */
};

static struct hi2c devs[MAXHI2C];
static int ndev;
static struct acpi_hidi2c adevs[MAXACPI];
static u8 astarted[MAXACPI];
static int nacpi = -1;
static u32 pci_tried[8];
static int npci_tried;

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static int reg_read(struct hi2c *d, u16 reg, u8 *buf, int n)
{
    u8 w[2];
    w[0] = (u8)reg;
    w[1] = (u8)(reg >> 8);
    return i2c_xfer(d->bus, d->addr, w, 2, buf, n);
}

/* command register: opcode with report type / ID, then data register data */
static int hid_cmd(struct hi2c *d, int op, int type, int id, const u8 *data, int dn)
{
    u8 b[64];
    int n = 0, k;
    b[n++] = (u8)d->cmdreg;
    b[n++] = (u8)(d->cmdreg >> 8);
    b[n++] = (u8)((type << 4) | (id >= 15 ? 15 : id));
    b[n++] = (u8)op;
    if (id >= 15) b[n++] = (u8)id;
    if (data) {
        int size = 2 + (id ? 1 : 0) + dn;
        b[n++] = (u8)d->datareg;
        b[n++] = (u8)(d->datareg >> 8);
        b[n++] = (u8)size;
        b[n++] = (u8)(size >> 8);
        if (id) b[n++] = (u8)id;
        for (k = 0; k < dn && n < (int)sizeof(b); k++) b[n++] = data[k];
    }
    return i2c_xfer(d->bus, d->addr, b, n, NULL, 0);
}

/* the interrupt line: 1 asserted, 0 not, -1 unknown */
static int irq_line(struct hi2c *d)
{
    int v;
    if (!d->gctl[0]) return -1;
    v = gpio_get(d->gctl, d->gpin);
    if (v < 0) return -1;
    return v == d->gact;
}

static int read_input(struct hi2c *d)
{
    u16 len;
    if (i2c_xfer(d->bus, d->addr, NULL, 0, d->buf, d->inlen)) {
        d->errors++;
        return -1;
    }
    len = le16(d->buf);
    if (len < 3 || len == 0xFFFF || len > d->inlen) return 0;   /* nothing (or the reset answer) */
    if (d->ptp) ptp_input(d->ptp, d->buf + 2, len - 2U);
    else if (d->mouse) hid_mouse_input(d->mouse, d->buf + 2, len - 2U);
    return 1;
}

static const u16 descregs[] = { 0x0001, 0x0020, 0x0000, 0x0030, 0x0010, 0x0002 };

/* a HID device at 'addr' of 'bus'? hint: the descriptor register ACPI names (FFFFh none);
   a: the ACPI device (GpioInt), NULL */
static int try_device(struct i2cbus *bus, u8 addr, u16 hint, const struct acpi_hidi2c *a)
{
    struct hi2c *d = &devs[ndev];
    u8 *rd = NULL;
    int i, rdlen, ok = 0, seen = 0;
    if (ndev >= MAXHI2C) return -1;
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->addr = addr;
    for (i = -1; i < (int)ARRAY_SIZE(descregs) && !ok; i++) {
        if (i < 0 && hint == 0xFFFF) continue;
        d->descreg = i < 0 ? hint : descregs[i];
        if (i >= 0 && d->descreg == hint) continue;
        if (reg_read(d, d->descreg, d->desc, 30)) {
            if (i == -1 || (i == 0 && hint == 0xFFFF)) return -1;     /* nobody at this address */
            continue;
        }
        ok = le16(d->desc) == 30 && le16(d->desc + 2) == 0x0100 && le16(d->desc + 4) && le16(d->desc + 4) < 4096 &&
             le16(d->desc + 10) >= 3 && le16(d->desc + 10) <= 512;
    }
    if (!ok) return -1;
    rdlen = le16(d->desc + 4);
    d->inreg = le16(d->desc + 8);
    d->inlen = le16(d->desc + 10);
    d->cmdreg = le16(d->desc + 16);
    d->datareg = le16(d->desc + 18);
    kprintf("i2c: %s address %02x: HID %04x:%04x (descriptor at %04x)\n", a ? a->seg : "I2C", addr,
            le16(d->desc + 20), le16(d->desc + 22), d->descreg);
    if (a && a->gctl[0]) {
        strlcpy(d->gctl, a->gctl, sizeof(d->gctl));
        d->gpin = a->gpin;
        d->gact = (u8)(((a->gflags >> 1) & 3) == 0 ? 1 : 0);    /* active high, else low */
        if (irq_line(d) < 0) {
            kprintf("i2c: interrupt line %s pin %u not readable, polling\n", d->gctl, d->gpin);
            d->gctl[0] = 0;
        }
    }
    /* power on, reset (the device asserts its interrupt and answers with an empty report) */
    hid_cmd(d, 8, 0, 0, NULL, 0);
    mdelay(1);
    hid_cmd(d, 1, 0, 0, NULL, 0);
    d->buf = kmalloc(d->inlen + 2U);
    rd = kmalloc((u32)rdlen + 2);
    if (!d->buf || !rd) goto fail;
    for (i = 0; i < 100; i++) {                     /* up to 1 s */
        mdelay(10);
        if (irq_line(d) == 1) seen = 1;
        if (d->gctl[0] && !seen && i < 50) continue;
        if (!i2c_xfer(bus, addr, NULL, 0, d->buf, d->inlen) && le16(d->buf) == 0) break;
    }
    if (d->gctl[0]) {
        if (seen && irq_line(d) == 0) {
            DBG("i2c: interrupt line %s pin %u follows the device\n", d->gctl, d->gpin);
        } else {
            kprintf("i2c: interrupt line %s pin %u not readable, polling\n", d->gctl, d->gpin);
            d->gctl[0] = 0;
        }
    }
    if (reg_read(d, le16(d->desc + 6), rd, rdlen)) goto fail;
    if (ptp_is_touchpad(rd, rdlen) && (d->ptp = ptp_new(rd, rdlen)) != NULL) {
        u8 fr[16];
        int w, fl;
        for (w = 0; w < 2; w++) {
            fl = ptp_feature(d->ptp, w, fr, sizeof(fr));
            if (fl) hid_cmd(d, 3, 3, fr[0], fr + 1, fl - 1);
        }
    } else {
        d->mouse = hid_mouse_new(rd, rdlen);
    }
    kfree(rd);
    rd = NULL;
    if (!d->ptp && !d->mouse) {
        kprintf("i2c: address %02x: not a touchpad, mouse or touch screen\n", addr);
        hid_cmd(d, 8, 0, 1, NULL, 0);               /* back to sleep */
        goto fail;
    }
    ndev++;
    return 0;
fail:
    if (rd) kfree(rd);
    if (d->buf) kfree(d->buf);
    d->buf = NULL;
    return -1;
}

/* the usual touchpad addresses when ACPI does not tell */
static const u8 usual[] = { 0x15, 0x2C, 0x20, 0x10, 0x14, 0x24, 0x38, 0x4B, 0x5D };

/* every controller of the list (and those ACPI names); idempotent: the Configuration Manager
   of Windows 95/98/ME hands the LPSS functions over one by one */
void i2c_start(struct pcidev *pcis, int n)
{
    int i, k;
    if (cb->opts & OPT_NOI2C) return;
    if (nacpi < 0) {
        nacpi = acpi_hid_i2c(adevs, MAXACPI);
        memset(astarted, 0, sizeof(astarted));
        kprintf("i2c: %d HID over I2C device(s) in ACPI\n", nacpi);
    }
    for (i = 0; i < nacpi; i++) {
        struct acpi_hidi2c *a = &adevs[i];
        struct i2cbus *bus;
        if (astarted[i] || !a->bus[0]) continue;
        bus = i2c_bus_get(a->bus);
        if (!bus) {
            kprintf("i2c: %s: controller %s not found\n", a->seg, a->bus);
            astarted[i] = 2;
            continue;
        }
        for (k = 0; k < (int)a->naddr; k++)
            if (!try_device(bus, (u8)a->addr[k], a->descreg, a)) break;
        astarted[i] = 1;
    }
    if (nacpi > 0) return;
    /* no ACPI description: the LPSS functions at the usual addresses */
    for (i = 0; i < n; i++) {
        struct pcidev *p = &pcis[i];
        struct i2cbus *bus;
        int before = ndev, j;
        if (p->cls != 0x0C || p->sub != 0x80 || p->vendor != 0x8086) continue;
        for (j = 0; j < npci_tried && pci_tried[j] != PCI_BDF(p); j++) ;
        if (j < npci_tried || npci_tried >= (int)ARRAY_SIZE(pci_tried)) continue;
        pci_tried[npci_tried++] = PCI_BDF(p);
        bus = i2c_bus_pci(p);
        if (!bus) continue;
        for (k = 0; k < (int)ARRAY_SIZE(usual) && ndev == before; k++) try_device(bus, usual[k], 0xFFFF, NULL);
    }
}

/* every 10 ms (8 ms in Windows, each tick in DOS); a few reads while reports come. With the
   interrupt line: only while it is asserted (and once a second, should an edge be lost) */
void i2c_poll(void)
{
    int i, k;
    u32 now = plat_ms();
    for (i = 0; i < ndev; i++) {
        struct hi2c *d = &devs[i];
        if (d->gctl[0]) {
            if (irq_line(d) != 1 && now - d->last < 1000) continue;
        } else if (now - d->last < (d->idle > 50 ? 30UL : 8UL)) {
            continue;
        }
        d->last = now;
        for (k = 0; k < 4; k++) {
            if (read_input(d) <= 0) break;
            if (d->gctl[0] && irq_line(d) != 1) {
                k++;
                break;
            }
        }
        if (k) d->idle = 0;
        else if (d->idle < 1000) d->idle++;
    }
    if (ndev) ptp_tick();
}

void i2c_shutdown(void)
{
    int i;
    for (i = 0; i < ndev; i++) {
        hid_cmd(&devs[i], 8, 0, 1, NULL, 0);    /* sleep */
        if (devs[i].ptp) ptp_free(devs[i].ptp);
        if (devs[i].mouse) hid_mouse_free(devs[i].mouse);
        kfree(devs[i].buf);
    }
    ndev = 0;
    nacpi = -1;
    npci_tried = 0;
    i2c_bus_off();
}
