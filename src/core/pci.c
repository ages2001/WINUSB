/* pci.c - PCI configuration mechanism #1 and bus scan */
#include "core.h"

static u32 cfgaddr(int bus, int dev, int fn, int off)
{
    return 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
}

u32 pci_rd32(int bus, int dev, int fn, int off)
{
    u32 f = irq_save(), v;
    outl(0xCF8, cfgaddr(bus, dev, fn, off));
    v = inl(0xCFC);
    irq_restore(f);
    return v;
}

void pci_wr32(int bus, int dev, int fn, int off, u32 v)
{
    u32 f = irq_save();
    outl(0xCF8, cfgaddr(bus, dev, fn, off));
    outl(0xCFC, v);
    irq_restore(f);
}

u16 pci_rd16(int bus, int dev, int fn, int off)
{
    return (u16)(pci_rd32(bus, dev, fn, off) >> ((off & 2) * 8));
}

u8 pci_rd8(int bus, int dev, int fn, int off)
{
    return (u8)(pci_rd32(bus, dev, fn, off) >> ((off & 3) * 8));
}

void pci_wr16(int bus, int dev, int fn, int off, u16 v)
{
    u32 f = irq_save();
    outl(0xCF8, cfgaddr(bus, dev, fn, off));
    outw((u16)(0xCFC + (off & 2)), v);
    irq_restore(f);
}

void pci_wr8(int bus, int dev, int fn, int off, u8 v)
{
    u32 f = irq_save();
    outl(0xCF8, cfgaddr(bus, dev, fn, off));
    outb((u16)(0xCFC + (off & 3)), v);
    irq_restore(f);
}

/* configuration mechanism #1 present (all PCI chipsets since the 430FX era) */
static int pci_present(void)
{
    u32 f = irq_save(), old, v;
    old = inl(0xCF8);
    outl(0xCF8, 0x80000000UL);
    v = inl(0xCF8);
    outl(0xCF8, old);
    irq_restore(f);
    return v == 0x80000000UL;
}

/* one function's identity: 0 if present */
int pci_fill(struct pcidev *out, int bus, int dev, int fn)
{
    u32 id = pci_rd32(bus, dev, fn, 0), cl;
    u8 ht;
    int i;
    if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) return -1;
    ht = pci_rd8(bus, dev, fn, 0x0E);
    cl = pci_rd32(bus, dev, fn, 8);
    out->bus = (u8)bus;
    out->dev = (u8)dev;
    out->fn = (u8)fn;
    out->vendor = (u16)id;
    out->device = (u16)(id >> 16);
    out->rev = (u8)cl;
    out->prog = (u8)(cl >> 8);
    out->sub = (u8)(cl >> 16);
    out->cls = (u8)(cl >> 24);
    out->irq = pci_rd8(bus, dev, fn, 0x3C);
    for (i = 0; i < 6; i++)
        out->bar[i] = (ht & 0x7F) == 0 ? pci_rd32(bus, dev, fn, 0x10 + i * 4) : 0;
    return 0;
}

int pci_scan(struct pcidev *out, int max)
{
    int n = 0, bus, dev, fn, i;
    if (!pci_present()) {
        kprintf("pci: no PCI configuration mechanism #1\n");
        return 0;
    }
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            int nfn = 1;
            for (fn = 0; fn < nfn; fn++) {
                u32 id = pci_rd32(bus, dev, fn, 0);
                u32 cl;
                u8 ht;
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                    if (fn == 0) break;     /* no function 0: no device */
                    continue;
                }
                ht = pci_rd8(bus, dev, fn, 0x0E);
                if (fn == 0 && (ht & 0x80)) nfn = 8;
                if (n >= max) continue;
                cl = pci_rd32(bus, dev, fn, 8);
                out[n].bus = (u8)bus;
                out[n].dev = (u8)dev;
                out[n].fn = (u8)fn;
                out[n].vendor = (u16)id;
                out[n].device = (u16)(id >> 16);
                out[n].rev = (u8)cl;
                out[n].prog = (u8)(cl >> 8);
                out[n].sub = (u8)(cl >> 16);
                out[n].cls = (u8)(cl >> 24);
                out[n].irq = pci_rd8(bus, dev, fn, 0x3C);
                for (i = 0; i < 6; i++)
                    out[n].bar[i] = (ht & 0x7F) == 0 ? pci_rd32(bus, dev, fn, 0x10 + i * 4) : 0;
                n++;
            }
        }
    }
    return n;
}

/* ---- 64-bit BARs above 4G: moved into a free part of the 32-bit MMIO hole ----
   Firmware on newer machines places 64-bit BARs above 4G (the Kaby Lake and later HD Audio /
   DSP controllers, NVMe, xHCI, Thunderbolt); a 32-bit protected-mode driver cannot reach
   them. The free space is found from the host bridge (TOLUD, PCIEXBAR and the fixed MCH
   ranges), every BAR, ROM and bridge window in the system, and the fixed chipset ranges. */
