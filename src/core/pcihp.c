/* pcihp.c - PCI hot plug behind Thunderbolt / USB4 tunnels. DOS has no PCI hot plug
   service, so the functions that appear behind a tunnel get their bus numbers, bridge
   windows and BARs from the ranges the BIOS reserved on the hot plug capable ports, then
   NVMe controllers and xHCI controllers are started; functions that vanish are removed. */
#include "tbt.h"

int nvme_probe_hot(struct pcidev *pd, int removable);
void nvme_remove(u32 bdf);
int usb_hotplug_hc(struct pcidev *p);
void usb_hotunplug_hc(u32 bdf);

#define MAXKNOWN    256
#define MAXBRIDGES  64
#define MAXHOT      32

struct bridge {
    u8 bus, dev, fn;
    u8 sec, sub;
    u8 hotplug;         /* slot implemented and hot plug capable */
    u8 ptype;           /* PCIe port type: 4 root, 5 upstream, 6 downstream */
    u8 cap;             /* PCIe capability offset */
    u8 linked;          /* link was up at the last check */
};

struct hotdev {
    u8 used, kind;      /* HK_* */
    u8 boot;            /* present at boot (enumerated by the BIOS) */
    u16 bdf;
    struct pcidev pd;
};
#define HK_OTHER    0
#define HK_NVME     1
#define HK_XHCI     2
#define HK_BRIDGE   3

static u16 known[MAXKNOWN];
static int nknown;
static struct bridge bridges[MAXBRIDGES];
static int nbridges;
static struct hotdev hot[MAXHOT];
static int rescan_pending, rescan_tries;
static u32 rescan_at, next_removal;
static int booted, active;

static u32 bdf(int bus, int dev, int fn) { return ((u32)bus << 8) | ((u32)dev << 3) | (u32)fn; }

static int is_known(u32 b)
{
    int i;
    for (i = 0; i < nknown; i++) if (known[i] == b) return 1;
    return 0;
}

static struct hotdev *hot_find(u32 b)
{
    int i;
    for (i = 0; i < MAXHOT; i++) if (hot[i].used && hot[i].bdf == b) return &hot[i];
    return NULL;
}

static struct hotdev *hot_add(u32 b, int kind, int boot)
{
    int i;
    struct hotdev *h = hot_find(b);
    if (h) return h;
    for (i = 0; i < MAXHOT; i++)
        if (!hot[i].used) {
            memset(&hot[i], 0, sizeof(hot[i]));
            hot[i].used = 1;
            hot[i].bdf = (u16)b;
            hot[i].kind = (u8)kind;
            hot[i].boot = (u8)boot;
            return &hot[i];
        }
    return NULL;
}

static int pcie_cap(int bus, int dev, int fn)
{
    struct pcidev p;
    memset(&p, 0, sizeof(p));
    p.bus = (u8)bus; p.dev = (u8)dev; p.fn = (u8)fn;
    return pci_find_cap(&p, 0x10);
}

static void fill_pcidev(struct pcidev *p, int bus, int dev, int fn)
{
    u32 id = pci_rd32(bus, dev, fn, 0), cl = pci_rd32(bus, dev, fn, 8);
    u8 ht = pci_rd8(bus, dev, fn, 0x0E);
    int i;
    memset(p, 0, sizeof(*p));
    p->bus = (u8)bus; p->dev = (u8)dev; p->fn = (u8)fn;
    p->vendor = (u16)id;
    p->device = (u16)(id >> 16);
    p->rev = (u8)cl;
    p->prog = (u8)(cl >> 8);
    p->sub = (u8)(cl >> 16);
    p->cls = (u8)(cl >> 24);
    p->irq = pci_rd8(bus, dev, fn, 0x3C);
    for (i = 0; i < 6; i++)
        p->bar[i] = (ht & 0x7F) == 0 ? pci_rd32(bus, dev, fn, 0x10 + i * 4) : 0;
}

