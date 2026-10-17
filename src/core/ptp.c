/* ptp.c - precision touchpads (HID digitizer "Touch Pad" collection) over USB and I2C, and the
   gesture engine of the touchpads that report their contacts in their own protocols (PS/2
   Synaptics and ELAN, RMI4 and ELAN over SMBus: ptp_new_abs / ptp_contacts).
   The touchpad is switched from mouse mode to touchpad mode (feature "Input Mode" = 3),
   then the contacts are turned into mouse events:
     one finger moves the pointer, a short tap clicks (two fingers: right, three: middle);
     two fingers moving up or down turn the wheel, sideways the horizontal wheel;
     the pad's button clicks left, right with two fingers on the pad. */
#ifndef USB_H
#include "usb.h"
#endif

#define MAXPTP      6
#define MAXSLOT     10
#define MAXCONTACT  10
#define CPMM        8           /* pointer counts per millimetre */
#define SCROLL_UM   2500        /* finger travel per wheel notch, micrometres */
#define TAP_MS      200
#define TAP_UM      2500
#define CLICK_MS    50

struct pfield {
    s16 off;                    /* bit offset after the report ID, -1: none */
    u8 size, sign;
    s32 lmin, lmax;
};

struct pslot {
    struct pfield tip, conf, id, x, y;
};

struct contact {
    u8 id, tip, conf;
    s32 x, y;
};

struct ptp {
    u8 used;
    u8 repid;                   /* input report of the touchpad collection */
    int nslot;
    struct pslot s[MAXSLOT];
    struct pfield count, btn[3], scan;
    u16 lastscan;
    u32 devms;                  /* device time from the scan time field (100 us units) */
    /* feature reports: input mode, selective reporting */
    u8 modeid, selid;
    struct pfield mode, surface, button;
    int modebits, selbits;      /* lengths of those reports in bits */
    /* geometry */
    s32 xrange, yrange;
    u32 xum, yum;               /* pad size in micrometres */
    /* frame being collected (hybrid reporting: contacts over several reports) */
    struct contact cur[MAXCONTACT];
    int ncur, expect, seen;      /* expect: contact count of the frame, -1 unknown */
    u8 curbtn;
    /* previous frame */
    struct contact prev[MAXCONTACT];
    int nprev;
    /* gesture state */
    u32 t_down;
    int maxn;
    u32 travel;                 /* micrometres moved during this touch */
    s32 kx, ky;                 /* pointer counts across the whole pad */
    s32 nx, ny;                 /* wheel notches across the whole pad, * 16 */
    s32 rx, ry;                 /* pointer remainders: units * kx, ky */
    s32 sx, sy;                 /* scroll remainders: units * nx, ny */
    int btnheld;                /* button bits held by the pad button */
    int clicked;                /* the pad button was pressed during this touch: no tap */
    int tapbtn;
    s32 tdx, tdy;               /* pointer counts of this touch (debug log) */
    u32 tapup;                  /* plat_ms() when the tap click ends */
    void (*out)(int dx, int dy, int dz, int dw, int buttons);
};

static struct ptp ptps[MAXPTP];

static s32 bits(const u8 *d, u32 len, const struct pfield *f)
{
    u32 v = 0, i;
    if (f->off < 0 || !f->size) return 0;
    for (i = 0; i < f->size && i < 32; i++) {
        u32 b = (u32)f->off + i;
        if (b / 8 >= len) break;
        if (d[b / 8] & (1 << (b % 8))) v |= 1UL << i;
    }
    if (f->sign && f->size < 32 && (v & (1UL << (f->size - 1)))) v |= ~((1UL << f->size) - 1);
    return (s32)v;
}

static void putbits(u8 *d, const struct pfield *f, u32 v)
{
    u32 i;
    if (f->off < 0) return;
    for (i = 0; i < f->size && i < 32; i++) {
        u32 b = (u32)f->off + i;
        if (v & (1UL << i)) d[b / 8] |= (u8)(1 << (b % 8));
        else d[b / 8] &= (u8)~(1 << (b % 8));
    }
}

