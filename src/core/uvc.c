/* uvc.c - USB Video Class 1.0 - 1.5 cameras: formats and frame sizes from the descriptors,
   probe / commit, isochronous or bulk streaming, frames assembled from the payloads into
   two buffers (one being filled, one complete for the reader). Formats: uncompressed YUY2
   and NV12, Motion-JPEG. The frames go to applications through WINF_CAM_* (WINCAM.EXE). */
#include "usb.h"

#define MAXCAM      4
#define MAXMODE     32
#define MAXALT      16
#define MAXFRAME    (640UL * 480 * 2)      /* largest frame kept */
#define NBULK       2

struct camalt {
    u8 alt;
    u8 *ep;
    u32 bytes;                  /* per service interval */
};

struct cam {
    struct usb_dev *d;
    u16 devid;
    u8 used, gone, vcif, vsif;
    u16 uvcver;
    char name[42];
    struct wincammode modes[MAXMODE];
    int nmodes;
    struct camalt alts[MAXALT];
    int nalts;
    u8 *bulkep;                 /* bulk streaming endpoint (alternate 0) */
    /* streaming */
    int active;                 /* mode + 1 */
    struct usb_pipe pipe;
    struct usb_iso iso;
    struct usb_xfer bx[NBULK];
    u8 *bbuf[NBULK];
    u32 payload;                /* dwMaxPayloadTransferSize; bulk: transfer size */
    u32 maxpay;                 /* bulk: dwMaxPayloadTransferSize */
    u32 bpay;                   /* bulk: bytes of the current payload */
    u8 info;                    /* header flags of the current payload */
    u32 lost;                   /* iso packets lost so far */
    u8 *buf[2];
    u32 bufsize;
    u32 len[2];
    u8 fill;                    /* buffer being filled */
    u8 ready;                   /* the other buffer holds a complete frame */
    u8 held;                    /* ... which the reader is copying */
    u8 bad;                     /* frame being filled is damaged */
    u8 fid;
    u32 seq, rseq;
    u32 frames, dropped;
    struct wincammode cur;
};

static struct cam *cams[MAXCAM];

static u32 rd32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static const u8 guid_yuy2[4] = { 'Y', 'U', 'Y', '2' };
static const u8 guid_nv12[4] = { 'N', 'V', '1', '2' };

static void frame_done(struct cam *c);

/* ---------------------------------------------------------------- descriptors */

static void add_mode(struct cam *c, u8 fmt, u8 type, const u8 *f)
{
    struct wincammode *m;
    u32 w, h, size;
    if (c->nmodes >= MAXMODE) return;
    m = &c->modes[c->nmodes++];
    m->fmt = fmt;
    m->frame = f[3];
    m->type = type;
    w = rd16(f + 5);
    h = rd16(f + 7);
    m->w = (u16)w;
    m->h = (u16)h;
    m->interval = rd32(f + 21);
    size = type == CAM_MJPEG ? rd32(f + 17) : type == CAM_NV12 ? w * h * 3 / 2 : w * h * 2;
    if (type == CAM_MJPEG && (size > MAXFRAME || !size)) size = MIN(w * h * 2, MAXFRAME);
    m->maxsize = size > MAXFRAME ? 0 : size;
}

/* the streaming interface: alternates, formats, frames */
static void parse_vs(struct cam *c, u8 *p, u8 *end)
{
    u8 fmt = 0, type = 0;
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == 4) {
            if (p[2] != c->vsif) break;
            if (c->nalts < MAXALT && p[3]) {
                u8 *ep = usb_find_desc(p + p[0], end, 5);
                if (ep && ep[2] & 0x80 && (ep[3] & 3) == EP_ISO) {
                    u16 mps = rd16(ep + 4);
                    struct camalt *a = &c->alts[c->nalts++];
                    a->alt = p[3];
                    a->ep = ep;
                    a->bytes = (u32)(mps & 0x7FF) * (1 + ((mps >> 11) & 3));
                    if (c->d->speed >= USB_SS && ep + ep[0] + 6 <= end && ep[ep[0] + 1] == 0x30)
                        a->bytes = (u32)rd16(ep + ep[0] + 4) ? rd16(ep + ep[0] + 4) :
                                   (u32)(mps & 0x7FF) * (ep[ep[0] + 2] + 1) * ((ep[ep[0] + 3] & 3) + 1);
                }
            } else if (!p[3]) {
                u8 *ep = usb_find_desc(p + p[0], end, 5);
                u8 *nx = usb_find_desc(p + p[0], end, 4);
                if (ep && (!nx || ep < nx) && (ep[2] & 0x80) && (ep[3] & 3) == EP_BULK) c->bulkep = ep;
            }
            continue;
        }
        if (p[1] != 0x24 || p[0] < 3) continue;
        switch (p[2]) {
        case 0x04:                      /* VS_FORMAT_UNCOMPRESSED */
            fmt = p[3];
            type = !memcmp(p + 5, guid_yuy2, 4) ? CAM_YUY2 : !memcmp(p + 5, guid_nv12, 4) ? CAM_NV12 : 0;
            break;
        case 0x06:                      /* VS_FORMAT_MJPEG */
            fmt = p[3];
            type = CAM_MJPEG;
            break;
        case 0x10:                      /* VS_FORMAT_FRAME_BASED, other formats */
        case 0x0C: case 0x0F: case 0x12:
            type = 0;
            break;
        case 0x05:                      /* VS_FRAME_UNCOMPRESSED */
        case 0x07:                      /* VS_FRAME_MJPEG */
            if (type && p[0] >= 26) add_mode(c, fmt, type, p);
            break;
        }
    }
}

