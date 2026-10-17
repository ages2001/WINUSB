/* ohci.c - OHCI host controller driver (USB 1.1, memory mapped) */
#include "usb.h"

#define HCREV       0x00
#define HCCTRL      0x04
#define HCCMD       0x08
#define HCINTSTS    0x0C
#define HCINTEN     0x10
#define HCINTDIS    0x14
#define HCHCCA      0x18
#define HCCTRLHEAD  0x20
#define HCCTRLCUR   0x24
#define HCBULKHEAD  0x28
#define HCBULKCUR   0x2C
#define HCDONE      0x30
#define HCFMINT     0x34
#define HCFMNUM     0x3C
#define HCPERSTART  0x40
#define HCLSTHR     0x44
#define HCRHDA      0x48
#define HCRHDB      0x4C
#define HCRHSTS     0x50
#define HCRHPORT    0x54

#define ED_SKIP     (1UL << 14)
#define ED_LS       (1UL << 13)
#define TD_R        (1UL << 18)
#define TD_DP_SETUP (0UL << 19)
#define TD_DP_OUT   (1UL << 19)
#define TD_DP_IN    (2UL << 19)
#define TD_NOINT    (7UL << 21)
#define TD_T0       (2UL << 24)
#define TD_T1       (3UL << 24)
#define TD_CC_NA    (0xFUL << 28)

struct otd {                    /* 32 bytes, 16-byte aligned */
    volatile u32 info, cbp, next, be;
    u32 start, len;
    struct otd *vnext;
    u32 isstatus;
};

struct oed {                    /* 16 bytes, 16-byte aligned */
    volatile u32 info, tail, head, next;
};

struct opipe {
    struct usb_pipe *p;
    struct oed *ed;
    struct otd *dummy;          /* current tail */
    struct otd *first;          /* first TD of the active transfer */
    struct otd *status;
    struct usb_xfer *active, *qhead, *qtail;
    int short_seen;
    u8 *setup;
    int listtype;               /* 0 control 1 bulk 2 interrupt */
    struct opipe *next;
};

struct ohci {
    struct usb_hc *hc;
    u8 *r;
    u8 *hcca;
    struct oed *anchor[4];      /* control, bulk, interrupt, isochronous (after interrupt) */
    struct opipe *pipes;
    int nports;
    u32 lastfr;
};

static struct usb_hcops ohci_ops;
static struct usb_hubops ohci_hubops;

#define RD(o, reg) RD32((o)->r + (reg))
#define WR(o, reg, v) WR32((o)->r + (reg), (v))

static void wait_frame(struct ohci *o)
{
    u32 f = RD(o, HCFMNUM) & 0xFFFF, t0 = plat_ms();
    WR(o, HCINTSTS, 0x04);
    while ((RD(o, HCFMNUM) & 0xFFFF) == f && plat_ms() - t0 < 5) ;
    udelay(200);
}

static struct otd *new_td(void)
{
    struct otd *t = kmalloc_align(sizeof(struct otd), 16);
    return t;
}

static void fill_td(struct otd *t, u32 info, u8 *buf, u32 len, struct otd *next)
{
    t->info = info | TD_CC_NA;
    t->cbp = len ? vphys(buf) : 0;
    t->be = len ? vphys(buf) + len - 1 : 0;
    t->next = vphys(next);
    t->vnext = next;
    t->start = t->cbp;
    t->len = len;
    t->isstatus = 0;
}