struct prange { u32 lo, hi; };          /* inclusive */
#define MAXRANGE 512
static struct prange used[MAXRANGE];
static int nused;

static void use(u32 lo, u32 size)
{
    if (!size || nused >= MAXRANGE) return;
    used[nused].lo = lo;
    used[nused].hi = lo + size - 1 < lo ? 0xFFFFFFFFUL : lo + size - 1;
    nused++;
}

/* size of the BAR at 'off' (decode off while probing, interrupts off) */
static u32 bar_size(int b, int d, int f, int off, int is64, int rom, u32 *hisz)
{
    u32 fl = irq_save(), lo, hi = 0, v, vh = 0;
    u16 cmd = pci_rd16(b, d, f, 4);
    pci_wr16(b, d, f, 4, (u16)(cmd & ~3U));
    lo = pci_rd32(b, d, f, off);
    if (is64) hi = pci_rd32(b, d, f, off + 4);
    pci_wr32(b, d, f, off, rom ? 0xFFFFF800UL : 0xFFFFFFFFUL);
    v = pci_rd32(b, d, f, off);
    if (is64) {
        pci_wr32(b, d, f, off + 4, 0xFFFFFFFFUL);
        vh = pci_rd32(b, d, f, off + 4);
        pci_wr32(b, d, f, off + 4, hi);
    }
    pci_wr32(b, d, f, off, lo);
    pci_wr16(b, d, f, 4, cmd);
    irq_restore(fl);
    if (hisz) *hisz = is64 ? ~vh : 0;   /* nonzero: the BAR is 4G or larger */
    v &= rom ? 0xFFFFF800UL : 0xFFFFFFF0UL;
    if (!v) return 0;
    return ~v + 1;
}

static void collect_function(int b, int d, int f, u8 ht)
{
    int i, nbar = (ht & 0x7F) == 0 ? 6 : (ht & 0x7F) == 1 ? 2 : 0;
    for (i = 0; i < nbar; i++) {
        int off = 0x10 + i * 4;
        u32 v = pci_rd32(b, d, f, off), hisz = 0, size;
        int is64 = !(v & 1) && (v & 6) == 4;
        if (v & 1) continue;                    /* I/O */
        if (is64 && pci_rd32(b, d, f, off + 4)) { i++; continue; }  /* above 4G: not in our way */
        /* also with decode off: its driver may turn it on later */
        if (v & ~15UL) {
            size = bar_size(b, d, f, off, is64, 0, &hisz);
            if (!hisz) use(v & ~15UL, size);
        }
        if (is64) i++;
    }
    {
        int roff = (ht & 0x7F) == 1 ? 0x38 : 0x30;
        u32 r = (ht & 0x7F) <= 1 ? pci_rd32(b, d, f, roff) : 0;
        if ((r & 1) && (r & 0xFFFFF800UL)) use(r & 0xFFFFF800UL, bar_size(b, d, f, roff, 0, 1, NULL));
    }
    if ((ht & 0x7F) == 1) {
        u32 w = pci_rd32(b, d, f, 0x20), lo, hi;
        lo = (w & 0xFFF0) << 16;
        hi = (w & 0xFFF00000UL) | 0xFFFFF;
        if (hi > lo) use(lo, hi - lo + 1);
        w = pci_rd32(b, d, f, 0x24);
        lo = (w & 0xFFF0) << 16;
        hi = (w & 0xFFF00000UL) | 0xFFFFF;
        if (hi > lo && !pci_rd32(b, d, f, 0x28)) use(lo, hi - lo + 1);
    }
}

/* ---- PCI Express extended configuration space (ACPI MCFG), for Resizable BAR ---- */
struct mcfgent { u32 base, basehi; u16 seg; u8 bus0, bus1; u32 rsv; };
static struct mcfgent mcfg[4];
static int nmcfg = -1;

static void mcfg_load(void)
{
    u8 *t;
    u32 len, i;
    if (nmcfg >= 0) return;
    nmcfg = 0;
    t = acpi_table("MCFG", 0);
    if (!t) return;
    len = *(u32 *)(t + 4);
    for (i = 44; i + 16 <= len && nmcfg < (int)ARRAY_SIZE(mcfg); i += 16) {
        memcpy(&mcfg[nmcfg], t + i, 16);
        if (!mcfg[nmcfg].basehi && !mcfg[nmcfg].seg && mcfg[nmcfg].bus1 >= mcfg[nmcfg].bus0) nmcfg++;
    }
}

