/* uhci.c - UHCI host controller driver (USB 1.1, I/O ports) */
#include "usb.h"

#define CMD     0x00
#define STS     0x02
#define INTR    0x04
#define FRNUM   0x06
#define FRBASE  0x08
#define SOFMOD  0x0C
#define PORTSC  0x10

#define TD_ACTIVE   (1UL << 23)
#define TD_STALL    (1UL << 22)
#define TD_DBERR    (1UL << 21)
#define TD_BABBLE   (1UL << 20)
#define TD_NAK      (1UL << 19)
#define TD_CRCTO    (1UL << 18)
#define TD_BITSTUFF (1UL << 17)
#define TD_ERRMASK  (TD_STALL | TD_DBERR | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)
#define TD_IOC      (1UL << 24)
#define TD_LS       (1UL << 26)
#define TD_CERR3    (3UL << 27)
#define TD_SPD      (1UL << 29)

#define PID_SETUP   0x2D
#define PID_IN      0x69
#define PID_OUT     0xE1

#define LP_T    1UL
#define LP_Q    2UL
#define LP_VF   4UL

struct utd {                    /* 32 bytes, 16-byte aligned */
    volatile u32 link, cs, token, buf;
    u32 maxlen;
    struct utd *next;
    u32 rsv[2];
};

struct uqh {                    /* 16 bytes used, 16-byte aligned */
    volatile u32 link, elem;
    u32 rsv[2];
};

struct upipe {
    struct usb_pipe *p;
    struct uqh *qh;
    struct usb_xfer *active, *qhead, *qtail;
    struct utd *tds;
    int ntd;
    struct utd *status_td;
    int short_ctrl;
    struct upipe *next;
};

struct uhci {
    struct usb_hc *hc;
    u16 io;
    u32 *frames;
    struct uqh *qh_int, *qh_ctrl, *qh_bulk;
    struct upipe *pipes;
    int nports;
    u32 lastfr;                 /* frame counter extended to 32 bits */
};

static struct usb_hcops uhci_ops;
static struct usb_hubops uhci_hubops;

static u16 rdw(struct uhci *u, int r) { return inw((u16)(u->io + r)); }
static void wrw(struct uhci *u, int r, u16 v) { outw((u16)(u->io + r), v); }

static void qh_insert(struct uqh *anchor, struct uqh *qh)
{
    qh->link = anchor->link;
    anchor->link = vphys(qh) | LP_Q;
}

static void qh_remove(struct uqh *anchor, struct uqh *qh, struct uqh *last)
{
    /* walk the horizontal list starting at the anchor; stop at 'last' */
    struct uqh *cur = anchor;
    u32 target = vphys(qh) | LP_Q;
    while (cur && cur != last) {
        if (cur->link == target) {
            cur->link = qh->link;
            return;
        }
        if (cur->link & LP_T) return;
        cur = pvirt(cur->link & ~0xFUL);
    }
}

static int start_xfer(struct uhci *u, struct upipe *up, struct usb_xfer *x)
{
    struct usb_pipe *p = up->p;
    struct usb_dev *d = p->dev;
    u32 mps = p->maxpkt ? p->maxpkt : 8, len = x->len, off = 0, ntd, i;
    u32 base = TD_ACTIVE | TD_CERR3 | (d->speed == USB_LS ? TD_LS : 0);
    u32 tok = ((u32)d->addr << 8) | ((u32)(p->ep & 0x0F) << 15);
    struct utd *td;
    int toggle;
    u8 pid;
    (void)u;

    ntd = (len + mps - 1) / mps;
    if (p->type == EP_CTRL) ntd += 2;
    if (!ntd) ntd = 1;
    td = kmalloc_align((ntd + 1) * sizeof(struct utd), 16);
    if (!td) return XS_ERROR;
    up->tds = td;
    up->ntd = (int)ntd;
    up->status_td = NULL;
    up->short_ctrl = 0;
    i = 0;
    if (p->type == EP_CTRL) {
        u8 *setup = (u8 *)&td[ntd];         /* setup packet lives in DMA memory */
        memcpy(setup, x->setup, 8);
        td[i].cs = base;
        td[i].token = tok | PID_SETUP | (7UL << 21);
        td[i].buf = vphys(setup);
        td[i].maxlen = 8;
        i++;
        toggle = 1;
        pid = x->in ? PID_IN : PID_OUT;
    } else {
        toggle = p->toggle;
        pid = (p->ep & 0x80) ? PID_IN : PID_OUT;
    }
    do {
        u32 n = MIN(mps, len - off);
        if (p->type == EP_CTRL && len == 0) break;
        td[i].cs = base | (pid == PID_IN ? TD_SPD : 0);
        td[i].token = tok | pid | ((u32)toggle << 19) | (((n ? n : 0x800) - 1) << 21);
        td[i].buf = n ? vphys(x->buf + off) : 0;
        td[i].maxlen = n;
        toggle ^= 1;
        off += n;
        i++;
    } while (off < len);
    if (p->type == EP_CTRL) {
        td[i].cs = base;
        td[i].token = tok | (x->in && len ? PID_OUT : PID_IN) | (1UL << 19) | (0x7FFUL << 21);
        td[i].buf = 0;
        up->status_td = &td[i];
        i++;
    }
    ntd = i;
    up->ntd = (int)ntd;
    for (i = 0; i < ntd; i++) {
        td[i].next = (i + 1 < ntd) ? &td[i + 1] : NULL;
        td[i].link = (i + 1 < ntd) ? (vphys(&td[i + 1]) | LP_VF) : LP_T;
    }
    td[ntd - 1].cs |= TD_IOC;
    up->active = x;
    x->hcpriv = up;
    up->qh->elem = vphys(&td[0]);
    return 0;
}