static int start_xfer(struct ohci *o, struct opipe *op, struct usb_xfer *x)
{
    struct usb_pipe *p = op->p;
    struct otd *cur = op->dummy, *first = op->dummy, *nd;
    u32 len = x->len, off = 0, dir;
    op->status = NULL;
    if (p->type == EP_CTRL) {
        memcpy(op->setup, x->setup, 8);
        nd = new_td();
        if (!nd) return XS_ERROR;
        fill_td(cur, TD_DP_SETUP | TD_T0 | TD_NOINT, op->setup, 8, nd);
        cur = nd;
        dir = x->in ? TD_DP_IN : TD_DP_OUT;
        while (off < len) {
            u32 n = MIN(4096UL - ((vphys(x->buf + off)) & 4095) + 4096UL, len - off);
            nd = new_td();
            fill_td(cur, dir | TD_R | TD_NOINT | (off == 0 ? TD_T1 : 0), x->buf + off, n, nd);
            cur = nd;
            off += n;
        }
        nd = new_td();
        fill_td(cur, (x->in && len ? TD_DP_OUT : TD_DP_IN) | TD_T1, NULL, 0, nd);
        cur->isstatus = 1;
        op->status = cur;
        cur = nd;
    } else {
        dir = (p->ep & 0x80) ? TD_DP_IN : TD_DP_OUT;
        do {
            u32 n = MIN(4096UL - ((vphys(x->buf + off)) & 4095) + 4096UL, len - off);
            nd = new_td();
            fill_td(cur, dir | TD_R | (off + n >= len ? 0 : TD_NOINT), x->buf + off, n, nd);
            cur = nd;
            off += n;
        } while (off < len);
    }
    memset(cur, 0, sizeof(*cur));
    op->first = first;
    op->dummy = cur;
    op->active = x;
    op->short_seen = 0;
    op->ed->tail = vphys(cur);
    if (op->listtype == 0) WR(o, HCCMD, 2);
    if (op->listtype == 1) WR(o, HCCMD, 4);
    return 0;
}

static void free_chain(struct otd *t, struct otd *stop)
{
    while (t && t != stop) {
        struct otd *n = t->vnext;
        kfree(t);
        t = n;
    }
}

static void finish(struct ohci *o, struct opipe *op, int status, struct usb_xfer **done)
{
    struct usb_xfer *x = op->active;
    free_chain(op->first, op->dummy);
    op->first = NULL;
    op->active = NULL;
    x->status = status;
    x->next = *done;
    *done = x;
    if (op->qhead) {
        struct usb_xfer *n = op->qhead;
        op->qhead = n->next;
        if (!op->qhead) op->qtail = NULL;
        n->next = NULL;
        if (start_xfer(o, op, n)) {
            n->status = XS_ERROR;
            n->next = *done;
            *done = n;
        }
    }
}

/* drop TDs still queued after a short packet or error */
static void ed_flush(struct ohci *o, struct opipe *op, struct otd *resume)
{
    op->ed->info |= ED_SKIP;
    wait_frame(o);
    op->ed->head = (resume ? vphys(resume) : vphys(op->dummy)) | (op->ed->head & 2);
    op->ed->info &= ~ED_SKIP;
    if (op->listtype == 0) WR(o, HCCMD, 2);
}

static int cc_status(u32 cc)
{
    switch (cc) {
    case 0: case 9: return XS_OK;
    case 4: return XS_STALL;
    case 8: return XS_BABBLE;
    }
    return XS_ERROR;
}

static void check_xfer(struct ohci *o, struct opipe *op, struct usb_xfer **done)
{
    struct usb_xfer *x = op->active;
    struct otd *t;
    u32 actual = 0, cc;
    int halted = (op->ed->head & 1) != 0;
    if (op->short_seen) {
        cc = op->status->info >> 28;
        if (cc == 0xF && !halted) return;
        if (halted) op->ed->head &= ~3UL;
        finish(o, op, cc == 0xF ? XS_ERROR : cc_status(cc), done);
        return;
    }
    for (t = op->first; t && t != op->dummy; t = t->vnext) {
        u32 got;
        cc = t->info >> 28;
        if (cc == 0xF) {
            if (!halted) return;
            /* halted without an error code on this TD: treat as error */
            cc = 5;
        }
        if (cc && cc != 9) {
            x->actual = actual;
            op->ed->head &= ~3UL;
            ed_flush(o, op, NULL);
            op->p->toggle = 0;
            finish(o, op, cc_status(cc), done);
            return;
        }
        if (t->isstatus) break;
        if (op->p->type == EP_CTRL && t == op->first) continue;     /* setup stage */
        got = t->cbp ? t->cbp - t->start : t->len;
        actual += got;
        if (got < t->len) {
            x->actual = actual;
            if (op->status) {
                if (t->vnext != op->status) ed_flush(o, op, op->status);
                op->short_seen = 1;
                return;
            }
            if (t->vnext != op->dummy) ed_flush(o, op, NULL);
            finish(o, op, XS_OK, done);
            return;
        }
    }
    x->actual = actual;
    finish(o, op, XS_OK, done);
}