static void mcfg_ranges(void)
{
    int i;
    mcfg_load();
    for (i = 0; i < nmcfg; i++)
        use(mcfg[i].base + ((u32)mcfg[i].bus0 << 20), ((u32)(mcfg[i].bus1 - mcfg[i].bus0) + 1) << 20);
}

/* the 4K extended configuration space of a function (mapped once), NULL without MCFG */
static struct { u16 bdf; u8 *p; } ecmap[16];
static int necmap;

static u8 *ecfg(int b, int d, int f)
{
    u16 bdf = (u16)((b << 8) | (d << 3) | f);
    int i;
    mcfg_load();
    for (i = 0; i < necmap; i++)
        if (ecmap[i].bdf == bdf) return ecmap[i].p;
    for (i = 0; i < nmcfg; i++) {
        u8 *p;
        if (b < mcfg[i].bus0 || b > mcfg[i].bus1) continue;
        if (necmap >= (int)ARRAY_SIZE(ecmap)) return NULL;
        p = plat_mmio(mcfg[i].base + ((u32)b << 20) + ((u32)d << 15) + ((u32)f << 12), 4096);
        if (!p || RD32(p) != pci_rd32(b, d, f, 0)) return NULL;
        ecmap[necmap].bdf = bdf;
        ecmap[necmap++].p = p;
        return p;
    }
    return NULL;
}

/* Resizable BAR extended capability: offset of the control register of BAR 'bar', 0 if none */
static u32 rebar_ctl(int b, int d, int f, int bar, u32 *caps)
{
    u8 *e = ecfg(b, d, f);
    u32 off = 0x100, guard = 64;
    if (!e) return 0;
    while (off >= 0x100 && off < 0xFFC && guard--) {
        u32 h = RD32(e + off);
        if (!h || h == 0xFFFFFFFFUL) return 0;
        if ((h & 0xFFFF) == 0x15) {
            u32 n = (RD32(e + off + 8) >> 5) & 7, i;
            for (i = 0; i < n && i < 6; i++)
                if ((RD32(e + off + 8 + i * 8) & 7) == (u32)bar) {
                    *caps = RD32(e + off + 4 + i * 8);
                    return off + 8 + i * 8;
                }
            return 0;
        }
        off = h >> 20;
    }
    return 0;
}

/* every MMIO range in use below 4G; holelo..holehi receives the PCI hole */
static void collect_used(int skipbus, int skipdev, int skipfn, u32 *holelo, u32 *holehi)
{
    int b, d, f;
    u32 id0 = pci_rd32(0, 0, 0, 0);
    nused = 0;
    *holelo = 0;
    *holehi = 0xFEC00000UL;
    use(0xFEC00000UL, 0x01400000UL);           /* IOAPIC, HPET, LAPIC, firmware flash */
    if ((id0 & 0xFFFF) == 0x8086 && pci_rd8(0, 0, 0, 0x0B) == 6 && (pci_rd32(0, 0, 0, 0x48) & 1) &&
        (pci_rd32(0, 0, 0, 0x48) & 0xFFFF8000UL) >= 0xFE000000UL && !pci_rd32(0, 0, 0, 0x4C)) {
        /* Core-family host bridge (Sandy Bridge and later): the firmware gives the PCI root
           bus [TOLUD, PCIEXBAR) and keeps the ranges above for the chipset */
        u32 tolud = pci_rd32(0, 0, 0, 0xBC) & 0xFFF00000UL;
        u32 pciex = pci_rd32(0, 0, 0, 0x60);
        static const u8 fixed[] = { 0x48, 0x50, 0x68, 0x78 };   /* MCHBAR GDXCBAR DMIBAR EDRAMBAR */
        int k;
        if (tolud >= 0x10000000UL && tolud < 0xF0000000UL) {
            *holelo = tolud;
            *holehi = 0xFD000000UL;
            if ((pciex & 1) && !pci_rd32(0, 0, 0, 0x64)) {
                u32 base = pciex & 0xFC000000UL;
                if (base > tolud) *holehi = base;
                else use(base, 0x10000000UL >> ((pciex >> 1) & 3));
            }
        }
        for (k = 0; k < (int)sizeof(fixed); k++) {
            u32 v = pci_rd32(0, 0, 0, fixed[k]);
            if ((v & 1) && !pci_rd32(0, 0, 0, fixed[k] + 4)) use(v & 0xFFFFF000UL, 0x10000UL);
        }
    }
    if (((id0 & 0xFFFF) == 0x1022 || (id0 & 0xFFFF) == 0x1D94) && (cpu_features() & 0x20)) {
        /* AMD (and Hygon) K8 and later: the PCI hole starts at TOP_MEM */
        u32 tom = cpu_rdmsr(0xC001001AUL) & 0xFF800000UL;
        if (tom >= 0x10000000UL && tom < 0xF0000000UL) *holelo = tom;
    }
    mcfg_ranges();
    for (b = 0; b < 256; b++)
        for (d = 0; d < 32; d++) {
            int nfn = 1;
            for (f = 0; f < nfn; f++) {
                u32 id = pci_rd32(b, d, f, 0);
                u8 ht;
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                    if (f == 0) break;
                    continue;
                }
                ht = pci_rd8(b, d, f, 0x0E);
                if (f == 0 && (ht & 0x80)) nfn = 8;
                if (b == skipbus && d == skipdev && f == skipfn) continue;
                collect_function(b, d, f, ht);
            }
        }
    if (!*holelo) {
        /* no Intel host bridge: the hole starts at the lowest range in use */
        int i;
        u32 m = 0xFEC00000UL;
        for (i = 0; i < nused; i++)
            if (used[i].lo >= 0x10000000UL && used[i].lo < m) m = used[i].lo;
        *holelo = m;
    }
}