static struct bridge *bridge_add(int bus, int dev, int fn)
{
    struct bridge *b;
    int cap, i;
    for (i = 0; i < nbridges; i++)
        if (bridges[i].bus == bus && bridges[i].dev == dev && bridges[i].fn == fn) return &bridges[i];
    if (nbridges >= MAXBRIDGES) return NULL;
    b = &bridges[nbridges++];
    memset(b, 0, sizeof(*b));
    b->bus = (u8)bus; b->dev = (u8)dev; b->fn = (u8)fn;
    b->sec = pci_rd8(bus, dev, fn, 0x19);
    b->sub = pci_rd8(bus, dev, fn, 0x1A);
    cap = pcie_cap(bus, dev, fn);
    if (cap) {
        u16 flags = pci_rd16(bus, dev, fn, cap + 2);
        b->cap = (u8)cap;
        b->ptype = (u8)((flags >> 4) & 0xF);
        if ((flags & 0x100) && (pci_rd32(bus, dev, fn, cap + 0x14) & 0x40)) b->hotplug = 1;
    }
    return b;
}

static struct bridge *bridge_of_bus(int bus)
{
    int i;
    for (i = 0; i < nbridges; i++)
        if (bridges[i].sec == bus && bridges[i].sec) return &bridges[i];
    return NULL;
}

/* a hot plug capable PCIe port (root or downstream) is somewhere above this bus */
int pcihp_behind_hotplug(int bus)
{
    int guard = 8;
    while (bus && guard--) {
        struct bridge *b = bridge_of_bus(bus);
        if (!b) return 0;
        if (b->hotplug && (b->ptype == 4 || b->ptype == 6)) return 1;
        bus = b->bus;
    }
    return 0;
}

int pcihp_is_tbt_device(struct pcidev *p)
{
    return pcihp_behind_hotplug(p->bus);
}

void pcihp_init(struct pcidev *list, int n)
{
    int i;
    if (active) return;             /* resume after Windows: keep what was plugged meanwhile */
    nknown = 0;
    nbridges = 0;
    active = 1;
    memset(hot, 0, sizeof(hot));
    for (i = 0; i < n && nknown < MAXKNOWN; i++) {
        struct pcidev *p = &list[i];
        u8 ht = pci_rd8(p->bus, p->dev, p->fn, 0x0E);
        known[nknown++] = (u16)PCI_BDF(p);
        if ((ht & 0x7F) == 1) bridge_add(p->bus, p->dev, p->fn);
    }
    DBG("pcihp: %d functions, %d bridges at boot\n", nknown, nbridges);
}

/* ---- resource assignment for new functions below a hot plug port ---- */
struct window {
    u32 base, limit;    /* memory window of the port, inclusive */
    u32 cur;            /* next free address */
};

static int fn_count(int bus, int dev)
{
    u32 id = pci_rd32(bus, dev, 0, 0);
    if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) return 0;
    return (pci_rd8(bus, dev, 0, 0x0E) & 0x80) ? 8 : 1;
}

static int fn_present(int bus, int dev, int fn)
{
    u32 id = pci_rd32(bus, dev, fn, 0);
    return (id & 0xFFFF) != 0xFFFF && (id & 0xFFFF) != 0;
}

static int is_new(int bus, int dev, int fn)
{
    u32 b = bdf(bus, dev, fn);
    return !is_known(b) && !hot_find(b);
}

/* bus numbers for the new bridges below 'bus'; returns the highest bus number used */
static int assign_buses(int bus, int next, int max)
{
    int dev, fn, nfn, high = bus;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++) {
            u8 ht;
            if (!fn_present(bus, dev, fn)) continue;
            ht = pci_rd8(bus, dev, fn, 0x0E);
            if ((ht & 0x7F) != 1) continue;
            if (!is_new(bus, dev, fn)) {
                int sub = pci_rd8(bus, dev, fn, 0x1A);
                if (sub > high) high = sub;
                continue;
            }
            if (next > max) {
                kprintf("pcihp: no bus numbers left below bus %02x\n", bus);
                return high;
            }
            pci_wr8(bus, dev, fn, 0x18, (u8)bus);
            pci_wr8(bus, dev, fn, 0x19, (u8)next);
            pci_wr8(bus, dev, fn, 0x1A, (u8)max);
            {
                int sub = assign_buses(next, next + 1, max);
                pci_wr8(bus, dev, fn, 0x1A, (u8)sub);
                if (sub > high) high = sub;
                if (sub + 1 > next) next = sub + 1;
            }
        }
    }
    return high;
}