static void ohci_poll(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    struct opipe *op;
    struct usb_xfer *done = NULL, *x;
    u32 st = RD(o, HCINTSTS);
    if (st & 0x7F) WR(o, HCINTSTS, st & 0x7F);
    for (op = o->pipes; op; op = op->next)
        if (op->active) check_xfer(o, op, &done);
    while ((x = done) != NULL) {
        done = x->next;
        x->next = NULL;
        if (x->status != XS_OK) hc->errors++;
        if (x->cb) x->cb(x);
    }
}

static int ohci_irq(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    u32 st = RD(o, HCINTSTS) & RD(o, HCINTEN) & 0x7F;
    if (!st || st == 0x7F) return 0;
    WR(o, HCINTSTS, st);
    hc->pending = 1;
    return 1;
}

static int ohci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ohci *o = hc->priv;
    struct opipe *op = x->pipe->hcpriv;
    if (!op) return XS_ERROR;
    x->next = NULL;
    if (op->active) {
        if (op->qtail) op->qtail->next = x; else op->qhead = x;
        op->qtail = x;
        return 0;
    }
    return start_xfer(o, op, x);
}

static void ohci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ohci *o = hc->priv;
    struct opipe *op = x->pipe->hcpriv;
    struct usb_xfer **pp, *done = NULL;
    if (!op) return;
    for (pp = &op->qhead; *pp; pp = &(*pp)->next) {
        if (*pp == x) {
            *pp = x->next;
            if (op->qtail == x) {
                struct usb_xfer *t = op->qhead;
                while (t && t->next) t = t->next;
                op->qtail = t;
            }
            x->status = XS_CANCEL;
            return;
        }
    }
    if (op->active == x) {
        ed_flush(o, op, NULL);
        op->ed->head &= ~3UL;
        finish(o, op, XS_CANCEL, &done);
    }
}

static void ed_info(struct opipe *op)
{
    struct usb_pipe *p = op->p;
    u32 d = p->type == EP_CTRL ? 0 : ((p->ep & 0x80) ? 2UL : 1UL);
    op->ed->info = (op->ed->info & ED_SKIP) | p->dev->addr | ((u32)(p->ep & 0x0F) << 7) | (d << 11) |
                   (p->dev->speed == USB_LS ? ED_LS : 0) | ((u32)(p->maxpkt ? p->maxpkt : 8) << 16);
}

static int ohci_pipe_open(struct usb_hc *hc, struct usb_pipe *p)
{
    struct ohci *o = hc->priv;
    struct opipe *op;
    struct oed *a;
    if (p->type == EP_ISO) {
        p->hcpriv = NULL;           /* scheduled by the isochronous code */
        return 0;
    }
    op = kmalloc(sizeof(*op));
    if (!op) return -1;
    op->p = p;
    op->ed = kmalloc_align(sizeof(struct oed), 16);
    op->dummy = new_td();
    op->setup = kmalloc(16);
    op->listtype = p->type == EP_CTRL ? 0 : p->type == EP_BULK ? 1 : 2;
    ed_info(op);
    op->ed->tail = vphys(op->dummy);
    op->ed->head = vphys(op->dummy);
    a = o->anchor[op->listtype];
    op->ed->next = a->next;
    a->next = vphys(op->ed);
    op->next = o->pipes;
    o->pipes = op;
    p->hcpriv = op;
    return 0;
}