/* ---------------------------------------------------------------- descriptor */

#define MAXIDS 16

struct roff {
    u8 id;
    s32 in, feat;
};

/* physical size in micrometres from the physical range, unit and exponent */
static u32 phys_um(s32 pmin, s32 pmax, u32 unit, u32 uexp)
{
    s32 v = pmax - pmin;
    u32 mult;
    int e = (int)(uexp & 15);
    if (e >= 8) e -= 16;
    if (v <= 0) return 0;
    if ((unit & 0xFFFF) == 0x0011) mult = 10000;            /* centimetre */
    else if ((unit & 0xFFFF) == 0x0013) mult = 25400;       /* inch */
    else return 0;
    if (e > 0) {
        while (e--) v *= 10;
        return (u32)v * mult;
    }
    {
        u32 um = (u32)v * mult;
        while (e++ < 0) um /= 10;
        return um;
    }
}

static int parse(struct ptp *p, const u8 *d, int n)
{
    u32 upage = 0, unit = 0, uexp = 0, rsize = 0, rcount = 0, usages[16], umin = 0, umax = 0;
    s32 lmin = 0, lmax = 0, pmin = 0, pmax = 0;
    u32 stack[4][9];
    int sp = 0, nus = 0, depth = 0, tpdepth = -1, slot = -1, i, tpid = -1;
    u8 id = 0;
    struct roff ro[MAXIDS];
    int nro = 1;
    memset(ro, 0, sizeof(ro));
    p->scan.off = p->count.off = p->mode.off = p->surface.off = p->button.off = -1;
    for (i = 0; i < 3; i++) p->btn[i].off = -1;
    for (i = 0; i < MAXSLOT; i++)
        p->s[i].tip.off = p->s[i].conf.off = p->s[i].id.off = p->s[i].x.off = p->s[i].y.off = -1;
    while (n > 0) {
        u8 b = *d++;
        u32 sz = b & 3, v = 0, k;
        u8 tag = (u8)(b >> 4), type = (u8)((b >> 2) & 3);
        n--;
        if (b == 0xFE) {
            if (n < 2) break;
            k = d[0];
            d += 2 + k;
            n -= 2 + (int)k;
            continue;
        }
        if (sz == 3) sz = 4;
        if ((int)sz > n) break;
        for (k = 0; k < sz; k++) v |= (u32)d[k] << (8 * k);
        d += sz;
        n -= (int)sz;
        if (type == 1) {
            s32 sv = (s32)v;
            if (sz && sz < 4 && (v & (1UL << (sz * 8 - 1)))) sv = (s32)(v | ~((1UL << (sz * 8)) - 1));
            switch (tag) {
            case 0: upage = v; break;
            case 1: lmin = sv; break;
            case 2: lmax = (lmin < 0) ? sv : (s32)v; break;
            case 3: pmin = sv; break;
            case 4: pmax = (pmin < 0) ? sv : (s32)v; break;
            case 5: uexp = v; break;
            case 6: unit = v; break;
            case 7: rsize = v; break;
            case 8:
                id = (u8)v;
                for (i = 0; i < nro && ro[i].id != id; i++) ;
                if (i == nro && nro < MAXIDS) {
                    ro[nro].id = id;
                    nro++;
                }
                break;
            case 9: rcount = v; break;
            case 10:                                    /* push */
                if (sp < 4) {
                    u32 *s = stack[sp++];
                    s[0] = upage; s[1] = (u32)lmin; s[2] = (u32)lmax; s[3] = (u32)pmin; s[4] = (u32)pmax;
                    s[5] = uexp; s[6] = unit; s[7] = rsize; s[8] = rcount;
                }
                break;
            case 11:                                    /* pop */
                if (sp > 0) {
                    u32 *s = stack[--sp];
                    upage = s[0]; lmin = (s32)s[1]; lmax = (s32)s[2]; pmin = (s32)s[3]; pmax = (s32)s[4];
                    uexp = s[5]; unit = s[6]; rsize = s[7]; rcount = s[8];
                }
                break;
            }
        } else if (type == 2) {
            u32 u = sz == 4 ? v : (upage << 16) | v;
            if (tag == 0 && nus < 16) usages[nus++] = u;
            if (tag == 1) umin = u;
            if (tag == 2) umax = u;
        } else if (type == 0) {
            if (tag == 0x0A) {                          /* collection */
                u32 cu = nus ? usages[0] : 0;
                if (v == 1 && cu == 0x000D0005UL && tpdepth == -1) {
                    tpdepth = depth;                    /* application: touch pad */
                    tpid = -1;
                }
                if (tpdepth >= 0 && cu == 0x000D0022UL && slot + 1 < MAXSLOT) slot++;   /* finger */
                depth++;
            } else if (tag == 0x0C) {
                depth--;
                if (depth == tpdepth) tpdepth = -2;     /* only the first touch pad collection */
            } else if (tag == 0x08 || tag == 0x0B) {    /* input, feature */
                struct roff *r;
                u32 f;
                for (i = 0; i < nro && ro[i].id != id; i++) ;
                if (i == nro) i = 0;
                r = &ro[i];
                for (f = 0; f < rcount; f++) {
                    u32 u = 0;
                    struct pfield fl;
                    s32 *off = tag == 0x08 ? &r->in : &r->feat;
                    if (!(v & 1)) {
                        if (nus) u = usages[f < (u32)nus ? f : (u32)nus - 1];
                        else if (umin) u = umin + f <= umax || !umax ? umin + f : umax;
                    }
                    fl.off = (s16)*off;
                    fl.size = (u8)rsize;
                    fl.sign = lmin < 0;
                    fl.lmin = lmin;
                    fl.lmax = lmax;
                    *off += (s32)rsize;
                    if (tag == 0x0B) {
                        if (u == 0x000D0052UL && p->mode.off < 0) {
                            p->mode = fl;
                            p->modeid = id;
                        } else if (u == 0x000D0057UL && p->surface.off < 0) {
                            p->surface = fl;
                            p->selid = id;
                        } else if (u == 0x000D0058UL && p->button.off < 0) {
                            p->button = fl;
                            p->selid = id;
                        }
                        continue;
                    }
                    if (tpdepth < 0 || tag != 0x08) continue;
                    if (tpid < 0) tpid = id;            /* the first input report of the collection */
                    p->repid = (u8)tpid;
                    if (id != tpid || !u) continue;
                    if (u == 0x000D0054UL) p->count = fl;
                    else if (u == 0x000D0056UL) p->scan = fl;
                    else if (u >= 0x00090001UL && u <= 0x00090003UL) p->btn[u - 0x00090001UL] = fl;
                    else if (slot >= 0) {
                        struct pslot *s = &p->s[slot];
                        if (u == 0x000D0042UL) s->tip = fl;
                        else if (u == 0x000D0047UL) s->conf = fl;
                        else if (u == 0x000D0051UL) s->id = fl;
                        else if (u == 0x00010030UL) {
                            s->x = fl;
                            if (!p->xrange) {
                                p->xrange = lmax - lmin;
                                p->xum = phys_um(pmin, pmax, unit, uexp);
                            }
                        } else if (u == 0x00010031UL) {
                            s->y = fl;
                            if (!p->yrange) {
                                p->yrange = lmax - lmin;
                                p->yum = phys_um(pmin, pmax, unit, uexp);
                            }
                        }
                    }
                }
            }
            if (tag == 0x08 || tag == 0x09 || tag == 0x0A || tag == 0x0B || tag == 0x0C) {
                nus = 0;
                umin = umax = 0;
            }
        }
    }
    for (i = 0; i < nro; i++) {
        if (ro[i].id == p->modeid) p->modebits = (int)ro[i].feat;
        if (ro[i].id == p->selid) p->selbits = (int)ro[i].feat;
    }
    /* finger slots with a position */
    for (i = 0; i <= slot && i < MAXSLOT; i++)
        if (p->s[i].x.off < 0 || p->s[i].y.off < 0 || p->s[i].tip.off < 0) break;
    p->nslot = i;
    if (p->xrange <= 0) p->xrange = 1000;
    if (p->yrange <= 0) p->yrange = 1000;
    if (p->xum < 20000 || p->xum > 400000) p->xum = 100000;     /* unknown: 100 x 60 mm */
    if (p->yum < 15000 || p->yum > 300000) p->yum = 60000;
    p->kx = (s32)(p->xum * CPMM / 1000);
    p->ky = (s32)(p->yum * CPMM / 1000);
    p->nx = (s32)(p->xum * 16 / SCROLL_UM);
    p->ny = (s32)(p->yum * 16 / SCROLL_UM);
    return p->nslot ? 0 : -1;
}

