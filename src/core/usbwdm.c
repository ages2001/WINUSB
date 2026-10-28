/* usbwdm.c - the USB WDM bridge (Windows 98 / ME): USB devices and interfaces no WINDRV class
   driver serves go to the WDM bus driver WINUSBW.SYS, whose Windows drivers send their USB
   requests here (inc/wdmbr.h). The bus driver queues requests at any time (wb_queue only
   links them and asks for a poll); they are started, and their results given back, from
   the core's poll, where the host controller drivers run anyway. */
#include "usb.h"
#include "../inc/wdmbr.h"

int usb_wdm_on;                         /* the platform has a bus driver (VxD of Windows 98 / ME) */
u16 usb_wdm_ids[8][2];                  /* devices given to the bridge though a class driver knows them */
int usb_wdm_nids;

#define MAXFN   32
#define CHUNK   65536UL                 /* bounce buffer of a transfer piece */

struct bpipe {
    struct usb_pipe p;
    struct bfn *fn;
    u8 ifnum;
    u8 used;
};

struct bfn {
    struct usb_dev *d;
    u8 used, iface, gone, cfgset;
    u8 gen;
    u8 ctlbusy;                         /* a control transfer of this function runs */
    struct bpipe *pipes[WB_MAXIF * WB_MAXEP];
    int npipes;
};

static struct bfn fns[MAXFN];
static struct wbops ops;
static int attached, changed, dropall;
static struct wbreq *inq, *inq_tail;    /* queued by the bus driver */
static struct wbreq *active;            /* started, not finished */
static struct wbreq *ctlwait;           /* control transfers waiting for the default pipe */
static u32 nlog;                        /* /D: requests logged */

/* request private fields */
#define R_XFER(r)   ((struct usb_xfer *)(r)->vx[0])
#define R_BOUNCE(r) ((u8 *)(r)->vx[1])
#define R_FN(r)     ((struct bfn *)(r)->vx[2])

/* ---- functions ---- */
static struct bfn *fn_of(u32 h)
{
    u32 i = h & 0xFF;
    if (i >= MAXFN || !fns[i].used || fns[i].gone || fns[i].gen != (u8)(h >> 8)) return NULL;
    return &fns[i];
}

static u32 fn_handle(struct bfn *f)
{
    return (u32)(f - fns) | ((u32)f->gen << 8);
}

static int forced(struct usb_dev *d)
{
    u16 vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8)), pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
    int i;
    for (i = 0; i < usb_wdm_nids; i++)
        if (usb_wdm_ids[i][0] == vid && (usb_wdm_ids[i][1] == pid || usb_wdm_ids[i][1] == 0xFFFF)) return 1;
    return 0;
}

/* devices given to the bridge even though a class driver of WINUSB knows them */
int usb_wdm_forced(struct usb_dev *d)
{
    return usb_wdm_on && usb_wdm_nids && d->ddesc[4] != 9 && forced(d);
}

/* number of interfaces (alternate setting 0) of the active configuration */
static int count_ifaces(struct usb_dev *d)
{
    u8 *p = d->cdesc, *end = d->cdesc + d->cdesc_len;
    int n = 0;
    while (p && p + 2 <= end && p[0]) {
        if (p[1] == 4 && p[3] == 0) n++;
        p += p[0];
    }
    return n;
}

static int wb_probe(struct usb_dev *d, u8 *iface, u8 *end)
{
    int i, slot = -1;
    (void)end;
    if (!usb_wdm_on || iface[5] == 9) return -1;
    for (i = 0; i < MAXFN; i++) {
        if (!fns[i].used) { if (slot < 0) slot = i; continue; }
        if (fns[i].d == d && fns[i].iface == iface[2]) return 0;
    }
    if (slot < 0) return -1;
    {
        u8 gen = fns[slot].gen;         /* a new generation: handles of the slot's last user die */
        memset(&fns[slot], 0, sizeof(fns[slot]));
        gen = (u8)(gen + 1);
        fns[slot].gen = gen ? gen : 1;
    }
    fns[slot].d = d;
    fns[slot].iface = iface[2];
    fns[slot].used = 1;
    changed = 1;
    plat_kick();
    return 0;
}