static void ohci_pipe_close(struct usb_hc *hc, struct usb_pipe *p)
{
    struct ohci *o = hc->priv;
    struct opipe *op = p->hcpriv, **pp;
    struct oed *a;
    struct usb_xfer *x, *done = NULL;
    if (!op) return;
    while ((x = op->qhead) != NULL) { op->qhead = x->next; x->status = XS_CANCEL; }
    op->qtail = NULL;
    op->ed->info |= ED_SKIP;
    wait_frame(o);
    if (op->active) finish(o, op, XS_CANCEL, &done);
    a = o->anchor[op->listtype];
    while (a) {
        if (a->next == vphys(op->ed)) { a->next = op->ed->next; break; }
        a = a->next ? pvirt(a->next) : NULL;
    }
    wait_frame(o);
    for (pp = &o->pipes; *pp; pp = &(*pp)->next) if (*pp == op) { *pp = op->next; break; }
    kfree(op->dummy);
    kfree(op->ed);
    kfree(op->setup);
    kfree(op);
    p->hcpriv = NULL;
}

static int ohci_dev_init(struct usb_hc *hc, struct usb_dev *d)
{
    if (ohci_pipe_open(hc, &d->ep0)) return -1;
    d->ep0.open = 1;
    return 0;
}

static int ohci_dev_update(struct usb_hc *hc, struct usb_dev *d)
{
    struct opipe *op = d->ep0.hcpriv;
    (void)hc;
    if (op) ed_info(op);
    return 0;
}

/* ---- root hub ---- */
static int rh_nports(struct usb_hub *h) { return ((struct ohci *)h->hc->priv)->nports; }

static u32 rh_status(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    u32 v = RD(o, HCRHPORT + (port - 1) * 4), s = 0;
    if (v & 0x0001) s |= PS_CONNECT;
    if (v & 0x0002) s |= PS_ENABLE;
    if (v & 0x0010) s |= PS_RESET;
    if (v & 0x0100) s |= PS_POWER;
    if (v & 0x0008) s |= PS_OVERCUR;
    if (v & 0x10000UL) s |= PS_CCHANGE;
    if (v & 0x1E0000UL) s |= PS_OTHERCHG;
    s |= (u32)((v & 0x0200) ? USB_LS : USB_FS) << PS_SPEEDSH;
    return s;
}

static void rh_clear(struct usb_hub *h, int port, u32 st)
{
    struct ohci *o = h->hc->priv;
    (void)st;
    WR(o, HCRHPORT + (port - 1) * 4, RD(o, HCRHPORT + (port - 1) * 4) & 0x1F0000UL);
}

static int rh_reset(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    int r = HCRHPORT + (port - 1) * 4, i;
    WR(o, r, 0x0010);
    for (i = 0; i < 100; i++) {
        mdelay(2);
        if (RD(o, r) & 0x100000UL) break;
    }
    WR(o, r, 0x100000UL);
    mdelay(20);
    return (RD(o, r) & 0x0002) ? 0 : -1;
}

static void rh_disable(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    WR(o, HCRHPORT + (port - 1) * 4, 0x0001);
}

/* the BIOS's legacy USB setup (SMM owns the controller while InterruptRouting is set) as
   found the first time; given back when the controller stops */
struct obios {
    u32 phys, ctrl, inten, hcca, chead, bhead, fmint, perstart, lsthr, hce;
    u8 used, emul;
};
static struct obios obios[8];

static struct obios *obios_of(u32 phys, int add)
{
    int i;
    for (i = 0; i < 8; i++)
        if (obios[i].used && obios[i].phys == phys) return &obios[i];
    if (!add) return NULL;
    for (i = 0; i < 8; i++)
        if (!obios[i].used) {
            obios[i].used = 1;
            obios[i].phys = phys;
            return &obios[i];
        }
    return NULL;
}

