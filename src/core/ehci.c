/* ehci.c - EHCI host controller driver (USB 2.0) with companion hand-off and split transactions */
#include "usb.h"

#define OP_CMD      0x00
#define OP_STS      0x04
#define OP_INTR     0x08
#define OP_FRINDEX  0x0C
#define OP_SEGMENT  0x10
#define OP_PERIODIC 0x14
#define OP_ASYNC    0x18
#define OP_CONFIG   0x40
#define OP_PORTSC   0x44

#define QTD_ACTIVE  0x80
#define QTD_HALTED  0x40
#define QTD_DBE     0x20
#define QTD_BABBLE  0x10
#define QTD_XACT    0x08
#define QTD_IOC     (1UL << 15)
#define PID_OUT     0
#define PID_IN      1
#define PID_SETUP   2
#define LP_T        1UL
#define LP_QH       2UL

#define PSC_W1C     0x2AUL      /* CSC | PEC | OCC */

struct qtd {                    /* 64 bytes, 32-byte aligned */
    volatile u32 next, alt, token, buf[5], bufhi[5];
    u32 len;
    struct qtd *vnext;
    u32 status_stage;
};

struct qh {                     /* 96 bytes, 32-byte aligned */
    volatile u32 hlink, ep1, ep2, cur;
    volatile u32 next, alt, token, buf[5], bufhi[5];
    u32 rsv[7];
};

struct epipe {
    struct usb_pipe *p;
    struct qh *qh;
    struct qtd *first, *last, *status;
    struct usb_xfer *active, *qhead, *qtail;
    u8 *setup;
    int periodic;
    struct epipe *next;
};

struct ehci {
    struct usb_hc *hc;
    u8 *cap, *op;
    u32 *frames;
    struct qh *ahead;           /* async list head (H bit) */
    struct qh *ihead;           /* periodic anchor */
    struct qtd *dead;           /* inactive qTD used as alternate next */
    struct epipe *pipes;
    int nports;
    int ncc;
    u32 lastfr;
};

static struct usb_hcops ehci_ops;
static struct usb_hubops ehci_hubops;

#define ORD(e, r) RD32((e)->op + (r))
#define OWR(e, r, v) WR32((e)->op + (r), (v))

static void doorbell(struct ehci *e)
{
    u32 t0 = plat_ms();
    if (!(ORD(e, OP_CMD) & 0x20)) return;   /* async schedule off */
    OWR(e, OP_STS, 0x20);
    OWR(e, OP_CMD, ORD(e, OP_CMD) | 0x40);
    while (!(ORD(e, OP_STS) & 0x20) && plat_ms() - t0 < 50) cpu_pause();
    OWR(e, OP_STS, 0x20);
}

static void wait_frame(struct ehci *e)
{
    u32 f = ORD(e, OP_FRINDEX) >> 3, t0 = plat_ms();
    while ((ORD(e, OP_FRINDEX) >> 3) == f && plat_ms() - t0 < 5) ;
    udelay(150);
}

static void qh_link(struct ehci *e, struct epipe *ep)
{
    struct qh *head = ep->periodic ? e->ihead : e->ahead;
    ep->qh->hlink = head->hlink;
    head->hlink = vphys(ep->qh) | LP_QH;
}

static void qh_unlink(struct ehci *e, struct epipe *ep)
{
    struct qh *cur = ep->periodic ? e->ihead : e->ahead;
    u32 me = vphys(ep->qh) | LP_QH;
    int guard = 0;
    while (guard++ < 256) {
        if (cur->hlink == me) {
            cur->hlink = ep->qh->hlink;
            break;
        }
        if (cur->hlink & LP_T) break;
        cur = pvirt(cur->hlink & ~0x1FUL);
        if (!cur || cur == e->ahead) break;
    }
    if (ep->periodic) wait_frame(e);
    else doorbell(e);
}