/* the closest bridge whose secondary..subordinate covers 'bus': its 32-bit windows */
static int parent_window(int bus, u32 *lo, u32 *hi, u32 *plo, u32 *phi)
{
    int b, d, f, best = -1;
    *plo = 1;
    *phi = 0;
    for (b = 0; b < 256; b++)
        for (d = 0; d < 32; d++) {
            int nfn = 1;
            for (f = 0; f < nfn; f++) {
                u32 id = pci_rd32(b, d, f, 0), bn, w;
                u8 ht;
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                    if (f == 0) break;
                    continue;
                }
                ht = pci_rd8(b, d, f, 0x0E);
                if (f == 0 && (ht & 0x80)) nfn = 8;
                if ((ht & 0x7F) != 1) continue;
                bn = pci_rd32(b, d, f, 0x18);
                if ((int)((bn >> 8) & 0xFF) > bus || (int)((bn >> 16) & 0xFF) < bus) continue;
                if ((int)((bn >> 8) & 0xFF) <= best) continue;
                best = (int)((bn >> 8) & 0xFF);
                w = pci_rd32(b, d, f, 0x20);
                *lo = (w & 0xFFF0) << 16;
                *hi = (w & 0xFFF00000UL) | 0xFFFFF;
                w = pci_rd32(b, d, f, 0x24);
                if (!pci_rd32(b, d, f, 0x28)) {
                    *plo = (w & 0xFFF0) << 16;
                    *phi = (w & 0xFFF00000UL) | 0xFFFFF;
                } else {
                    *plo = 1;
                    *phi = 0;
                }
            }
        }
    return best;
}

static int overlaps(u32 lo, u32 hi)
{
    int i;
    for (i = 0; i < nused; i++)
        if (lo <= used[i].hi && hi >= used[i].lo) return 1;
    return 0;
}

/* highest free block of 'size' bytes in [lo, hi], aligned to 'align' (a power of two) */
static u32 find_gap_al(u32 lo, u32 hi, u32 size, u32 align)
{
    u32 a;
    if (!size || hi < lo || hi - lo + 1 < size) return 0;
    a = (hi - size + 1) & ~(align - 1);
    for (;;) {
        if (a < lo || !a) return 0;
        if (!overlaps(a, a + size - 1)) return a;
        if (a < align) return 0;
        a -= align;
    }
}

/* naturally aligned */
static u32 find_gap(u32 lo, u32 hi, u32 size)
{
    return find_gap_al(lo, hi, size, size);
}

static int pci_relocate(struct pcidev *p, int bar)
{
    int off = 0x10 + bar * 4, pbus;
    u32 hisz = 0, size, holelo, holehi, lo, hi, plo, phi, a = 0, fl;
    u16 cmd;
    size = bar_size(p->bus, p->dev, p->fn, off, 1, 0, &hisz);
    if (!size || hisz) {
        kprintf("pci %02x:%02x.%u BAR%d above 4G (%s), not usable\n", p->bus, p->dev, p->fn, bar,
                hisz ? "4G or larger" : "size unknown");
        return -1;
    }
    collect_used(p->bus, p->dev, p->fn, &holelo, &holehi);
    pbus = p->bus ? parent_window(p->bus, &lo, &hi, &plo, &phi) : -1;
    if (pbus < 0) {
        if (size < 0x1000) size = 0x1000;       /* keep other drivers' 4K mappings apart */
        a = find_gap(holelo, holehi - 1, size);
    } else {
        if (p->bar[bar] & 8) a = find_gap(plo, phi, size);
        if (!a) a = find_gap(lo, hi, size);
    }
    if (!a) {
        kprintf("pci %02x:%02x.%u BAR%d above 4G: no free %luK block below 4G\n", p->bus, p->dev, p->fn,
                bar, size >> 10);
        return -1;
    }
    fl = irq_save();
    cmd = pci_rd16(p->bus, p->dev, p->fn, 4);
    pci_wr16(p->bus, p->dev, p->fn, 4, (u16)(cmd & ~2U));
    pci_wr32(p->bus, p->dev, p->fn, off + 4, 0);
    pci_wr32(p->bus, p->dev, p->fn, off, a | (p->bar[bar] & 15));
    pci_wr16(p->bus, p->dev, p->fn, 4, cmd);
    irq_restore(fl);
    kprintf("pci %02x:%02x.%u BAR%d moved from %lx%08lx to %08lx (%luK)\n", p->bus, p->dev, p->fn, bar,
            p->bar[bar + 1], p->bar[bar] & ~15UL, a, size >> 10);
    p->bar[bar] = pci_rd32(p->bus, p->dev, p->fn, off);
    p->bar[bar + 1] = 0;
    return 0;
}

