/* hid.c - USB HID keyboard and mouse (boot and report protocol, wheel, tilt), tablets,
   precision touchpads (ptp.c) */
#include "usb.h"

/* USB usage -> set 1 scan code. 0x00XX plain, 0xE0XX extended, 0xFFxx special */
#define SC_PRTSC 0xFF01
#define SC_PAUSE 0xFF02
static const u16 usage2sc[0x68] = {
    0, 0, 0, 0,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,  /* a-p */
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                                      /* q-z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                                      /* 1-0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,  /* 28-37 */
    0x35, 0x3A,                                                                                        /* / caps */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,                          /* F1-F12 */
    SC_PRTSC, 0x46, SC_PAUSE,
    0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D, 0xE04B, 0xE050, 0xE048,                  /* nav */
    0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52,
    0x53, 0x56, 0xE05D, 0xE05E, 0x59
};
static const u16 mod2sc[8] = { 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C };

static u16 usage_sc(u8 u)
{
    if (u < ARRAY_SIZE(usage2sc)) return usage2sc[u];
    switch (u) {
    case 0x87: return 0x73;     /* Ro */
    case 0x88: return 0x70;     /* Katakana/Hiragana */
    case 0x89: return 0x7D;     /* Yen */
    case 0x8A: return 0x79;     /* Henkan */
    case 0x8B: return 0x7B;     /* Muhenkan */
    }
    return 0;
}

#define MAXHID 8

struct hidfield { s16 off; u8 size; u8 sign; u8 abs; s32 lmin, lmax; };

struct hiddev {
    struct usb_dev *dev;
    int iface;
    int kind;                   /* 1 keyboard, 2 mouse, 3 touchpad */
    struct ptp *ptp;
    struct usb_pipe ip;
    struct usb_xfer x[8];       /* touchpads: several reports per poll in DOS mode */
    int nx;
    u8 *buf;
    u32 len;
    /* keyboard */
    u8 last[8];
    u8 leds, ledsent;
    u8 rep_key;
    u32 rep_time;
    int rep_active;
    /* mouse */
    int boot;
    u8 repid;
    struct hidfield bx[5], fx, fy, fw, fp;
    u8 nbtn;
    int lastb;
};

static struct hiddev *hids[MAXHID];
static u32 rep_delay = 500, rep_rate = 33;

static void emit(u16 sc, int brk)
{
    u8 b[8];
    int n = 0;
    if (sc == SC_PRTSC) {
        if (!brk) { b[0] = 0xE0; b[1] = 0x2A; b[2] = 0xE0; b[3] = 0x37; }
        else { b[0] = 0xE0; b[1] = 0xB7; b[2] = 0xE0; b[3] = 0xAA; }
        plat_key(b, 4);
        return;
    }
    if (sc == SC_PAUSE) {
        if (!brk) {
            static const u8 p[6] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
            plat_key(p, 6);
        }
        return;
    }
    if (!sc) return;
    DBG2("kbd %04x %s\n", sc, brk ? "up" : "down");
    if (sc & 0xFF00) b[n++] = 0xE0;
    b[n++] = (u8)((sc & 0x7F) | (brk ? 0x80 : 0));
    plat_key(b, n);
}

static int in_list(const u8 *k, u8 v)
{
    int i;
    for (i = 2; i < 8; i++) if (k[i] == v) return 1;
    return 0;
}

/* modifier keys held on all keyboards together: Shift held on one keyboard stays held
   when the other keyboard releases its Shift */
static u8 modcount[8];

static void kbd_report(struct hiddev *h, const u8 *r)
{
    int i;
    u8 mods = r[0], old = h->last[0];
    if (r[2] == 0x01) return;               /* phantom/rollover error */
    for (i = 0; i < 8; i++) {
        u8 m = (u8)(1 << i);
        if ((mods & m) && !(old & m) && modcount[i]++ == 0) emit(mod2sc[i], 0);
        if (!(mods & m) && (old & m) && modcount[i] && --modcount[i] == 0) emit(mod2sc[i], 1);
    }
    for (i = 2; i < 8; i++) {
        u8 k = h->last[i];
        if (k > 3 && !in_list(r, k)) {
            emit(usage_sc(k), 1);
            if (h->rep_key == k) h->rep_active = 0;
        }
    }
    for (i = 2; i < 8; i++) {
        u8 k = r[i];
        if (k > 3 && !in_list(h->last, k)) {
            DBG2("hid: dev %u key %02x\n", h->dev->id, k);
            emit(usage_sc(k), 0);
            if (usage_sc(k) != SC_PAUSE && usage_sc(k) != SC_PRTSC) {
                h->rep_key = k;
                h->rep_time = plat_ms() + rep_delay;
                h->rep_active = 1;
            }
        }
    }
    memcpy(h->last, r, 8);
}

