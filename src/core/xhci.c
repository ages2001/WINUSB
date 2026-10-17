/* xhci.c - xHCI host controller driver (USB 3.x / 2.0 / 1.1 devices, bulk streams for UAS) */
#include "usb.h"

/* TRB types */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_EN_SLOT     9
#define TRB_DIS_SLOT    10
#define TRB_ADDR_DEV    11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_RESET_EP    14
#define TRB_STOP_EP     15
#define TRB_SET_DEQ     16
#define TRB_ISOCH       5
#define TRB_EV_XFER     32
#define TRB_EV_CMD      33
#define TRB_EV_PORT     34

#define TRB_CYCLE   (1UL << 0)
#define TRB_ENT     (1UL << 1)
#define TRB_ISP     (1UL << 2)
#define TRB_CH      (1UL << 4)
#define TRB_IOC     (1UL << 5)
#define TRB_IDT     (1UL << 6)
#define TRB_TYPE(t) ((u32)(t) << 10)

#define RING_TRBS   256
#define ISO_TRBS    1024
#define EVT_TRBS    1024

/* operational registers */
#define USBCMD      0x00
#define USBSTS      0x04
#define PAGESIZE    0x08
#define DNCTRL      0x14
#define CRCR        0x18
#define DCBAAP      0x30
#define CONFIG      0x38
#define PORTSC      0x400

#define PORT_RO     0x41003C09UL
#define PORT_RWS    0x0E00C3E0UL
#define PORT_CHG    0x00FE0000UL

struct trb { volatile u32 p0, p1, st, ctl; };

struct ring {
    struct trb *t;
    u32 phys;
    u32 enq;
    u32 cycle;
    u32 n;                          /* TRBs, the last one is the link */
};

struct xiso {
    struct usb_iso *s;
    u16 *slot_of;                   /* ring index -> packet slot */
    u32 *trb;                       /* packet slot -> TRB address */
    u16 *actual;
    u8 *done;                       /* 1 ok, 2 error */
    u32 *qtime;                     /* plat_ms() when queued */
    int head, count;
};

struct xpipe {
    struct usb_pipe *p;
    struct xdev *xd;
    u32 dci;
    struct ring ring;               /* stream-less ring */
    u8 *sca;                        /* stream context array (UAS) */
    struct ring sring[2];           /* rings for stream 1 (index 1) */
    int streams;
    struct usb_xfer *active[2];     /* [0] plain or stream 0 unused, [1] stream 1 */
    struct usb_xfer *qhead, *qtail;
    u32 trb_phys[16];
    u32 trb_len[16];
    int ntrb[2];
    u32 sumlen[2];
    int halted;
    struct xiso *iso;
};

struct xdev {
    struct usb_dev *d;
    int slot;
    u8 *out;                        /* output device context */
    u8 *in;                         /* input context */
    struct xpipe *pipes[32];
    u32 ctx_entries;
};

struct xhci {
    struct usb_hc *hc;
    u8 *cap, *op, *rt, *db;
    u32 csz;
    int maxslots, maxports;
    u32 *dcbaa;
    struct ring cmd;
    struct trb *evt;
    u32 evt_phys, evt_deq, evt_cycle;
    u32 *erst;
    u8 portmajor[MAXPORTS + 1];
    u8 portpsiv[MAXPORTS + 1];      /* port speed ID of the device on each root port */
    struct xdev *slots[64];
    /* command completion */
    u32 cmd_phys;
    volatile int cmd_done;
    u32 cmd_cc, cmd_slot;
    void **scratch;
    int nscratch;
    int maxpsa;
    u32 maplen;         /* mapped bytes of the register BAR */
    u32 erdp_age;               /* event polls since the dequeue pointer was written */
};

static struct usb_hcops xhci_ops;
static struct usb_hubops xhci_hubops;

#define ORD(x, r) RD32((x)->op + (r))
#define OWR(x, r, v) WR32((x)->op + (r), (v))

static int ring_init_n(struct ring *r, u32 n)
{
    r->t = kmalloc_align(n * 16, 4096);
    if (!r->t) return -1;
    r->n = n;
    r->phys = vphys(r->t);
    r->enq = 0;
    r->cycle = 1;
    r->t[n - 1].p0 = r->phys;
    r->t[n - 1].p1 = 0;
    r->t[n - 1].ctl = TRB_TYPE(TRB_LINK) | TRB_ENT;
    return 0;
}

static int ring_init(struct ring *r)
{
    return ring_init_n(r, RING_TRBS);
}

static void ring_free(struct ring *r)
{
    if (r->t) kfree(r->t);
    r->t = NULL;
}

