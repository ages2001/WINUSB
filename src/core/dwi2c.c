/* dwi2c.c - I2C master on the Synopsys DesignWare controllers of the Intel LPSS (PCI class
   0C/80) and of the AMD FCH: the audio codecs and amplifiers of the DSP machines and the HID
   over I2C touchpads. The controller is found by the _ADR of its ACPI device (the
   I2cSerialBus resource of the device names it), else by the usual device numbers of the
   Intel chipsets. Bay Trail, Cherry Trail, Haswell and Broadwell machines mostly run the LPSS
   in ACPI mode: the controller is then no PCI function but an ACPI device (80860F41,
   808622C1, INT33C2/3, INT3432/3; Sunrise Point INT3446/7, Broxton 80865AAC...) whose
   registers the _CRS template gives, powered up through the PSAT field as its _PS0 does. The
   AMD controllers (AMD0010, AMDI0010, AMDI0510) are always ACPI devices with fixed registers
   and no private space. Polled transfers, fast mode (400 kHz at most). */
#include "i2cbus.h"

#define IC_CON          0x00
#define IC_TAR          0x04
#define IC_DATA_CMD     0x10
#define IC_SS_HCNT      0x14
#define IC_SS_LCNT      0x18
#define IC_FS_HCNT      0x1C
#define IC_FS_LCNT      0x20
#define IC_INTR_MASK    0x30
#define IC_RAW_INTR     0x34
#define IC_RX_TL        0x38
#define IC_TX_TL        0x3C
#define IC_CLR_INTR     0x40
#define IC_CLR_TX_ABRT  0x54
#define IC_ENABLE       0x6C
#define IC_TXFLR        0x74
#define IC_RXFLR        0x78
#define IC_SDA_HOLD     0x7C
#define IC_EN_STATUS    0x9C
#define IC_COMP_PARAM1  0xF4
#define IC_COMP_TYPE    0xFC
#define DW_TYPE         0x44570140UL
#define CMD_READ        0x100
#define CMD_STOP        0x200
#define CMD_RESTART     0x400
#define INTR_TX_ABRT    0x40
#define INTR_STOP_DET   0x200
#define LPSS_RESETS     0x204           /* Sunrise Point and later (private space at 200) */
#define LPSS_REMAP      0x240
#define LPSS8_CLK       0x800           /* Atom, Haswell / Broadwell (private space at 800) */
#define LPSS8_RESETS    0x804

#define MAXBUS 8

struct i2cbus {
    u8 *r;
    u32 phys;
    u16 bdf;                            /* PCI function, FFFFh: ACPI device */
    int txdepth, rxdepth, tar;
};

static struct i2cbus buses[MAXBUS];
static int nbus;
static u16 fmcn[3], sscn[3];
static int timing_read;

static u32 rd(struct i2cbus *c, u32 o) { return RD32(c->r + o); }
static void wr(struct i2cbus *c, u32 o, u32 v) { WR32(c->r + o, v); }

static int enable(struct i2cbus *c, int on)
{
    int i;
    wr(c, IC_ENABLE, on ? 1 : 0);
    for (i = 0; i < 100; i++) {
        if ((rd(c, IC_EN_STATUS) & 1) == (u32)(on ? 1 : 0)) return 0;
        udelay(25);
    }
    return -1;
}

/* the LPSS generation of a controller: 0 private space at 200, 1 Atom (private space at 800),
   2 Haswell / Broadwell (800, with a clock gate), 3 no private space (AMD) */
static int lpss_gen(u16 did)
{
    if ((did >= 0x0F41 && did <= 0x0F47) || (did >= 0x22C1 && did <= 0x22C7)) return 1;
    if (did == 0x9C61 || did == 0x9C62 || did == 0x9CE1 || did == 0x9CE2) return 2;
    return 0;
}