static void finish(struct uhci *u, struct upipe *up, int status, struct usb_xfer **done)
{
    struct usb_xfer *x = up->active;
    (void)u;
    up->qh->elem = LP_T;
    if (up->tds) kfree(up->tds);
    up->tds = NULL;
    up->active = NULL;
    x->status = status;
    x->next = *done;
    *done = x;
    if (up->qhead) {
        struct usb_xfer *n = up->qhead;
        up->qhead = n->next;
        if (!up->qhead) up->qtail = NULL;
        n->next = NULL;
        if (start_xfer(u, up, n)) {
            n->status = XS_ERROR;
            n->next = *done;
            *done = n;
        }
    }
}

/* returns 1 when the active transfer finished */
static int check_xfer(struct uhci *u, struct upipe *up, struct usb_xfer **done)
{
    struct usb_xfer *x = up->active;
    struct usb_pipe *p = up->p;
    int i;
    u32 actual = 0;
    if (up->short_ctrl) {
        u32 cs = up->status_td->cs;
        if (cs & TD_ACTIVE) return 0;
        finish(u, up, (cs & TD_STALL) ? XS_STALL : (cs & TD_ERRMASK) ? XS_ERROR : XS_OK, done);
        return 1;
    }
    for (i = 0; i < up->ntd; i++) {
        struct utd *td = &up->tds[i];
        u32 cs = td->cs, got;
        int isdata = !(p->type == EP_CTRL && (i == 0 || td == up->status_td));
        if (cs & TD_ACTIVE) return 0;
        if (cs & TD_ERRMASK) {
            x->actual = actual;
            if (p->type != EP_CTRL) p->toggle = (u8)((td->token >> 19) & 1);
            finish(u, up, (cs & TD_STALL) ? XS_STALL : (cs & TD_BABBLE) ? XS_BABBLE : XS_ERROR, done);
            return 1;
        }
        got = (cs + 1) & 0x7FF;
        if (!isdata) continue;
        actual += got;
        if (p->type != EP_CTRL) p->toggle = (u8)(((td->token >> 19) & 1) ^ 1);
        if (got < td->maxlen) {
            x->actual = actual;
            if (p->type == EP_CTRL && up->status_td) {
                /* short data stage: continue with the status stage */
                up->short_ctrl = 1;
                up->qh->elem = vphys(up->status_td);
                return 0;
            }
            finish(u, up, XS_OK, done);
            return 1;
        }
    }
    x->actual = actual;
    finish(u, up, XS_OK, done);
    return 1;
}

static void uhci_poll(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;
    struct upipe *up;
    struct usb_xfer *done = NULL, *x;
    u16 st = rdw(u, STS);
    if (st & 0x3F) wrw(u, STS, (u16)(st & 0x3F));
    for (up = u->pipes; up; up = up->next)
        if (up->active) check_xfer(u, up, &done);
    while ((x = done) != NULL) {
        done = x->next;
        x->next = NULL;
        if (x->status != XS_OK) hc->errors++;
        if (x->cb) x->cb(x);
    }
}