static void obios_save(u8 *r, u32 phys)
{
    struct obios *b;
    if (obios_of(phys, 0) || !(RD32(r + 0x04) & 0x100)) return;
    b = obios_of(phys, 1);
    if (!b) return;
    b->ctrl = RD32(r + 0x04);
    b->inten = RD32(r + 0x10);
    b->hcca = RD32(r + 0x18);
    b->chead = RD32(r + 0x20);
    b->bhead = RD32(r + 0x28);
    b->fmint = RD32(r + 0x34);
    b->perstart = RD32(r + 0x40);
    b->lsthr = RD32(r + 0x44);
    b->emul = (u8)((RD32(r + 0x00) & 0x100) != 0);     /* legacy emulation registers */
    b->hce = b->emul ? RD32(r + 0x100) : 0;
}

/* the controller (stopped and reset) back to SMM: its schedule, InterruptRouting */
static void obios_restore(u8 *r, u32 phys)
{
    struct obios *b = obios_of(phys, 0);
    if (!b) return;
    WR32(r + 0x18, b->hcca);
    WR32(r + 0x20, b->chead);
    WR32(r + 0x28, b->bhead);
    WR32(r + 0x34, b->fmint | 0x80000000UL);
    WR32(r + 0x40, b->perstart);
    WR32(r + 0x44, b->lsthr);
    WR32(r + 0x0C, 0xC000007FUL);
    WR32(r + 0x10, b->inten);
    WR32(r + 0x04, b->ctrl | 0x100);
    if (b->emul) WR32(r + 0x100, b->hce);
    kprintf("ohci: controller back to the BIOS\n");
}

/* SMM (BIOS legacy USB) owns the controller while InterruptRouting is set */
static void ohci_handoff(u8 *r)
{
    int i;
    if (!(RD32(r + 0x04) & 0x100)) return;
    WR32(r + 0x10, 0x40000000UL);           /* ownership change interrupt enabled */
    WR32(r + 0x08, 0x08);                   /* OwnershipChangeRequest */
    for (i = 0; i < 1000 && (RD32(r + 0x04) & 0x100); i++) mdelay(1);
    if (RD32(r + 0x04) & 0x100) kprintf("ohci: BIOS did not release the controller, taking it anyway\n");
}

void ohci_early(struct pcidev *p)
{
    u32 phys = pci_bar_mem(p, 0, NULL);
    u8 *r;
    if (!phys) return;
    pci_enable(p, 1, 0, 1);
    r = usb_map(phys, 0x1000);
    if (!r) return;
    obios_save(r, phys);
    ohci_handoff(r);
    WR32(r + 0x14, 0xC000007FUL);           /* HcInterruptDisable: all */
}

static int ohci_start(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    u32 ctrl, fi, rha;
    int i;
    obios_save(o->r, pci_bar_mem(&hc->pci, 0, NULL));
    ohci_handoff(o->r);
    ctrl = RD(o, HCCTRL);
    WR(o, HCINTDIS, 0xC000007FUL);
    fi = RD(o, HCFMINT) & 0x3FFF;
    if (!fi) fi = 11999;
    WR(o, HCCTRL, 0);                   /* USB reset state */
    mdelay(50);
    WR(o, HCCMD, 0x01);
    for (i = 0; i < 100 && (RD(o, HCCMD) & 1); i++) udelay(10);
    o->hcca = kmalloc_align(256, 256);
    for (i = 0; i < 4; i++) {
        o->anchor[i] = kmalloc_align(sizeof(struct oed), 16);
        o->anchor[i]->info = ED_SKIP;
    }
    o->anchor[2]->next = vphys(o->anchor[3]);  /* interrupt EDs, then isochronous EDs */
    for (i = 0; i < 32; i++) *(u32 *)(o->hcca + i * 4) = vphys(o->anchor[2]);
    WR(o, HCHCCA, vphys(o->hcca));
    WR(o, HCCTRLHEAD, vphys(o->anchor[0]));
    WR(o, HCBULKHEAD, vphys(o->anchor[1]));
    WR(o, HCFMINT, ((((fi - 210) * 6) / 7) << 16) | fi | 0x80000000UL);
    WR(o, HCPERSTART, (fi * 9) / 10);
    WR(o, HCLSTHR, 0x628);
    WR(o, HCINTSTS, 0x7F);
    WR(o, HCINTEN, 0x80000000UL | 0x02 | 0x10 | 0x40);      /* MIE, WDH, UE, RHSC */
    WR(o, HCCTRL, 0x80 | 0x3C | 3);                          /* operational, PLE IE CLE BLE, 4:1 */
    mdelay(5);
    rha = RD(o, HCRHDA);
    o->nports = rha & 0xFF;
    if (o->nports > 15) o->nports = 15;
    WR(o, HCRHDA, (rha & ~0x00001000UL) | 0x00000200UL);    /* no overcurrent protection reporting changes */
    WR(o, HCRHDA, rha);
    WR(o, HCRHSTS, 0x10000UL);                               /* set global power */
    for (i = 0; i < o->nports; i++) WR(o, HCRHPORT + i * 4, 0x0100);
    mdelay(MAX(((rha >> 24) & 0xFF) * 2, 20));
    usb_new_hub(hc, NULL, &ohci_hubops, o->nports);
    usb_irq_setup(hc);
    kprintf("ohci: %s rev %02lx irq %d, %d ports\n", hc->name, RD(o, HCREV) & 0xFF, hc->irq, o->nports);
    return 0;
}