/* enqueue one TRB, returns its physical address */
static u32 ring_put(struct ring *r, u32 p0, u32 p1, u32 st, u32 ctl)
{
    struct trb *t = &r->t[r->enq];
    u32 ph = r->phys + r->enq * 16;
    t->p0 = p0;
    t->p1 = p1;
    t->st = st;
    t->ctl = (ctl & ~TRB_CYCLE) | r->cycle;
    r->enq++;
    if (r->enq == r->n - 1) {
        struct trb *l = &r->t[r->n - 1];
        l->ctl = (l->ctl & ~TRB_CYCLE) | r->cycle | (ctl & TRB_CH);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return ph;
}

static u32 ring_deq(struct ring *r)
{
    return (r->phys + r->enq * 16) | r->cycle;
}

static void ring_db(struct xhci *x, int slot, u32 val)
{
    WR32(x->db + slot * 4, val);
}

static u8 *ctx(struct xhci *x, u8 *base, int idx)
{
    return base + idx * x->csz;
}

static void handle_event(struct xhci *x, struct trb *e, struct usb_xfer **done);

static void process_events(struct xhci *x, struct usb_xfer **done)
{
    int n = 0;
    for (;;) {
        struct trb *e = &x->evt[x->evt_deq];
        if ((e->ctl & 1) != x->evt_cycle) break;
        handle_event(x, e, done);
        x->evt_deq++;
        if (x->evt_deq == EVT_TRBS) {
            x->evt_deq = 0;
            x->evt_cycle ^= 1;
        }
        n++;
    }
    /* the dequeue pointer again now and then: a BIOS that still polls the controller from
       its timer interrupt (SeaBIOS with a USB keyboard) may have written its own */
    if (n || ++x->erdp_age >= 64) {
        x->erdp_age = 0;
        WR32(x->rt + 0x20 + 0x18, (x->evt_phys + x->evt_deq * 16) | 8);
        WR32(x->rt + 0x20 + 0x1C, 0);
    }
}

static void complete_list(struct usb_hc *hc, struct usb_xfer *done)
{
    struct usb_xfer *t;
    while ((t = done) != NULL) {
        done = t->next;
        t->next = NULL;
        if (t->status != XS_OK) hc->errors++;
        if (t->cb) t->cb(t);
    }
}

static int command(struct xhci *x, u32 p0, u32 p1, u32 st, u32 ctl, u32 *slot_out)
{
    u32 t0;
    struct usb_xfer *done = NULL;
    x->cmd_done = 0;
    x->cmd_phys = ring_put(&x->cmd, p0, p1, st, ctl);
    x->erdp_age = 64;                                   /* our dequeue pointer is in place */
    process_events(x, &done);
    ring_db(x, 0, 0);
    t0 = plat_ms();
    while (!x->cmd_done) {
        process_events(x, &done);
        if (plat_ms() - t0 > 2000) {
            kprintf("xhci: command %lu timeout\n", (ctl >> 10) & 0x3F);
            complete_list(x->hc, done);
            return -1;
        }
        cpu_pause();
    }
    complete_list(x->hc, done);
    if (slot_out) *slot_out = x->cmd_slot;
    if (x->cmd_cc != 1) {
        kprintf("xhci: command %lu completion code %lu\n", (ctl >> 10) & 0x3F, x->cmd_cc);
        return -(int)x->cmd_cc;
    }
    return 0;
}

static int xfer_cc_status(u32 cc)
{
    switch (cc) {
    case 1: case 13: return XS_OK;
    case 6: return XS_STALL;
    case 3: return XS_BABBLE;
    }
    return XS_ERROR;
}

static void recover_halt(struct xhci *x, struct xpipe *xp, int s)
{
    struct ring *r = xp->streams ? &xp->sring[1] : &xp->ring;
    u32 deq = ring_deq(r);
    command(x, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((u32)xp->xd->slot << 24) | (xp->dci << 16), NULL);
    command(x, deq | (xp->streams ? 2 : 0), 0, xp->streams ? ((u32)s << 16) : 0,
            TRB_TYPE(TRB_SET_DEQ) | ((u32)xp->xd->slot << 24) | (xp->dci << 16), NULL);
    xp->halted = 0;
}

static int start_xfer(struct xhci *x, struct xpipe *xp, struct usb_xfer *t);

static void finish(struct xhci *x, struct xpipe *xp, int s, int status, struct usb_xfer **done)
{
    struct usb_xfer *t = xp->active[s];
    xp->active[s] = NULL;
    t->status = status;
    t->next = *done;
    *done = t;
    if (!xp->active[0] && !xp->active[1] && xp->qhead && !xp->halted) {
        struct usb_xfer *n = xp->qhead;
        xp->qhead = n->next;
        if (!xp->qhead) xp->qtail = NULL;
        n->next = NULL;
        if (start_xfer(x, xp, n)) {
            n->status = XS_ERROR;
            n->next = *done;
            *done = n;
        }
    }
}

/* isochronous transfer event: one per packet (TD of one TRB) */
static void iso_event(struct xpipe *xp, u32 ptr, u32 cc, u32 resid)
{
    struct xiso *xi = xp->iso;
    u32 idx;
    int k;
    if (cc == 14 || cc == 15) {                     /* ring underrun / overrun: we were late */
        if (xi->s->packets) usb_iso_late(xi->s);
        return;
    }
    if (ptr < xp->ring.phys || ptr >= xp->ring.phys + xp->ring.n * 16) return;
    idx = (ptr - xp->ring.phys) / 16;
    k = xi->slot_of[idx];
    if (k >= xi->s->npkt || xi->trb[k] != ptr || xi->done[k]) return;
    if (cc == 1 || cc == 13) {
        xi->done[k] = 1;
        xi->actual[k] = (u16)(xi->actual[k] - MIN(resid, (u32)xi->actual[k]));
    } else {
        xi->done[k] = 2;
    }
}

static void handle_event(struct xhci *x, struct trb *e, struct usb_xfer **done)
{
    u32 type = (e->ctl >> 10) & 0x3F, cc = e->st >> 24;
    if (type == TRB_EV_CMD) {
        if (e->p0 == x->cmd_phys) {
            x->cmd_cc = cc;
            x->cmd_slot = e->ctl >> 24;
            x->cmd_done = 1;
        }
        return;
    }
    if (type == TRB_EV_XFER) {
        u32 slot = e->ctl >> 24, dci = (e->ctl >> 16) & 0x1F, ptr = e->p0, resid = e->st & 0xFFFFFF;
        struct xdev *xd = slot < 64 ? x->slots[slot] : NULL;
        struct xpipe *xp = xd ? xd->pipes[dci] : NULL;
        struct usb_xfer *t;
        int s, i, k;
        if (!xp) return;
        if (xp->iso) {
            iso_event(xp, ptr, cc, resid);
            return;
        }
        s = xp->streams ? 1 : 0;
        t = xp->active[s];
        if (!t) return;
        /* locate the TRB in this transfer */
        for (k = -1, i = 0; i < xp->ntrb[s]; i++) if (xp->trb_phys[i] == ptr) k = i;
        if (k < 0) return;
        if (cc == 1 || cc == 13) {
            u32 before = 0;
            for (i = 0; i < k; i++) before += xp->trb_len[i];
            if (xp->p->type == EP_CTRL) {
                /* data stage event records the length, status stage completes */
                if (xp->trb_len[k] && k == 1) {
                    t->actual = xp->trb_len[k] - resid;
                    if (cc == 13 && k < xp->ntrb[s] - 1) return;
                }
                if (k == xp->ntrb[s] - 1) finish(x, xp, s, XS_OK, done);
                return;
            }
            t->actual = before + xp->trb_len[k] - resid;
            if (cc == 13 || k == xp->ntrb[s] - 1) finish(x, xp, s, XS_OK, done);
            return;
        }
        if (cc == 26 || cc == 27) return;           /* stopped: cancel path handles it */
        {
            u32 before = 0;
            for (i = 0; i < k; i++) before += xp->trb_len[i];
            if (xp->p->type != EP_CTRL) t->actual = before + xp->trb_len[k] - resid;
        }
        if (cc == 6 || cc == 3 || cc == 4) xp->halted = 1;
        DBG("xhci: slot %lu ep %lu completion code %lu\n", slot, dci, cc);
        finish(x, xp, s, xfer_cc_status(cc), done);
        return;
    }
}

static int start_xfer(struct xhci *x, struct xpipe *xp, struct usb_xfer *t)
{
    struct usb_pipe *p = xp->p;
    int s = (xp->streams && t->stream) ? 1 : 0;
    struct ring *r = s ? &xp->sring[1] : &xp->ring;
    u32 len = t->len, off = 0, mps = p->maxpkt ? p->maxpkt : 8;
    int n = 0;
    if (xp->halted) recover_halt(x, xp, s);
    if (p->type == EP_CTRL) {
        u32 *sp = (u32 *)t->setup;
        u32 trt = len ? (t->in ? 3UL : 2UL) : 0;
        xp->trb_phys[n] = ring_put(r, sp[0], sp[1], 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
        xp->trb_len[n++] = 0;
        if (len) {
            xp->trb_phys[n] = ring_put(r, vphys(t->buf), 0, len, TRB_TYPE(TRB_DATA) | TRB_ISP | (t->in ? (1UL << 16) : 0));
            xp->trb_len[n++] = len;
        }
        xp->trb_phys[n] = ring_put(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((t->in && len) ? 0 : (1UL << 16)));
        xp->trb_len[n++] = 0;
        t->actual = len;            /* a short data stage event lowers it */
    } else {
        do {
            u32 ph = vphys(t->buf + off);
            u32 chunk = MIN(65536UL - (ph & 0xFFFF), len - off);
            u32 left, tdsize;
            if (n >= 15) return XS_ERROR;
            left = len - off - chunk;
            tdsize = (left + mps - 1) / mps;
            if (tdsize > 31) tdsize = 31;
            xp->trb_len[n] = chunk;
            xp->trb_phys[n] = ring_put(r, len ? ph : 0, 0, chunk | (tdsize << 17),
                                       TRB_TYPE(TRB_NORMAL) | TRB_ISP | (left ? TRB_CH : TRB_IOC));
            n++;
            off += chunk;
        } while (off < len);
    }
    xp->ntrb[s] = n;
    xp->active[s] = t;
    t->hcpriv = xp;
    ring_db(x, xp->xd->slot, xp->dci | (s ? ((u32)t->stream << 16) : 0));
    return 0;
}

static void xhci_poll(struct usb_hc *hc)
{
    struct xhci *x = hc->priv;
    struct usb_xfer *done = NULL;
    u32 st = ORD(x, USBSTS), im = RD32(x->rt + 0x20);
    if (st & 0x08) OWR(x, USBSTS, 0x08);
    if (im & 1) WR32(x->rt + 0x20, im | 1);
    process_events(x, &done);
    complete_list(hc, done);
}

static int xhci_irq(struct usb_hc *hc)
{
    struct xhci *x = hc->priv;
    u32 st = ORD(x, USBSTS), im = RD32(x->rt + 0x20);
    if (st == 0xFFFFFFFFUL || (!(st & 0x08) && !(im & 1))) return 0;
    OWR(x, USBSTS, 0x08);
    WR32(x->rt + 0x20, im | 1);
    hc->pending = 1;
    return 1;
}

static int xhci_submit(struct usb_hc *hc, struct usb_xfer *t)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = t->pipe->hcpriv;
    int s;
    if (!xp) return XS_ERROR;
    s = (xp->streams && t->stream) ? 1 : 0;
    t->next = NULL;
    if (xp->streams) {
        if (xp->active[s]) return XS_ERROR;
        return start_xfer(x, xp, t);
    }
    if (xp->active[0]) {
        if (xp->qtail) xp->qtail->next = t; else xp->qhead = t;
        xp->qtail = t;
        return 0;
    }
    return start_xfer(x, xp, t);
}

static void xhci_cancel(struct usb_hc *hc, struct usb_xfer *t)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = t->pipe->hcpriv;
    struct usb_xfer **pp, *done = NULL;
    int s;
    if (!xp) return;
    for (pp = &xp->qhead; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            if (xp->qtail == t) {
                struct usb_xfer *q = xp->qhead;
                while (q && q->next) q = q->next;
                xp->qtail = q;
            }
            t->status = XS_CANCEL;
            return;
        }
    }
    for (s = 0; s < 2; s++) {
        if (xp->active[s] != t) continue;
        command(x, 0, 0, 0, TRB_TYPE(TRB_STOP_EP) | ((u32)xp->xd->slot << 24) | (xp->dci << 16), NULL);
        {
            struct ring *r = s ? &xp->sring[1] : &xp->ring;
            command(x, ring_deq(r) | (s ? 2 : 0), 0, s ? ((u32)t->stream << 16) : 0,
                    TRB_TYPE(TRB_SET_DEQ) | ((u32)xp->xd->slot << 24) | (xp->dci << 16), NULL);
        }
        if (xp->active[s] == t) finish(x, xp, s, XS_CANCEL, &done);
    }
}