static void qh_setup(struct epipe *ep)
{
    struct usb_pipe *p = ep->p;
    struct usb_dev *d = p->dev;
    u32 eps = d->speed == USB_HS ? 2 : d->speed == USB_LS ? 1 : 0, mps = p->maxpkt ? p->maxpkt : 8;
    struct qh *q = ep->qh;
    q->ep1 = d->addr | ((u32)(p->ep & 0x0F) << 8) | (eps << 12) | ((u32)mps << 16) |
             (p->type == EP_CTRL ? (1UL << 14) : 0) |
             ((p->type == EP_CTRL && d->speed != USB_HS) ? (1UL << 27) : 0) |
             (p->type == EP_INT ? 0 : (8UL << 28));
    q->ep2 = (1UL << 30);
    if (d->speed != USB_HS && d->tthub)
        q->ep2 |= ((u32)d->tthub->addr << 16) | ((u32)d->ttport << 23);
    if (p->type == EP_INT) {
        q->ep2 |= 0x01;                             /* S-mask: microframe 0 */
        if (d->speed != USB_HS) q->ep2 |= 0x1C00;   /* C-mask: microframes 2..4 */
    }
    if (d->speed == USB_HS && p->mult) q->ep2 = (q->ep2 & ~(3UL << 30)) | ((u32)(p->mult + 1) << 30);
}

static struct qtd *mk_qtd(struct ehci *e, u32 pid, u8 *buf, u32 len, int toggle, struct qtd *alt)
{
    struct qtd *t = kmalloc_align(sizeof(struct qtd), 32);
    u32 ph, i;
    if (!t) return NULL;
    t->next = LP_T;
    t->alt = alt ? vphys(alt) : vphys(e->dead);
    t->token = QTD_ACTIVE | (pid << 8) | (3UL << 10) | (len << 16) | ((u32)toggle << 31);
    t->len = len;
    if (len) {
        ph = vphys(buf);
        t->buf[0] = ph;
        for (i = 1; i < 5; i++) t->buf[i] = (ph & ~4095UL) + i * 4096UL;
    }
    return t;
}

static void chain(struct qtd **first, struct qtd **last, struct qtd *t)
{
    if (*last) {
        (*last)->next = vphys(t);
        (*last)->vnext = t;
    } else {
        *first = t;
    }
    *last = t;
}

static int start_xfer(struct ehci *e, struct epipe *ep, struct usb_xfer *x)
{
    struct usb_pipe *p = ep->p;
    struct qtd *first = NULL, *last = NULL, *t, *st = NULL;
    u32 len = x->len, off = 0, pid;
    int tog = 0;
    ep->status = NULL;
    if (p->type == EP_CTRL) {
        memcpy(ep->setup, x->setup, 8);
        st = mk_qtd(e, (x->in && len) ? PID_OUT : PID_IN, NULL, 0, 1, NULL);
        st->token |= QTD_IOC;
        st->status_stage = 1;
        chain(&first, &last, mk_qtd(e, PID_SETUP, ep->setup, 8, 0, NULL));
        tog = 1;
        pid = x->in ? PID_IN : PID_OUT;
        while (off < len) {
            u32 ph = vphys(x->buf + off), n = MIN(5 * 4096UL - (ph & 4095), len - off);
            t = mk_qtd(e, pid, x->buf + off, n, tog, st);
            chain(&first, &last, t);
            off += n;
            if ((n / p->maxpkt) & 1) tog ^= 1;
        }
        chain(&first, &last, st);
        ep->status = st;
    } else {
        pid = (p->ep & 0x80) ? PID_IN : PID_OUT;
        do {
            u32 ph = vphys(x->buf + off), n = MIN(5 * 4096UL - (ph & 4095), len - off);
            if (n > 16384 && len - off > n) n = 16384;
            t = mk_qtd(e, pid, x->buf + off, n, 0, NULL);
            chain(&first, &last, t);
            off += n;
        } while (off < len);
        last->token |= QTD_IOC;
    }
    for (t = first; t; t = t->vnext) if (!t) return XS_ERROR;
    ep->first = first;
    ep->last = last;
    ep->active = x;
    x->hcpriv = ep;
    /* load the queue head overlay (keep the data toggle for bulk/interrupt) */
    ep->qh->alt = LP_T;
    ep->qh->token &= 0x80000000UL;
    ep->qh->next = vphys(first);
    return 0;
}

static void free_qtds(struct epipe *ep)
{
    struct qtd *t = ep->first;
    while (t) {
        struct qtd *n = t->vnext;
        kfree(t);
        t = n;
    }
    ep->first = ep->last = ep->status = NULL;
}

static void finish(struct ehci *e, struct epipe *ep, int status, struct usb_xfer **done)
{
    struct usb_xfer *x = ep->active;
    free_qtds(ep);
    ep->active = NULL;
    x->status = status;
    x->next = *done;
    *done = x;
    if (ep->qhead) {
        struct usb_xfer *n = ep->qhead;
        ep->qhead = n->next;
        if (!ep->qhead) ep->qtail = NULL;
        n->next = NULL;
        if (start_xfer(e, ep, n)) {
            n->status = XS_ERROR;
            n->next = *done;
            *done = n;
        }
    }
}