/* ---------------------------------------------------------------- interface */

int ptp_is_touchpad(const u8 *rd, int n)
{
    int i;
    for (i = 0; i + 3 < n; i++)
        if (rd[i] == 0x05 && rd[i + 1] == 0x0D && rd[i + 2] == 0x09 && rd[i + 3] == 0x05) return 1;
    return 0;
}

struct ptp *ptp_new(const u8 *rd, int n)
{
    int i;
    struct ptp *p;
    for (i = 0; i < MAXPTP && ptps[i].used; i++) ;
    if (i == MAXPTP) return NULL;
    p = &ptps[i];
    memset(p, 0, sizeof(*p));
    if (parse(p, rd, n)) return NULL;
    p->out = plat_mouse;
    p->used = 1;
    kprintf("ptp: touchpad, %d contact slot(s), %lu x %lu mm, report %u%s\n", p->nslot, p->xum / 1000, p->yum / 1000,
            p->repid, p->mode.off >= 0 ? "" : ", no input mode feature");
    return p;
}

void ptp_free(struct ptp *p)
{
    if (p) p->used = 0;
}

/* feature report 'which' (0: input mode = touchpad, 1: surface and button reporting on);
   buf[0] = report ID; returns the length or 0 */
int ptp_feature(struct ptp *p, int which, u8 *buf, int max)
{
    int len = 1 + ((which ? p->selbits : p->modebits) + 7) / 8;
    if (which ? (p->surface.off < 0 && p->button.off < 0) : p->mode.off < 0) return 0;
    if (len > max) return 0;
    memset(buf, 0, (u32)len);
    buf[0] = which ? p->selid : p->modeid;
    if (!which) {
        putbits(buf + 1, &p->mode, 3);
    } else {
        putbits(buf + 1, &p->surface, 1);
        putbits(buf + 1, &p->button, 1);
    }
    return len;
}

