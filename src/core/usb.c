/* usb.c - USB core: controllers, hub port state machine, enumeration, transfers, API */
#include "usb.h"

#define MAXHC 8
struct usb_hc *hcs[MAXHC];
int nhc;
struct usb_dev *usb_devices;
static struct usb_hub *hubs;
static u16 nextid = 1;
static u8 *ctlbuf;              /* bounce for synchronous control transfers */
static struct usb_class *classes[] = { &hub_class, &hid_class, &msc_class, &uaudio_class, &uvc_class };
static struct usb_iso *isos;

u32 speed_maxpkt0(int speed)
{
    switch (speed) {
    case USB_LS: return 8;
    case USB_FS: return 64;
    case USB_HS: return 64;
    default: return 512;
    }
}

static const char *speed_name(int s)
{
    static const char *n[] = { "low", "full", "high", "super", "super+" };
    return s >= 0 && s <= 4 ? n[s] : "?";
}

int usb_addr_alloc(struct usb_hc *hc)
{
    int a;
    for (a = 1; a < 128; a++) {
        if (!(hc->addrmap[a >> 3] & (1 << (a & 7)))) {
            hc->addrmap[a >> 3] |= (u8)(1 << (a & 7));
            return a;
        }
    }
    return -1;
}

void usb_addr_free(struct usb_hc *hc, int a)
{
    if (a > 0 && a < 128) hc->addrmap[a >> 3] &= (u8)~(1 << (a & 7));
}

struct usb_hc *usb_add_hc(struct usb_hcops *ops, struct pcidev *p, void *priv)
{
    struct usb_hc *hc;
    if (nhc >= MAXHC) return NULL;
    hc = kmalloc(sizeof(*hc));
    if (!hc) return NULL;
    hc->ops = ops;
    hc->pci = *p;
    hc->priv = priv;
    hc->irq = p->irq;
    hc->index = (u8)nhc;
    ksnprintf(hc->name, sizeof(hc->name), "%s %02x:%02x.%u", ops->name, p->bus, p->dev, p->fn);
    hcs[nhc++] = hc;
    return hc;
}

void usb_init(void)
{
    nhc = 0;
    hubs = NULL;
    usb_devices = NULL;
    if (!ctlbuf) ctlbuf = kmalloc_align(4096, 4096);
}

/* MMIO mappings are permanent: the early BIOS handoff and the driver share them */
static struct { u32 phys, len; void *lin; } maps[MAXHC * 2];
static int nmaps;

void *usb_map(u32 phys, u32 len)
{
    int i;
    void *v;
    for (i = 0; i < nmaps; i++)
        if (maps[i].phys == phys && maps[i].len >= len) return maps[i].lin;
    v = plat_mmio(phys, len);
    if (v && nmaps < (int)ARRAY_SIZE(maps)) {
        maps[nmaps].phys = phys;
        maps[nmaps].len = len;
        maps[nmaps].lin = v;
        nmaps++;
    }
    return v;
}

static const u32 type_opt[4] = { OPT_NOUHCI, OPT_NOOHCI, OPT_NOEHCI, OPT_NOXHCI };

/* controllers that are not used at all */
static int usb_skip(struct pcidev *p)
{
    if (p->cls != 0x0C || p->sub != 0x03 || (p->prog & 0x0F) || p->prog > 0x30) return 1;
    if (cb->opts & type_opt[p->prog >> 4]) return 1;
    /* AMD-8111 EHCI does not work (AMD erratum); its OHCI companions handle the ports */
    if (p->vendor == 0x1022 && p->device == 0x7463) return 1;
    return 0;
}

static struct pcidev *find_pci(struct pcidev *list, int n, u16 ven, u16 dev)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i].vendor == ven && list[i].device == dev) return &list[i];
    return NULL;
}

/* chipset fixes applied before any controller is touched */
static void usb_quirks(struct pcidev *list, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        struct pcidev *p = &list[i];
        u8 v;
        if (usb_skip(p) || p->prog != 0x20) continue;
        /* ATI/AMD SB600 and SB700 A12: EHCI stops responding unless "disable async park" is set */
        if (p->vendor == 0x1002 && (p->device == 0x4386 || p->device == 0x4396)) {
            struct pcidev *sm = find_pci(list, n, 0x1002, 0x4385);
            u8 rev = sm ? (u8)pci_rd8(sm->bus, sm->dev, sm->fn, 8) : 0;
            if (p->device == 0x4386 || rev == 0x3B) {
                v = (u8)pci_rd8(p->bus, p->dev, p->fn, 0x53);
                pci_wr8(p->bus, p->dev, p->fn, 0x53, (u8)(v | 0x08));
                kprintf("usb: AMD SB600/SB700 EHCI fix applied\n");
            }
        }
        /* VIA VT6212: no async schedule park */
        if (p->vendor == 0x1106 && p->device == 0x3104 && (pci_rd8(p->bus, p->dev, p->fn, 8) & 0xF0) == 0x60) {
            v = (u8)pci_rd8(p->bus, p->dev, p->fn, 0x4B);
            if (!(v & 0x20)) pci_wr8(p->bus, p->dev, p->fn, 0x4B, (u8)(v | 0x20));
        }
    }
}

/* Intel 7/8/9-series and some SoCs: route the switchable ports from EHCI to xHCI. Must run
   after the BIOS handed both controllers over. */