/* size of a memory BAR; 0 = none / I/O / unusable. *flags gets the low BAR bits */
static u32 bar_size(int bus, int dev, int fn, int i, u32 *flags, int *is64)
{
    u32 off = 0x10 + (u32)i * 4, orig, sz, orighi = 0, szhi = 0xFFFFFFFFUL;
    orig = pci_rd32(bus, dev, fn, (int)off);
    *flags = orig & 0xF;
    *is64 = 0;
    if (orig & 1) return 0;
    pci_wr32(bus, dev, fn, (int)off, 0xFFFFFFFFUL);
    sz = pci_rd32(bus, dev, fn, (int)off);
    pci_wr32(bus, dev, fn, (int)off, orig);
    if ((orig & 6) == 4 && i < 5) {
        *is64 = 1;
        orighi = pci_rd32(bus, dev, fn, (int)off + 4);
        pci_wr32(bus, dev, fn, (int)off + 4, 0xFFFFFFFFUL);
        szhi = pci_rd32(bus, dev, fn, (int)off + 4);
        pci_wr32(bus, dev, fn, (int)off + 4, orighi);
    }
    sz &= ~0xFUL;
    if (!sz) return 0;
    if (*is64 && szhi != 0xFFFFFFFFUL) return 0;       /* 4 GB or more */
    return ~sz + 1;
}

static u32 align_up(u32 v, u32 a)
{
    return (v + a - 1) & ~(a - 1);
}

/* memory needed by the new functions on 'bus' and below, 1 MB granularity per bridge */
static u32 size_bus(int bus)
{
    int dev, fn, nfn, i, is64;
    u32 total = 0, flags, sz;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++) {
            u8 ht;
            if (!fn_present(bus, dev, fn) || !is_new(bus, dev, fn)) continue;
            ht = pci_rd8(bus, dev, fn, 0x0E);
            if ((ht & 0x7F) == 1) {
                u32 need = size_bus(pci_rd8(bus, dev, fn, 0x19));
                total = align_up(total, 0x100000UL) + align_up(need, 0x100000UL);
                continue;
            }
            if ((ht & 0x7F) != 0) continue;
            for (i = 0; i < 6; i++) {
                sz = bar_size(bus, dev, fn, i, &flags, &is64);
                if (sz) total = align_up(total, sz) + sz;
                if (is64) i++;
            }
        }
    }
    return total;
}

static void assign_bus(int bus, struct window *w, int boot);

static void assign_function(int bus, int dev, int fn, struct window *w)
{
    int i, is64;
    u32 flags, sz, addr;
    for (i = 0; i < 6; i++) {
        sz = bar_size(bus, dev, fn, i, &flags, &is64);
        if (sz) {
            addr = align_up(w->cur, sz < 4096 ? 4096 : sz);
            if (addr + sz - 1 > w->limit || addr + sz < addr) {
                kprintf("pcihp: %02x:%02x.%u BAR%d (%lu KB) does not fit the window\n", bus, dev, fn, i, sz >> 10);
                pci_wr32(bus, dev, fn, 0x10 + i * 4, 0);
            } else {
                pci_wr32(bus, dev, fn, 0x10 + i * 4, addr | flags);
                w->cur = addr + sz;
            }
            if (is64) pci_wr32(bus, dev, fn, 0x14 + i * 4, 0);
        }
        if (is64) i++;
    }
    pci_wr16(bus, dev, fn, 4, (u16)(pci_rd16(bus, dev, fn, 4) | 6));
}

static void assign_bridge(int bus, int dev, int fn, struct window *w, int boot)
{
    int sec = pci_rd8(bus, dev, fn, 0x19);
    u32 need = align_up(size_bus(sec), 0x100000UL), base, limit;
    struct window cw;
    pci_wr16(bus, dev, fn, 0x1C, 0x00F0);               /* no I/O window */
    pci_wr32(bus, dev, fn, 0x24, 0x0000FFF0UL);         /* no prefetchable window */
    pci_wr32(bus, dev, fn, 0x28, 0);
    pci_wr32(bus, dev, fn, 0x2C, 0);
    if (need) {
        base = align_up(w->cur, 0x100000UL);
        limit = base + need - 1;
        if (limit > w->limit || limit < base) {
            kprintf("pcihp: bridge %02x:%02x.%u needs %lu MB, window full\n", bus, dev, fn, need >> 20);
            need = 0;
        }
    }
    if (need) {
        pci_wr32(bus, dev, fn, 0x20, ((base >> 16) & 0xFFF0) | (limit & 0xFFF00000UL));
        w->cur = limit + 1;
        cw.base = base; cw.limit = limit; cw.cur = base;
    } else {
        pci_wr32(bus, dev, fn, 0x20, 0x0000FFF0UL);     /* no memory window */
        cw.base = 1; cw.limit = 0; cw.cur = 1;
    }
    pci_wr16(bus, dev, fn, 0x3E, (u16)(pci_rd16(bus, dev, fn, 0x3E) & ~0x40));   /* no bus reset */
    pci_wr16(bus, dev, fn, 4, (u16)(pci_rd16(bus, dev, fn, 4) | 6));
    bridge_add(bus, dev, fn);
    hot_add(bdf(bus, dev, fn), HK_BRIDGE, boot);
    assign_bus(sec, &cw, boot);
}