static s32 getbits(const u8 *d, u32 len, struct hidfield *f)
{
    u32 v = 0, i;
    if (f->size == 0 || f->off < 0) return 0;
    for (i = 0; i < f->size && i < 32; i++) {
        u32 bit = (u32)f->off + i;
        if (bit / 8 >= len) break;
        if (d[bit / 8] & (1 << (bit % 8))) v |= 1UL << i;
    }
    if (f->sign && f->size < 32 && (v & (1UL << (f->size - 1)))) v |= ~((1UL << f->size) - 1);
    return (s32)v;
}

static void mouse_report(struct hiddev *h, const u8 *r, u32 len)
{
    int b = 0, i;
    s32 dx, dy, dw = 0, dp = 0;
    if (h->boot) {
        if (len < 3) return;
        b = r[0] & 0x1F;               /* bits 3 and 4: back and forward on most mice */
        dx = (s8)r[1];
        dy = (s8)r[2];
        if (len >= 4) dw = (s8)r[3];
    } else {
        if (h->repid) {
            if (r[0] != h->repid) return;
            r++;
            len--;
        }
        for (i = 0; i < h->nbtn && i < 5; i++)
            if (getbits(r, len, &h->bx[i])) b |= 1 << i;
        dx = getbits(r, len, &h->fx);
        dy = getbits(r, len, &h->fy);
        dw = getbits(r, len, &h->fw);
        dp = getbits(r, len, &h->fp);
    }
    if (!h->boot && h->fx.abs && h->fy.abs) {
        /* tablet, touch screen, virtual machine pointer: normalized 0..65535 */
        s32 rx = h->fx.lmax - h->fx.lmin, ry = h->fy.lmax - h->fy.lmin;
        s32 x = dx - h->fx.lmin, y = dy - h->fy.lmin;
        if (rx <= 0) rx = 32767;
        if (ry <= 0) ry = 32767;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > rx) x = rx;
        if (y > ry) y = ry;
        plat_mouse_abs((int)((u32)x * 65535UL / (u32)rx), (int)((u32)y * 65535UL / (u32)ry), (int)dw, (int)dp, b);
    } else {
        plat_mouse((int)dx, (int)dy, (int)dw, (int)dp, b);
    }
    h->lastb = b;
}

static void hid_cb(struct usb_xfer *x)
{
    struct hiddev *h = x->priv;
    if (x->status == XS_OK) {
        if (h->kind == 1 && x->actual >= 8) kbd_report(h, x->buf);
        else if (h->kind == 2 && x->actual) mouse_report(h, x->buf, x->actual);
        else if (h->kind == 3 && x->actual) ptp_input(h->ptp, x->buf, x->actual);
        usb_submit(x);
    } else if (x->status == XS_STALL) {
        h->rep_active = 0;
    } else if (x->status != XS_CANCEL && x->status != XS_GONE && !h->dev->gone) {
        usb_submit(x);
    }
}