static void usb_route_intel(struct pcidev *list, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        struct pcidev *p = &list[i];
        u16 d = p->device;
        u32 m;
        if (p->vendor != 0x8086 || p->cls != 0x0C || p->sub != 0x03 || p->prog != 0x30) continue;
        if (d != 0x1E31 && d != 0x8C31 && d != 0x9C31 && d != 0x8CB1 && d != 0x9CB1 && d != 0x8D31 && d != 0x0F35)
            continue;
        if (cb->opts & (OPT_NOXHCI | OPT_NOROUTE)) {
            kprintf("usb: Intel xHCI port routing left as set by the BIOS: USB2 %08lx\n",
                    pci_rd32(p->bus, p->dev, p->fn, 0xD0));
            continue;
        }
        m = pci_rd32(p->bus, p->dev, p->fn, 0xDC);
        pci_wr32(p->bus, p->dev, p->fn, 0xD8, m);
        m = pci_rd32(p->bus, p->dev, p->fn, 0xD4);
        pci_wr32(p->bus, p->dev, p->fn, 0xD0, m);
        kprintf("usb: Intel xHCI port routing: USB3 %08lx USB2 %08lx\n",
                pci_rd32(p->bus, p->dev, p->fn, 0xD8), pci_rd32(p->bus, p->dev, p->fn, 0xD0));
    }
}

static struct pcidev *plist;
static int plist_n, plist_fixed;

/* Plug and Play start: controllers come in groups, IRQ sharing is judged against all functions */
void usb_pci_list(struct pcidev *l, int n)
{
    plist = l;
    plist_n = n;
    plist_fixed = 1;
}
static int intx_later;
static u16 irq_miss[16];

/* PCI interrupt lines are level triggered (ELCR); an edge-triggered line belongs to an ISA
   device (floppy on IRQ 6, serial ports, ...) */
static int irq_level(int irq)
{
    return (inb((u16)(0x4D0 + (irq >> 3))) >> (irq & 7)) & 1;
}

/* another PCI function with an interrupt pin on the same line (not one of our controllers) */
static int irq_shared(struct usb_hc *hc)
{
    int i;
    for (i = 0; i < plist_n; i++) {
        struct pcidev *p = &plist[i];
        if (p->irq != hc->irq || PCI_BDF(p) == PCI_BDF(&hc->pci)) continue;
        if (!pci_rd8(p->bus, p->dev, p->fn, 0x3D)) continue;
        if (p->cls == 0x0C && p->sub == 0x03 && !usb_skip(p)) continue;
        return 1;
    }
    return 0;
}

/* Interrupt or polling mode. An IRQ is used only when it is a PCI line that no other device
   uses: on a shared line a device nobody serves (or the BIOS handler of another one) can
   keep it asserted. Polled controllers do not assert INTx at all. In DOS the real-mode
   handler is installed after the core returns, so INTx is switched on at the first poll. */
void usb_irq_setup(struct usb_hc *hc)
{
    const char *why = NULL;
    if (cb->opts & OPT_POLL) why = "/POLL";
    else if (hc->irq == 0 || hc->irq > 15 || hc->irq == 2) why = "no IRQ assigned";
    else if (!irq_level(hc->irq)) why = "ISA (edge triggered) IRQ";
    else if (irq_shared(hc)) why = "IRQ shared with other devices";
    else if (plat_irq_attach(hc->irq)) why = "IRQ not available";
    if (why) {
        kprintf("usb: %s IRQ %d: polled (%s)\n", hc->name, hc->irq, why);
        hc->irq = 0xFF;
        pci_intx(&hc->pci, 0);
        return;
    }
    kprintf("usb: %s uses IRQ %d\n", hc->name, hc->irq);
    irq_miss[hc->irq] = 0;
    if (plat_mode == PM_DOS) {
        pci_intx(&hc->pci, 0);
        hc->intx_later = 1;
        intx_later = 1;
    } else {
        pci_intx(&hc->pci, 1);
    }
}

static void intx_on_now(void)
{
    int i;
    intx_later = 0;
    for (i = 0; i < nhc; i++) {
        if (!hcs[i]->intx_later) continue;
        hcs[i]->intx_later = 0;
        if (hcs[i]->irq <= 15) pci_intx(&hcs[i]->pci, 1);
    }
}

/* a line that keeps firing for another device: give it up and poll */
static void irq_give_up(int irq)
{
    int i;
    kprintf("usb: IRQ %d keeps firing for another device, controllers switched to polling\n", irq);
    for (i = 0; i < nhc; i++) {
        if (hcs[i]->irq != irq) continue;
        hcs[i]->irq = 0xFF;
        pci_intx(&hcs[i]->pci, 0);
    }
    for (i = 0; i < 8; i++)
        if (cb->hcirq[i] == irq) cb->hcirq[i] = 0xFF;
    plat_irq_detach(irq);
}

/* IRQs of our controllers that other PCI functions (or IDE 14/15) also use */
static void find_shared_irqs(struct pcidev *list, int n)
{
    int i, k, j;
    cb->shirq = 0;
    for (k = 0; k < 8; k++) {
        cb->hcbdf[k] = k < nhc ? (u16)PCI_BDF(&hcs[k]->pci) : 0;
        cb->hcirq[k] = k < nhc ? hcs[k]->irq : 0xFF;
    }
    for (k = 0; k < nhc; k++) {
        u8 irq = hcs[k]->irq;
        if (irq > 15) continue;
        if (irq == 14 || irq == 15) cb->shirq |= (u16)(1U << irq);
        for (i = 0; i < n; i++) {
            struct pcidev *p = &list[i];
            int ours = 0;
            if (p->irq != irq) continue;
            for (j = 0; j < nhc; j++)
                if (PCI_BDF(&hcs[j]->pci) == PCI_BDF(p)) ours = 1;
            if (!ours) {
                cb->shirq |= (u16)(1U << irq);
                kprintf("usb: IRQ %d shared with %02x:%02x.%u %04x:%04x\n", irq, p->bus, p->dev, p->fn,
                        p->vendor, p->device);
            }
        }
    }
}