static int uvc_probe(struct usb_dev *d, u8 *ifd, u8 *end)
{
    struct cam *c;
    u8 *p, *cfg = d->cdesc, *cend = d->cdesc + d->cdesc_len;
    int i, slot = -1;
    if (ifd[5] != 0x0E || ifd[6] != 0x01) return -1;
    /* the same camera plugged in again keeps its number */
    for (i = 0; i < MAXCAM; i++)
        if (cams[i] && !cams[i]->used && cams[i]->d == NULL &&
            !memcmp(cams[i]->name, d->product, MIN(sizeof(cams[i]->name), sizeof(d->product)))) {
            slot = i;
            break;
        }
    if (slot < 0)
        for (i = 0; i < MAXCAM; i++)
            if (!cams[i] || !cams[i]->used) {
                slot = i;
                break;
            }
    if (slot < 0) return -1;
    if (!cams[slot]) cams[slot] = kmalloc(sizeof(struct cam));
    c = cams[slot];
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->d = d;
    c->devid = d->id;
    c->vcif = ifd[2];
    /* class-specific VC header: version and the streaming interface */
    for (p = ifd + ifd[0]; p + 2 <= end && p[0] && p[1] != 4; p += p[0])
        if (p[1] == 0x24 && p[2] == 0x01 && p[0] >= 13) {
            c->uvcver = rd16(p + 3);
            c->vsif = p[12];
        }
    if (!c->vsif) {
        kprintf("uvc: dev %u: no streaming interface\n", d->id);
        return -1;
    }
    for (p = cfg; p + 2 <= cend && p[0]; p += p[0])
        if (p[1] == 4 && p[2] == c->vsif && p[3] == 0) {
            parse_vs(c, p, cend);
            break;
        }
    if (!c->nmodes) {
        kprintf("uvc: dev %u: no YUY2/NV12/MJPEG format\n", d->id);
        return -1;
    }
    if (d->product[0]) strlcpy(c->name, d->product, sizeof(c->name));
    else ksnprintf(c->name, sizeof(c->name), "USB Camera %04x:%04x", rd16(d->ddesc + 8), rd16(d->ddesc + 10));
    c->used = 1;
    d->driver = UD_VIDEO;
    kprintf("uvc: dev %u: %s, UVC %x.%02x, %d mode(s), %d iso alternate(s)%s\n", d->id, c->name, c->uvcver >> 8,
            c->uvcver & 0xFF, c->nmodes, c->nalts, c->bulkep ? ", bulk" : "");
    for (i = 0; i < c->nmodes; i++)
        kprintf("uvc:   %d: %s %ux%u, %lu.%lu fps%s\n", i, c->modes[i].type == CAM_MJPEG ? "MJPEG" :
                c->modes[i].type == CAM_NV12 ? "NV12" : "YUY2", c->modes[i].w, c->modes[i].h,
                c->modes[i].interval ? 10000000UL / c->modes[i].interval : 0,
                c->modes[i].interval ? (100000000UL / c->modes[i].interval) % 10 : 0,
                c->modes[i].maxsize ? "" : " (too large)");
    return 0;
}

/* ---------------------------------------------------------------- streaming */