/* a device whose interfaces all came here is one function, unless Windows would split it
   (several interfaces of a device without a class of its own: a composite device) */
static int whole(struct bfn *f)
{
    struct usb_dev *d = f->d;
    int i, mine = 0;
    for (i = 0; i < d->ncls; i++)
        if (d->cls[i] != &wdm_class) return 0;
    for (i = 0; i < MAXFN; i++)
        if (fns[i].used && !fns[i].gone && fns[i].d == d) mine++;
    if (mine != count_ifaces(d)) return 0;
    return mine == 1 || d->ddesc[4] != 0;
}

/* the first function entry of a whole device stands for it */
static int listed(struct bfn *f)
{
    int i;
    if (!whole(f)) return 1;
    for (i = 0; i < MAXFN; i++)
        if (fns[i].used && !fns[i].gone && fns[i].d == f->d) return &fns[i] == f;
    return 0;
}

static void finish(struct wbreq *r, long st);
static void pipes_close(struct bfn *f, int ifnum);

/* a started request ended here (not by its completion): taken off the controller first; the
   completion the controller drivers may still call for it is ignored */
static void kill(struct wbreq *r, long st)
{
    r->vx[3] = (void *)1;
    if (R_XFER(r) && R_XFER(r)->status == XS_PENDING) usb_cancel(R_XFER(r));
    if (!r->pipe && R_FN(r)) R_FN(r)->ctlbusy = 0;
    finish(r, st);
}

static void wb_detach(struct usb_dev *d)
{
    int i;
    struct wbreq **pp, *r;
    for (i = 0; i < MAXFN; i++) {
        struct bfn *f = &fns[i];
        if (!f->used || f->d != d) continue;
        f->gone = 1;
        changed = 1;
    }
    /* transfers of the device end: cancelled at the controller first */
    for (pp = &active; (r = *pp) != NULL;) {
        struct bfn *f = R_FN(r);
        if (f && f->d == d) {
            *pp = r->next;
            kill(r, WBS_GONE);
        } else {
            pp = &r->next;
        }
    }
    for (pp = &ctlwait; (r = *pp) != NULL;) {
        struct bfn *f = R_FN(r);
        if (f && f->d == d) { *pp = r->next; finish(r, WBS_GONE); }
        else pp = &r->next;
    }
    for (i = 0; i < MAXFN; i++) {
        struct bfn *f = &fns[i];
        if (!f->used || f->d != d) continue;
        pipes_close(f, -1);
        f->d = NULL;
        f->used = 0;
    }
    plat_kick();
}

/* ---- pipes ---- */
static void pipes_close(struct bfn *f, int ifnum)
{
    int i, k = 0;
    for (i = 0; i < f->npipes; i++) {
        struct bpipe *bp = f->pipes[i];
        if (ifnum < 0 || bp->ifnum == ifnum) {
            if (!f->gone || bp->p.open) usb_pipe_close(&bp->p);
            bp->used = 0;
            kfree(bp);
        } else {
            f->pipes[k++] = bp;
        }
    }
    f->npipes = k;
}

static struct bpipe *pipe_of(struct bfn *f, u32 h)
{
    int i;
    for (i = 0; i < f->npipes; i++)
        if ((u32)f->pipes[i] == h && f->pipes[i]->used) return f->pipes[i];
    return NULL;
}

/* interface ifnum, alternate alt in the configuration descriptor */
static u8 *find_alt(struct usb_dev *d, int ifnum, int alt)
{
    u8 *p = d->cdesc, *end = d->cdesc + d->cdesc_len;
    while (p && p + 9 <= end && p[0]) {
        if (p[1] == 4 && p[2] == ifnum && p[3] == alt) return p;
        p += p[0];
    }
    return NULL;
}