/* All controllers: chipset fixes, take every controller from the BIOS (legacy USB / SMM),
   port routing, then start the drivers. */
void usb_start_all(struct pcidev *list, int n)
{
    int i;
    if (!plist_fixed) {
        plist = list;
        plist_n = n;
    }
    usb_quirks(list, n);
    for (i = 0; i < n; i++) {
        struct pcidev *p = &list[i];
        if (usb_skip(p)) continue;
        kprintf("usb: %02x:%02x.%u %04x:%04x %s, BIOS handoff\n", p->bus, p->dev, p->fn, p->vendor, p->device,
                p->prog == 0x30 ? "xHCI" : p->prog == 0x20 ? "EHCI" : p->prog == 0x10 ? "OHCI" : "UHCI");
        switch (p->prog) {
        case 0x00: uhci_early(p); break;
        case 0x10: ohci_early(p); break;
        case 0x20: ehci_early(p); break;
        case 0x30: xhci_early(p); break;
        }
    }
    usb_route_intel(list, n);
    for (i = 0; i < n; i++) {
        struct pcidev *p = &list[i];
        int r = -1;
        if (usb_skip(p)) continue;
        kprintf("usb: starting %02x:%02x.%u\n", p->bus, p->dev, p->fn);
        switch (p->prog) {
        case 0x00: r = uhci_attach(p); break;
        case 0x10: r = ohci_attach(p); break;
        case 0x20: r = ehci_attach(p); break;
        case 0x30: r = xhci_attach(p); break;
        }
        if (r) kprintf("usb: %02x:%02x.%u not usable\n", p->bus, p->dev, p->fn);
    }
    find_shared_irqs(list, n);
}

/* an xHCI controller that arrived through a Thunderbolt / USB4 tunnel after boot. It is
   polled: the real-mode interrupt table was built at boot and cannot grow. */
int usb_hotplug_hc(struct pcidev *p)
{
    if (usb_skip(p) || p->prog != 0x30) return -1;
    p->irq = 0;
    xhci_early(p);
    if (xhci_attach(p)) return -1;
    kprintf("usb: %s started (polled)\n", hcs[nhc - 1]->name);
    return 0;
}

/* the controller at bdf vanished: its devices are gone, its root hub leaves the list */
void usb_hotunplug_hc(u32 bdf)
{
    int i, k, p;
    for (i = 0; i < nhc; i++) {
        struct usb_hc *hc = hcs[i];
        if (PCI_BDF(&hc->pci) != bdf) continue;
        for (p = 1; p <= hc->root.nports; p++)
            if (hc->root.port[p].dev) usb_port_gone(&hc->root, p);
        hc->ops->stop(hc);
        usb_free_hub(&hc->root);
        kprintf("usb: %s removed\n", hc->name);
        kfree(hc);
        for (k = i; k < nhc - 1; k++) {
            hcs[k] = hcs[k + 1];
            hcs[k]->index = (u8)k;
        }
        nhc--;
        hcs[nhc] = NULL;
        return;
    }
}

int usb_irq(int irq)
{
    int i, r = 0;
    if (irq < 0 || irq > 15) return 0;
    for (i = 0; i < nhc; i++) {
        struct usb_hc *hc = hcs[i];
        if (hc->irq != irq) continue;
        /* the real-mode handler may have switched INTx off at an unsafe moment */
        if (pci_rd16(hc->pci.bus, hc->pci.dev, hc->pci.fn, 4) & 0x0400) pci_intx(&hc->pci, 1);
        if (hc->ops->irq && hc->ops->irq(hc)) {
            hc->irqs++;
            r = 1;
        }
    }
    if (r) irq_miss[irq] = 0;
    else if (++irq_miss[irq] > 2000) irq_give_up(irq);
    return r;
}

/* ---- transfers ---- */
int usb_submit(struct usb_xfer *x)
{
    struct usb_hc *hc = x->pipe->dev->hc;
    int r;
    x->status = XS_PENDING;
    x->actual = 0;
    if (x->pipe->dev->gone) {
        x->status = XS_GONE;
        return XS_GONE;
    }
    r = hc->ops->submit(hc, x);
    if (r) {
        x->status = r < 0 ? r : XS_ERROR;
        return x->status;
    }
    x->pipe->dev->xfers++;
    return 0;
}

void usb_cancel(struct usb_xfer *x)
{
    struct usb_hc *hc = x->pipe->dev->hc;
    if (x->status == XS_PENDING) {
        hc->ops->cancel(hc, x);
        if (x->status == XS_PENDING) x->status = XS_CANCEL;
    }
}