static int uhci_irq(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;
    u16 st = rdw(u, STS);
    if (!(st & 0x1F)) return 0;
    wrw(u, STS, (u16)(st & 0x3F));
    hc->pending = 1;
    return 1;
}

static int uhci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct uhci *u = hc->priv;
    struct upipe *up = x->pipe->hcpriv;
    if (!up) return XS_ERROR;
    x->next = NULL;
    if (up->active) {
        if (up->qtail) up->qtail->next = x; else up->qhead = x;
        up->qtail = x;
        return 0;
    }
    return start_xfer(u, up, x);
}

static void uhci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct uhci *u = hc->priv;
    struct upipe *up = x->pipe->hcpriv;
    struct usb_xfer **pp, *done = NULL;
    if (!up) return;
    for (pp = &up->qhead; *pp; pp = &(*pp)->next) {
        if (*pp == x) {
            *pp = x->next;
            if (up->qtail == x) {
                struct usb_xfer *t = up->qhead;
                while (t && t->next) t = t->next;
                up->qtail = t;
            }
            x->status = XS_CANCEL;
            return;
        }
    }
    if (up->active == x) {
        up->qh->elem = LP_T;
        udelay(1100);                   /* let the current frame finish */
        finish(u, up, XS_CANCEL, &done);
    }
}

static int uhci_pipe_open(struct usb_hc *hc, struct usb_pipe *p)
{
    struct uhci *u = hc->priv;
    struct upipe *up;
    if (p->type == EP_ISO) {
        p->hcpriv = NULL;           /* scheduled by the isochronous code */
        return 0;
    }
    up = kmalloc(sizeof(*up));
    if (!up) return -1;
    up->p = p;
    up->qh = kmalloc_align(sizeof(struct uqh), 16);
    up->qh->elem = LP_T;
    qh_insert(p->type == EP_INT ? u->qh_int : p->type == EP_CTRL ? u->qh_ctrl : u->qh_bulk, up->qh);
    up->next = u->pipes;
    u->pipes = up;
    p->hcpriv = up;
    return 0;
}

static void uhci_pipe_close(struct usb_hc *hc, struct usb_pipe *p)
{
    struct uhci *u = hc->priv;
    struct upipe *up = p->hcpriv, **pp;
    struct usb_xfer *x;
    if (!up) return;
    while ((x = up->qhead) != NULL) {
        up->qhead = x->next;
        x->status = XS_CANCEL;
    }
    up->qhead = up->qtail = NULL;
    if (up->active) {
        struct usb_xfer *done = NULL;
        up->qh->elem = LP_T;
        udelay(1100);
        finish(u, up, XS_CANCEL, &done);
    }
    qh_remove(p->type == EP_INT ? u->qh_int : p->type == EP_CTRL ? u->qh_ctrl : u->qh_bulk, up->qh, NULL);
    udelay(1100);
    for (pp = &u->pipes; *pp; pp = &(*pp)->next) {
        if (*pp == up) { *pp = up->next; break; }
    }
    kfree(up->qh);
    kfree(up);
    p->hcpriv = NULL;
}

static int uhci_dev_init(struct usb_hc *hc, struct usb_dev *d)
{
    if (uhci_pipe_open(hc, &d->ep0)) return -1;
    d->ep0.open = 1;
    return 0;
}

/* ---- root hub ---- */
static int rh_nports(struct usb_hub *h) { return ((struct uhci *)h->hc->priv)->nports; }

static u32 rh_status(struct usb_hub *h, int port)
{
    struct uhci *u = h->hc->priv;
    u16 v = rdw(u, PORTSC + (port - 1) * 2);
    u32 s = PS_POWER;
    if (v & 0x0001) s |= PS_CONNECT;
    if (v & 0x0004) s |= PS_ENABLE;
    if (v & 0x0002) s |= PS_CCHANGE;
    if (v & 0x0008) s |= PS_OTHERCHG;
    if (v & 0x0200) s |= PS_RESET;
    s |= (u32)((v & 0x0100) ? USB_LS : USB_FS) << PS_SPEEDSH;
    return s;
}

static void rh_clear(struct usb_hub *h, int port, u32 st)
{
    struct uhci *u = h->hc->priv;
    int r = PORTSC + (port - 1) * 2;
    u16 v = rdw(u, r);
    (void)st;
    wrw(u, r, (u16)((v & ~0x000AU) | (v & 0x000A)));
}