/* the endpoints of an interface setting opened as pipes, described in w */
static int pipes_open(struct bfn *f, struct wbif *w)
{
    struct usb_dev *d = f->d;
    u8 *ifd = find_alt(d, w->ifnum, w->alt), *p, *end = d->cdesc + d->cdesc_len;
    int n = 0;
    if (!ifd) return WBS_BADPARAM;
    w->cls = ifd[5];
    w->sub = ifd[6];
    w->proto = ifd[7];
    for (p = ifd + ifd[0]; p + 2 <= end && p[0] && p[1] != 4; p += p[0]) {
        struct bpipe *bp;
        struct wbpipe *o;
        if (p[1] != 5 || p[0] < 7 || n >= WB_MAXEP || f->npipes >= WB_MAXIF * WB_MAXEP) continue;
        bp = kmalloc(sizeof(*bp));
        if (!bp) return WBS_NOMEM;
        if (usb_pipe_open(d, &bp->p, p)) {
            kprintf("wdm: device %u endpoint %02x: pipe not opened\n", d->id, p[2]);
            kfree(bp);
            return WBS_ERROR;
        }
        bp->fn = f;
        bp->ifnum = w->ifnum;
        bp->used = 1;
        f->pipes[f->npipes++] = bp;
        o = &w->pipes[n++];
        o->handle = (u32)bp;
        o->ep = p[2];
        o->type = (u8)(p[3] & 3);
        o->interval = p[6];
        o->maxpkt = (u16)(bp->p.maxpkt * (bp->p.mult + 1U));
    }
    w->npipes = (u8)n;
    return WBS_OK;
}

/* every transfer of a pipe (NULL: of the function) cancelled */
static void abort_pipe(struct bfn *f, struct bpipe *bp)
{
    struct wbreq **pp, *r;
    for (pp = &active; (r = *pp) != NULL;) {
        if (R_FN(r) == f && (!bp || r->pipe == (u32)bp)) {
            *pp = r->next;
            kill(r, WBS_CANCEL);
        } else {
            pp = &r->next;
        }
    }
    for (pp = &ctlwait; (r = *pp) != NULL;) {
        if (R_FN(r) == f && !bp) { *pp = r->next; finish(r, WBS_CANCEL); }
        else pp = &r->next;
    }
}

/* ---- requests ---- */
static void finish(struct wbreq *r, long st)
{
    if ((cb->opts & OPT_DEBUG) && nlog < 200 && st != WBS_OK) {
        nlog++;
        kprintf("wdm: fn %04lx op %lu: status %ld\n", r->func, r->op, st);
    }
    if (R_XFER(r)) kfree(R_XFER(r));
    if (R_BOUNCE(r)) kfree(R_BOUNCE(r));
    r->vx[0] = r->vx[1] = NULL;
    r->status = st;
    if (attached && ops.done) ops.done(ops.ctx, r);
}

static void active_remove(struct wbreq *r)
{
    struct wbreq **pp;
    for (pp = &active; *pp; pp = &(*pp)->next)
        if (*pp == r) { *pp = r->next; return; }
}

static long xs_status(int s)
{
    switch (s) {
    case XS_OK: return WBS_OK;
    case XS_STALL: return WBS_STALL;
    case XS_TIMEOUT: return WBS_TIMEOUT;
    case XS_CANCEL: return WBS_CANCEL;
    case XS_BABBLE: return WBS_BABBLE;
    case XS_GONE: return WBS_GONE;
    }
    return WBS_ERROR;
}

static int start_piece(struct wbreq *r);
static void start_ctl_waiting(struct bfn *f);

static void xfer_done(struct usb_xfer *x)
{
    struct wbreq *r = x->priv;
    struct bfn *f = R_FN(r);
    u32 got = x->actual;
    int in = (r->flags & WBRF_IN) != 0;
    if (x->status == XS_PENDING || r->vx[3]) return;
    if (in && got) memcpy(r->buf + r->actual, R_BOUNCE(r), got);
    r->actual += got;
    if (x->status == XS_OK && !r->cancel && r->pipe && r->actual < r->len && got == x->len) {
        if (start_piece(r) == 0) return;            /* the next piece of a long transfer */
    }
    active_remove(r);
    if (!r->pipe && f) {
        f->ctlbusy = 0;
        start_ctl_waiting(f);
    }
    if (x->status != XS_OK) {
        finish(r, r->cancel ? WBS_CANCEL : xs_status(x->status));
        return;
    }
    if (r->cancel) { finish(r, WBS_CANCEL); return; }
    if (in && r->actual < r->len && !(r->flags & WBRF_SHORTOK) && r->pipe) { finish(r, WBS_SHORT); return; }
    finish(r, WBS_OK);
}