/* ---- 64-bit BARs and bridge windows above 4G, moved below 4G at the start ----
   Before Windows (and its PCI enumerator and display drivers) sees the machine, the root
   bus BARs above 4G get free blocks of the PCI hole; a bridge whose prefetchable window is
   above 4G gets its whole subtree laid out again below 4G (nested windows and the BARs
   behind them, largest first). A BAR too large for the hole is made smaller through the
   Resizable BAR capability when the device has one. By default only the devices WINDRV
   drives are moved (and a bridge window only with one of them behind it): graphics cards
   and the other devices stay where the firmware put them. */
#define MAXRITEM 96
struct ritem {
    u8 bus, dev, fn;
    u8 kind;                    /* RI_BAR, RI_WIN */
    u8 off;                     /* BAR offset in configuration space */
    u8 skip;
    s16 parent;                 /* bridge item, -1 for the root bus */
    u32 size, align, rel, addr;
    u32 rbctl, rbcaps;          /* Resizable BAR control register (extended space), sizes */
    u32 oldlo, oldhi;
    u8 drv;                     /* a function WINDRV drives */
};
#define RI_BAR 0
#define RI_WIN 1
static struct ritem ri[MAXRITEM];
static int nri;

static int win64_above(int b, int d, int f, u32 *lo, u32 *hi)
{
    u32 w = pci_rd32(b, d, f, 0x24), bu, lu;
    if ((w & 0xF) != 1) return 0;                       /* not a 64-bit prefetchable window */
    bu = pci_rd32(b, d, f, 0x28);
    lu = pci_rd32(b, d, f, 0x2C);
    *lo = (w & 0xFFF0) << 16;
    *hi = (w & 0xFFF00000UL) | 0xFFFFF;
    if (lu < bu || (lu == bu && *hi < *lo)) return 0;   /* window off */
    return bu != 0;
}

/* the functions WINDRV drives: USB host controllers, audio (HD Audio, AC'97, Intel DSP), mass
   storage (IDE, SATA, RAID, NVMe), SD hosts, Intel LPSS I2C and Thunderbolt host interfaces */
static int drv_class(int b, int d, int f)
{
    u32 cls = pci_rd32(b, d, f, 8) >> 8, vid = pci_rd32(b, d, f, 0) & 0xFFFF;
    switch (cls >> 8) {
    case 0x0C03: case 0x0403: case 0x0401: case 0x0805:
        return 1;
    case 0x0480: case 0x0C80: case 0x1180: case 0x0880:
        return vid == 0x8086;
    }
    return (cls >> 16) == 0x01;
}

static int add_item(int b, int d, int f, int kind, int off, int parent)
{
    struct ritem *r;
    if (nri >= MAXRITEM) return -1;
    r = &ri[nri];
    memset(r, 0, sizeof(*r));
    r->bus = (u8)b;
    r->dev = (u8)d;
    r->fn = (u8)f;
    r->kind = (u8)kind;
    r->off = (u8)off;
    r->parent = (s16)parent;
    r->drv = (u8)drv_class(b, d, f);
    return nri++;
}

/* the 64-bit BARs above 4G of one function; a bridge with its window above 4G: its bus too */
static void collect_bus(int bus, int parent, int depth);