static int rh_reset(struct usb_hub *h, int port)
{
    struct uhci *u = h->hc->priv;
    int r = PORTSC + (port - 1) * 2, i;
    u16 v = rdw(u, r) & ~0x000AU;
    wrw(u, r, (u16)(v | 0x0200));
    mdelay(50);
    v = rdw(u, r) & ~0x000AU;
    wrw(u, r, (u16)(v & ~0x0200U));
    udelay(100);
    for (i = 0; i < 10; i++) {
        v = rdw(u, r);
        if (!(v & 0x0001)) return -1;
        if (v & 0x000A) wrw(u, r, (u16)((v & ~0x000AU) | (v & 0x000A)));
        if (v & 0x0004) break;
        wrw(u, r, (u16)((v & ~0x000AU) | 0x0004));
        mdelay(10);
    }
    mdelay(10);
    return (rdw(u, r) & 0x0004) ? 0 : -1;
}

static void rh_disable(struct usb_hub *h, int port)
{
    struct uhci *u = h->hc->priv;
    int r = PORTSC + (port - 1) * 2;
    wrw(u, r, (u16)(rdw(u, r) & ~0x000EU));
}

/* the BIOS's legacy USB setup of a controller as found the first time (keyboard and mouse
   of the BIOS through SMM): given back when the controller stops (end of Windows, restart
   in MS-DOS mode), so that the BIOS keyboard works again without a new POST */
struct ubios {
    u16 bdf, legsup, cmd, intr, frnum;
    u32 frbase;
    u8 sof, used;
};
static struct ubios ubios[8];

static struct ubios *ubios_of(struct pcidev *p, int add)
{
    u16 bdf = (u16)((p->bus << 8) | (p->dev << 3) | p->fn);
    int i;
    for (i = 0; i < 8; i++)
        if (ubios[i].used && ubios[i].bdf == bdf) return &ubios[i];
    if (!add) return NULL;
    for (i = 0; i < 8; i++)
        if (!ubios[i].used) {
            ubios[i].used = 1;
            ubios[i].bdf = bdf;
            return &ubios[i];
        }
    return NULL;
}

static void ubios_save(struct pcidev *p, u16 io)
{
    struct ubios *b;
    u16 leg = pci_rd16(p->bus, p->dev, p->fn, 0xC0);
    if (ubios_of(p, 0) || !(leg & 0x001F)) return;      /* known, or no BIOS legacy support */
    b = ubios_of(p, 1);
    if (!b) return;
    b->legsup = leg;
    b->cmd = io ? inw(io) : 0;
    b->intr = io ? inw((u16)(io + 4)) : 0;
    b->frnum = io ? inw((u16)(io + 6)) : 0;
    b->frbase = io ? inl((u16)(io + 8)) : 0;
    b->sof = io ? inb((u16)(io + 12)) : 0x40;
}

/* the controller (stopped and reset) back to the BIOS */
static void ubios_restore(struct pcidev *p, u16 io)
{
    struct ubios *b = ubios_of(p, 0);
    if (!b) return;
    if (b->frbase && (b->cmd & 1)) {
        outl((u16)(io + 8), b->frbase);
        outw((u16)(io + 6), b->frnum);
        outb((u16)(io + 12), b->sof);
        outw((u16)(io + 2), 0x3F);
        outw((u16)(io + 4), b->intr);
        outw(io, (u16)(b->cmd & 0x00C1));
    }
    pci_wr16(p->bus, p->dev, p->fn, 0xC0, 0x8F00);      /* status bits cleared */
    pci_wr16(p->bus, p->dev, p->fn, 0xC0, (u16)(b->legsup & 0x20FF));
    kprintf("uhci: %s back to the BIOS (legacy support %04x)\n", "controller", b->legsup);
}