static void check_xfer(struct ehci *e, struct epipe *ep, struct usb_xfer **done)
{
    struct usb_xfer *x = ep->active;
    struct qtd *t;
    u32 actual = 0;
    int shortpkt = 0;
    for (t = ep->first; t; t = t->vnext) {
        u32 tok = t->token, rem, got;
        if (tok & QTD_ACTIVE) {
            if (shortpkt && !ep->status) break;     /* rest skipped through the alternate pointer */
            if (shortpkt && ep->status && !t->status_stage) continue;
            /* still running unless the queue head is halted */
            if (ep->qh->token & QTD_HALTED) {
                x->actual = actual;
                ep->qh->token &= 0x80000000UL;
                finish(e, ep, XS_ERROR, done);
            }
            return;
        }
        if (tok & QTD_HALTED) {
            int st = (tok & (QTD_BABBLE)) ? XS_BABBLE : (tok & (QTD_DBE | QTD_XACT)) ? XS_ERROR : XS_STALL;
            x->actual = actual;
            ep->qh->token = 0;          /* clear halt, reset toggle */
            ep->p->toggle = 0;
            finish(e, ep, st, done);
            return;
        }
        if (t->status_stage) break;
        if (ep->p->type == EP_CTRL && t == ep->first) continue;     /* setup */
        rem = (tok >> 16) & 0x7FFF;
        got = t->len - rem;
        actual += got;
        if (rem && ((tok >> 8) & 3) == PID_IN) shortpkt = 1;
        if (shortpkt && !ep->status) break;
    }
    x->actual = actual;
    finish(e, ep, XS_OK, done);
}

static void ehci_poll(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    struct epipe *ep;
    struct usb_xfer *done = NULL, *x;
    u32 st = ORD(e, OP_STS);
    if (st & 0x1F) OWR(e, OP_STS, st & 0x1F);
    for (ep = e->pipes; ep; ep = ep->next)
        if (ep->active) check_xfer(e, ep, &done);
    while ((x = done) != NULL) {
        done = x->next;
        x->next = NULL;
        if (x->status != XS_OK) hc->errors++;
        if (x->cb) x->cb(x);
    }
}

static int ehci_irq(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    u32 st = ORD(e, OP_STS) & 0x1F;
    if (!st || st == 0x1F) return 0;
    OWR(e, OP_STS, st);
    hc->pending = 1;
    return 1;
}

static int ehci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ehci *e = hc->priv;
    struct epipe *ep = x->pipe->hcpriv;
    if (!ep) return XS_ERROR;
    x->next = NULL;
    if (ep->active) {
        if (ep->qtail) ep->qtail->next = x; else ep->qhead = x;
        ep->qtail = x;
        return 0;
    }
    return start_xfer(e, ep, x);
}

static void reset_qh(struct ehci *e, struct epipe *ep)
{
    qh_unlink(e, ep);
    ep->qh->cur = 0;
    ep->qh->next = LP_T;
    ep->qh->alt = LP_T;
    ep->qh->token &= 0x80000000UL;
    qh_link(e, ep);
}

static void ehci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ehci *e = hc->priv;
    struct epipe *ep = x->pipe->hcpriv;
    struct usb_xfer **pp, *done = NULL;
    if (!ep) return;
    for (pp = &ep->qhead; *pp; pp = &(*pp)->next) {
        if (*pp == x) {
            *pp = x->next;
            if (ep->qtail == x) {
                struct usb_xfer *t = ep->qhead;
                while (t && t->next) t = t->next;
                ep->qtail = t;
            }
            x->status = XS_CANCEL;
            return;
        }
    }
    if (ep->active == x) {
        reset_qh(e, ep);
        finish(e, ep, XS_CANCEL, &done);
    }
}