/* ---------------------------------------------------------------- gestures */

static s32 to_um(struct ptp *p, s32 d, int y)
{
    return d * (s32)((y ? p->yum : p->xum) / 10) / (y ? p->yrange : p->xrange) * 10;
}

static void emit(struct ptp *p, s32 dx, s32 dy, int dz, int dw)
{
    int b = p->btnheld | (p->tapup ? p->tapbtn : 0);
    p->out((int)dx, (int)dy, dz, dw, b);
}

static void frame(struct ptp *p)
{
    struct contact *c = p->cur;
    int n = p->ncur, i, k, matched = 0;
    s32 mx = 0, my = 0;
    u32 now = p->scan.off >= 0 ? p->devms : plat_ms();   /* reports may arrive in bursts */
    s32 dx = 0, dy = 0;
    int dz = 0, dw = 0;
    /* movement of the contacts that were there before */
    for (i = 0; i < n && i < 2; i++)
        for (k = 0; k < p->nprev; k++)
            if (p->prev[k].id == c[i].id) {
                mx += c[i].x - p->prev[k].x;
                my += c[i].y - p->prev[k].y;
                matched++;
                break;
            }
    if (n && !p->nprev) {                           /* touch begins */
        p->t_down = now;
        p->maxn = 0;
        p->travel = 0;
        p->rx = p->ry = p->sx = p->sy = 0;
        p->tdx = p->tdy = 0;
        p->clicked = 0;
    }
    if (n > p->maxn) p->maxn = n;
    if (n != p->nprev) p->sx = p->sy = 0;           /* finger count changed: new gesture */
    if (matched) {
        s32 ux = to_um(p, mx / matched, 0), uy = to_um(p, my / matched, 1);
        p->travel += (u32)(ux < 0 ? -ux : ux) + (u32)(uy < 0 ? -uy : uy);
        /* exact: remainders kept in units times the scale */
        if (n == 1) {
            p->rx += mx / matched * p->kx;
            p->ry += my / matched * p->ky;
            dx = p->rx / p->xrange;
            dy = p->ry / p->yrange;
            p->rx -= dx * p->xrange;
            p->ry -= dy * p->yrange;
        } else if (n == 2) {
            s32 ux16 = p->xrange * 16, uy16 = p->yrange * 16;
            p->sx += mx / matched * p->nx;
            p->sy += my / matched * p->ny;
            /* fingers down: the page goes down (wheel towards the user) */
            dz = -(int)(p->sy / uy16);
            dw = (int)(p->sx / ux16);
            p->sy += dz * uy16;
            p->sx -= dw * ux16;
        }
    }
    /* pad button: left, right when two fingers rest on the pad */
    if (p->curbtn && !p->btnheld) {
        p->btnheld = (p->curbtn & 2) || n >= 2 ? 2 : (p->curbtn & 4) ? 4 : 1;
        p->clicked = 1;
        DBG2("ptp: pad button, button %d\n", p->btnheld);
    }
    if (!p->curbtn && p->btnheld) {
        p->btnheld = 0;
        DBG2("ptp: pad button up, %d contact(s)\n", n);
    }
    /* tap: short touch without travel and without the pad button */
    if (!n && p->nprev && now - p->t_down < TAP_MS && p->travel < TAP_UM && !p->clicked && !p->curbtn) {
        p->tapbtn = p->maxn >= 3 ? 4 : p->maxn == 2 ? 2 : 1;
        p->tapup = plat_ms() + CLICK_MS;   /* ptp_tick ends it */
        if (!p->tapup) p->tapup = 1;
        DBG2("ptp: tap, button %d\n", p->tapbtn);
    }
    if (dz || dw) DBG2("ptp: wheel %d %d\n", dz, dw);
    p->tdx += dx;
    p->tdy += dy;
    if (!n && p->nprev) DBG2("ptp: touch ends, %d finger(s), pointer %ld %ld\n", p->maxn, p->tdx, p->tdy);
    if (dx || dy || dz || dw || p->tapup || n != p->nprev || p->curbtn) emit(p, dx, dy, dz, dw);
    memcpy(p->prev, c, sizeof(p->cur));
    p->nprev = n;
}