static void ohci_stop(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    int i;
    WR(o, HCINTDIS, 0xC000007FUL);
    WR(o, HCCTRL, 0);
    mdelay(2);
    WR(o, HCCMD, 0x01);
    mdelay(1);
    while (o->pipes) {
        struct opipe *op = o->pipes;
        o->pipes = op->next;
        free_chain(op->first, op->dummy);
        kfree(op->dummy);
        kfree(op->ed);
        kfree(op->setup);
        kfree(op);
    }
    for (i = 0; i < 4; i++) kfree(o->anchor[i]);
    obios_restore(o->r, pci_bar_mem(&hc->pci, 0, NULL));
    kfree(o->hcca);
    kfree(o);
}

/* ---- isochronous: an ED behind the interrupt EDs, one ITD per packet ---- */
struct oitd {                   /* 32 bytes, 32-byte aligned */
    volatile u32 info, bp0, next, be;
    volatile u16 psw[8];
};

struct oiso {
    struct oed *ed;
    struct oitd *td;            /* npkt; one is always the dummy at the tail */
    u32 *frame;
    int head, count, tail;      /* tail: index of the dummy TD */
    u32 next;
};

static u32 frame_now(struct ohci *o)
{
    u16 f = (u16)(RD(o, HCFMNUM) & 0xFFFF);
    u32 d = (u32)(u16)(f - (u16)o->lastfr);
    o->lastfr += d;
    return o->lastfr;
}

static int ohci_iso_start(struct usb_hc *hc, struct usb_iso *s)
{
    struct ohci *o = hc->priv;
    struct usb_pipe *p = s->pipe;
    struct oiso *oi = kmalloc(sizeof(*oi));
    if (!oi) return -1;
    oi->ed = kmalloc_align(sizeof(struct oed), 16);
    oi->td = kmalloc_align((u32)s->npkt * sizeof(struct oitd), 32);
    oi->frame = kmalloc((u32)s->npkt * 4);
    if (!oi->ed || !oi->td || !oi->frame) return -1;
    oi->head = oi->count = oi->tail = 0;
    oi->ed->info = p->dev->addr | ((u32)(p->ep & 0x0F) << 7) | ((s->in ? 2UL : 1UL) << 11) | (1UL << 15) |
                   ((u32)s->pktsize << 16);
    oi->ed->head = oi->ed->tail = vphys(&oi->td[0]);
    oi->ed->next = o->anchor[3]->next;
    o->anchor[3]->next = vphys(oi->ed);
    oi->next = frame_now(o) + 2;
    s->hcpriv = oi;
    return 0;
}