/* start of a payload: its header; a new frame begins when the frame ID toggles */
static int pay_head(struct cam *c, const u8 *p, u32 n)
{
    u32 hl;
    u8 info;
    if (n < 2) return -1;
    hl = p[0];
    info = p[1];
    if (hl < 2 || hl > n) return -1;
    if (!(info & 0x80) && hl != 2 && hl != 12) return -1;  /* not a header */
    c->info = info;
    if ((info & 1) != c->fid) {
        if (c->len[c->fill] && !c->bad) {
            /* the previous frame ended without EOF: complete only when its size is exact */
            u32 want = c->cur.type == CAM_MJPEG ? 0 : c->cur.maxsize;
            if (!want || c->len[c->fill] == want) frame_done(c);
        }
        c->fid = (u8)(info & 1);
        c->len[c->fill] = 0;
        c->bad = 0;
    }
    if (info & 0x40) c->bad = 1;                            /* error */
    return (int)hl;
}

static void pay_data(struct cam *c, const u8 *p, u32 n)
{
    if (!n) return;
    if (c->len[c->fill] + n > c->bufsize) {
        c->bad = 1;
    } else if (!c->bad) {
        memcpy(c->buf[c->fill] + c->len[c->fill], p, n);
        c->len[c->fill] += n;
    }
}

/* end of a payload: complete the frame on EOF */
static void pay_end(struct cam *c)
{
    if (!(c->info & 2)) return;
    if (!c->bad && c->len[c->fill] && (c->cur.type == CAM_MJPEG || c->len[c->fill] == c->cur.maxsize))
        frame_done(c);
    c->len[c->fill] = 0;
    c->bad = 0;
    c->fid ^= 1;
}

static void frame_done(struct cam *c)
{
    u8 *b = c->buf[c->fill];
    u32 n = c->len[c->fill], i;
    if (c->cur.type == CAM_MJPEG) {
        /* a whole JPEG: SOI at the start, EOI at the end (some cameras pad after it) */
        for (i = n; i >= 2 && i + 64 > n && (b[i - 2] != 0xFF || b[i - 1] != 0xD9); i--) ;
        if (n < 4 || b[0] != 0xFF || b[1] != 0xD8 || i < 2 || i + 64 <= n) {
            c->dropped++;
            return;
        }
    }
    if (c->held) {
        /* the reader still copies the other buffer: this frame is lost */
        c->dropped++;
        return;
    }
    c->fill ^= 1;
    c->ready = 1;
    c->seq++;
    c->frames++;
}

/* isochronous: one payload per packet */
static void iso_done(struct usb_iso *s, u8 *src, int len)
{
    struct cam *c = s->priv;
    int hl;
    /* packets the host controller lost: the frame being filled has a gap */
    if (s->missed + s->errors != c->lost) {
        c->lost = s->missed + s->errors;
        c->bad = 1;
    }
    if (len <= 0 || (hl = pay_head(c, src, (u32)len)) < 0) return;
    pay_data(c, src + hl, (u32)len - hl);
    pay_end(c);
}

/* bulk: a payload (up to dwMaxPayloadTransferSize) may take several transfers; it ends
   with a short transfer or when it reaches the maximum size */
static void bulk_done(struct usb_xfer *x)
{
    struct cam *c = x->priv;
    if (!c->active || c->gone) return;
    if (x->status == XS_OK && (x->actual || c->bpay)) {       /* zero length: end of payload */
        u32 n = x->actual;
        if (!c->bpay) {
            int hl = pay_head(c, x->buf, n);
            if (hl >= 0) {
                pay_data(c, x->buf + hl, n - hl);
                c->bpay = n;
            }
        } else {
            pay_data(c, x->buf, n);
            c->bpay += n;
        }
        if (c->bpay && (n < x->len || c->bpay >= c->maxpay)) {
            pay_end(c);
            c->bpay = 0;
        }
    }
    if (x->status == XS_STALL) usb_clear_halt(x->pipe);
    x->len = c->payload;
    if (x->status != XS_GONE && x->status != XS_CANCEL) usb_submit(x);
}