/* controller registers at phys: out of reset, clocked, timing for fast mode */
static int bus_init(struct i2cbus *c, u32 phys, int gen)
{
    u32 p1;
    c->r = plat_mmio(phys, 0x1000);
    if (!c->r) return -1;
    c->phys = phys;
    if (gen == 3) {
        /* AMD: nothing in front of the DesignWare block */
    } else if (gen) {
        if (gen == 2) WR32(c->r + LPSS8_CLK, RD32(c->r + LPSS8_CLK) | 1);
        WR32(c->r + LPSS8_RESETS, RD32(c->r + LPSS8_RESETS) | 3);   /* functional and APB reset off */
    } else {
        WR32(c->r + LPSS_RESETS, 0);
        WR32(c->r + LPSS_RESETS, 7);
        WR32(c->r + LPSS_REMAP, phys);
        WR32(c->r + LPSS_REMAP + 4, 0);
    }
    if (rd(c, IC_COMP_TYPE) != DW_TYPE) {
        DBG("i2c: %08lx: component type %08lx, not a DesignWare I2C\n", phys, rd(c, IC_COMP_TYPE));
        return -1;
    }
    enable(c, 0);
    p1 = rd(c, IC_COMP_PARAM1);
    c->txdepth = (int)((p1 >> 16) & 0xFF) + 1;
    c->rxdepth = (int)((p1 >> 8) & 0xFF) + 1;
    if (c->txdepth < 2 || c->txdepth > 256) c->txdepth = 32;
    if (c->rxdepth < 2 || c->rxdepth > 256) c->rxdepth = 32;
    if (!timing_read) {
        struct acpi_i2c tmp[1];
        acpi_i2c_devices(tmp, 0, fmcn, sscn);
        timing_read = 1;
    }
    wr(c, IC_SS_HCNT, sscn[0] ? sscn[0] : 540);
    wr(c, IC_SS_LCNT, sscn[1] ? sscn[1] : 640);
    wr(c, IC_FS_HCNT, fmcn[0] ? fmcn[0] : 140);
    wr(c, IC_FS_LCNT, fmcn[1] ? fmcn[1] : 300);
    if (fmcn[2]) wr(c, IC_SDA_HOLD, fmcn[2]);
    wr(c, IC_CON, 0x01 | (2 << 1) | 0x20 | 0x40);       /* master, fast, restart, no slave */
    wr(c, IC_INTR_MASK, 0);
    wr(c, IC_RX_TL, 0);
    wr(c, IC_TX_TL, 0);
    c->tar = -1;
    return 0;
}

static int bus_start(struct pcidev *p, struct i2cbus *c)
{
    u32 phys = pci_bar_mem(p, 0, NULL);
    int pm;
    if (!phys || p->vendor != 0x8086) return -1;
    pci_enable(p, 1, 0, 1);
    pci_intx(p, 0);
    pm = pci_find_cap(p, 1);
    if (pm && (pci_rd16(p->bus, p->dev, p->fn, pm + 4) & 3)) {
        pci_wr16(p->bus, p->dev, p->fn, pm + 4, (u16)(pci_rd16(p->bus, p->dev, p->fn, pm + 4) & ~3));
        mdelay(10);
    }
    c->bdf = (u16)PCI_BDF(p);
    return bus_init(c, phys, lpss_gen(p->device));
}

/* a controller in ACPI mode: the LPSS generation of its _HID, -1 when it is none */
static int acpi_lpss(const char *hid)
{
    static const char *const ids[] = {
        "80860F41", "808622C1",                                         /* Atom */
        "INT33C2", "INT33C3", "INT3432", "INT3433",                     /* Haswell, Broadwell */
        "INT3446", "INT3447", "80865AAC", "80865AAE", "80865AB0",       /* Sunrise Point, Broxton */
        "80865AB2", "80865AB4", "80865AB6", "80863196", "80863198",
        "AMD0010", "AMDI0010", "AMDI0510"                               /* AMD FCH */
    };
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(ids); i++)
        if (!strncmp(hid, ids[i], 9)) return i < 2 ? 1 : i < 6 ? 2 : i < 16 ? 0 : 3;
    return -1;
}

static struct i2cbus *acpi_bus(const char *acpipath, int gen)
{
    u32 base[4], len[4], psat;
    int n = acpi_dev_mem(acpipath, base, len, 4), i;
    u8 *p;
    if (n < 1 || !base[0] || (base[0] & 0xFFF) || base[0] == 0xFFFFFFFFUL) {
        DBG("i2c: %s: no register address in ACPI\n", acpipath);
        return NULL;
    }
    for (i = 0; i < nbus; i++)
        if (buses[i].phys == base[0]) return &buses[i];
    if (nbus >= MAXBUS) return NULL;
    /* D0 as _PS0 does it: the power state bits of the PCI configuration mirror */
    if (!acpi_dev_field(acpipath, "PSAT", &psat) && (p = plat_mmio(psat, 4)) != NULL && (RD32(p) & 3)) {
        WR32(p, RD32(p) & ~3UL);
        mdelay(10);
    }
    buses[nbus].bdf = 0xFFFF;
    if (bus_init(&buses[nbus], base[0], gen)) {
        kprintf("i2c: %s (%08lx): controller does not start\n", acpipath, base[0]);
        return NULL;
    }
    DBG("i2c: %s at %08lx (ACPI mode)\n", acpipath, base[0]);
    return &buses[nbus++];
}

/* the usual LPSS I2C functions: I2C0-3 at 15.0-15.3, I2C4/5 at 19.0/19.1 (19.2/19.1 on
   Skylake / Kaby Lake), I2C6/7 at 10.0/10.1 (Tiger Lake and later) */
static int usual_devfn(int n, int skl)
{
    static const u8 df[8] = { 0xA8, 0xA9, 0xAA, 0xAB, 0xC8, 0xC9, 0x80, 0x81 };
    if (n < 0 || n > 7) return -1;
    if (skl && n == 4) return 0xCA;
    return df[n];
}