/* one piece of a transfer through the bounce buffer (control transfers in one piece) */
static int start_piece(struct wbreq *r)
{
    struct usb_xfer *x = R_XFER(r);
    struct bfn *f = R_FN(r);
    u32 n = r->len - r->actual;
    if (r->pipe && n > CHUNK) n = CHUNK;
    memset(x, 0, sizeof(*x));
    if (r->pipe) {
        x->pipe = &((struct bpipe *)r->pipe)->p;
    } else {
        x->pipe = &f->d->ep0;
        memcpy(x->setup, r->setup, 8);
    }
    x->buf = R_BOUNCE(r);
    x->len = n;
    x->in = (u8)((r->flags & WBRF_IN) != 0);
    x->cb = xfer_done;
    x->priv = r;
    if (!x->in && n) memcpy(R_BOUNCE(r), r->buf + r->actual, n);
    return usb_submit(x);
}

static void start_xfer(struct wbreq *r)
{
    struct bfn *f = R_FN(r);
    u32 blen;
    int e;
    if (r->pipe) {
        struct bpipe *bp = pipe_of(f, r->pipe);
        if (!bp) { finish(r, WBS_BADPARAM); return; }
        if (bp->p.type == EP_ISO) { finish(r, WBS_BADPARAM); return; }
        /* the direction is the endpoint's */
        if (bp->p.type != EP_CTRL) r->flags = (r->flags & ~WBRF_IN) | ((bp->p.ep & 0x80) ? WBRF_IN : 0);
        blen = r->len > CHUNK ? CHUNK : r->len;
    } else {
        if (f->ctlbusy) {               /* one control transfer at a time on the default pipe */
            struct wbreq **pp = &ctlwait;
            while (*pp) pp = &(*pp)->next;
            r->next = NULL;
            *pp = r;
            return;
        }
        r->flags = (r->flags & ~WBRF_IN) | ((r->setup[0] & 0x80) ? WBRF_IN : 0);
        r->len = MIN(r->len, (u32)(r->setup[6] | (r->setup[7] << 8)));
        blen = r->len;
    }
    r->vx[0] = kmalloc(sizeof(struct usb_xfer));
    r->vx[1] = kmalloc_align(blen ? blen : 16, 64);
    if (!R_XFER(r) || !R_BOUNCE(r)) { finish(r, WBS_NOMEM); return; }
    r->actual = 0;
    r->next = active;
    active = r;
    if (!r->pipe) f->ctlbusy = 1;
    e = start_piece(r);
    if (e) {
        active_remove(r);
        if (!r->pipe) { f->ctlbusy = 0; }
        finish(r, xs_status(e));
        if (!r->pipe) start_ctl_waiting(f);
    }
}

static void start_ctl_waiting(struct bfn *f)
{
    struct wbreq **pp, *r;
    for (pp = &ctlwait; (r = *pp) != NULL; pp = &r->next) {
        if (R_FN(r) != f) continue;
        *pp = r->next;
        start_xfer(r);
        return;
    }
}

/* SET_CONFIGURATION (a whole device) / SET_INTERFACE and the pipes */
static long select_cfg(struct bfn *f, struct wbsel *s, int iface_only)
{
    struct usb_dev *d = f->d;
    int i, r;
    long st;
    if (!s || s->nif > WB_MAXIF) return WBS_BADPARAM;
    abort_pipe(f, NULL);
    if (iface_only) {
        if (s->nif != 1) return WBS_BADPARAM;
        if (f->iface != WBF_WHOLE && s->ifc[0].ifnum != f->iface) return WBS_BADPARAM;
        pipes_close(f, s->ifc[0].ifnum);
    } else {
        pipes_close(f, -1);
        /* a whole device is configured again; a function of a composite device shares the
           configuration WINUSB set with the other functions */
        if (listed(f) && whole(f)) {
            if (s->cfgval != d->config) {
                kprintf("wdm: device %u: configuration %u not available, %u used\n", d->id, s->cfgval, d->config);
                return WBS_BADPARAM;
            }
            r = usb_control(d, 0x00, 0x09, d->config, 0, NULL, 0);
            if (r < 0) return xs_status(r);
        }
    }
    for (i = 0; i < s->nif; i++) {
        struct wbif *w = &s->ifc[i];
        if (!whole(f) && w->ifnum != f->iface) return WBS_BADPARAM;
        if (iface_only || w->alt) {
            r = usb_set_interface(d, w->ifnum, w->alt);
            if (r < 0 && (iface_only ? w->alt : 1)) return xs_status(r);
        }
        st = pipes_open(f, w);
        if (st != WBS_OK) return st;
    }
    f->cfgset = 1;
    return WBS_OK;
}