int usb_wait(struct usb_xfer *x, u32 timeout_ms)
{
    struct usb_hc *hc = x->pipe->dev->hc;
    u32 t0 = plat_ms();
    while (x->status == XS_PENDING) {
        hc->ops->poll(hc);
        if (x->status != XS_PENDING) break;
        if (plat_ms() - t0 > timeout_ms) {
            usb_cancel(x);
            if (x->status == XS_CANCEL) x->status = XS_TIMEOUT;
            kprintf("usb: dev %u ep %02x transfer timeout (%lu bytes)\n", x->pipe->dev->id, x->pipe->ep, x->len);
            break;
        }
        cpu_pause();
    }
    if (x->status != XS_OK) x->pipe->dev->errors++;
    return x->status;
}

int usb_control(struct usb_dev *d, u8 rt, u8 req, u16 val, u16 idx, void *buf, u16 len)
{
    struct usb_xfer x;
    int r;
    if (len > 4096) return XS_ERROR;
    memset(&x, 0, sizeof(x));
    x.pipe = &d->ep0;
    x.setup[0] = rt;
    x.setup[1] = req;
    x.setup[2] = (u8)val;
    x.setup[3] = (u8)(val >> 8);
    x.setup[4] = (u8)idx;
    x.setup[5] = (u8)(idx >> 8);
    x.setup[6] = (u8)len;
    x.setup[7] = (u8)(len >> 8);
    x.buf = ctlbuf;
    x.len = len;
    x.in = (rt & 0x80) != 0;
    if (!x.in && len) memcpy(ctlbuf, buf, len);
    if (usb_submit(&x)) return x.status;
    r = usb_wait(&x, 3000);
    if (r == XS_OK && x.in && len) memcpy(buf, ctlbuf, x.actual);
    if (r == XS_OK) return (int)x.actual;
    return r;
}

int usb_xfer_sync_stream(struct usb_pipe *p, void *buf, u32 len, int in, u32 timeout_ms, u32 *actual, u16 stream)
{
    struct usb_xfer x;
    memset(&x, 0, sizeof(x));
    x.pipe = p;
    x.buf = buf;
    x.len = len;
    x.in = (u8)in;
    x.stream = stream;
    if (usb_submit(&x) == 0) usb_wait(&x, timeout_ms);
    if (actual) *actual = x.actual;
    return x.status;
}

int usb_xfer_sync(struct usb_pipe *p, void *buf, u32 len, int in, u32 timeout_ms, u32 *actual)
{
    return usb_xfer_sync_stream(p, buf, len, in, timeout_ms, actual, 0);
}

int usb_pipe_open(struct usb_dev *d, struct usb_pipe *p, u8 *ep)
{
    u16 mps = ep[4] | (ep[5] << 8);
    u8 *ss = ep + ep[0];
    memset(p, 0, sizeof(*p));
    p->dev = d;
    p->ep = ep[2];
    p->type = ep[3] & 3;
    p->maxpkt = mps & 0x7FF;
    p->mult = (u8)((mps >> 11) & 3);
    p->interval = ep[6];
    if (d->speed >= USB_SS && ss[1] == 0x30) {
        p->maxburst = ss[2];
        if (p->type == EP_BULK) p->maxstreams = ss[3] & 0x1F;
        if (p->type == EP_ISO || p->type == EP_INT) p->mult = ss[3] & 3;
    }
    if (d->hc->ops->pipe_open(d->hc, p)) return -1;
    p->open = 1;
    return 0;
}

void usb_pipe_close(struct usb_pipe *p)
{
    if (!p->open) return;
    p->dev->hc->ops->pipe_close(p->dev->hc, p);
    p->open = 0;
}

int usb_clear_halt(struct usb_pipe *p)
{
    struct usb_dev *d = p->dev;
    int r = usb_control(d, 0x02, 0x01, 0, p->ep, NULL, 0);     /* CLEAR_FEATURE(ENDPOINT_HALT) */
    p->toggle = 0;
    if (d->hc->ops->clear_halt) d->hc->ops->clear_halt(d->hc, p);
    return r < 0 ? r : 0;
}

u8 *usb_find_desc(u8 *p, u8 *end, u8 type)
{
    while (p + 2 <= end && p[0]) {
        if (p[1] == type) return p;
        p += p[0];
    }
    return NULL;
}

int usb_set_interface(struct usb_dev *d, int ifnum, int alt)
{
    int r = usb_control(d, 0x01, 0x0B, (u16)alt, (u16)ifnum, NULL, 0);
    return r < 0 ? r : 0;
}

/* ---- isochronous streams ---- */
int usb_iso_start(struct usb_iso *s)
{
    struct usb_hc *hc = s->pipe->dev->hc;
    struct usb_pipe *p = s->pipe;
    u32 iv = p->interval ? p->interval : 1, bytes;
    if (!hc->ops->iso_start) return -1;
    if (iv > 16) iv = 16;
    s->period_us = (p->dev->speed >= USB_HS ? 125UL : 1000UL) << (iv - 1);
    if (!s->pktsize) s->pktsize = (u16)(p->maxpkt * (p->mult + 1UL) * (p->maxburst + 1UL));
    /* the DOS core runs every timer tick (55 ms), the VxD every 10 ms */
    if (!s->ahead_us) s->ahead_us = plat_mode == PM_DOS ? 150000UL : 40000UL;
    s->npkt = (int)(s->ahead_us / s->period_us) + 8;
    if (s->npkt < 16) s->npkt = 16;
    if (s->npkt > 1024) s->npkt = 1024;
    if (!s->maxbytes) s->maxbytes = 0x40000UL;
    if ((u32)s->npkt * s->pktsize > s->maxbytes) s->npkt = (int)MAX(16UL, s->maxbytes / s->pktsize);
    bytes = (u32)s->npkt * s->pktsize;
    s->ahead_us = MIN(s->ahead_us, (u32)(s->npkt - 8) * s->period_us);
    s->buf = kmalloc_align(bytes, 4096);
    while (!s->buf && s->npkt > 32) {                   /* less memory: fewer packets ahead */
        s->npkt = s->npkt * 3 / 4;
        bytes = (u32)s->npkt * s->pktsize;
        s->buf = kmalloc_align(bytes, 4096);
    }
    if (!s->buf) return -1;
    s->ahead_us = MIN(s->ahead_us, (u32)(s->npkt - 8) * s->period_us);
    if (hc->ops->iso_start(hc, s)) {
        kfree(s->buf);
        s->buf = NULL;
        return -1;
    }
    s->running = 1;
    s->next = isos;
    isos = s;
    return 0;
}