static int ehci_pipe_open(struct usb_hc *hc, struct usb_pipe *p)
{
    struct ehci *e = hc->priv;
    struct epipe *ep;
    if (p->type == EP_ISO) {
        p->hcpriv = NULL;           /* scheduled by the isochronous code */
        return 0;
    }
    ep = kmalloc(sizeof(*ep));
    if (!ep) return -1;
    ep->p = p;
    ep->qh = kmalloc_align(sizeof(struct qh), 32);
    ep->setup = kmalloc(16);
    ep->periodic = p->type == EP_INT;
    qh_setup(ep);
    ep->qh->next = LP_T;
    ep->qh->alt = LP_T;
    qh_link(e, ep);
    ep->next = e->pipes;
    e->pipes = ep;
    p->hcpriv = ep;
    return 0;
}

static void ehci_pipe_close(struct usb_hc *hc, struct usb_pipe *p)
{
    struct ehci *e = hc->priv;
    struct epipe *ep = p->hcpriv, **pp;
    struct usb_xfer *x, *done = NULL;
    if (!ep) return;
    while ((x = ep->qhead) != NULL) { ep->qhead = x->next; x->status = XS_CANCEL; }
    ep->qtail = NULL;
    qh_unlink(e, ep);
    if (ep->active) finish(e, ep, XS_CANCEL, &done);
    for (pp = &e->pipes; *pp; pp = &(*pp)->next) if (*pp == ep) { *pp = ep->next; break; }
    kfree(ep->qh);
    kfree(ep->setup);
    kfree(ep);
    p->hcpriv = NULL;
}

static int ehci_dev_init(struct usb_hc *hc, struct usb_dev *d)
{
    if (ehci_pipe_open(hc, &d->ep0)) return -1;
    d->ep0.open = 1;
    return 0;
}

static int ehci_dev_update(struct usb_hc *hc, struct usb_dev *d)
{
    struct ehci *e = hc->priv;
    struct epipe *ep = d->ep0.hcpriv;
    if (!ep) return 0;
    qh_unlink(e, ep);
    qh_setup(ep);
    qh_link(e, ep);
    return 0;
}

/* ---- root hub ---- */
static int rh_nports(struct usb_hub *h) { return ((struct ehci *)h->hc->priv)->nports; }

static u32 rh_status(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    u32 v = ORD(e, OP_PORTSC + (port - 1) * 4), s = 0;
    if (v & 0x2000) return PS_OWNER;
    if (v & 0x0001) s |= PS_CONNECT;
    if (v & 0x0004) s |= PS_ENABLE;
    if (v & 0x0100) s |= PS_RESET;
    if (v & 0x1000) s |= PS_POWER;
    if (v & 0x0010) s |= PS_OVERCUR;
    if (v & 0x0002) s |= PS_CCHANGE;
    if (v & 0x0028) s |= PS_OTHERCHG;
    if (((v >> 10) & 3) == 1) s |= PS_LINEK;
    s |= (u32)USB_HS << PS_SPEEDSH;
    return s;
}

static void rh_clear(struct usb_hub *h, int port, u32 st)
{
    struct ehci *e = h->hc->priv;
    int r = OP_PORTSC + (port - 1) * 4;
    (void)st;
    OWR(e, r, ORD(e, r));               /* write back: clears the W1C change bits */
}

static void give_companion(struct ehci *e, int port)
{
    int r = OP_PORTSC + (port - 1) * 4;
    OWR(e, r, (ORD(e, r) & ~PSC_W1C) | 0x2000);
    kprintf("ehci: port %d handed to companion controller\n", port);
}

static int rh_reset(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    int r = OP_PORTSC + (port - 1) * 4, i;
    u32 v = ORD(e, r);
    if (e->ncc && ((v >> 10) & 3) == 1) {      /* K-state: low speed device */
        give_companion(e, port);
        return 1;
    }
    v &= ~(PSC_W1C | 0x0004UL);
    OWR(e, r, v | 0x0100);
    mdelay(50);
    OWR(e, r, ORD(e, r) & ~(PSC_W1C | 0x0100UL));
    for (i = 0; i < 100 && (ORD(e, r) & 0x0100); i++) udelay(100);
    mdelay(5);
    v = ORD(e, r);
    if (!(v & 0x0001)) return -1;
    if (!(v & 0x0004)) {
        if (e->ncc) {
            give_companion(e, port);
            return 1;
        }
        return -1;
    }
    return 0;
}

static void rh_disable(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    int r = OP_PORTSC + (port - 1) * 4;
    OWR(e, r, ORD(e, r) & ~(PSC_W1C | 0x0004UL));
}