/* ---- report descriptor parser (mouse fields) ---- */
static void parse_mouse(struct hiddev *h, const u8 *p, int n)
{
    u32 upage = 0, rsize = 0, rcount = 0, lmin = 0, lmax = 0, umin = 0, umax = 0;
    u32 usages[16];
    int nus = 0, coll = 0, in_mouse = 0, done = 0;
    s32 off = 0;
    u8 cur_id = 0;
    int i;
    h->fx.off = h->fy.off = h->fw.off = h->fp.off = -1;
    for (i = 0; i < 5; i++) h->bx[i].off = -1;
    while (n > 0 && !done) {
        u8 b = *p++;
        u32 sz = b & 3, v = 0, k;
        u8 tag = (u8)(b >> 4), type = (u8)((b >> 2) & 3);
        n--;
        if (b == 0xFE) {                   /* long item */
            if (n < 2) break;
            k = p[0];
            p += 2 + k; n -= 2 + (int)k;
            continue;
        }
        if (sz == 3) sz = 4;
        if ((int)sz > n) break;
        for (k = 0; k < sz; k++) v |= (u32)p[k] << (8 * k);
        p += sz; n -= (int)sz;
        if (type == 1) {                    /* global */
            switch (tag) {
            case 0: upage = v; break;
            case 1: lmin = v; if (sz && sz < 4 && (v & (1UL << (sz * 8 - 1)))) lmin |= ~((1UL << (sz * 8)) - 1); break;
            case 2: lmax = v; break;
            case 7: rsize = v; break;
            case 8:
                if (in_mouse && cur_id != (u8)v && h->repid) { done = 1; break; }
                cur_id = (u8)v; off = 0;
                break;
            case 9: rcount = v; break;
            }
        } else if (type == 2) {             /* local */
            if (tag == 0 && nus < 16) usages[nus++] = (sz == 4) ? v : (upage << 16) | v;
            if (tag == 1) umin = (sz == 4) ? v : (upage << 16) | v;
            if (tag == 2) umax = (sz == 4) ? v : (upage << 16) | v;
        } else if (type == 0) {             /* main */
            if (tag == 0x0A) {              /* collection */
                /* application collections: mouse, pointer, digitizer, pen, touch screen */
                if (v == 1 && nus && (usages[0] == 0x00010002UL || usages[0] == 0x00010001UL ||
                    usages[0] == 0x000D0001UL || usages[0] == 0x000D0002UL || usages[0] == 0x000D0004UL))
                    in_mouse = 1;
                coll++;
            } else if (tag == 0x0C) {
                coll--;
                if (in_mouse && coll == 0) done = 1;
            } else if (tag == 0x08 && in_mouse) {   /* input */
                u32 f;
                if (!h->repid) h->repid = cur_id;
                for (f = 0; f < rcount; f++) {
                    u32 u = 0;
                    if (!(v & 1)) {
                        if (nus) u = usages[f < (u32)nus ? f : (u32)nus - 1];
                        else if (umin) u = umin + f;
                    }
                    if (u && cur_id == h->repid) {
                        struct hidfield fld;
                        fld.off = (s16)off;
                        fld.size = (u8)rsize;
                        fld.sign = (s32)lmin < 0;
                        fld.abs = !(v & 4);
                        fld.lmin = (s32)lmin;
                        fld.lmax = (s32)lmax;
                        if ((u >> 16) == 9 && (u & 0xFFFF) >= 1 && (u & 0xFFFF) <= 5) {
                            h->bx[(u & 0xFFFF) - 1] = fld;
                            if ((int)(u & 0xFFFF) > h->nbtn) h->nbtn = (u8)(u & 0xFFFF);
                        } else if (u == 0x000D0042UL && h->bx[0].off < 0) {     /* tip switch */
                            h->bx[0] = fld;
                            if (h->nbtn < 1) h->nbtn = 1;
                        } else if ((u == 0x000D0044UL || u == 0x000D0045UL) && h->bx[1].off < 0) { /* barrel, eraser */
                            h->bx[1] = fld;
                            if (h->nbtn < 2) h->nbtn = 2;
                        } else if (u == 0x00010030UL && h->fx.off < 0) h->fx = fld;
                        else if (u == 0x00010031UL && h->fy.off < 0) h->fy = fld;
                        else if (u == 0x00010038UL) h->fw = fld;
                        else if (u == 0x000C0238UL) h->fp = fld;
                    }
                    off += (s32)rsize;
                }
            }
            if (tag == 0x08 || tag == 0x09 || tag == 0x0B || tag == 0x0A || tag == 0x0C) {
                nus = 0; umin = umax = 0;
            }
        }
    }
    if (h->fx.size && h->fx.size < 32 && h->fx.off >= 0 && !h->fx.abs) h->fx.sign = 1;
    if (h->fy.size && h->fy.off >= 0 && !h->fy.abs) h->fy.sign = 1;
    if (h->fw.size && h->fw.off >= 0) h->fw.sign = 1;
    if (h->fp.size && h->fp.off >= 0) h->fp.sign = 1;
}