void usb_iso_stop(struct usb_iso *s)
{
    struct usb_iso **pp;
    struct usb_hc *hc = s->pipe->dev->hc;
    if (!s->running) return;
    for (pp = &isos; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    hc->ops->iso_stop(hc, s);
    s->running = 0;
    kfree(s->buf);
    s->buf = NULL;
}

/* the stream ran dry: schedule further ahead from now on (up to the packet slots) */
void usb_iso_late(struct usb_iso *s)
{
    u32 max = (u32)(s->npkt - 8) * s->period_us;
    s->missed++;
    if (s->ahead_us < max) {
        s->ahead_us = MIN(s->ahead_us + 10000UL, max);
        kprintf("usb: isochronous stream late, now %lu ms ahead\n", s->ahead_us / 1000);
    }
}

static void iso_service_all(void)
{
    struct usb_iso *s;
    for (s = isos; s; s = s->next) {
        struct usb_hc *hc = s->pipe->dev->hc;
        if (!s->pipe->dev->gone) hc->ops->iso_service(hc, s);
    }
}

/* ---- device enumeration ---- */
static void get_string(struct usb_dev *d, u8 idx, char *out, int max, u16 lang)
{
    u8 buf[128];
    int n, i, o = 0;
    out[0] = 0;
    if (!idx) return;
    n = usb_control(d, 0x80, 0x06, (u16)(0x0300 | idx), lang, buf, sizeof(buf));
    if (n < 2) return;
    if (buf[0] < n) n = buf[0];
    for (i = 2; i + 1 < n && o < max - 1; i += 2) {
        u16 c = buf[i] | (buf[i + 1] << 8);
        out[o++] = (char)((c >= 0x20 && c < 0x7F) ? c : '?');
    }
    out[o] = 0;
    trim_string(out);
}

static void bind_classes(struct usb_dev *d)
{
    u8 *p = d->cdesc, *end = d->cdesc + d->cdesc_len;
    u32 k;
    while (p && p + 2 <= end && p[0]) {
        if (p[1] == 4 && p[3] == 0 && d->ncls < 4) {     /* interface, alternate 0 */
            for (k = 0; k < ARRAY_SIZE(classes); k++) {
                if (classes[k]->probe(d, p, end) == 0) {
                    d->cls[d->ncls++] = classes[k];
                    break;
                }
            }
        }
        p += p[0];
    }
}

static int enumerate(struct usb_hub *h, int port, u32 st)
{
    struct usb_hc *hc = h->hc;
    struct usb_dev *d, *par = h->dev;
    u8 buf[18];
    int r, a, tries;
    u16 lang = 0x0409;
    u8 cfg9[9];

    d = kmalloc(sizeof(*d));
    if (!d) return -1;
    d->hc = hc;
    d->hub = h;
    d->parent = par;
    d->port = (u8)port;
    d->speed = (u8)PS_SPEED(st);
    d->depth = par ? (u8)(par->depth + 1) : 1;
    d->rootport = par ? par->rootport : (u8)port;
    if (par && d->depth > 1) d->route = par->route | ((u32)(port > 15 ? 15 : port) << (4 * (par->depth - 1)));
    if (d->speed < USB_HS && par) {
        if (par->speed == USB_HS) { d->tthub = par; d->ttport = (u8)port; }
        else { d->tthub = par->tthub; d->ttport = par->ttport; }
    }
    d->ep0.dev = d;
    d->ep0.type = EP_CTRL;
    d->ep0.maxpkt = (u16)(d->speed == USB_FS ? 8 : speed_maxpkt0(d->speed));
    if (hc->ops->dev_init(hc, d)) {
        kprintf("usb: %s port %d: device init failed\n", hc->name, port);
        goto fail;
    }
    /* first 8 bytes of the device descriptor at the default address */
    for (tries = 0; tries < 3; tries++) {
        r = usb_control(d, 0x80, 0x06, 0x0100, 0, buf, 8);
        if (r >= 8) break;
        mdelay(10);
    }
    if (r < 8) {
        kprintf("usb: %s port %d: no device descriptor (%d)\n", hc->name, port, r);
        goto fail;
    }
    if (buf[7] && buf[7] != d->ep0.maxpkt && d->speed < USB_SS) {
        d->ep0.maxpkt = buf[7];
        if (hc->ops->dev_update) hc->ops->dev_update(hc, d);
    }
    if (hc->ops->type != HC_XHCI) {
        a = usb_addr_alloc(hc);
        if (a < 0) goto fail;
        r = usb_control(d, 0x00, 0x05, (u16)a, 0, NULL, 0);
        if (r < 0) {
            usb_addr_free(hc, a);
            kprintf("usb: %s port %d: SET_ADDRESS failed (%d)\n", hc->name, port, r);
            goto fail;
        }
        mdelay(5);
        d->addr = (u8)a;
        if (hc->ops->dev_update) hc->ops->dev_update(hc, d);
    }
    r = usb_control(d, 0x80, 0x06, 0x0100, 0, d->ddesc, 18);
    if (r < 18) {
        kprintf("usb: %s port %d: device descriptor failed (%d)\n", hc->name, port, r);
        goto fail;
    }
    r = usb_control(d, 0x80, 0x06, 0x0200, 0, cfg9, 9);
    if (r < 9) goto fail;
    d->cdesc_len = (u16)(cfg9[2] | (cfg9[3] << 8));
    if (d->cdesc_len > 2048) d->cdesc_len = 2048;
    d->cdesc = kmalloc(d->cdesc_len);
    r = usb_control(d, 0x80, 0x06, 0x0200, 0, d->cdesc, d->cdesc_len);
    if (r < 9) goto fail;
    d->cdesc_len = (u16)r;
    d->config = d->cdesc[5];
    d->maxpower = (u16)(d->cdesc[8] * (d->speed >= USB_SS ? 8 : 2));
    if (d->ddesc[14] || d->ddesc[15] || d->ddesc[16]) {
        u8 lb[4];
        if (usb_control(d, 0x80, 0x06, 0x0300, 0, lb, 4) >= 4) lang = (u16)(lb[2] | (lb[3] << 8));
        get_string(d, d->ddesc[14], d->manuf, sizeof(d->manuf), lang);
        get_string(d, d->ddesc[15], d->product, sizeof(d->product), lang);
        get_string(d, d->ddesc[16], d->serial, sizeof(d->serial), lang);
    }
    if ((d->ddesc[2] | (d->ddesc[3] << 8)) >= 0x0210) {
        r = usb_control(d, 0x80, 0x06, 0x0F00, 0, d->bos, 5);
        if (r >= 5) {
            u16 bl = (u16)(d->bos[2] | (d->bos[3] << 8));
            if (bl > sizeof(d->bos)) bl = sizeof(d->bos);
            r = usb_control(d, 0x80, 0x06, 0x0F00, 0, d->bos, bl);
            d->bos_len = (u16)(r > 0 ? r : 0);
        }
    }
    r = usb_control(d, 0x00, 0x09, d->config, 0, NULL, 0);
    if (r < 0) {
        kprintf("usb: SET_CONFIGURATION failed (%d)\n", r);
        goto fail;
    }
    d->id = nextid++;
    d->next = usb_devices;
    usb_devices = d;
    h->port[port].dev = d;
    kprintf("usb: %s port %d: %04x:%04x %s speed, class %02x, \"%s\" \"%s\"\n", hc->name, port,
            d->ddesc[8] | (d->ddesc[9] << 8), d->ddesc[10] | (d->ddesc[11] << 8), speed_name(d->speed),
            d->ddesc[4], d->manuf, d->product);
    bind_classes(d);
    return 0;
fail:
    if (d->addr) usb_addr_free(hc, d->addr);
    if (hc->ops->dev_free) hc->ops->dev_free(hc, d);
    if (d->cdesc) kfree(d->cdesc);
    kfree(d);
    return -1;
}

static void dev_teardown(struct usb_dev *d)
{
    struct usb_dev **pp;
    int i;
    d->gone = 1;
    if (d->ownhub) {
        struct usb_hub *h = d->ownhub;
        for (i = 1; i <= h->nports; i++)
            if (h->port[i].dev) usb_port_gone(h, i);
    }
    for (i = d->ncls - 1; i >= 0; i--)
        if (d->cls[i]->detach) d->cls[i]->detach(d);
    d->ncls = 0;
    if (d->ownhub) {
        usb_free_hub(d->ownhub);
        d->ownhub = NULL;
    }
    usb_pipe_close(&d->ep0);
    for (pp = &usb_devices; *pp; pp = &(*pp)->next) {
        if (*pp == d) { *pp = d->next; break; }
    }
    if (d->hc->ops->dev_free) d->hc->ops->dev_free(d->hc, d);
    if (d->addr) usb_addr_free(d->hc, d->addr);
    if (d->cdesc) kfree(d->cdesc);
    kfree(d);
}

void usb_port_gone(struct usb_hub *h, int port)
{
    struct usb_dev *d = h->port[port].dev;
    if (!d) return;
    kprintf("usb: %s port %d: device %u disconnected\n", h->hc->name, port, d->id);
    h->port[port].dev = NULL;
    dev_teardown(d);
}

struct usb_hub *usb_new_hub(struct usb_hc *hc, struct usb_dev *dev, struct usb_hubops *ops, int nports)
{
    struct usb_hub *h = dev ? kmalloc(sizeof(*h)) : &hc->root;
    if (!h) return NULL;
    if (!dev) memset(h, 0, sizeof(*h));
    h->hc = hc;
    h->dev = dev;
    h->ops = ops;
    h->nports = nports > MAXPORTS ? MAXPORTS : nports;
    h->depth = dev ? dev->depth : 0;
    h->nexthub = hubs;
    hubs = h;
    if (dev) dev->ownhub = h;
    return h;
}

void usb_free_hub(struct usb_hub *h)
{
    struct usb_hub **pp;
    for (pp = &hubs; *pp; pp = &(*pp)->nexthub) {
        if (*pp == h) { *pp = h->nexthub; break; }
    }
    if (h->dev) kfree(h);
}

static void hub_scan(struct usb_hub *h)
{
    int p;
    u32 now = plat_ms();
    if (h->dev) {
        if (!h->changed && (s32)(now - h->next_scan) < 0) return;
        h->changed = 0;
        h->next_scan = now + 1000;
    }
    for (p = 1; p <= h->nports; p++) {
        struct usb_port *pt = &h->port[p];
        u32 st = h->ops->status(h, p);
        if (st == 0xFFFFFFFFUL) continue;
        if (st & (PS_CCHANGE | PS_OTHERCHG)) h->ops->clear(h, p, st);
        switch (pt->state) {
        case PST_EMPTY:
            if ((st & PS_CONNECT) && !(st & PS_OWNER)) {
                pt->state = PST_DEBOUNCE;
                pt->deadline = now + 120;
                if (h->dev) h->changed = 1;     /* rescan soon */
            }
            break;
        case PST_DEBOUNCE:
            if (!(st & PS_CONNECT)) {
                pt->state = PST_EMPTY;
            } else if ((s32)(now - pt->deadline) >= 0) {
                int r = h->ops->reset(h, p);
                if (r == 1) {
                    pt->state = PST_COMPANION;
                } else if (r < 0) {
                    kprintf("usb: %s port %d: reset failed\n", h->hc->name, p);
                    pt->state = PST_FAILED;
                } else if (enumerate(h, p, h->ops->status(h, p)) == 0) {
                    pt->state = PST_ACTIVE;
                } else if (++pt->tries < 3) {
                    pt->state = PST_DEBOUNCE;
                    pt->deadline = now + 500;
                } else {
                    pt->state = PST_FAILED;
                    if (h->ops->disable) h->ops->disable(h, p);
                }
            } else if (h->dev) {
                h->changed = 1;
            }
            break;
        case PST_ACTIVE:
        case PST_EJECTED:
            if (!(st & PS_CONNECT) || (st & PS_CCHANGE)) {
                usb_port_gone(h, p);
                pt->state = PST_EMPTY;
                pt->tries = 0;
            }
            break;
        case PST_FAILED:
            if (!(st & PS_CONNECT)) { pt->state = PST_EMPTY; pt->tries = 0; }
            break;
        case PST_COMPANION:
            if (!(st & PS_OWNER)) { pt->state = PST_EMPTY; pt->tries = 0; }
            break;
        }
        if (pt->state == PST_DEBOUNCE && h->dev) h->changed = 1;
    }
}

void usb_hc_poll_all(void)
{
    int i;
    for (i = 0; i < nhc; i++) hcs[i]->ops->poll(hcs[i]);
}

void usb_poll(void)
{
    struct usb_hub *h, *next;
    if (intx_later) intx_on_now();
    usb_hc_poll_all();
    for (h = hubs; h; h = next) {
        next = h->nexthub;
        hub_scan(h);
    }
    hid_tick();
    msc_tick();
    uaudio_tick();              /* sound engine: rings of the USB audio devices */
    iso_service_all();          /* packets for the next frames */
}

void usb_shutdown(void)
{
    int i, p;
    for (i = nhc - 1; i >= 0; i--) {
        struct usb_hc *hc = hcs[i];
        for (p = 1; p <= hc->root.nports; p++)
            if (hc->root.port[p].dev) usb_port_gone(&hc->root, p);
    }
    for (i = nhc - 1; i >= 0; i--) {
        struct usb_hc *hc = hcs[i];
        hc->ops->stop(hc);
        kfree(hc);
        hcs[i] = NULL;
    }
    nhc = 0;
    hubs = NULL;
    usb_devices = NULL;
    isos = NULL;
}

/* DOS start: poll until the devices plugged in at boot are set up (no new device for half a
   second), so that their partitions are counted for the drive letters; max_ms at most */
void usb_settle(u32 max_ms)
{
    u32 t0 = plat_ms(), last = t0;
    int n = -1, k;
    struct usb_dev *d;
    while (plat_ms() - t0 < max_ms) {
        usb_poll();
        for (k = 0, d = usb_devices; d; d = d->next) k++;
        k += blk_count() * 64;
        if (k != n) {
            n = k;
            last = plat_ms();
        } else if (plat_ms() - last > 500 && plat_ms() - t0 > 300) {
            break;
        }
        mdelay(10);
    }
}

/* 1 while a device on some port is being connected (debounce, reset, enumeration) */
int usb_pending(void)
{
    struct usb_hub *h;
    int p;
    for (h = hubs; h; h = h->nexthub)
        for (p = 1; p <= h->nports; p++) {
            u32 st;
            if (h->port[p].state == PST_DEBOUNCE) return 1;
            if (h->port[p].state != PST_EMPTY) continue;
            st = h->ops->status(h, p);
            if (st != 0xFFFFFFFFUL && (st & PS_CONNECT) && !(st & PS_OWNER)) return 1;
        }
    return 0;
}

/* ---- API for USBTREE ---- */
static struct usb_dev *find_dev(u32 id)
{
    struct usb_dev *d;
    for (d = usb_devices; d; d = d->next) if (d->id == id) return d;
    return NULL;
}

static int put_node(struct winusbnode *out, int max, int *n)
{
    if (*n >= max) return -1;
    memset(&out[*n], 0, sizeof(*out));
    return (*n)++;
}

static void tree_hub(struct usb_hub *h, struct winusbnode *out, int max, int *n, u16 parent, int depth)
{
    int p, k;
    for (p = 1; p <= h->nports; p++) {
        struct usb_port *pt = &h->port[p];
        struct usb_dev *d = pt->dev;
        u32 st = h->ops->status(h, p);
        struct winusbnode *nd;
        k = put_node(out, max, n);
        if (k < 0) return;
        nd = &out[k];
        nd->parent = parent;
        nd->port = (u8)p;
        nd->depth = (u8)depth;
        nd->portstatus = st | ((u32)pt->state << 24);
        if (!d) {
            nd->type = UN_PORT;
            nd->id = (u16)(0x8000 | k);
            nd->speed = (u8)PS_SPEED(st);
            continue;
        }
        nd->type = UN_DEV;
        nd->id = d->id;
        nd->speed = d->speed;
        nd->vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8));
        nd->pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
        nd->bcd = (u16)(d->ddesc[12] | (d->ddesc[13] << 8));
        nd->bcdusb = (u16)(d->ddesc[2] | (d->ddesc[3] << 8));
        nd->cls = d->ddesc[4];
        nd->sub = d->ddesc[5];
        nd->proto = d->ddesc[6];
        nd->addr = d->addr;
        nd->config = d->config;
        nd->maxpower = d->maxpower;
        nd->errors = d->errors;
        nd->xfers = d->xfers;
        nd->slot = d->slot;
        nd->driver = d->driver;
        nd->rsv = d->drvinfo;
        nd->hctype = h->hc->ops->type;
        nd->iface_count = d->cdesc ? d->cdesc[4] : 0;
        if (d->ownhub) nd->nports = (u8)d->ownhub->nports;
        strlcpy(nd->manuf, d->manuf, sizeof(nd->manuf));
        strlcpy(nd->product, d->product, sizeof(nd->product));
        strlcpy(nd->serial, d->serial, sizeof(nd->serial));
        if (d->driver == UD_BOT || d->driver == UD_UAS) {
            int i, m = 0;
            for (i = 0; i < blk_count() && m < 7; i++) {
                struct blkdev *b = blk_get(i);
                if (b->kind == DK_USB && b->owner == d->id)
                    m += blk_find_unit_letter(b, nd->letters + m, 8 - m);
            }
            nd->luns = d->uas;
        }
        if (d->ejected) nd->portstatus |= 0x00800000UL;
        if (d->ownhub) tree_hub(d->ownhub, out, max, n, d->id, depth + 1);
    }
}