static void legacy_handoff(struct pcidev *p, u32 hccparams)
{
    u32 eecp = (hccparams >> 8) & 0xFF;
    int guard = 0, i;
    while (eecp >= 0x40 && guard++ < 16) {
        u32 v = pci_rd32(p->bus, p->dev, p->fn, (int)eecp);
        if ((v & 0xFF) == 1) {
            if (v & (1UL << 16)) {
                pci_wr8(p->bus, p->dev, p->fn, (int)eecp + 3, 1);
                for (i = 0; i < 1000; i++) {
                    if (!(pci_rd32(p->bus, p->dev, p->fn, (int)eecp) & (1UL << 16))) break;
                    mdelay(1);
                }
                if (i == 1000) {
                    /* BIOS bug: take it anyway, like other systems do */
                    kprintf("ehci: BIOS did not release the controller, taking it anyway\n");
                    pci_wr8(p->bus, p->dev, p->fn, (int)eecp + 2, 0);
                }
            } else {
                pci_wr8(p->bus, p->dev, p->fn, (int)eecp + 3, 1);
            }
            /* SMIs off, but the one on a change of ownership stays as the BIOS set it: it
               tells the BIOS when the controller is given back */
            pci_wr32(p->bus, p->dev, p->fn, (int)eecp + 4,
                     (pci_rd32(p->bus, p->dev, p->fn, (int)eecp + 4) & 0x2000UL) | 0xE0000000UL);
        }
        eecp = (v >> 8) & 0xFF;
    }
}

/* the controller back to the BIOS (legacy USB keyboard in DOS after Windows): OS Owned
   cleared, the BIOS takes it on the ownership SMI */
static void legacy_release(struct pcidev *p, u32 hccparams)
{
    u32 eecp = (hccparams >> 8) & 0xFF;
    int guard = 0;
    while (eecp >= 0x40 && guard++ < 16) {
        u32 v = pci_rd32(p->bus, p->dev, p->fn, (int)eecp);
        if ((v & 0xFF) == 1) {
            if (pci_rd32(p->bus, p->dev, p->fn, (int)eecp + 4) & 0x2000UL) {
                pci_wr8(p->bus, p->dev, p->fn, (int)eecp + 3, 0);
                kprintf("ehci: controller back to the BIOS\n");
            }
            return;
        }
        eecp = (v >> 8) & 0xFF;
    }
}

static int ehci_start(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    u32 hcs = RD32(e->cap + 4), hcc = RD32(e->cap + 8);
    int i;
    e->op = e->cap + RD8(e->cap);
    e->nports = hcs & 0x0F;
    e->ncc = (hcs >> 12) & 0x0F;
    legacy_handoff(&hc->pci, hcc);
    OWR(e, OP_INTR, 0);
    OWR(e, OP_CMD, ORD(e, OP_CMD) & ~1UL);
    for (i = 0; i < 100 && !(ORD(e, OP_STS) & 0x1000); i++) mdelay(1);
    OWR(e, OP_CMD, 2);
    for (i = 0; i < 250 && (ORD(e, OP_CMD) & 2); i++) mdelay(1);
    e->frames = kmalloc_align(4096, 4096);
    e->dead = kmalloc_align(sizeof(struct qtd), 32);
    e->dead->next = LP_T;
    e->dead->alt = LP_T;
    e->dead->token = QTD_HALTED;
    e->ahead = kmalloc_align(sizeof(struct qh), 32);
    e->ahead->hlink = vphys(e->ahead) | LP_QH;
    e->ahead->ep1 = (1UL << 15) | (2UL << 12) | (64UL << 16);
    e->ahead->next = LP_T;
    e->ahead->alt = LP_T;
    e->ahead->token = QTD_HALTED;
    e->ihead = kmalloc_align(sizeof(struct qh), 32);
    e->ihead->hlink = LP_T;
    e->ihead->ep1 = (2UL << 12) | (64UL << 16);
    e->ihead->next = LP_T;
    e->ihead->alt = LP_T;
    for (i = 0; i < 1024; i++) e->frames[i] = vphys(e->ihead) | LP_QH;
    if (hcc & 1) OWR(e, OP_SEGMENT, 0);
    OWR(e, OP_PERIODIC, vphys(e->frames));
    OWR(e, OP_ASYNC, vphys(e->ahead));
    OWR(e, OP_STS, 0x3F);
    OWR(e, OP_INTR, 0x07);              /* USBINT, USBERRINT, port change */
    OWR(e, OP_CMD, (8UL << 16) | 0x20 | 0x10 | 1);  /* ITC 1ms, async+periodic enable, run */
    OWR(e, OP_CONFIG, 1);
    mdelay(5);
    if (hcs & 0x10)
        for (i = 0; i < e->nports; i++)
            OWR(e, OP_PORTSC + i * 4, (ORD(e, OP_PORTSC + i * 4) & ~PSC_W1C) | 0x1000);
    mdelay(20);
    usb_new_hub(hc, NULL, &ehci_hubops, e->nports);
    usb_irq_setup(hc);
    kprintf("ehci: %s irq %d, %d ports, %d companion(s)\n", hc->name, hc->irq, e->nports, e->ncc);
    return 0;
}