static u32 port_speed_id(int speed)
{
    switch (speed) {
    case USB_FS: return 1;
    case USB_LS: return 2;
    case USB_HS: return 3;
    case USB_SS: return 4;
    }
    return 5;
}

static void fill_slot(struct xhci *x, struct xdev *xd, u8 *sc)
{
    struct usb_dev *d = xd->d;
    u32 *s = (u32 *)sc;
    /* on a root port the speed ID that PORTSC reported (USB 3.1/3.2 and USB4 controllers
       may number the SuperSpeedPlus rates in their own way), behind hubs the defaults */
    u32 sid = port_speed_id(d->speed);
    if (!(d->route & 0xFFFFF) && d->rootport <= MAXPORTS && x->portpsiv[d->rootport]) sid = x->portpsiv[d->rootport];
    s[0] = (d->route & 0xFFFFF) | (sid << 20) | (xd->ctx_entries << 27);
    s[1] = (u32)d->rootport << 16;
    s[2] = 0;
    s[3] = 0;
    if (d->speed < USB_HS && d->tthub && d->tthub->hcpriv) {
        struct xdev *tt = d->tthub->hcpriv;
        s[2] = (u32)tt->slot | ((u32)d->ttport << 8);
        if (d->tthub->ownhub && d->tthub->ownhub->mtt) s[0] |= 1UL << 25;
    }
    if (d->ownhub) {
        struct usb_hub *h = d->ownhub;
        s[0] |= 1UL << 26;
        s[1] |= (u32)h->nports << 24;
        if (d->speed == USB_HS) {
            s[2] |= (u32)(h->ttthink & 3) << 16;
            if (h->mtt) s[0] |= 1UL << 25;
        }
    }
    (void)x;
}