int usb_api(struct winapireq *r, u8 *buf)
{
    switch (r->fn) {
    case WINF_USBTREE:
    {
        struct winusbnode *out = (struct winusbnode *)buf;
        int max = (int)(r->buflen / sizeof(struct winusbnode)), n = 0, i, k;
        if (!buf) return WINE_SMALL;
        for (i = 0; i < nhc; i++) {
            struct usb_hc *hc = hcs[i];
            k = put_node(out, max, &n);
            if (k < 0) break;
            out[k].type = UN_HC;
            out[k].id = (u16)(0x4000 | i);
            out[k].hctype = hc->ops->type;
            out[k].nports = (u8)hc->root.nports;
            out[k].pciloc = PCI_BDF(&hc->pci);
            out[k].vid = hc->pci.vendor;
            out[k].pid = hc->pci.device;
            out[k].irq = hc->irq;
            out[k].errors = hc->errors;
            out[k].xfers = hc->irqs;
            out[k].mmio = hc->mmio ? hc->pci.bar[hc->ops->type == HC_EHCI ? 0 : 0] : hc->iobase;
            strlcpy(out[k].product, hc->name, sizeof(out[k].product));
            tree_hub(&hc->root, out, max, &n, out[k].id, 1);
        }
        r->retlen = (u32)n * sizeof(struct winusbnode);
        return WINE_OK;
    }
    case WINF_USBDESC:
    {
        struct usb_dev *d = find_dev(r->arg0);
        u32 n = 0;
        if (!d) return WINE_NODEV;
        if (r->buflen < 18 + 6 + d->cdesc_len + d->bos_len) return WINE_SMALL;
        buf[n++] = 18; buf[n++] = 0;
        memcpy(buf + n, d->ddesc, 18); n += 18;
        buf[n++] = (u8)d->cdesc_len; buf[n++] = (u8)(d->cdesc_len >> 8);
        memcpy(buf + n, d->cdesc, d->cdesc_len); n += d->cdesc_len;
        buf[n++] = (u8)d->bos_len; buf[n++] = (u8)(d->bos_len >> 8);
        memcpy(buf + n, d->bos, d->bos_len); n += d->bos_len;
        r->retlen = n;
        return WINE_OK;
    }
    case WINF_USBEJECT:
    {
        struct usb_dev *d = find_dev(r->arg0);
        if (!d) return WINE_NODEV;
        if (d->driver != UD_BOT && d->driver != UD_UAS) return WINE_BADFN;
        return msc_eject(d) ? WINE_IO : WINE_OK;
    }
    case WINF_SETOPT:
        if (r->arg0 == 4) dbg_level = r->arg1;
        return WINE_OK;
    }
    return WINE_BADFN;
}