static int hid_probe(struct usb_dev *d, u8 *ifc, u8 *end)
{
    struct hiddev *h;
    u8 *ep, *hd, *rd = NULL;
    int slot, kind = 0, rdlen = 0, i;
    if (ifc[5] != 3) return -1;
    if (cb->opts & OPT_NOHID) return -1;
    hd = usb_find_desc(ifc + ifc[0], end, 0x21);
    ep = usb_find_desc(ifc + ifc[0], end, 5);
    if (!ep || !(ep[2] & 0x80) || (ep[3] & 3) != EP_INT) return -1;
    if (ifc[6] == 1 && ifc[7] == 1) kind = 1;
    else if (ifc[6] == 1 && ifc[7] == 2) kind = 2;
    if (hd && hd[0] >= 9 && hd[6] == 0x22) {
        rdlen = hd[7] | (hd[8] << 8);
        if (rdlen > 1024) rdlen = 1024;
        rd = kmalloc((u32)rdlen + 1);
        rdlen = usb_control(d, 0x81, 0x06, 0x2200, ifc[2], rd, (u16)rdlen);
        if (rdlen < 0) rdlen = 0;
        if (rdlen >= 4 && ptp_is_touchpad(rd, rdlen)) kind = 3;
        if (!kind && rdlen >= 4) {
            /* Usage Page (Generic Desktop), Usage (Keyboard/Mouse) */
            for (i = 0; i + 3 < rdlen; i++) {
                if (rd[i] == 0x05 && rd[i + 1] == 0x01 && rd[i + 2] == 0x09) {
                    if (rd[i + 3] == 0x06) kind = 1;
                    if (rd[i + 3] == 0x02 || rd[i + 3] == 0x01) kind = 2;      /* mouse, pointer */
                    break;
                }
                if (rd[i] == 0x05 && rd[i + 1] == 0x0D && rd[i + 2] == 0x09 &&
                    (rd[i + 3] == 0x01 || rd[i + 3] == 0x02 || rd[i + 3] == 0x04)) {
                    kind = 2;                                                    /* digitizer */
                    break;
                }
            }
        }
    }
    if (!kind) {
        if (rd) kfree(rd);
        return -1;
    }
    for (slot = 0; slot < MAXHID && hids[slot]; slot++) ;
    if (slot == MAXHID) {
        if (rd) kfree(rd);
        return -1;
    }
    h = kmalloc(sizeof(*h));
    memset(h, 0, sizeof(*h));
    if (kind == 3) {
        u8 fr[16];
        int fl, w;
        h->ptp = ptp_new(rd, rdlen);
        if (!h->ptp) {
            kfree(h);
            kfree(rd);
            return -1;
        }
        /* touchpad mode, then contacts and button reported */
        for (w = 0; w < 2; w++) {
            fl = ptp_feature(h->ptp, w, fr, sizeof(fr));
            if (fl) usb_control(d, 0x21, 0x09, (u16)(0x0300 | fr[0]), ifc[2], fr[0] ? fr : fr + 1, (u16)(fr[0] ? fl : fl - 1));
        }
        usb_control(d, 0x21, 0x0A, 0, ifc[2], NULL, 0);
        d->driver = UD_MOUSE;
    }
    h->dev = d;
    h->iface = ifc[2];
    h->kind = kind;
    if (kind == 3) {
        /* set up above */
    } else if (kind == 1) {
        if (ifc[6] == 1) usb_control(d, 0x21, 0x0B, 0, ifc[2], NULL, 0);    /* SET_PROTOCOL(boot) */
        usb_control(d, 0x21, 0x0A, 0, ifc[2], NULL, 0);                      /* SET_IDLE(0) */
        d->driver = UD_KBD;
        h->ledsent = 0xFF;
    } else {
        if (rdlen > 0) parse_mouse(h, rd, rdlen);
        if (h->fx.off < 0 || h->fy.off < 0) {
            h->boot = 1;
            if (ifc[6] == 1) usb_control(d, 0x21, 0x0B, 0, ifc[2], NULL, 0);
        } else if (ifc[6] == 1) {
            usb_control(d, 0x21, 0x0B, 1, ifc[2], NULL, 0);                  /* report protocol */
        }
        usb_control(d, 0x21, 0x0A, 0, ifc[2], NULL, 0);
        if (d->driver == UD_NONE) d->driver = (!h->boot && h->fx.abs) ? UD_TABLET : UD_MOUSE;
        kprintf("usb: %s %s protocol, %d buttons%s%s\n", !h->boot && h->fx.abs ? "tablet" : "mouse",
                h->boot ? "boot" : "report", h->boot ? 3 : h->nbtn,
                h->fw.off >= 0 ? ", wheel" : "", h->fp.off >= 0 ? ", tilt" : "");
    }
    if (rd) kfree(rd);
    if (usb_pipe_open(d, &h->ip, ep)) {
        kfree(h);
        return -1;
    }
    h->len = h->ip.maxpkt ? h->ip.maxpkt : 8;
    if (h->len > 64) h->len = 64;
    h->nx = kind == 3 ? 8 : 1;
    h->buf = kmalloc(64UL * h->nx);
    for (i = 0; i < h->nx; i++) {
        struct usb_xfer *x = &h->x[i];
        x->pipe = &h->ip;
        x->buf = h->buf + 64 * i;
        x->len = h->len;
        x->in = 1;
        x->cb = hid_cb;
        x->priv = h;
    }
    hids[slot] = h;
    if (kind == 1 && d->driver != UD_KBD) d->driver = UD_KBD;
    for (i = 0; i < h->nx; i++) usb_submit(&h->x[i]);
    if (kind == 1) kprintf("usb: keyboard ready\n");
    return 0;
}