static void collect_fn(int b, int d, int f, u8 ht, int parent, int depth)
{
    int i, nbar = (ht & 0x7F) == 0 ? 6 : (ht & 0x7F) == 1 ? 2 : 0;
    u32 wlo, whi;
    for (i = 0; i < nbar; i++) {
        int off = 0x10 + i * 4, k;
        u32 v = pci_rd32(b, d, f, off), hi, hisz;
        if (v & 1) continue;
        if ((v & 6) != 4 || i == nbar - 1) continue;
        hi = pci_rd32(b, d, f, off + 4);
        if (hi) {
            k = add_item(b, d, f, RI_BAR, off, parent);
            if (k >= 0) {
                struct ritem *r = &ri[k];
                r->oldlo = v;
                r->oldhi = hi;
                r->size = bar_size(b, d, f, off, 1, 0, &hisz);
                if (hisz) r->size = 0;                  /* 4G or more */
                r->rbctl = rebar_ctl(b, d, f, i, &r->rbcaps);
                if (!r->size && r->rbctl) {
                    /* start from the largest size below 4G the BAR supports */
                    int c;
                    for (c = 11; c >= 0 && !(r->rbcaps & (0x10UL << c)); c--) ;
                    r->size = c >= 0 ? 0x100000UL << c : 0;
                }
                if (!r->size) r->skip = 1;
                r->align = r->size;
            }
        }
        i++;
    }
    if ((ht & 0x7F) == 1 && depth < 8 && win64_above(b, d, f, &wlo, &whi)) {
        int k = add_item(b, d, f, RI_WIN, 0x24, parent);
        if (k >= 0) {
            ri[k].oldlo = pci_rd32(b, d, f, 0x24);
            ri[k].oldhi = pci_rd32(b, d, f, 0x28);
            collect_bus((int)((pci_rd32(b, d, f, 0x18) >> 8) & 0xFF), k, depth + 1);
        }
    }
}

static void collect_bus(int bus, int parent, int depth)
{
    int d, f;
    for (d = 0; d < 32; d++) {
        int nfn = 1;
        for (f = 0; f < nfn; f++) {
            u32 id = pci_rd32(bus, d, f, 0);
            u8 ht;
            if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                if (f == 0) break;
                continue;
            }
            ht = pci_rd8(bus, d, f, 0x0E);
            if (f == 0 && (ht & 0x80)) nfn = 8;
            collect_fn(bus, d, f, ht, parent, depth);
        }
    }
}

/* size and alignment of a window: its children packed largest alignment first */
static void calc(int k)
{
    struct ritem *w = &ri[k];
    u32 off = 0, maxal = 0x100000UL;
    int i, any = 0;
    if (w->kind != RI_WIN) return;
    for (i = 0; i < nri; i++)
        if (ri[i].parent == k && !ri[i].skip) calc(i);
    for (i = 0; i < nri; i++)
        if (ri[i].parent == k) ri[i].rel = 0xFFFFFFFFUL;
    for (;;) {
        int best = -1;
        for (i = 0; i < nri; i++)
            if (ri[i].parent == k && !ri[i].skip && ri[i].rel == 0xFFFFFFFFUL &&
                (best < 0 || ri[i].align > ri[best].align))
                best = i;
        if (best < 0) break;
        off = ALIGN_UP(off, ri[best].align);
        ri[best].rel = off;
        off += ri[best].size;
        if (ri[best].align > maxal) maxal = ri[best].align;
        any = 1;
    }
    w->size = any ? ALIGN_UP(off, 0x100000UL) : 0;
    w->align = maxal;
    if (!any) w->skip = 1;                  /* nothing behind it above 4G: left alone */
}

static void assign(int k, u32 base)
{
    int i;
    ri[k].addr = base;
    if (ri[k].kind != RI_WIN) return;
    for (i = 0; i < nri; i++)
        if (ri[i].parent == k && !ri[i].skip) assign(i, base + ri[i].rel);
}

static int in_group(int i, int top)
{
    while (i >= 0 && i != top) i = ri[i].parent;
    return i == top;
}

/* the largest Resizable BAR of a group one size smaller; 0 if none can shrink */
static int shrink(int top)
{
    int i, best = -1;
    for (i = 0; i < nri; i++) {
        u32 code;
        if (ri[i].kind != RI_BAR || ri[i].skip || !ri[i].rbctl || !in_group(i, top)) continue;
        for (code = 0; code < 20 && (0x100000UL << code) < ri[i].size; code++) ;
        while (code > 0 && !(ri[i].rbcaps & (0x10UL << (code - 1)))) code--;
        if (!code) continue;
        if (best < 0 || ri[i].size > ri[best].size) best = i;
    }
    if (best < 0) return 0;
    {
        u32 code;
        for (code = 0; (0x100000UL << code) < ri[best].size; code++) ;
        do code--; while (!(ri[best].rbcaps & (0x10UL << code)));
        ri[best].size = ri[best].align = 0x100000UL << code;
    }
    return 1;
}