static void fill_ep0(struct xhci *x, struct xdev *xd, u8 *ec)
{
    u32 *e = (u32 *)ec;
    struct xpipe *xp = xd->pipes[1];
    (void)x;
    e[0] = 0;
    e[1] = (3UL << 1) | (4UL << 3) | ((u32)xd->d->ep0.maxpkt << 16);
    e[2] = ring_deq(&xp->ring);
    e[3] = 0;
    e[4] = 8;
}

static int xhci_dev_init(struct usb_hc *hc, struct usb_dev *d)
{
    struct xhci *x = hc->priv;
    struct xdev *xd;
    struct xpipe *xp;
    u32 slot = 0;
    int r;
    if (command(x, 0, 0, 0, TRB_TYPE(TRB_EN_SLOT), &slot) || !slot || slot >= 64) return -1;
    xd = kmalloc(sizeof(*xd));
    xd->d = d;
    xd->slot = (int)slot;
    xd->out = kmalloc_align(32 * x->csz, 64);
    xd->in = kmalloc_align(33 * x->csz, 64);
    xd->ctx_entries = 1;
    x->dcbaa[slot * 2] = vphys(xd->out);
    x->dcbaa[slot * 2 + 1] = 0;
    x->slots[slot] = xd;
    d->hcpriv = xd;
    d->slot = (u8)slot;
    xp = kmalloc(sizeof(*xp));
    xp->p = &d->ep0;
    xp->xd = xd;
    xp->dci = 1;
    ring_init(&xp->ring);
    xd->pipes[1] = xp;
    d->ep0.hcpriv = xp;
    d->ep0.open = 1;
    memset(xd->in, 0, 33 * x->csz);
    *(u32 *)(xd->in + 4) = 3;                       /* add slot + ep0 */
    fill_slot(x, xd, ctx(x, xd->in, 1));
    fill_ep0(x, xd, ctx(x, xd->in, 2));
    r = command(x, vphys(xd->in), 0, 0, TRB_TYPE(TRB_ADDR_DEV) | (slot << 24), NULL);
    if (r) {
        kprintf("xhci: address device failed (%d)\n", r);
        return -1;
    }
    d->addr = (u8)(*(u32 *)(ctx(x, xd->out, 0) + 12) & 0xFF);
    mdelay(2);
    return 0;
}

static int xhci_dev_update(struct usb_hc *hc, struct usb_dev *d)
{
    struct xhci *x = hc->priv;
    struct xdev *xd = d->hcpriv;
    if (!xd) return -1;
    memset(xd->in, 0, 33 * x->csz);
    if (d->ownhub) {
        /* hub information is taken by Configure Endpoint with the slot context only */
        *(u32 *)(xd->in + 4) = 1;
        memcpy(ctx(x, xd->in, 1), ctx(x, xd->out, 0), x->csz);
        fill_slot(x, xd, ctx(x, xd->in, 1));
        return command(x, vphys(xd->in), 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((u32)xd->slot << 24), NULL);
    }
    *(u32 *)(xd->in + 4) = 2;                       /* evaluate ep0 */
    memcpy(ctx(x, xd->in, 2), ctx(x, xd->out, 1), x->csz);
    *(u32 *)(ctx(x, xd->in, 2) + 4) = (*(u32 *)(ctx(x, xd->in, 2) + 4) & 0xFFFFUL) | ((u32)d->ep0.maxpkt << 16);
    return command(x, vphys(xd->in), 0, 0, TRB_TYPE(TRB_EVAL_CTX) | ((u32)xd->slot << 24), NULL);
}

static void xhci_dev_free(struct usb_hc *hc, struct usb_dev *d)
{
    struct xhci *x = hc->priv;
    struct xdev *xd = d->hcpriv;
    int i;
    if (!xd) return;
    command(x, 0, 0, 0, TRB_TYPE(TRB_DIS_SLOT) | ((u32)xd->slot << 24), NULL);
    for (i = 0; i < 32; i++) {
        struct xpipe *xp = xd->pipes[i];
        if (!xp) continue;
        ring_free(&xp->ring);
        ring_free(&xp->sring[1]);
        if (xp->sca) kfree(xp->sca);
        if (xp->p) xp->p->hcpriv = NULL;
        kfree(xp);
    }
    x->dcbaa[xd->slot * 2] = 0;
    x->slots[xd->slot] = NULL;
    kfree(xd->out);
    kfree(xd->in);
    kfree(xd);
    d->hcpriv = NULL;
    d->ep0.open = 0;
}

static u32 ep_interval(struct usb_pipe *p)
{
    u32 iv = p->interval ? p->interval : 1, e = 0;
    if (p->type != EP_INT && p->type != EP_ISO) return 0;
    if (p->dev->speed >= USB_HS) return (iv > 16 ? 16 : iv) - 1;
    if (p->type == EP_ISO) return (iv > 16 ? 16 : iv) - 1 + 3;     /* full speed: 2^(bInterval-1) frames */
    /* full/low speed: interval in frames -> 125us units, exponent */
    iv *= 8;
    while ((1UL << (e + 1)) <= iv && e < 14) e++;
    if (e < 3) e = 3;
    return e;
}