/* mice, tablets and touch screens of other transports (I2C): report protocol */
struct hiddev *hid_mouse_new(const u8 *rd, int n)
{
    struct hiddev *h = kmalloc(sizeof(*h));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->kind = 2;
    parse_mouse(h, rd, n);
    if (h->fx.off < 0 || h->fy.off < 0) {
        kfree(h);
        return NULL;
    }
    kprintf("hid: %s, %d buttons%s\n", h->fx.abs ? "tablet / touch screen" : "mouse", h->nbtn,
            h->fw.off >= 0 ? ", wheel" : "");
    return h;
}

void hid_mouse_input(struct hiddev *h, const u8 *r, u32 len)
{
    mouse_report(h, r, len);
}

void hid_mouse_free(struct hiddev *h)
{
    if (h) kfree(h);
}

static void hid_detach(struct usb_dev *d)
{
    int i, k;
    for (i = 0; i < MAXHID; i++) {
        struct hiddev *h = hids[i];
        if (!h || h->dev != d) continue;
        if (h->kind == 1) {
            /* release keys that are still down */
            u8 zero[8];
            memset(zero, 0, 8);
            kbd_report(h, zero);
        }
        for (k = 0; k < h->nx; k++) usb_cancel(&h->x[k]);
        usb_pipe_close(&h->ip);
        ptp_free(h->ptp);
        kfree(h->buf);
        kfree(h);
        hids[i] = NULL;
    }
}

u8 plat_kbd_leds(void);

void hid_tick(void)
{
    int i;
    u32 now = plat_ms();
    u8 leds = plat_kbd_leds();
    ptp_tick();
    for (i = 0; i < MAXHID; i++) {
        struct hiddev *h = hids[i];
        if (!h || h->kind != 1 || h->dev->gone) continue;
        if (h->rep_active && (s32)(now - h->rep_time) >= 0) {
            emit(usage_sc(h->rep_key), 0);
            h->rep_time = now + rep_rate;
        }
        if (leds != h->ledsent) {
            u8 v = leds;
            h->ledsent = leds;
            DBG2("hid: dev %u LEDs %x\n", h->dev->id, leds);
            usb_control(h->dev, 0x21, 0x09, 0x0200, (u16)h->iface, &v, 1);
        }
    }
}

struct usb_class hid_class = { "hid", hid_probe, hid_detach, NULL };