static void write_group(int top)
{
    u16 bdfs[MAXRITEM], cmds[MAXRITEM];
    int n = 0, i, j;
    u32 fl = irq_save();
    /* decode off on every function of the group (bridges first in the list: parents) */
    for (i = 0; i < nri; i++) {
        u16 bdf;
        if (!in_group(i, top) || ri[i].skip) continue;
        bdf = (u16)((ri[i].bus << 8) | (ri[i].dev << 3) | ri[i].fn);
        for (j = 0; j < n && bdfs[j] != bdf; j++) ;
        if (j < n) continue;
        bdfs[n] = bdf;
        cmds[n] = pci_rd16(ri[i].bus, ri[i].dev, ri[i].fn, 4);
        pci_wr16(ri[i].bus, ri[i].dev, ri[i].fn, 4, (u16)(cmds[n] & ~2U));
        n++;
    }
    for (i = 0; i < nri; i++) {
        struct ritem *r = &ri[i];
        if (!in_group(i, top) || r->skip) continue;
        if (r->kind == RI_WIN) {
            u32 v = (r->oldlo & 0x000F000FUL) | ((r->addr >> 16) & 0xFFF0) | ((r->addr + r->size - 1) & 0xFFF00000UL);
            pci_wr32(r->bus, r->dev, r->fn, 0x28, 0);
            pci_wr32(r->bus, r->dev, r->fn, 0x2C, 0);
            pci_wr32(r->bus, r->dev, r->fn, 0x24, v);
        } else {
            if (r->rbctl) {
                u8 *e = ecfg(r->bus, r->dev, r->fn);
                u32 code, c;
                for (code = 0; (0x100000UL << code) < r->size; code++) ;
                c = RD32(e + r->rbctl);
                if (((c >> 8) & 0x3F) != code) WR32(e + r->rbctl, (c & ~0x3F00UL) | (code << 8));
            }
            pci_wr32(r->bus, r->dev, r->fn, r->off, r->addr | (r->oldlo & 15));
            pci_wr32(r->bus, r->dev, r->fn, r->off + 4, 0);
        }
    }
    for (i = n - 1; i >= 0; i--) pci_wr16(bdfs[i] >> 8, (bdfs[i] >> 3) & 31, bdfs[i] & 7, 4, cmds[i]);
    irq_restore(fl);
}

/* 1 if the group of item k has a function WINDRV drives */
static int group_drv(int k)
{
    int i;
    for (i = 0; i < nri; i++)
        if (ri[i].drv && !ri[i].skip && in_group(i, k)) return 1;
    return 0;
}

void pci_relocate_all(int all)
{
    u8 rootbus[256];
    u32 holelo, holehi;
    int b, d, f, i, moved = 0, failed = 0;
    if (!pci_present()) return;
    /* root buses: not behind any bridge */
    memset(rootbus, 1, sizeof(rootbus));
    for (b = 0; b < 256; b++)
        for (d = 0; d < 32; d++) {
            int nfn = 1;
            for (f = 0; f < nfn; f++) {
                u32 id = pci_rd32(b, d, f, 0), bn;
                u8 ht;
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
                    if (f == 0) break;
                    continue;
                }
                ht = pci_rd8(b, d, f, 0x0E);
                if (f == 0 && (ht & 0x80)) nfn = 8;
                if ((ht & 0x7F) != 1 && (ht & 0x7F) != 2) continue;
                bn = pci_rd32(b, d, f, 0x18);
                for (i = (int)((bn >> 8) & 0xFF); i <= (int)((bn >> 16) & 0xFF) && i > 0; i++) rootbus[i] = 0;
            }
        }
    nri = 0;
    for (b = 0; b < 256; b++)
        if (rootbus[b]) collect_bus(b, -1, 0);
    if (!nri) return;
    for (i = 0; i < nri; i++)
        if (ri[i].parent < 0 && ri[i].kind == RI_WIN) calc(i);
    for (i = 0; !all && i < nri; i++)
        if (ri[i].parent < 0 && !ri[i].skip && !group_drv(i)) {
            kprintf("pci %02x:%02x.%u %s above 4G left in place (not a WINDRV device; /ALL4G moves it)\n",
                    ri[i].bus, ri[i].dev, ri[i].fn, ri[i].kind == RI_WIN ? "window" : "BAR");
            ri[i].skip = 1;
        }
    collect_used(-1, -1, -1, &holelo, &holehi);
    /* root items, largest first */
    for (;;) {
        int top = -1, tries;
        u32 a = 0;
        for (i = 0; i < nri; i++)
            if (ri[i].parent < 0 && !ri[i].skip && !ri[i].addr && (top < 0 || ri[i].size > ri[top].size)) top = i;
        if (top < 0) break;
        for (tries = 0; tries < 64; tries++) {
            u32 size = ri[top].size, al = ri[top].align;
            if (ri[top].kind == RI_BAR && size < 0x1000) size = al = 0x1000;   /* own page */
            a = find_gap_al(holelo, holehi - 1, size, al);
            if (a || !shrink(top)) break;
            if (ri[top].kind == RI_WIN) calc(top);
        }
        if (!a) {
            kprintf("pci %02x:%02x.%u %s above 4G (%luM): no room below 4G, left in place\n", ri[top].bus,
                    ri[top].dev, ri[top].fn, ri[top].kind == RI_WIN ? "prefetchable window" : "BAR",
                    ri[top].size >> 20);
            ri[top].skip = 1;
            failed++;
            continue;
        }
        assign(top, a);
        write_group(top);
        use(a, ri[top].kind == RI_BAR && ri[top].size < 0x1000 ? 0x1000 : ri[top].size);
        for (i = 0; i < nri; i++) {
            struct ritem *r = &ri[i];
            if (!in_group(i, top) || r->skip) continue;
            moved++;
            kprintf("pci %02x:%02x.%u %s %lx%08lx -> %08lx (%luK)\n", r->bus, r->dev, r->fn,
                    r->kind == RI_WIN ? "window" : "BAR", r->oldhi,
                    r->kind == RI_WIN ? (r->oldlo & 0xFFF0) << 16 : r->oldlo & ~15UL, r->addr, r->size >> 10);
        }
    }
    if (moved || failed)
        kprintf("pci: %d resource(s) above 4G moved below 4G%s\n", moved,
                failed ? ", some did not fit (disable \"Above 4G Decoding\" in the firmware setup)" : "");
}