static void ehci_stop(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    int i;
    OWR(e, OP_INTR, 0);
    OWR(e, OP_CMD, 0);
    for (i = 0; i < 100 && !(ORD(e, OP_STS) & 0x1000); i++) mdelay(1);
    OWR(e, OP_CONFIG, 0);               /* route every port back to the companions */
    OWR(e, OP_CMD, 2);                  /* host controller reset */
    for (i = 0; i < 250 && (ORD(e, OP_CMD) & 2); i++) mdelay(1);
    legacy_release(&hc->pci, RD32(e->cap + 8));
    while (e->pipes) {
        struct epipe *ep = e->pipes;
        e->pipes = ep->next;
        free_qtds(ep);
        kfree(ep->qh);
        kfree(ep->setup);
        kfree(ep);
    }
    kfree(e->frames);
    kfree(e->dead);
    kfree(e->ahead);
    kfree(e->ihead);
    kfree(e);
}

/* ---- isochronous ----
   High-speed endpoints: one iTD per frame with the packets of its microframes.
   Full-speed endpoints behind a transaction translator: one siTD per packet (frame).
   Both are linked in front of the frame's interrupt queue heads. */
struct itd {                    /* 64 bytes used (92 with 64-bit extensions), 32-byte aligned */
    volatile u32 next, tr[8], bp[7], bphi[7];
    u32 rsv[9];
};

struct sitd {                   /* 28 bytes used (36 with 64-bit extensions), 32-byte aligned */
    volatile u32 next, caps, sched, state, bp0, bp1, back, bphi[2];
    u32 rsv[7];
};

struct eiso {
    int fs;                     /* siTDs */
    int ppf;                    /* packets per iTD (frame) */
    u32 fstep;                  /* frames between iTDs / siTDs */
    int nslot;                  /* TDs */
    void *td;                   /* nslot iTDs or siTDs */
    u32 *frame;
    u16 *len;                   /* siTD: bytes requested */
    int head, count;
    u32 next;
};

#define LP_ITD  0UL
#define LP_SITD 4UL

static u32 frame_now(struct ehci *e)
{
    u32 f = (ORD(e, OP_FRINDEX) >> 3) & 0x7FF;
    u32 d = (f - (e->lastfr & 0x7FF)) & 0x7FF;
    e->lastfr += d;
    return e->lastfr;
}

static void iso_unlink(struct ehci *e, u32 me, u32 f)
{
    volatile u32 *pp = &e->frames[f & 1023];
    int guard = 0;
    while (guard++ < 32) {
        u32 v = *pp, type = (v >> 1) & 3;
        if ((v & ~0x1FUL) == me) {
            *pp = type == 0 ? ((struct itd *)pvirt(me))->next : ((struct sitd *)pvirt(me))->next;
            return;
        }
        if ((v & LP_T) || type == 1 || type == 3) return;       /* queue head / FSTN: end of the iso part */
        pp = (volatile u32 *)pvirt(v & ~0x1FUL);                  /* next link is the first dword */
    }
}