static void start_function(int bus, int dev, int fn, int boot)
{
    struct hotdev *h;
    struct pcidev pd;
    fill_pcidev(&pd, bus, dev, fn);
    if (pd.cls == 1 && pd.sub == 8 && pd.prog == 2) {
        if (cb->opts & OPT_NONVME) return;
        h = hot_add(bdf(bus, dev, fn), HK_NVME, boot);
        if (!h) return;
        h->pd = pd;
        kprintf("pcihp: NVMe controller %02x:%02x.%u %04x:%04x%s\n", bus, dev, fn, pd.vendor, pd.device,
                boot ? " behind a hot plug port" : " hot plugged");
        /* a disk that can be unplugged: slot drive letters, in DOS and in the VxD alike */
        if (nvme_probe_hot(&pd, 1)) h->used = 0;
        else if (!boot) plat_kick();
        return;
    }
    if (pd.cls == 0x0C && pd.sub == 0x03 && pd.prog == 0x30) {
        h = hot_add(bdf(bus, dev, fn), HK_XHCI, boot);
        if (!h) return;
        h->pd = pd;
        if (boot) return;                   /* usb_start_all already drives it */
        kprintf("pcihp: xHCI controller %02x:%02x.%u %04x:%04x hot plugged\n", bus, dev, fn, pd.vendor, pd.device);
        if (usb_hotplug_hc(&pd)) h->used = 0;
        return;
    }
    if (boot) return;
    h = hot_add(bdf(bus, dev, fn), HK_OTHER, boot);
    if (h) h->pd = pd;
    DBG("pcihp: %02x:%02x.%u %04x:%04x class %02x%02x%02x not used\n", bus, dev, fn, pd.vendor, pd.device,
        pd.cls, pd.sub, pd.prog);
}

/* assign resources to the new functions on 'bus' (window of the parent bridge), start them */
static void assign_bus(int bus, struct window *w, int boot)
{
    int dev, fn, nfn;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++) {
            u8 ht;
            if (!fn_present(bus, dev, fn) || !is_new(bus, dev, fn)) continue;
            ht = pci_rd8(bus, dev, fn, 0x0E);
            if ((ht & 0x7F) == 1) assign_bridge(bus, dev, fn, w, boot);
            else if ((ht & 0x7F) == 0) {
                assign_function(bus, dev, fn, w);
                start_function(bus, dev, fn, boot);
            }
        }
    }
}

/* addresses already in use below a port by functions the BIOS configured */
static u32 used_top(int bus, u32 base, u32 limit)
{
    int dev, fn, nfn, i;
    u32 top = base;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++) {
            u8 ht;
            if (!fn_present(bus, dev, fn) || is_new(bus, dev, fn)) continue;
            ht = pci_rd8(bus, dev, fn, 0x0E);
            if ((ht & 0x7F) == 1) {
                u32 v = pci_rd32(bus, dev, fn, 0x20), b = (v & 0xFFF0) << 16, l = (v & 0xFFF00000UL) | 0xFFFFF;
                if (b <= l && b >= base && l <= limit && l + 1 > top) top = l + 1;
            } else if ((ht & 0x7F) == 0) {
                for (i = 0; i < 6; i++) {
                    u32 v = pci_rd32(bus, dev, fn, 0x10 + i * 4), a = v & ~0xFUL;
                    if ((v & 1) || !a) continue;
                    if (a >= base && a <= limit && a + 0x100000UL > top) top = a + 0x100000UL;  /* assume <= 1 MB */
                    if ((v & 6) == 4) i++;
                }
            }
        }
    }
    return top;
}

/* link of a hot plug port: data link layer active, or (ports that do not report it, and
   firmware hot plug that leaves the bit alone) a function answering on the secondary bus */