static int xhci_pipe_open(struct usb_hc *hc, struct usb_pipe *p)
{
    struct xhci *x = hc->priv;
    struct xdev *xd = p->dev->hcpriv;
    struct xpipe *xp;
    u32 dci, type, *ec, mps, esit, maxps = 0;
    int r;
    if (!xd) return -1;
    dci = (u32)(p->ep & 0x0F) * 2 + ((p->ep & 0x80) ? 1 : 0);
    if (p->type == EP_CTRL) dci = (u32)(p->ep & 0x0F) * 2 + 1;
    if (dci < 2 || dci > 31) return -1;
    xp = kmalloc(sizeof(*xp));
    xp->p = p;
    xp->xd = xd;
    xp->dci = dci;
    if (ring_init_n(&xp->ring, p->type == EP_ISO ? ISO_TRBS : RING_TRBS)) return -1;
    type = p->type == EP_CTRL ? 4 : (((p->ep & 0x80) ? 4 : 0) + p->type);
    mps = p->maxpkt;
    if (p->dev->speed == USB_HS && (p->type == EP_ISO || p->type == EP_INT) && p->mult) {
        p->maxburst = p->mult;      /* high-bandwidth: additional transactions are a burst for xHCI */
        p->mult = 0;
    }
    esit = mps * (p->maxburst + 1UL) * (p->mult + 1UL);
    memset(xd->in, 0, 33 * x->csz);
    if (dci > xd->ctx_entries) xd->ctx_entries = dci;
    *(u32 *)(xd->in + 4) = 1 | (1UL << dci);
    memcpy(ctx(x, xd->in, 1), ctx(x, xd->out, 0), x->csz);
    fill_slot(x, xd, ctx(x, xd->in, 1));
    ec = (u32 *)ctx(x, xd->in, dci + 1);
    if (p->maxstreams && !x->maxpsa) p->maxstreams = 0;
    if (p->maxstreams && p->type == EP_BULK && p->dev->speed >= USB_SS) {
        /* primary stream array with 4 entries; stream 1 used */
        u32 *sca;
        xp->streams = 1;
        maxps = 1;
        xp->sca = kmalloc_align(4 * 16, 64);
        ring_init(&xp->sring[1]);
        sca = (u32 *)xp->sca;
        sca[4] = ring_deq(&xp->sring[1]) | (1UL << 1);      /* SCT = primary TR */
        sca[5] = 0;
        ec[0] = (maxps << 10) | (1UL << 15);
        ec[2] = vphys(xp->sca);
    } else {
        ec[0] = ((u32)p->mult << 8) | (ep_interval(p) << 16);
        ec[2] = ring_deq(&xp->ring);
    }
    ec[1] = ((p->type == EP_ISO ? 0UL : 3UL) << 1) | (type << 3) | ((u32)p->maxburst << 8) | (mps << 16);
    ec[3] = 0;
    ec[4] = (p->type == EP_INT || p->type == EP_ISO ? MIN(esit, 3072UL) : (p->type == EP_CTRL ? 8UL : 3072UL)) |
            ((esit & 0xFFFF) << 16);
    xd->pipes[dci] = xp;
    p->hcpriv = xp;
    r = command(x, vphys(xd->in), 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((u32)xd->slot << 24), NULL);
    if (r) {
        kprintf("xhci: configure endpoint %02x failed (%d)\n", p->ep, r);
        xd->pipes[dci] = NULL;
        p->hcpriv = NULL;
        ring_free(&xp->ring);
        ring_free(&xp->sring[1]);
        if (xp->sca) kfree(xp->sca);
        kfree(xp);
        return -1;
    }
    return 0;
}

static void xhci_pipe_close(struct usb_hc *hc, struct usb_pipe *p)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = p->hcpriv;
    struct xdev *xd;
    struct usb_xfer *t, *done = NULL;
    int s;
    if (!xp) return;
    xd = xp->xd;
    while ((t = xp->qhead) != NULL) { xp->qhead = t->next; t->status = XS_CANCEL; }
    xp->qtail = NULL;
    for (s = 0; s < 2; s++) if (xp->active[s]) finish(x, xp, s, XS_CANCEL, &done);
    if (xp->dci != 1 && !p->dev->gone) {
        memset(xd->in, 0, 33 * x->csz);
        *(u32 *)(xd->in + 0) = 1UL << xp->dci;
        *(u32 *)(xd->in + 4) = 1;
        memcpy(ctx(x, xd->in, 1), ctx(x, xd->out, 0), x->csz);
        command(x, vphys(xd->in), 0, 0, TRB_TYPE(TRB_CONFIG_EP) | ((u32)xd->slot << 24), NULL);
    }
    if (xp->dci == 1) return;       /* ep0 is released with the slot */
    xd->pipes[xp->dci] = NULL;
    ring_free(&xp->ring);
    ring_free(&xp->sring[1]);
    if (xp->sca) kfree(xp->sca);
    kfree(xp);
    p->hcpriv = NULL;
}

static int xhci_clear_halt(struct usb_hc *hc, struct usb_pipe *p)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = p->hcpriv;
    if (!xp) return 0;
    if (xp->halted) recover_halt(x, xp, xp->streams ? 1 : 0);
    return 0;
}

/* ---- isochronous: one Isoch TRB per packet, scheduled as soon as possible ---- */
static int xhci_iso_start(struct usb_hc *hc, struct usb_iso *s)
{
    struct xpipe *xp = s->pipe->hcpriv;
    struct xiso *xi;
    int n = s->npkt;
    (void)hc;
    if (!xp) return -1;
    if (n > ISO_TRBS - 8) n = s->npkt = ISO_TRBS - 8;
    xi = kmalloc(sizeof(*xi));
    if (!xi) return -1;
    xi->s = s;
    xi->slot_of = kmalloc(ISO_TRBS * 2);
    xi->trb = kmalloc((u32)n * 4);
    xi->actual = kmalloc((u32)n * 2);
    xi->done = kmalloc((u32)n);
    xi->qtime = kmalloc((u32)n * 4);
    if (!xi->slot_of || !xi->trb || !xi->actual || !xi->done || !xi->qtime) return -1;
    memset(xi->slot_of, 0xFF, ISO_TRBS * 2);
    xi->head = xi->count = 0;
    xp->iso = xi;
    s->hcpriv = xi;
    return 0;
}