static int ehci_iso_start(struct usb_hc *hc, struct usb_iso *s)
{
    struct ehci *e = hc->priv;
    struct usb_dev *d = s->pipe->dev;
    struct eiso *ei = kmalloc(sizeof(*ei));
    u32 sz;
    if (!ei) return -1;
    ei->fs = d->speed < USB_HS;
    if (ei->fs && !d->tthub) return -1;                 /* full speed on a root port: companion's job */
    if (ei->fs) {
        ei->ppf = 1;
        ei->fstep = s->period_us / 1000;
        ei->nslot = s->npkt;
        sz = sizeof(struct sitd);
    } else {
        u32 uf = s->period_us / 125;                    /* microframes between packets */
        ei->ppf = uf >= 8 ? 1 : (int)(8 / uf);
        ei->fstep = uf >= 8 ? uf / 8 : 1;
        ei->nslot = s->npkt / ei->ppf;
        if (ei->nslot < 4) ei->nslot = 4;
        if (ei->nslot * ei->ppf > s->npkt) ei->nslot = s->npkt / ei->ppf;
        sz = sizeof(struct itd);
    }
    if (!ei->fstep) ei->fstep = 1;
    ei->td = kmalloc_align((u32)ei->nslot * sz, 32);
    ei->frame = kmalloc((u32)ei->nslot * 4);
    ei->len = kmalloc((u32)ei->nslot * 2);
    if (!ei->td || !ei->frame || !ei->len) return -1;
    memset(ei->td, 0, (u32)ei->nslot * sz);
    ei->head = ei->count = 0;
    ei->next = frame_now(e) + 2 + ((RD32(e->cap + 8) >> 4) & 0xF) / 8 + 1;    /* isochronous threshold */
    s->hcpriv = ei;
    return 0;
}

static void sitd_fill(struct ehci *e, struct usb_iso *s, struct eiso *ei, int k)
{
    struct sitd *t = (struct sitd *)ei->td + k;
    struct usb_pipe *p = s->pipe;
    struct usb_dev *d = p->dev;
    u8 *b = s->buf + (u32)k * s->pktsize;
    int len = s->in ? s->pktsize : s->fill(s, b, s->pktsize);
    u32 ph = vphys(b), nss;
    (void)e;
    if (len < 0) len = 0;
    ei->len[k] = (u16)len;
    t->caps = (s->in ? (1UL << 31) : 0) | ((u32)d->ttport << 24) | ((u32)d->tthub->addr << 16) |
              ((u32)(p->ep & 0x0F) << 8) | d->addr;
    if (s->in) {
        t->sched = 0x01 | (0xFCUL << 8);                /* start split in uframe 0, complete splits 2..7 */
        t->bp1 = (ph + (u32)len) & ~0xFFFUL;
    } else {
        nss = ((u32)len + 187) / 188;
        if (!nss) nss = 1;
        if (nss > 6) nss = 6;
        t->sched = (1UL << nss) - 1;                    /* one start split per 188 bytes */
        t->bp1 = ((ph + (u32)len) & ~0xFFFUL) | ((nss > 1 ? 1UL : 0UL) << 3) | nss;    /* TP: all / begin */
    }
    t->bp0 = ph;
    t->bphi[0] = t->bphi[1] = 0;
    t->back = LP_T;
    t->state = ((u32)len << 16) | 0x80;                /* active */
}

static void itd_fill(struct ehci *e, struct usb_iso *s, struct eiso *ei, int k)
{
    struct itd *t = (struct itd *)ei->td + k;
    struct usb_pipe *p = s->pipe;
    u32 uf = s->period_us / 125, base = vphys(s->buf + (u32)k * ei->ppf * s->pktsize), pg, i;
    (void)e;
    if (!uf || uf > 8) uf = 8;
    for (pg = 0; pg < 7; pg++) {
        t->bp[pg] = (base & ~0xFFFUL) + pg * 4096UL;
        t->bphi[pg] = 0;
    }
    t->bp[0] |= p->dev->addr | ((u32)(p->ep & 0x0F) << 8);
    t->bp[1] |= (u32)p->maxpkt | (s->in ? (1UL << 11) : 0);
    t->bp[2] |= (u32)(p->mult + 1);
    for (i = 0; i < 8; i++) t->tr[i] = 0;
    for (i = 0; i < (u32)ei->ppf; i++) {
        u8 *b = s->buf + ((u32)k * ei->ppf + i) * s->pktsize;
        u32 ph = vphys(b), off = ph - (base & ~0xFFFUL);
        int len = s->in ? s->pktsize : s->fill(s, b, s->pktsize);
        if (len < 0) len = 0;
        t->tr[i * uf] = (1UL << 31) | ((u32)len << 16) | ((off >> 12) << 12) | (off & 0xFFF);
    }
}