/* one input report (with the report ID when the device uses IDs). Hybrid reporting:
   the first report of a frame carries the contact count, the others count 0 */
void ptp_input(struct ptp *p, const u8 *r, u32 len)
{
    int i, cnt;
    if (p->repid) {
        if (!len || r[0] != p->repid) return;
        r++;
        len--;
    }
    cnt = p->count.off >= 0 ? (int)bits(r, len, &p->count) : -1;
    if (p->scan.off >= 0) {
        u16 sc = (u16)bits(r, len, &p->scan), d = (u16)(sc - p->lastscan);
        if (cnt != 0) p->devms += d / 10 > 1000 ? 10 : d / 10U;
        p->lastscan = sc;
    }
    if (!cnt && p->expect <= 0) return;                 /* the rest of a frame whose start was lost */
    if (cnt != 0) {
        p->ncur = p->seen = 0;
        p->curbtn = 0;
        p->expect = cnt;
    }
    for (i = 0; i < 3; i++)
        if (bits(r, len, &p->btn[i])) p->curbtn |= (u8)(1 << i);
    for (i = 0; i < p->nslot && p->expect != 0; i++) {
        struct pslot *s = &p->s[i];
        struct contact c;
        if (p->expect > 0 && p->seen >= p->expect) break;
        p->seen++;
        c.tip = (u8)bits(r, len, &s->tip);
        c.conf = s->conf.off >= 0 ? (u8)bits(r, len, &s->conf) : 1;
        c.id = (u8)(s->id.off >= 0 ? bits(r, len, &s->id) : i);
        c.x = bits(r, len, &s->x);
        c.y = bits(r, len, &s->y);
        if (c.tip && c.conf && p->ncur < MAXCONTACT) p->cur[p->ncur++] = c;   /* not lifted, not a palm */
    }
    if (p->expect > 0 && p->seen < p->expect) return;   /* more contacts follow */
    p->expect = 0;
    frame(p);
}