static void run(struct wbreq *r)
{
    struct bfn *f = fn_of(r->func);
    r->vx[0] = r->vx[1] = r->vx[3] = NULL;
    /* /D: the first requests of the bus driver */
    if ((cb->opts & OPT_DEBUG) && nlog < 200) {
        nlog++;
        kprintf("wdm: fn %04lx op %lu pipe %08lx setup %02x %02x %02x%02x %02x%02x len %lu\n", r->func, r->op, r->pipe,
                r->setup[0], r->setup[1], r->setup[3], r->setup[2], r->setup[5], r->setup[4], r->len);
    }
    r->vx[2] = f;
    r->actual = 0;
    if (!f || !f->d || f->d->gone) { finish(r, WBS_GONE); return; }
    if (r->cancel) { finish(r, WBS_CANCEL); return; }
    switch (r->op) {
    case WBR_XFER:
        start_xfer(r);
        return;
    case WBR_SELCFG:
    case WBR_SELIF:
        finish(r, select_cfg(f, (struct wbsel *)r->buf, r->op == WBR_SELIF));
        return;
    case WBR_RESETPIPE:
    {
        struct bpipe *bp = pipe_of(f, r->pipe);
        if (!bp) { finish(r, WBS_BADPARAM); return; }
        abort_pipe(f, bp);
        finish(r, usb_clear_halt(&bp->p) ? WBS_ERROR : WBS_OK);
        return;
    }
    case WBR_ABORTPIPE:
    {
        struct bpipe *bp = r->pipe ? pipe_of(f, r->pipe) : NULL;
        if (r->pipe && !bp) { finish(r, WBS_BADPARAM); return; }
        abort_pipe(f, bp);
        finish(r, WBS_OK);
        return;
    }
    case WBR_UNCONFIG:
        abort_pipe(f, NULL);
        pipes_close(f, -1);
        f->cfgset = 0;
        finish(r, WBS_OK);
        return;
    }
    finish(r, WBS_BADPARAM);
}

/* the poll: queued requests started, cancel requests carried out */
static void wb_poll(void)
{
    struct wbreq *q, *r, **pp;
    u32 fl;
    if (!usb_wdm_on) return;
    if (dropall) {
        int i;
        dropall = 0;
        for (i = 0; i < MAXFN; i++)
            if (fns[i].used && !fns[i].gone) {
                abort_pipe(&fns[i], NULL);
                pipes_close(&fns[i], -1);
                fns[i].cfgset = 0;
            }
    }
    fl = irq_save();
    q = inq;
    inq = inq_tail = NULL;
    irq_restore(fl);
    while (q) {
        r = q;
        q = q->next;
        r->next = NULL;
        run(r);
    }
    for (pp = &active; (r = *pp) != NULL;) {
        if (r->cancel && R_XFER(r) && R_XFER(r)->status == XS_PENDING) {
            struct bfn *f = R_FN(r);
            int ctl = !r->pipe;
            *pp = r->next;
            kill(r, WBS_CANCEL);
            if (ctl && f) start_ctl_waiting(f);
            continue;
        }
        pp = &r->next;
    }
    for (pp = &ctlwait; (r = *pp) != NULL;) {
        if (r->cancel) { *pp = r->next; finish(r, WBS_CANCEL); }
        else pp = &r->next;
    }
    if (changed) {
        changed = 0;
        if (cb->opts & OPT_DEBUG) kprintf("wdm: functions changed%s\n", attached ? "" : " (no bus driver)");
        if (attached && ops.changed) ops.changed(ops.ctx);
    }
}