static void ohci_iso_service(struct usb_hc *hc, struct usb_iso *s)
{
    struct ohci *o = hc->priv;
    struct oiso *oi = s->hcpriv;
    u32 now = frame_now(o), step = s->period_us / 1000, ahead = s->ahead_us / 1000;
    int n = s->npkt;
    if (!step) step = 1;
    while (oi->count) {
        struct oitd *t = &oi->td[oi->head];
        u32 cc = t->info >> 28;
        if (cc == 0xF) {
            if ((s32)(now - oi->frame[oi->head]) < 64) break;
            /* never processed (halted ED): drop everything queued */
            oi->ed->info |= ED_SKIP;
            wait_frame(o);
            oi->ed->head = vphys(&oi->td[oi->tail]);
            oi->ed->info &= ~ED_SKIP;
            s->missed += (u32)oi->count;
            oi->head = oi->tail;
            oi->count = 0;
            break;
        } else {
            u32 psw = t->psw[0], pcc = psw >> 12;
            if (pcc && pcc != 9) s->errors++;
            else if (s->in) s->done(s, s->buf + (u32)oi->head * s->pktsize, (int)(psw & 0x7FF));
        }
        s->packets++;
        oi->head = (oi->head + 1) % n;
        oi->count--;
    }
    if ((s32)(oi->next - (now + 1)) < 0) {
        if (oi->count == 0 && s->packets) usb_iso_late(s);
        oi->next = now + 2;
    }
    while (oi->count < n - 1 && (s32)(oi->next - (now + ahead)) < 0) {
        int k = oi->tail, nk = (oi->tail + 1) % n, len;
        struct oitd *t = &oi->td[k];
        u8 *b = s->buf + (u32)k * s->pktsize;
        u32 ph;
        /* the dummy at the tail becomes the packet, the next free TD the new dummy */
        len = s->in ? s->pktsize : s->fill(s, b, s->pktsize);
        if (len <= 0) len = 1;
        ph = vphys(b);
        memset(&oi->td[nk], 0, sizeof(struct oitd));
        t->bp0 = ph & ~0xFFFUL;
        t->be = ph + (u32)len - 1;
        t->psw[0] = (u16)(0xE000 | (ph & 0xFFF));
        t->next = vphys(&oi->td[nk]);
        t->info = (oi->next & 0xFFFF) | (7UL << 21) | (0xFUL << 28);       /* FC 0: one frame */
        oi->frame[k] = oi->next;
        oi->tail = nk;
        oi->ed->tail = vphys(&oi->td[nk]);
        oi->count++;
        oi->next += step;
    }
}

static void ohci_iso_stop(struct usb_hc *hc, struct usb_iso *s)
{
    struct ohci *o = hc->priv;
    struct oiso *oi = s->hcpriv;
    struct oed *a = o->anchor[3];
    if (!oi) return;
    oi->ed->info |= ED_SKIP;
    wait_frame(o);
    while (a) {
        if (a->next == vphys(oi->ed)) {
            a->next = oi->ed->next;
            break;
        }
        a = a->next ? pvirt(a->next) : NULL;
    }
    wait_frame(o);
    kfree(oi->ed);
    kfree(oi->td);
    kfree(oi->frame);
    kfree(oi);
    s->hcpriv = NULL;
}

static struct usb_hubops ohci_hubops = { rh_nports, rh_status, rh_clear, rh_reset, rh_disable, NULL };

static struct usb_hcops ohci_ops = {
    "OHCI", HC_OHCI, ohci_start, ohci_stop, ohci_dev_init, ohci_dev_update, NULL,
    ohci_pipe_open, ohci_pipe_close, ohci_submit, ohci_cancel, ohci_poll, ohci_irq, NULL, NULL,
    ohci_iso_start, ohci_iso_stop, ohci_iso_service
};

int ohci_attach(struct pcidev *p)
{
    struct ohci *o;
    struct usb_hc *hc;
    u32 phys = pci_bar_mem(p, 0, NULL);
    if (!phys) return -1;
    pci_enable(p, 1, 0, 1);
    o = kmalloc(sizeof(*o));
    o->r = usb_map(phys, 0x1000);
    if (!o->r) return -1;
    hc = usb_add_hc(&ohci_ops, p, o);
    if (!hc) return -1;
    o->hc = hc;
    hc->mmio = o->r;
    return ohci_start(hc);
}