static int link_up(struct bridge *b)
{
    u32 lcap, lsts;
    if (fn_present(b->sec, 0, 0)) return 1;
    if (!b->cap) return 0;
    lcap = pci_rd32(b->bus, b->dev, b->fn, b->cap + 0x0C);
    lsts = pci_rd16(b->bus, b->dev, b->fn, b->cap + 0x12);
    if (lcap & 0x00100000UL) return (lsts & 0x2000) != 0;
    return 0;
}

/* slots with a power controller: power up a card that was inserted, power down (eject) when
   the attention button is pressed; Thunderbolt ports have no power controller */
static void slot_service(struct bridge *b)
{
    u32 sltcap;
    u16 sta, ctl;
    if (!b->cap) return;
    sltcap = pci_rd32(b->bus, b->dev, b->fn, b->cap + 0x14);
    sta = pci_rd16(b->bus, b->dev, b->fn, b->cap + 0x1A);
    if (sltcap & 2) {
        ctl = pci_rd16(b->bus, b->dev, b->fn, b->cap + 0x18);
        if ((sta & 1) && !(ctl & 0x400)) {  /* attention button on a powered slot: eject */
            kprintf("pcihp: port %02x:%02x.%u: attention button, powering the slot off\n", b->bus, b->dev, b->fn);
            ctl = (u16)((ctl & ~0x300) | 0x300 | 0x400);     /* indicator off, power off */
            pci_wr16(b->bus, b->dev, b->fn, b->cap + 0x18, ctl);
        } else if ((sta & 0x40) && (ctl & 0x400)) {         /* card present, power off: power on */
            kprintf("pcihp: port %02x:%02x.%u: card present, powering the slot on\n", b->bus, b->dev, b->fn);
            ctl = (u16)((ctl & ~(0x300 | 0x400)) | 0x100);  /* indicator on, power on */
            pci_wr16(b->bus, b->dev, b->fn, b->cap + 0x18, ctl);
            pcihp_request_rescan(1000);                     /* link training after power up */
        }
    }
    if (sta & 0x11F) pci_wr16(b->bus, b->dev, b->fn, b->cap + 0x1A, (u16)(sta & 0x11F));   /* clear events */
}

static int port_has_new(int bus)
{
    int dev, fn, nfn;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++)
            if (fn_present(bus, dev, fn) && is_new(bus, dev, fn)) return 1;
    }
    return 0;
}

/* boot: look for NVMe (and xHCI) functions already enumerated behind hot plug ports */
static int boot_scan(int bus)
{
    int dev, fn, nfn, n = 0;
    for (dev = 0; dev < 32; dev++) {
        nfn = fn_count(bus, dev);
        for (fn = 0; fn < nfn; fn++) {
            u8 ht;
            if (!fn_present(bus, dev, fn)) continue;
            ht = pci_rd8(bus, dev, fn, 0x0E);
            if ((ht & 0x7F) == 1) {
                hot_add(bdf(bus, dev, fn), HK_BRIDGE, 1);
                n += boot_scan(pci_rd8(bus, dev, fn, 0x19));
            } else if ((ht & 0x7F) == 0) {
                start_function(bus, dev, fn, 1);
                n++;
            }
        }
    }
    return n;
}

int pcihp_rescan(void)
{
    int i, found = 0;
    for (i = 0; i < nbridges; i++) {
        struct bridge *b = &bridges[i];
        u32 v, base, limit;
        struct window w;
        if (!b->hotplug || (b->ptype != 4 && b->ptype != 6)) continue;
        if (!b->sec || b->sub < b->sec) continue;
        if (!booted) {
            found += boot_scan(b->sec);
            continue;
        }
        if (!link_up(b) || !port_has_new(b->sec)) continue;
        v = pci_rd32(b->bus, b->dev, b->fn, 0x20);
        base = (v & 0xFFF0) << 16;
        limit = (v & 0xFFF00000UL) | 0xFFFFF;
        kprintf("pcihp: new functions below port %02x:%02x.%u (buses %02x-%02x, window %08lx-%08lx)\n",
                b->bus, b->dev, b->fn, b->sec, b->sub, base, limit);
        if (base > limit) {
            kprintf("pcihp: the BIOS reserved no memory window on this port: enable Thunderbolt /\n"
                    "pcihp: PCIe hot plug resource reservation in the BIOS setup\n");
            continue;
        }
        pci_wr16(b->bus, b->dev, b->fn, 4, (u16)(pci_rd16(b->bus, b->dev, b->fn, 4) | 6));
        assign_buses(b->sec, b->sec + 1, b->sub);
        w.base = base;
        w.limit = limit;
        w.cur = used_top(b->sec, base, limit);
        assign_bus(b->sec, &w, 0);
        found++;
    }
    return found;
}