static int uhci_start(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;
    int i;
    struct pcidev *p = &hc->pci;
    /* take the controller from BIOS legacy support */
    ubios_save(p, u->io);
    pci_wr16(p->bus, p->dev, p->fn, 0xC0, 0x8F00);
    wrw(u, INTR, 0);
    wrw(u, CMD, 0);
    mdelay(2);
    wrw(u, CMD, 0x0004);            /* global reset */
    mdelay(20);
    wrw(u, CMD, 0);
    wrw(u, CMD, 0x0002);            /* host controller reset */
    for (i = 0; i < 100 && (rdw(u, CMD) & 0x0002); i++) udelay(100);
    u->frames = kmalloc_align(4096, 4096);
    u->qh_int = kmalloc_align(sizeof(struct uqh), 16);
    u->qh_ctrl = kmalloc_align(sizeof(struct uqh), 16);
    u->qh_bulk = kmalloc_align(sizeof(struct uqh), 16);
    u->qh_bulk->link = LP_T;
    u->qh_bulk->elem = LP_T;
    u->qh_ctrl->link = vphys(u->qh_bulk) | LP_Q;
    u->qh_ctrl->elem = LP_T;
    u->qh_int->link = vphys(u->qh_ctrl) | LP_Q;
    u->qh_int->elem = LP_T;
    for (i = 0; i < 1024; i++) u->frames[i] = vphys(u->qh_int) | LP_Q;
    outl((u16)(u->io + FRBASE), vphys(u->frames));
    wrw(u, FRNUM, 0);
    outb((u16)(u->io + SOFMOD), 64);
    wrw(u, STS, 0x3F);
    wrw(u, INTR, 0x000F);
    wrw(u, CMD, 0x00C1);            /* run, configured, 64-byte max packet */
    pci_wr16(p->bus, p->dev, p->fn, 0xC0, 0x2000);  /* PIRQ enable */
    /* count ports: bit 7 of a real PORTSC reads as 1 */
    u->nports = 0;
    for (i = 0; i < 8; i++) {
        u16 v = rdw(u, PORTSC + i * 2);
        if (!(v & 0x0080) || v == 0xFFFF) break;
        u->nports++;
    }
    if (u->nports < 2) u->nports = 2;
    usb_new_hub(hc, NULL, &uhci_hubops, u->nports);
    usb_irq_setup(hc);
    kprintf("uhci: %s io %04x irq %d, %d ports\n", hc->name, u->io, hc->irq, u->nports);
    return 0;
}

static void uhci_stop(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;
    int i;
    wrw(u, INTR, 0);
    wrw(u, CMD, 0);
    for (i = 0; i < 100 && !(rdw(u, STS) & 0x20); i++) udelay(100);
    wrw(u, CMD, 0x0002);
    mdelay(2);
    for (i = 0; i < u->nports; i++) wrw(u, PORTSC + i * 2, 0x000A);
    while (u->pipes) {
        struct upipe *up = u->pipes;
        u->pipes = up->next;
        if (up->tds) kfree(up->tds);
        kfree(up->qh);
        kfree(up);
    }
    ubios_restore(&hc->pci, u->io);
    kfree(u->frames);
    kfree(u->qh_int);
    kfree(u->qh_ctrl);
    kfree(u->qh_bulk);
    kfree(u);
}

/* ---- isochronous: one TD per frame, at the head of the frame's list ---- */
#define TD_ISO      (1UL << 25)

struct uiso {
    struct utd *td;             /* npkt TDs */
    u32 *frame;                 /* frame of each queued TD */
    int head, count;            /* oldest queued, number queued */
    u32 next;                   /* next frame to use */
};

static u32 frame_now(struct uhci *u)
{
    u16 f = rdw(u, FRNUM) & 0x7FF;
    u32 d = (u32)(f - (u->lastfr & 0x7FF)) & 0x7FF;
    u->lastfr += d;
    return u->lastfr;
}

static void iso_unlink(struct uhci *u, struct utd *td, u32 f)
{
    volatile u32 *pp = &u->frames[f & 1023];
    u32 me = vphys(td);
    int guard = 0;
    while (guard++ < 32) {
        u32 v = *pp;
        if ((v & ~0xFUL) == me && !(v & LP_Q)) {
            *pp = td->link;
            return;
        }
        if (v & (LP_T | LP_Q)) return;
        pp = &((struct utd *)pvirt(v & ~0xFUL))->link;
    }
}

static int uhci_iso_start(struct usb_hc *hc, struct usb_iso *s)
{
    struct uhci *u = hc->priv;
    struct uiso *ui = kmalloc(sizeof(*ui));
    if (!ui) return -1;
    ui->td = kmalloc_align((u32)s->npkt * sizeof(struct utd), 16);
    ui->frame = kmalloc((u32)s->npkt * 4);
    if (!ui->td || !ui->frame) return -1;
    ui->head = ui->count = 0;
    ui->next = frame_now(u) + 2;
    s->hcpriv = ui;
    return 0;
}