static int probe_commit(struct cam *c, struct wincammode *m)
{
    u8 *b = kmalloc_align(64, 64);
    int len = c->uvcver >= 0x0150 ? 48 : c->uvcver >= 0x0110 ? 34 : 26, r;
    u16 vsif = c->vsif;
    if (!b) return -1;
    memset(b, 0, 64);
    b[0] = 1;                   /* bmHint: frame interval fixed */
    b[2] = m->fmt;
    b[3] = m->frame;
    wr32(b + 4, m->interval);
    r = usb_control(c->d, 0x21, 0x01, 0x0100, vsif, b, (u16)len);       /* SET_CUR probe */
    if (r < 0) {
        /* UVC 1.1 devices that only take the 1.0 size */
        len = 26;
        r = usb_control(c->d, 0x21, 0x01, 0x0100, vsif, b, (u16)len);
    }
    if (r >= 0) r = usb_control(c->d, 0xA1, 0x81, 0x0100, vsif, b, (u16)len);    /* GET_CUR probe */
    if (r >= 0) {
        c->payload = rd32(b + 22);
        if (rd32(b + 18) && m->type == CAM_MJPEG) c->bufsize = MIN(rd32(b + 18), MAXFRAME);
        b[2] = m->fmt;
        b[3] = m->frame;
        r = usb_control(c->d, 0x21, 0x01, 0x0200, vsif, b, (u16)len);   /* SET_CUR commit */
    }
    kfree(b);
    return r < 0 ? -1 : 0;
}

static void cam_stop(struct cam *c)
{
    int i;
    if (!c->active) return;
    c->active = 0;
    if (c->iso.running) usb_iso_stop(&c->iso);
    for (i = 0; i < NBULK; i++)
        if (c->bbuf[i]) {
            usb_cancel(&c->bx[i]);
            kfree(c->bbuf[i]);
            c->bbuf[i] = NULL;
        }
    usb_pipe_close(&c->pipe);
    if (c->d && !c->d->gone && c->nalts) usb_set_interface(c->d, c->vsif, 0);
    for (i = 0; i < 2; i++)
        if (c->buf[i]) {
            kfree(c->buf[i]);
            c->buf[i] = NULL;
        }
    kprintf("uvc: %s stopped, %lu frames, %lu dropped, %lu packets lost\n", c->name, c->frames, c->dropped, c->lost);
}

static int cam_start(struct cam *c, int mode)
{
    struct wincammode *m;
    int i, alt = -1;
    if (mode < 0 || mode >= c->nmodes || c->gone || !c->d) return WINE_NODEV;
    m = &c->modes[mode];
    if (!m->maxsize) return WINE_SMALL;
    cam_stop(c);
    c->cur = *m;
    c->bufsize = m->maxsize;
    if (probe_commit(c, m)) {
        kprintf("uvc: %s: probe/commit failed\n", c->name);
        return WINE_IO;
    }
    if (c->bufsize < m->maxsize && m->type != CAM_MJPEG) c->bufsize = m->maxsize;
    c->buf[0] = kmalloc(c->bufsize);
    c->buf[1] = kmalloc(c->bufsize);
    if (!c->buf[0] || !c->buf[1]) {
        kprintf("uvc: no memory for two %lu byte frames\n", c->bufsize);
        cam_stop(c);
        return WINE_SMALL;
    }
    c->fill = c->ready = c->held = 0;
    c->bad = 1;                 /* the stream starts inside a frame: wait for its end */
    c->len[0] = c->len[1] = 0;
    c->frames = c->dropped = 0;
    c->active = mode + 1;
    if (c->nalts) {
        /* the smallest alternate that carries one payload per interval */
        for (i = 0; i < c->nalts; i++)
            if (c->alts[i].bytes >= c->payload && (alt < 0 || c->alts[i].bytes < c->alts[alt].bytes)) alt = i;
        if (alt < 0)
            for (i = 0; i < c->nalts; i++)
                if (alt < 0 || c->alts[i].bytes > c->alts[alt].bytes) alt = i;
        if (usb_set_interface(c->d, c->vsif, c->alts[alt].alt) || usb_pipe_open(c->d, &c->pipe, c->alts[alt].ep)) {
            cam_stop(c);
            return WINE_IO;
        }
        memset(&c->iso, 0, sizeof(c->iso));
        c->lost = 0;
        c->iso.pipe = &c->pipe;
        c->iso.in = 1;
        c->iso.done = iso_done;
        c->iso.priv = c;
        c->iso.ahead_us = 150000UL;  /* video is not latency sensitive: room for slow servicing */
        c->iso.maxbytes = 0x200000UL;
        if (usb_iso_start(&c->iso)) {
            cam_stop(c);
            return WINE_SMALL;
        }
        kprintf("uvc: %s: %ux%u, alternate %u (%lu bytes per interval, payload %lu)\n", c->name, m->w, m->h,
                c->alts[alt].alt, c->alts[alt].bytes, c->payload);
    } else if (c->bulkep) {
        c->maxpay = c->payload ? c->payload : 0xFFFFFFFFUL;
        c->bpay = 0;
        if (!c->payload || c->payload > 0x10000UL) c->payload = 0x10000UL;
        if (usb_pipe_open(c->d, &c->pipe, c->bulkep)) {
            cam_stop(c);
            return WINE_IO;
        }
        for (i = 0; i < NBULK; i++) {
            struct usb_xfer *x = &c->bx[i];
            c->bbuf[i] = kmalloc_align(c->payload, 4096);
            if (!c->bbuf[i]) break;
            memset(x, 0, sizeof(*x));
            x->pipe = &c->pipe;
            x->buf = c->bbuf[i];
            x->len = c->payload;
            x->in = 1;
            x->cb = bulk_done;
            x->priv = c;
            usb_submit(x);
        }
        kprintf("uvc: %s: %ux%u, bulk, payload %lu\n", c->name, m->w, m->h, c->payload);
    } else {
        cam_stop(c);
        return WINE_NODEV;
    }
    return WINE_OK;
}