struct i2cbus *i2c_bus_pci(struct pcidev *p)
{
    int i;
    for (i = 0; i < nbus; i++)
        if (buses[i].bdf == (u16)PCI_BDF(p)) return &buses[i];
    if (nbus >= MAXBUS || p->cls != 0x0C || p->sub != 0x80) return NULL;
    buses[nbus].bdf = (u16)PCI_BDF(p);
    if (bus_start(p, &buses[nbus])) return NULL;
    return &buses[nbus++];
}

u32 i2c_bus_id(struct i2cbus *b)
{
    return b->bdf != 0xFFFF ? b->bdf : 0x10000UL | (u32)(b - buses);
}

/* every controller disabled (Windows' drivers or the BIOS take them over) */
void i2c_bus_off(void)
{
    int i;
    for (i = 0; i < nbus; i++) enable(&buses[i], 0);
    nbus = 0;
}

struct i2cbus *i2c_bus_get(const char *acpipath)
{
    u32 adr, uid, mem[2];
    int devfn = -1, i, n, gen;
    struct pcidev p;
    char hid[9];
    if (acpi_dev_info(acpipath, hid, &uid, mem, 2) >= 0 && (gen = acpi_lpss(hid)) >= 0) {
        struct i2cbus *b = acpi_bus(acpipath, gen);
        if (b) return b;
    }
    if (acpi_path_adr(acpipath, &adr) == 0 && (adr >> 16) < 32 && (adr & 0xFFFF) < 8)
        devfn = (int)(((adr >> 16) << 3) | (adr & 7));
    if (devfn < 0) {
        /* \_SB.PCI0.I2Cn */
        const char *s = acpipath + strlen(acpipath);
        u16 hostdev;
        while (s > acpipath && s[-1] != '.') s--;
        if (strncmp(s, "I2C", 3) || s[3] < '0' || s[3] > '7') return NULL;
        n = s[3] - '0';
        hostdev = pci_rd16(0, 0x1F, 0, 2);
        devfn = usual_devfn(n, (hostdev & 0xFF00) == 0x9D00 || (hostdev & 0xFF00) == 0xA100 ||
                               (hostdev & 0xFF00) == 0xA200);
    }
    for (i = 0; i < nbus; i++)
        if (buses[i].bdf == (u16)devfn) return &buses[i];
    if (nbus >= MAXBUS) return NULL;
    if (pci_fill(&p, 0, devfn >> 3, devfn & 7) || p.cls != 0x0C || p.sub != 0x80) {
        kprintf("i2c: %s (00:%02x.%u) is not an LPSS I2C controller\n", acpipath, devfn >> 3, devfn & 7);
        return NULL;
    }
    buses[nbus].bdf = (u16)devfn;
    if (bus_start(&p, &buses[nbus])) {
        kprintf("i2c: %s: controller does not start\n", acpipath);
        return NULL;
    }
    DBG("i2c: %s = 00:%02x.%u\n", acpipath, devfn >> 3, devfn & 7);
    return &buses[nbus++];
}

/* write 'wn' bytes, then (repeated start) read 'rn' bytes; 0 or -1 (no answer) */
int i2c_xfer(struct i2cbus *c, int addr, const u8 *w, int wn, u8 *rbuf, int rn)
{
    int sent = 0, issued = 0, got = 0, total = wn + rn;
    u32 t0;
    if (!c || !total) return -1;
    if (addr != c->tar) {
        if (enable(c, 0)) return -1;
        wr(c, IC_TAR, (u32)addr);
        c->tar = addr;
    }
    if (enable(c, 1)) return -1;
    (void)rd(c, IC_CLR_INTR);
    t0 = plat_ms();
    while (got < rn || sent < total) {
        u32 raw;
        while (sent < total && (int)rd(c, IC_TXFLR) < c->txdepth) {
            u32 cmd;
            if (sent < wn) {
                cmd = w[sent];
            } else {
                if (issued - got >= c->rxdepth) break;
                cmd = CMD_READ;
                if (sent == wn && wn) cmd |= CMD_RESTART;
                issued++;
            }
            if (sent == total - 1) cmd |= CMD_STOP;
            wr(c, IC_DATA_CMD, cmd);
            sent++;
        }
        while (got < rn && rd(c, IC_RXFLR)) rbuf[got++] = (u8)rd(c, IC_DATA_CMD);
        raw = rd(c, IC_RAW_INTR);
        if (raw & INTR_TX_ABRT) {
            (void)rd(c, IC_CLR_TX_ABRT);
            return -1;
        }
        if (plat_ms() - t0 > 20) return -1;
    }
    while (!(rd(c, IC_RAW_INTR) & INTR_STOP_DET))
        if (plat_ms() - t0 > 20) return -1;
    if (rd(c, IC_RAW_INTR) & INTR_TX_ABRT) {
        (void)rd(c, IC_CLR_TX_ABRT);
        return -1;
    }
    return 0;
}