static void uhci_iso_service(struct usb_hc *hc, struct usb_iso *s)
{
    struct uhci *u = hc->priv;
    struct uiso *ui = s->hcpriv;
    struct usb_pipe *p = s->pipe;
    u32 now = frame_now(u), step = s->period_us / 1000, ahead = s->ahead_us / 1000;
    if (!step) step = 1;
    /* retire packets of frames already done */
    while (ui->count && (s32)(now - ui->frame[ui->head]) > 0) {
        struct utd *td = &ui->td[ui->head];
        u32 cs = td->cs;
        iso_unlink(u, td, ui->frame[ui->head]);
        if (cs & TD_ACTIVE) s->missed++;
        else if (cs & TD_ERRMASK) s->errors++;
        else if (s->in) s->done(s, s->buf + (u32)ui->head * s->pktsize, (int)((cs + 1) & 0x7FF));
        s->packets++;
        ui->head = (ui->head + 1) % s->npkt;
        ui->count--;
    }
    if ((s32)(ui->next - (now + 1)) < 0) {        /* we were late: skip */
        if (ui->count == 0 && s->packets) usb_iso_late(s);
        ui->next = now + 2;
    }
    while (ui->count < s->npkt && (s32)(ui->next - (now + ahead)) < 0) {
        int k = (ui->head + ui->count) % s->npkt, len;
        struct utd *td = &ui->td[k];
        u8 *b = s->buf + (u32)k * s->pktsize;
        len = s->in ? s->pktsize : s->fill(s, b, s->pktsize);
        td->cs = TD_ACTIVE | TD_ISO | (s->in ? TD_SPD : 0);
        td->token = (s->in ? PID_IN : PID_OUT) | ((u32)p->dev->addr << 8) | ((u32)(p->ep & 0x0F) << 15) |
                    ((u32)((len ? len : 0x800) - 1) << 21);
        td->buf = vphys(b);
        td->link = u->frames[ui->next & 1023];
        u->frames[ui->next & 1023] = vphys(td);
        ui->frame[k] = ui->next;
        ui->count++;
        ui->next += step;
    }
}

static void uhci_iso_stop(struct usb_hc *hc, struct usb_iso *s)
{
    struct uhci *u = hc->priv;
    struct uiso *ui = s->hcpriv;
    if (!ui) return;
    while (ui->count) {
        iso_unlink(u, &ui->td[ui->head], ui->frame[ui->head]);
        ui->head = (ui->head + 1) % s->npkt;
        ui->count--;
    }
    udelay(1100);
    kfree(ui->td);
    kfree(ui->frame);
    kfree(ui);
    s->hcpriv = NULL;
}

static struct usb_hubops uhci_hubops = { rh_nports, rh_status, rh_clear, rh_reset, rh_disable, NULL };

static struct usb_hcops uhci_ops = {
    "UHCI", HC_UHCI, uhci_start, uhci_stop, uhci_dev_init, NULL, NULL,
    uhci_pipe_open, uhci_pipe_close, uhci_submit, uhci_cancel, uhci_poll, uhci_irq, NULL, NULL,
    uhci_iso_start, uhci_iso_stop, uhci_iso_service
};

/* stop BIOS legacy support (SMIs, port 60h/64h traps) and the BIOS schedule */
void uhci_early(struct pcidev *p)
{
    u32 io = pci_bar_io(p, 4);
    ubios_save(p, (u16)io);
    pci_wr16(p->bus, p->dev, p->fn, 0xC0, 0x8F00);
    if (!io) return;
    pci_enable(p, 0, 1, 1);
    outw((u16)(io + 4), 0);                 /* USBINTR */
    outw((u16)io, (u16)(inw((u16)io) & ~1U)); /* stop */
}

int uhci_attach(struct pcidev *p)
{
    struct uhci *u;
    struct usb_hc *hc;
    u32 io = pci_bar_io(p, 4);
    if (!io) return -1;
    pci_enable(p, 0, 1, 1);
    u = kmalloc(sizeof(*u));
    u->io = (u16)io;
    hc = usb_add_hc(&uhci_ops, p, u);
    if (!hc) return -1;
    u->hc = hc;
    hc->iobase = (u16)io;
    return uhci_start(hc);
}