static void xhci_iso_service(struct usb_hc *hc, struct usb_iso *s)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = s->pipe->hcpriv;
    struct xiso *xi = s->hcpriv;
    u32 now = plat_ms(), maxq, mps = s->pipe->maxpkt ? s->pipe->maxpkt : 1, bursts = s->pipe->maxburst + 1UL;
    int queued = 0;
    if (!xp || !xi) return;
    /* completions in order; a packet without an event for too long is given up */
    while (xi->count) {
        int k = xi->head;
        if (!xi->done[k]) {
            if (now - xi->qtime[k] < s->ahead_us / 1000 + 200) break;
            s->missed++;
        } else if (xi->done[k] == 2) {
            s->errors++;
        } else if (s->in) {
            s->done(s, s->buf + (u32)k * s->pktsize, xi->actual[k]);
        }
        s->packets++;
        xi->head = (xi->head + 1) % s->npkt;
        xi->count--;
    }
    /* the queue ran empty: the device sent packets nobody received */
    if (!xi->count && s->packets) s->missed++;
    maxq = s->ahead_us / s->period_us;
    if (maxq > (u32)s->npkt - 1) maxq = (u32)s->npkt - 1;
    while ((u32)xi->count < maxq) {
        int k = (xi->head + xi->count) % s->npkt, len;
        u8 *b = s->buf + (u32)k * s->pktsize;
        u32 pk, tbc, tlbpc, ph;
        len = s->in ? s->pktsize : s->fill(s, b, s->pktsize);
        if (len < 0) len = 0;
        pk = ((u32)len + mps - 1) / mps;
        if (!pk) pk = 1;
        tbc = (pk + bursts - 1) / bursts - 1;
        tlbpc = (pk - 1) % bursts;
        xi->done[k] = 0;
        xi->actual[k] = (u16)len;
        xi->qtime[k] = now;
        xi->slot_of[xp->ring.enq] = (u16)k;
        ph = ring_put(&xp->ring, vphys(b), 0, (u32)len,
                      TRB_TYPE(TRB_ISOCH) | TRB_IOC | (s->in ? TRB_ISP : 0) | (1UL << 31) | (tbc << 7) | (tlbpc << 16));
        xi->trb[k] = ph;
        xi->count++;
        queued++;
    }
    if (queued) ring_db(x, xp->xd->slot, xp->dci);
}

static void xhci_iso_stop(struct usb_hc *hc, struct usb_iso *s)
{
    struct xhci *x = hc->priv;
    struct xpipe *xp = s->pipe->hcpriv;
    struct xiso *xi = s->hcpriv;
    if (!xi) return;
    if (xp && !s->pipe->dev->gone) {
        command(x, 0, 0, 0, TRB_TYPE(TRB_STOP_EP) | ((u32)xp->xd->slot << 24) | (xp->dci << 16), NULL);
        command(x, ring_deq(&xp->ring), 0, 0, TRB_TYPE(TRB_SET_DEQ) | ((u32)xp->xd->slot << 24) | (xp->dci << 16),
                NULL);
    }
    if (xp) xp->iso = NULL;
    kfree(xi->slot_of);
    kfree(xi->trb);
    kfree(xi->actual);
    kfree(xi->done);
    kfree(xi->qtime);
    kfree(xi);
    s->hcpriv = NULL;
}

/* ---- root hub ---- */
static int rh_nports(struct usb_hub *h) { return ((struct xhci *)h->hc->priv)->maxports; }

static u32 rh_status(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    u32 v = ORD(x, PORTSC + (port - 1) * 0x10), s = 0, sp = (v >> 10) & 0xF;
    if (port <= MAXPORTS) x->portpsiv[port] = (u8)sp;
    if (v & 0x0001) s |= PS_CONNECT;
    if (v & 0x0002) s |= PS_ENABLE;
    if (v & 0x0010) s |= PS_RESET;
    if (v & 0x0200) s |= PS_POWER;
    if (v & 0x0008) s |= PS_OVERCUR;
    if (v & (1UL << 17)) s |= PS_CCHANGE;
    if (v & (PORT_CHG & ~(1UL << 17))) s |= PS_OTHERCHG;
    switch (sp) {
    case 1: s |= (u32)USB_FS << PS_SPEEDSH; break;
    case 2: s |= (u32)USB_LS << PS_SPEEDSH; break;
    case 3: s |= (u32)USB_HS << PS_SPEEDSH; break;
    case 4: s |= (u32)USB_SS << PS_SPEEDSH; break;
    default: s |= (u32)USB_SSP << PS_SPEEDSH; break;
    }
    return s;
}

static void rh_clear(struct usb_hub *h, int port, u32 st)
{
    struct xhci *x = h->hc->priv;
    int r = PORTSC + (port - 1) * 0x10;
    u32 v = ORD(x, r);
    (void)st;
    OWR(x, r, (v & (PORT_RO | PORT_RWS)) | (v & PORT_CHG));
}

static int rh_reset(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    int r = PORTSC + (port - 1) * 0x10, i;
    u32 v = ORD(x, r);
    if (x->portmajor[port] == 3) {
        if ((v & 3) == 3) return 0;     /* trained and enabled */
        OWR(x, r, (v & (PORT_RO | PORT_RWS)) | (1UL << 31));      /* warm reset */
    } else {
        OWR(x, r, (v & (PORT_RO | PORT_RWS)) | 0x10);
    }
    for (i = 0; i < 100; i++) {
        mdelay(5);
        v = ORD(x, r);
        if ((v & (1UL << 21)) || (v & (1UL << 19))) break;
    }
    OWR(x, r, (v & (PORT_RO | PORT_RWS)) | (v & PORT_CHG));
    mdelay(10);
    v = ORD(x, r);
    return (v & 3) == 3 ? 0 : -1;
}

static void rh_disable(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    int r = PORTSC + (port - 1) * 0x10;
    u32 v = ORD(x, r);
    if (x->portmajor[port] != 3) OWR(x, r, (v & (PORT_RO | PORT_RWS)) | 0x02);
}

/* Extended capabilities live anywhere in the BAR (Intel: offset 8000h); only the mapped
   part is walked. */