void pcihp_request_rescan(u32 delay_ms)
{
    u32 at = plat_ms() + delay_ms;
    if (!rescan_pending || (s32)(at - rescan_at) > 0) rescan_at = at;
    rescan_pending = 1;
    rescan_tries = 0;
}

static void known_forget(u32 b)
{
    int i;
    for (i = 0; i < nknown; i++)
        if (known[i] == b) {
            known[i] = known[nknown - 1];
            nknown--;
            return;
        }
}

void pcihp_check_removed(void)
{
    int i;
    /* functions the BIOS enumerated behind a hot plug port that are gone: a new device may
       appear at the same address later and must then count as new */
    for (i = 0; i < nknown; i++) {
        u32 b = known[i];
        if (!(b >> 8) || !pcihp_behind_hotplug((int)(b >> 8))) continue;
        if ((pci_rd32((int)(b >> 8), (int)((b >> 3) & 31), (int)(b & 7), 0) & 0xFFFF) == 0xFFFF) {
            known_forget(b);
            i--;
        }
    }
    for (i = 0; i < MAXHOT; i++) {
        struct hotdev *h = &hot[i];
        u32 id;
        if (!h->used) continue;
        id = pci_rd32(h->bdf >> 8, (h->bdf >> 3) & 31, h->bdf & 7, 0);
        if ((id & 0xFFFF) != 0xFFFF) continue;
        kprintf("pcihp: %02x:%02x.%u removed\n", h->bdf >> 8, (h->bdf >> 3) & 31, h->bdf & 7);
        known_forget(h->bdf);
        if (h->kind == HK_NVME) nvme_remove(h->bdf);
        else if (h->kind == HK_XHCI) usb_hotunplug_hc(h->bdf);
        else if (h->kind == HK_BRIDGE) {
            int k;
            for (k = 0; k < nbridges; k++)
                if (bdf(bridges[k].bus, bridges[k].dev, bridges[k].fn) == h->bdf) {
                    bridges[k] = bridges[nbridges - 1];
                    nbridges--;
                    break;
                }
        }
        h->used = 0;
    }
}

int pcihp_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAXHOT; i++) if (hot[i].used && hot[i].kind != HK_BRIDGE) n++;
    return n;
}

/* functions behind the hot plug ports at boot, enumerated by the BIOS: NVMe controllers get
   fixed drive letters (called before the fixed units are assigned) */
void pcihp_boot(void)
{
    int i, ports = 0;
    if (booted || !active) return;
    for (i = 0; i < nbridges; i++)
        if (bridges[i].hotplug && (bridges[i].ptype == 4 || bridges[i].ptype == 6)) ports++;
    kprintf("pcihp: %d hot plug capable PCIe port(s)\n", ports);
    pcihp_rescan();                 /* boot pass: functions the BIOS already configured */
    booted = 1;
    for (i = 0; i < nbridges; i++) bridges[i].linked = (u8)(bridges[i].sec ? link_up(&bridges[i]) : 0);
}

void pcihp_poll(void)
{
    u32 now = plat_ms();
    if (!booted || !active) return;
    if (rescan_pending && (s32)(now - rescan_at) >= 0) {
        if (pcihp_rescan() || ++rescan_tries >= 6) rescan_pending = 0;
        else rescan_at = now + 500;     /* the PCIe link may still be training */
        pcihp_check_removed();
        next_removal = now + 1000;
    }
    if ((s32)(now - next_removal) >= 0) {
        int i;
        next_removal = now + 1000;
        pcihp_check_removed();
        /* a link that came up without a Thunderbolt event (plain PCIe hot plug slots) */
        for (i = 0; i < nbridges; i++) {
            struct bridge *b = &bridges[i];
            int up;
            if (!b->hotplug || (b->ptype != 4 && b->ptype != 6) || !b->sec) continue;
            slot_service(b);
            up = link_up(b);
            if (up && !b->linked) pcihp_request_rescan(300);
            b->linked = (u8)up;
        }
    }
}