/* tap clicks end a moment later (PS/2 touchpads report at interrupt time: interrupts off) */
void ptp_tick(void)
{
    int i;
    u32 now = plat_ms();
    for (i = 0; i < MAXPTP; i++) {
        struct ptp *p = &ptps[i];
        u32 f;
        if (!p->used || !p->tapup || (s32)(now - p->tapup) < 0) continue;
        f = irq_save();
        if (p->tapup && (s32)(now - p->tapup) >= 0) {
            p->tapup = 0;
            emit(p, 0, 0, 0, 0);
        }
        irq_restore(f);
    }
}

/* ---------------------------------------------------------------- contacts of other protocols */

/* a touchpad whose positions run from min to max (y downwards) over a pad of xum x yum
   micrometres (0: unknown); its events go to 'out' (NULL: plat_mouse) */
struct ptp *ptp_new_abs(s32 xmin, s32 xmax, s32 ymin, s32 ymax, u32 xum, u32 yum,
                        void (*out)(int dx, int dy, int dz, int dw, int buttons))
{
    int i;
    struct ptp *p;
    for (i = 0; i < MAXPTP && ptps[i].used; i++) ;
    if (i == MAXPTP) return NULL;
    p = &ptps[i];
    memset(p, 0, sizeof(*p));
    p->scan.off = p->count.off = p->mode.off = p->surface.off = p->button.off = -1;
    p->xrange = xmax - xmin > 0 ? xmax - xmin : 1000;
    p->yrange = ymax - ymin > 0 ? ymax - ymin : 1000;
    p->xum = xum;
    p->yum = yum;
    if (p->xum < 20000 || p->xum > 400000) p->xum = 100000;
    if (p->yum < 15000 || p->yum > 300000) p->yum = 60000;
    p->kx = (s32)(p->xum * CPMM / 1000);
    p->ky = (s32)(p->yum * CPMM / 1000);
    p->nx = (s32)(p->xum * 16 / SCROLL_UM);
    p->ny = (s32)(p->yum * 16 / SCROLL_UM);
    p->out = out ? out : plat_mouse;
    p->used = 1;
    kprintf("ptp: touchpad %ld x %ld, %lu x %lu mm\n", p->xrange, p->yrange, p->xum / 1000, p->yum / 1000);
    return p;
}

/* one frame: the fingers on the pad (a protocol that knows fewer positions than fingers
   repeats a position under another id) and the buttons (bit 0 the pad / left button, 1 right,
   2 middle) */
void ptp_contacts(struct ptp *p, const struct ptp_contact *c, int n, int buttons)
{
    int i;
    if (!p) return;
    if (n > MAXCONTACT) n = MAXCONTACT;
    for (i = 0; i < n; i++) {
        p->cur[i].id = c[i].id;
        p->cur[i].tip = p->cur[i].conf = 1;
        p->cur[i].x = c[i].x;
        p->cur[i].y = c[i].y;
    }
    p->ncur = n;
    p->curbtn = (u8)(buttons & 7);
    frame(p);
}