static void uvc_detach(struct usb_dev *d)
{
    int i;
    for (i = 0; i < MAXCAM; i++) {
        struct cam *c = cams[i];
        if (!c || c->d != d) continue;
        c->gone = 1;
        cam_stop(c);
        c->d = NULL;
        c->used = 0;
        kprintf("uvc: %s removed\n", c->name);
    }
}

struct usb_class uvc_class = { "video", uvc_probe, uvc_detach, NULL };

/* ---------------------------------------------------------------- API */

int uvc_api(struct winapireq *r, u8 *buf)
{
    int i, n = 0;
    struct cam *c = r->arg0 < MAXCAM ? cams[r->arg0] : NULL;
    switch (r->fn) {
    case WINF_CAM_DEVS:
    {
        struct wincamdev *o = (struct wincamdev *)buf;
        if (!buf) return WINE_SMALL;
        for (i = 0; i < MAXCAM; i++) {
            if (!cams[i] || (!cams[i]->used && !cams[i]->gone)) continue;
            if ((u32)(n + 1) * sizeof(*o) > r->buflen) break;
            memset(o, 0, sizeof(*o));
            o->id = (u8)i;
            o->nmodes = (u8)cams[i]->nmodes;
            o->gone = (u8)!cams[i]->used;
            o->active = (u8)cams[i]->active;
            o->usbdev = cams[i]->used ? cams[i]->devid : 0;
            strlcpy(o->name, cams[i]->name, sizeof(o->name));
            o++;
            n++;
        }
        r->retlen = (u32)n * sizeof(*o);
        return WINE_OK;
    }
    case WINF_CAM_MODES:
        if (!c || !buf) return WINE_NODEV;
        n = (int)MIN((u32)c->nmodes, r->buflen / sizeof(struct wincammode));
        memcpy(buf, c->modes, (u32)n * sizeof(struct wincammode));
        r->retlen = (u32)n * sizeof(struct wincammode);
        return WINE_OK;
    case WINF_CAM_START:
        if (!c || !c->used) return WINE_NODEV;
        return cam_start(c, (int)r->arg1);
    case WINF_CAM_STOP:
        if (!c) return WINE_NODEV;
        cam_stop(c);
        return WINE_OK;
    case WINF_CAM_FRAME:
    {
        u32 off = r->arg1, k, size;
        u8 *src;
        if (!c || !c->active || c->gone) return WINE_NODEV;
        if (!buf) return WINE_SMALL;
        if (off == 0) {
            struct winframe *f = (struct winframe *)buf;
            if (!c->ready || c->seq == c->rseq) return WINE_BUSY;
            if (r->buflen < sizeof(*f)) return WINE_SMALL;
            c->held = 1;
            c->rseq = c->seq;
            memset(f, 0, sizeof(*f));
            f->seq = c->seq;
            f->size = c->len[c->fill ^ 1];
            f->w = c->cur.w;
            f->h = c->cur.h;
            f->type = c->cur.type;
            buf += sizeof(*f);
            r->retlen = sizeof(*f);
            k = r->buflen - sizeof(*f);
        } else {
            if (!c->held) return WINE_BUSY;
            k = r->buflen;
        }
        size = c->len[c->fill ^ 1];
        src = c->buf[c->fill ^ 1];
        if (off > size) off = size;
        k = MIN(k, size - off);
        memcpy(buf, src + off, k);
        r->retlen += k;
        if (off + k >= size) c->held = 0;
        return WINE_OK;
    }
    }
    return WINE_BADFN;
}