static void bios_handoff(u8 *cap, u32 maplen)
{
    u32 off = (RD32(cap + 0x10) >> 16) * 4;
    int guard = 0, i;
    while (off && off + 8 <= maplen && guard++ < 64) {
        u8 *c = cap + off;
        u32 v = RD32(c), next = (v >> 8) & 0xFF;
        if (v == 0xFFFFFFFFUL) break;
        if ((v & 0xFF) == 1) {
            /* USB legacy support: claim the controller from the BIOS (SMM) */
            WR8(c + 3, 1);
            for (i = 0; i < 1000 && (RD32(c) & (1UL << 16)); i++) mdelay(1);
            if (RD32(c) & (1UL << 16)) {
                kprintf("xhci: BIOS did not release the controller, taking it anyway\n");
                WR32(c, RD32(c) & ~(1UL << 16));
            }
            /* SMIs off, SMI status bits cleared; the SMI on a change of ownership stays as
               the BIOS set it: it tells the BIOS when the controller is given back */
            WR32(c + 4, (RD32(c + 4) & ~0x0000C011UL) | 0xE0000000UL);
            return;
        }
        if (!next) break;
        off += next * 4;
    }
}

static void read_protocols(struct xhci *x, u32 hcc)
{
    u32 off = (hcc >> 16) * 4;
    int guard = 0;
    while (off && off + 16 <= x->maplen && guard++ < 64) {
        u8 *c = x->cap + off;
        u32 v = RD32(c), id = v & 0xFF, next = (v >> 8) & 0xFF;
        if (v == 0xFFFFFFFFUL) break;
        if (id == 2) {
            u32 major = v >> 24, p2 = RD32(c + 8), first = p2 & 0xFF, cnt = (p2 >> 8) & 0xFF, k;
            for (k = first; k < first + cnt && k <= MAXPORTS; k++) x->portmajor[k] = (u8)major;
        }
        if (!next) break;
        off += next * 4;
    }
}