/* returns physical address of a memory BAR; a 64-bit BAR above 4G is moved below 4G */
u32 pci_bar_mem(struct pcidev *p, int bar, u32 *size)
{
    u32 v = p->bar[bar];
    if (size) *size = 0;
    if (v & 1) return 0;
    if ((v & 6) == 4 && bar < 5 && p->bar[bar + 1] != 0) {
        if (pci_relocate(p, bar)) return 0;
        v = p->bar[bar];
    }
    return v & ~15UL;
}

u32 pci_bar_io(struct pcidev *p, int bar)
{
    u32 v = p->bar[bar];
    if (!(v & 1)) return 0;
    return v & 0xFFFC;
}

/* PCI command register bit 10: the function may (on = 1) or may not assert INTx */
void pci_intx(struct pcidev *p, int on)
{
    u16 c = pci_rd16(p->bus, p->dev, p->fn, 4);
    if (on) c &= ~0x0400; else c |= 0x0400;
    pci_wr16(p->bus, p->dev, p->fn, 4, c);
}

/* capability list: offset of capability 'id', 0 if none */
int pci_find_cap(struct pcidev *p, u8 id)
{
    int guard = 48;
    u8 off;
    if (!(pci_rd16(p->bus, p->dev, p->fn, 6) & 0x10)) return 0;
    off = (u8)(pci_rd8(p->bus, p->dev, p->fn, 0x34) & 0xFC);
    while (off >= 0x40 && guard--) {
        if (pci_rd8(p->bus, p->dev, p->fn, off) == id) return off;
        off = (u8)(pci_rd8(p->bus, p->dev, p->fn, off + 1) & 0xFC);
    }
    return 0;
}

/* Power state D0, MSI and MSI-X off (the drivers use INTx or polling). A function the
   firmware left in D3hot may have lost its BARs: they are written back. */
static void pci_wake(struct pcidev *p)
{
    int pm = pci_find_cap(p, 0x01), c, i;
    if (pm) {
        u16 csr = pci_rd16(p->bus, p->dev, p->fn, pm + 4);
        if (csr & 3) {
            kprintf("pci %02x:%02x.%u: power state D%u -> D0\n", p->bus, p->dev, p->fn, csr & 3);
            pci_wr16(p->bus, p->dev, p->fn, pm + 4, (u16)(csr & ~3U));
            mdelay(10);
            for (i = 0; i < 6; i++)
                if (p->bar[i]) pci_wr32(p->bus, p->dev, p->fn, 0x10 + i * 4, p->bar[i]);
            pci_wr8(p->bus, p->dev, p->fn, 0x3C, p->irq);
        }
    }
    c = pci_find_cap(p, 0x05);
    if (c) {
        u16 ctl = pci_rd16(p->bus, p->dev, p->fn, c + 2);
        if (ctl & 1) pci_wr16(p->bus, p->dev, p->fn, c + 2, (u16)(ctl & ~1U));
    }
    c = pci_find_cap(p, 0x11);
    if (c) {
        u16 ctl = pci_rd16(p->bus, p->dev, p->fn, c + 2);
        if (ctl & 0x8000) pci_wr16(p->bus, p->dev, p->fn, c + 2, (u16)(ctl & ~0x8000U));
    }
}

void pci_enable(struct pcidev *p, int mem, int io, int master)
{
    u16 c;
    pci_wake(p);
    c = pci_rd16(p->bus, p->dev, p->fn, 4);
    if (mem) c |= 2;
    if (io) c |= 1;
    if (master) c |= 4;
    c &= ~0x0400;   /* INTx enabled */
    pci_wr16(p->bus, p->dev, p->fn, 4, c);
}