struct usb_class wdm_class = { "wdm", wb_probe, wb_detach, wb_poll };

/* ---- services (vxd/pnpusb.c wrappers) ---- */
u32 wb_attach(struct wbops *o)
{
    if (!usb_wdm_on) return 0;
    if (!o) {
        attached = 0;                   /* the pipes are closed at the next poll */
        dropall = 1;
        plat_kick();
        return WDMBR_VERSION;
    }
    if (o->size < sizeof(ops)) return 0;
    memcpy(&ops, o, sizeof(ops));
    attached = 1;
    changed = 1;
    plat_kick();
    return WDMBR_VERSION;
}

static void path_of(struct usb_dev *d, char *out, int max)
{
    u8 ports[8];
    int np = 0, i;
    struct usb_dev *c = d;
    char tmp[8];
    while (c && np < 8) {
        ports[np++] = c->port;
        c = c->parent;
    }
    ksnprintf(out, (u32)max, "B%02XD%02XF%X", d->hc->pci.bus, d->hc->pci.dev, d->hc->pci.fn);
    for (i = np - 1; i >= 0; i--) {
        ksnprintf(tmp, sizeof(tmp), "%c%u", i == np - 1 ? 'P' : '.', ports[i]);
        strlcat(out, tmp, (u32)max);
    }
}

int wb_list(struct wbfunc *out, int max)
{
    int i, n = 0;
    for (i = 0; i < MAXFN && n < max; i++) {
        struct bfn *f = &fns[i];
        struct usb_dev *d = f->d;
        struct wbfunc *o;
        if (!f->used || f->gone || !d || !listed(f)) continue;
        o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->handle = fn_handle(f);
        o->vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8));
        o->pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
        o->bcd = (u16)(d->ddesc[12] | (d->ddesc[13] << 8));
        o->bcdusb = (u16)(d->ddesc[2] | (d->ddesc[3] << 8));
        o->speed = d->speed;
        o->port = d->port;
        o->ncfg = d->ddesc[17];
        if (whole(f) && (d->ddesc[4] || count_ifaces(d) == 1)) {
            o->iface = WBF_WHOLE;
            if (d->ddesc[4]) {
                o->cls = d->ddesc[4]; o->sub = d->ddesc[5]; o->proto = d->ddesc[6];
            } else {
                u8 *ifd = find_alt(d, f->iface, 0);
                if (ifd) { o->cls = ifd[5]; o->sub = ifd[6]; o->proto = ifd[7]; }
            }
        } else {
            u8 *ifd = find_alt(d, f->iface, 0);
            o->iface = f->iface;
            if (ifd) { o->cls = ifd[5]; o->sub = ifd[6]; o->proto = ifd[7]; }
        }
        path_of(d, o->inst, sizeof(o->inst));
        strlcpy(o->manuf, d->manuf, sizeof(o->manuf));
        strlcpy(o->product, d->product, sizeof(o->product));
        strlcpy(o->serial, d->serial, sizeof(o->serial));
    }
    return n;
}

int wb_desc(u32 h, int type, int index, void *buf, int len)
{
    struct bfn *f = fn_of(h);
    int n;
    if (!f || !f->d) return -1;
    if (type == 1) {
        n = MIN(len, 18);
        memcpy(buf, f->d->ddesc, (u32)n);
        return n;
    }
    if (type == 2 && index == 0 && f->d->cdesc) {
        n = MIN(len, (int)f->d->cdesc_len);
        memcpy(buf, f->d->cdesc, (u32)n);
        return n;
    }
    return -1;
}

void wb_queue(struct wbreq *r)
{
    u32 fl;
    if (!r || r->magic != WBR_MAGIC) return;
    r->next = NULL;
    r->status = WBS_PENDING;
    fl = irq_save();
    if (inq_tail) inq_tail->next = r;
    else inq = r;
    inq_tail = r;
    irq_restore(fl);
    plat_kick();
}

void wb_cancel(struct wbreq *r)
{
    if (!r || r->magic != WBR_MAGIC) return;
    r->cancel = 1;
    plat_kick();
}

u32 wb_frame(u32 h)
{
    (void)h;
    return plat_ms();
}