static void ehci_iso_service(struct usb_hc *hc, struct usb_iso *s)
{
    struct ehci *e = hc->priv;
    struct eiso *ei = s->hcpriv;
    u32 now = frame_now(e), ahead = s->ahead_us / 1000 + 1, i;
    while (ei->count && (s32)(now - ei->frame[ei->head]) > 0) {
        int k = ei->head;
        if (ei->fs) {
            struct sitd *t = (struct sitd *)ei->td + k;
            u32 st = t->state;
            iso_unlink(e, vphys(t), ei->frame[k]);
            if (st & 0x80) s->missed++;
            else if (st & 0x7C) s->errors++;
            else if (s->in) s->done(s, s->buf + (u32)k * s->pktsize, (int)(ei->len[k] - ((st >> 16) & 0x3FF)));
            s->packets++;
        } else {
            struct itd *t = (struct itd *)ei->td + k;
            u32 uf = s->period_us / 125;
            if (!uf || uf > 8) uf = 8;
            iso_unlink(e, vphys(t), ei->frame[k]);
            for (i = 0; i < (u32)ei->ppf; i++) {
                u32 tr = t->tr[i * uf];
                if (tr & (1UL << 31)) s->missed++;
                else if (tr & (7UL << 28)) s->errors++;
                else if (s->in)
                    s->done(s, s->buf + ((u32)k * ei->ppf + i) * s->pktsize, (int)((tr >> 16) & 0xFFF));
                s->packets++;
            }
        }
        ei->head = (ei->head + 1) % ei->nslot;
        ei->count--;
    }
    if ((s32)(ei->next - (now + 2)) < 0) {
        if (ei->count == 0 && s->packets) usb_iso_late(s);
        ei->next = now + 3;
    }
    while (ei->count < ei->nslot && (s32)(ei->next - (now + ahead)) < 0) {
        int k = (ei->head + ei->count) % ei->nslot;
        u32 f = ei->next & 1023, ph;
        if (ei->fs) {
            struct sitd *t = (struct sitd *)ei->td + k;
            sitd_fill(e, s, ei, k);
            ph = vphys(t) | LP_SITD;
            t->next = e->frames[f];
        } else {
            struct itd *t = (struct itd *)ei->td + k;
            itd_fill(e, s, ei, k);
            ph = vphys(t) | LP_ITD;
            t->next = e->frames[f];
        }
        e->frames[f] = ph;
        ei->frame[k] = ei->next;
        ei->count++;
        ei->next += ei->fstep;
    }
}

static void ehci_iso_stop(struct usb_hc *hc, struct usb_iso *s)
{
    struct ehci *e = hc->priv;
    struct eiso *ei = s->hcpriv;
    if (!ei) return;
    while (ei->count) {
        int k = ei->head;
        u32 me = ei->fs ? vphys((struct sitd *)ei->td + k) : vphys((struct itd *)ei->td + k);
        iso_unlink(e, me, ei->frame[k]);
        ei->head = (ei->head + 1) % ei->nslot;
        ei->count--;
    }
    wait_frame(e);
    wait_frame(e);
    kfree(ei->td);
    kfree(ei->frame);
    kfree(ei->len);
    kfree(ei);
    s->hcpriv = NULL;
}

static struct usb_hubops ehci_hubops = { rh_nports, rh_status, rh_clear, rh_reset, rh_disable, NULL };

static struct usb_hcops ehci_ops = {
    "EHCI", HC_EHCI, ehci_start, ehci_stop, ehci_dev_init, ehci_dev_update, NULL,
    ehci_pipe_open, ehci_pipe_close, ehci_submit, ehci_cancel, ehci_poll, ehci_irq, NULL, NULL,
    ehci_iso_start, ehci_iso_stop, ehci_iso_service
};

void ehci_early(struct pcidev *p)
{
    u32 phys = pci_bar_mem(p, 0, NULL);
    u8 *cap, *op;
    if (!phys) return;
    pci_enable(p, 1, 0, 1);
    cap = usb_map(phys, 0x400);
    if (!cap) return;
    legacy_handoff(p, RD32(cap + 8));
    op = cap + RD8(cap);
    WR32(op + OP_INTR, 0);
}

int ehci_attach(struct pcidev *p)
{
    struct ehci *e;
    struct usb_hc *hc;
    u32 phys = pci_bar_mem(p, 0, NULL);
    if (!phys) return -1;
    pci_enable(p, 1, 0, 1);
    e = kmalloc(sizeof(*e));
    e->cap = usb_map(phys, 0x400);
    if (!e->cap) return -1;
    hc = usb_add_hc(&ehci_ops, p, e);
    if (!hc) return -1;
    e->hc = hc;
    hc->mmio = e->cap;
    return ehci_start(hc);
}