static int xhci_start(struct usb_hc *hc)
{
    struct xhci *x = hc->priv;
    u32 hcs1 = RD32(x->cap + 4), hcs2 = RD32(x->cap + 8), hcc = RD32(x->cap + 0x10);
    int i;
    x->op = x->cap + RD8(x->cap);
    x->db = x->cap + (RD32(x->cap + 0x14) & ~3UL);
    x->rt = x->cap + (RD32(x->cap + 0x18) & ~0x1FUL);
    x->csz = (hcc & 4) ? 64 : 32;
    x->maxslots = (int)MIN(hcs1 & 0xFF, 63);
    x->maxports = (int)MIN((hcs1 >> 24) & 0xFF, MAXPORTS);
    x->maxpsa = (int)((hcc >> 12) & 0xF);
    bios_handoff(x->cap, x->maplen);
    read_protocols(x, hcc);
    /* Controller Not Ready after power-on: no operational register may be written */
    for (i = 0; i < 1000 && (ORD(x, USBSTS) & (1UL << 11)); i++) mdelay(1);
    if (ORD(x, USBSTS) & (1UL << 11)) {
        if (hc->pci.vendor == 0x1912)
            kprintf("xhci: Renesas controller not ready: it has no firmware ROM and needs a firmware loader\n");
        else
            kprintf("xhci: controller not ready\n");
        return -1;
    }
    OWR(x, USBCMD, ORD(x, USBCMD) & ~1UL);
    for (i = 0; i < 200 && !(ORD(x, USBSTS) & 1); i++) mdelay(1);
    OWR(x, USBCMD, 2);
    /* Intel: no register access for 1 ms after HCRST, or the system can hang */
    mdelay(1);
    for (i = 0; i < 1000 && ((ORD(x, USBCMD) & 2) || (ORD(x, USBSTS) & (1UL << 11))); i++) mdelay(1);
    if ((ORD(x, USBCMD) & 2) || (ORD(x, USBSTS) & (1UL << 11))) {
        kprintf("xhci: controller reset failed\n");
        return -1;
    }
    mdelay(1);
    OWR(x, CONFIG, (ORD(x, CONFIG) & ~0xFFUL) | (u32)x->maxslots);
    x->dcbaa = kmalloc_align(2048, 64);
    x->nscratch = (int)((((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F));
    if (x->nscratch) {
        u32 *arr = kmalloc_align((u32)x->nscratch * 8, 64);
        x->scratch = kmalloc((u32)x->nscratch * sizeof(void *));
        for (i = 0; i < x->nscratch; i++) {
            x->scratch[i] = kmalloc_align(4096, 4096);
            arr[i * 2] = vphys(x->scratch[i]);
            arr[i * 2 + 1] = 0;
        }
        x->dcbaa[0] = vphys(arr);
        x->dcbaa[1] = 0;
    }
    OWR(x, DCBAAP, vphys(x->dcbaa));
    OWR(x, DCBAAP + 4, 0);
    ring_init(&x->cmd);
    OWR(x, CRCR, x->cmd.phys | 1);
    OWR(x, CRCR + 4, 0);
    x->evt = kmalloc_align(EVT_TRBS * 16, 4096);
    x->evt_phys = vphys(x->evt);
    x->evt_cycle = 1;
    x->erst = kmalloc_align(64, 64);
    x->erst[0] = x->evt_phys;
    x->erst[1] = 0;
    x->erst[2] = EVT_TRBS;
    x->erst[3] = 0;
    WR32(x->rt + 0x20 + 0x08, 1);                    /* ERSTSZ */
    WR32(x->rt + 0x20 + 0x18, x->evt_phys);          /* ERDP */
    WR32(x->rt + 0x20 + 0x1C, 0);
    WR32(x->rt + 0x20 + 0x10, vphys(x->erst));       /* ERSTBA */
    WR32(x->rt + 0x20 + 0x14, 0);
    WR32(x->rt + 0x20 + 0x04, 1000);                 /* IMOD 250us */
    WR32(x->rt + 0x20 + 0x00, 3);                    /* IMAN: IE, clear IP */
    OWR(x, USBSTS, 0x041C);
    OWR(x, USBCMD, 0x05);                            /* run + interrupts */
    for (i = 0; i < 100 && (ORD(x, USBSTS) & 1); i++) mdelay(1);
    if (hcc & 8)
        for (i = 0; i < x->maxports; i++) {
            u32 v = ORD(x, PORTSC + i * 0x10);
            if (!(v & 0x200)) OWR(x, PORTSC + i * 0x10, (v & (PORT_RO | PORT_RWS)) | 0x200);
        }
    mdelay(20);
    usb_new_hub(hc, NULL, &xhci_hubops, x->maxports);
    usb_irq_setup(hc);
    kprintf("xhci: %s ver %04lx irq %d, %d ports, %d slots, %lu-byte contexts, %d scratchpads\n", hc->name,
            RD32(x->cap) >> 16, hc->irq, x->maxports, x->maxslots, x->csz, x->nscratch);
    return 0;
}

/* the controller (reset) back to the BIOS: OS Owned cleared, the BIOS takes it on the
   ownership SMI (legacy USB keyboard in DOS after Windows, restart in MS-DOS mode) */
static void bios_release(u8 *cap, u32 maplen)
{
    u32 off = (RD32(cap + 0x10) >> 16) * 4;
    int guard = 0;
    while (off && off + 8 <= maplen && guard++ < 64) {
        u8 *c = cap + off;
        u32 v = RD32(c), next = (v >> 8) & 0xFF;
        if (v == 0xFFFFFFFFUL) break;
        if ((v & 0xFF) == 1) {
            if (RD32(c + 4) & 0x2000UL) {
                WR8(c + 3, 0);
                kprintf("xhci: controller back to the BIOS\n");
            }
            return;
        }
        if (!next) break;
        off += next * 4;
    }
}

static void xhci_stop(struct usb_hc *hc)
{
    struct xhci *x = hc->priv;
    int i;
    OWR(x, USBCMD, 0);
    for (i = 0; i < 100 && !(ORD(x, USBSTS) & 1); i++) mdelay(1);
    OWR(x, USBCMD, 2);
    for (i = 0; i < 500 && (ORD(x, USBCMD) & 2); i++) mdelay(1);
    for (i = 0; i < 500 && (ORD(x, USBSTS) & (1UL << 11)); i++) mdelay(1);   /* CNR */
    bios_release(x->cap, x->maplen);
    for (i = 0; i < 64; i++) {
        struct xdev *xd = x->slots[i];
        int k;
        if (!xd) continue;
        for (k = 0; k < 32; k++) {
            struct xpipe *xp = xd->pipes[k];
            if (!xp) continue;
            ring_free(&xp->ring);
            ring_free(&xp->sring[1]);
            if (xp->sca) kfree(xp->sca);
            kfree(xp);
        }
        kfree(xd->out);
        kfree(xd->in);
        kfree(xd);
    }
    if (x->nscratch) {
        kfree(pvirt(x->dcbaa[0]));
        for (i = 0; i < x->nscratch; i++) kfree(x->scratch[i]);
        kfree(x->scratch);
    }
    ring_free(&x->cmd);
    kfree(x->evt);
    kfree(x->erst);
    kfree(x->dcbaa);
    kfree(x);
}

static struct usb_hubops xhci_hubops = { rh_nports, rh_status, rh_clear, rh_reset, rh_disable, NULL };

static struct usb_hcops xhci_ops = {
    "xHCI", HC_XHCI, xhci_start, xhci_stop, xhci_dev_init, xhci_dev_update, xhci_dev_free,
    xhci_pipe_open, xhci_pipe_close, xhci_submit, xhci_cancel, xhci_poll, xhci_irq, xhci_clear_halt, NULL,
    xhci_iso_start, xhci_iso_stop, xhci_iso_service
};

/* Bytes to map: capability, operational and runtime registers, doorbells and the
   extended capabilities (QEMU: right after the capability registers, Intel: 8000h,
   others anywhere below 64K). Reads beyond the BAR return all ones and are harmless. */
static u32 map_len(u32 phys, u8 **probe)
{
    u8 *pr = usb_map(phys, 0x40);
    u32 len, xecp;
    if (probe) *probe = pr;
    if (!pr) return 0;
    len = MAX(RD32(pr + 0x14), RD32(pr + 0x18)) + 0x2000;
    len = MAX(len, 0x400 + 0x10 * ((RD32(pr + 4) >> 24) & 0xFF) + RD8(pr));
    xecp = (RD32(pr + 0x10) >> 16) * 4;
    len = MAX(len, xecp + 0x1000);
    len = MAX(len, 0x10000UL);
    return MIN((len + 4095) & ~4095UL, 0x40000UL);
}

/* Broken stream support (Linux XHCI_BROKEN_STREAMS): UAS devices are used with BOT */
static int broken_streams(struct pcidev *p)
{
    if (p->vendor == 0x1B6F && (p->device == 0x7023 || p->device == 0x7052)) return 1;   /* Etron */
    if (p->vendor == 0x1B73 && (p->device == 0x1000 || p->device == 0x1009 || p->device == 0x1100))
        return 1;                                                                       /* Fresco Logic */
    return 0;
}

void xhci_early(struct pcidev *p)
{
    u32 phys = pci_bar_mem(p, 0, NULL), len;
    u8 *cap;
    if (!phys) return;
    pci_enable(p, 1, 0, 1);
    len = map_len(phys, NULL);
    if (!len || !(cap = usb_map(phys, len))) return;
    bios_handoff(cap, len);
}

int xhci_attach(struct pcidev *p)
{
    struct xhci *x;
    struct usb_hc *hc;
    u32 phys = pci_bar_mem(p, 0, NULL), len;
    if (!phys) return -1;
    pci_enable(p, 1, 0, 1);
    len = map_len(phys, NULL);
    if (!len) return -1;
    x = kmalloc(sizeof(*x));
    x->maplen = len;
    x->cap = usb_map(phys, len);
    if (!x->cap) return -1;
    hc = usb_add_hc(&xhci_ops, p, x);
    if (!hc) return -1;
    x->hc = hc;
    hc->mmio = x->cap;
    hc->nostreams = (u8)broken_streams(p);
    if (hc->nostreams) kprintf("xhci: streams not used on this controller\n");
    return xhci_start(hc);
}
