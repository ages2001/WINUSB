/* uaudio.c - USB Audio Class 1.0 and 2.0 (playback, recording, volume, record source)
   and USB MIDI output, on top of the sound engine (aeng.c).
   Each audio function becomes one sound device. The engine mixes into a software ring of
   16-bit stereo frames at the device rate; isochronous OUT packets are cut from it in the
   device's format (any channel count, 16/24/32-bit), recorded packets are written into the
   capture ring the same way. Asynchronous devices are followed through their feedback
   endpoint. */
#include "usb.h"
#include "audio.h"

#define MAXUA       6
#define MAXENT      48
#define RINGSZ      0x10000UL
#define RINGPER     1024

/* entity kinds */
#define E_IT        2
#define E_OT        3
#define E_MIXER     4
#define E_SEL       5
#define E_FU        6
#define E_CLKSRC    10
#define E_CLKSEL    11
#define E_OTHER     15

struct ent {
    u8 id, kind, nsrc, clock;
    u8 src[8];
    u16 ttype;                  /* terminal type */
    u8 vol;                     /* FU: channel with a volume control (0 master, 1 left), 0xFF none */
    u8 mute;                    /* FU: channel with a mute control, 0xFF none */
    u8 vol2;                    /* FU: right channel volume when the master has none */
};

struct ualt {                   /* chosen alternate setting of one direction */
    u8 ifnum, alt, ch, sub, bits, sync;
    u8 *ep;                     /* data endpoint descriptor */
    u8 *fbep;                   /* feedback endpoint descriptor, NULL none */
    u8 eprate;                  /* UAC1: the endpoint has a sampling frequency control */
    u8 term;                    /* terminal link */
    u32 rate;
};

struct ua;

struct ustream {
    struct ua *u;
    int dir;
    struct ualt alt;
    struct usb_pipe pipe, fbpipe;
    struct usb_iso iso, fbiso;
    u32 nominal;                /* frames per packet, 16.16 */
    u32 fb;                     /* from the feedback endpoint, 16.16 per packet, 0 none */
    u32 acc;                    /* fraction */
    u32 pos;                    /* bytes into the ring */
    u8 active;
    u8 implicit;                /* playback paced by the capture stream */
};

struct ua {
    struct adev a;
    struct usb_dev *d;
    u8 acif, uac2, used, rsv;
    struct ent ents[MAXENT];
    int nent;
    struct ustream out, in;
    u8 ofu, ifu, sel;           /* entity ids: playback / capture feature unit, selector */
    u8 inowner;                 /* capture runs for: 1 the engine, 2 implicit feedback */
    s16 vmin[2], vmax[2];       /* 1/256 dB */
    u8 selmap[8];               /* selector input -> AS_* */
    u8 nselin;
    u8 *rings;
    /* USB MIDI */
    u8 midi;
    u8 midiif;
    struct usb_pipe mpipe;
    struct usb_xfer mx;
    u8 *mbuf;                   /* DMA */
    u8 mq[256];                 /* queued 4-byte events */
    u16 mqh, mqt;
    u8 mbusy;
    u8 msx[3];                  /* system exclusive bytes waiting */
    u8 msxn, insx;
};

static struct ua *uas[MAXUA];

/* ---------------------------------------------------------------- class requests */

static int ac_req(struct ua *u, u8 rt, u8 req, u16 val, u8 ent, void *buf, u16 len)
{
    return usb_control(u->d, rt, req, val, (u16)(((u16)ent << 8) | u->acif), buf, len);
}

static struct ent *find_ent(struct ua *u, u8 id)
{
    int i;
    for (i = 0; i < u->nent; i++) if (u->ents[i].id == id) return &u->ents[i];
    return NULL;
}

/* volume range of a feature unit channel */
static int fu_range(struct ua *u, u8 fu, u8 ch, s16 *mn, s16 *mx)
{
    u8 b[8];
    if (u->uac2) {
        if (ac_req(u, 0xA1, 0x02, (u16)(0x0200 | ch), fu, b, 8) < 8) return -1;
        *mn = (s16)(b[2] | (b[3] << 8));
        *mx = (s16)(b[4] | (b[5] << 8));
    } else {
        if (ac_req(u, 0xA1, 0x82, (u16)(0x0200 | ch), fu, b, 2) < 2) return -1;
        *mn = (s16)(b[0] | (b[1] << 8));
        if (ac_req(u, 0xA1, 0x83, (u16)(0x0200 | ch), fu, b, 2) < 2) return -1;
        *mx = (s16)(b[0] | (b[1] << 8));
    }
    if (*mn >= *mx) return -1;
    return 0;
}

static void fu_set(struct ua *u, u8 fu, u8 ctl, u8 ch, s16 v, int bytes)
{
    u8 b[2];
    b[0] = (u8)v;
    b[1] = (u8)((u16)v >> 8);
    ac_req(u, 0x21, 0x01, (u16)(((u16)ctl << 8) | ch), fu, b, (u16)bytes);
}

/* 0..65535 -> dB: the top 60 dB of the range, 0 = mute */
static s16 to_db(u32 v, s16 mn, s16 mx)
{
    s32 range = (s32)mx - mn;
    if (range > 60 * 256) range = 60 * 256;
    return (s16)((s32)mx - (s32)(((u32)(65535 - v) * (u32)range) / 65535));
}

static void ua_set_vol(struct adev *a, int ctl, u32 lr)
{
    struct ua *u = a->priv;
    struct ent *f;
    int k = ctl == MX_MASTER ? 0 : ctl == MX_INGAIN ? 1 : -1;
    u32 l = lr & 0xFFFF, r = lr >> 16;
    if (k < 0 || !u->d || u->d->gone) return;
    f = find_ent(u, k ? u->ifu : u->ofu);
    if (!f) return;
    if (f->mute != 0xFF) fu_set(u, f->id, 1, f->mute, (s16)(!l && !r), 1);
    if (f->vol == 0xFF) return;
    if (f->vol == 0) {
        fu_set(u, f->id, 2, 0, to_db(MAX(l, r), u->vmin[k], u->vmax[k]), 2);
    } else {
        fu_set(u, f->id, 2, f->vol, to_db(l, u->vmin[k], u->vmax[k]), 2);
        if (f->vol2 != 0xFF) fu_set(u, f->id, 2, f->vol2, to_db(r, u->vmin[k], u->vmax[k]), 2);
    }
}

static void ua_set_src(struct adev *a, int src)
{
    struct ua *u = a->priv;
    u8 i, b;
    if (!u->sel || !u->d || u->d->gone) return;
    for (i = 0; i < u->nselin; i++)
        if (u->selmap[i] == src) {
            b = (u8)(i + 1);
            ac_req(u, 0x21, 0x01, u->uac2 ? 0x0100 : 0, u->sel, &b, 1);
            return;
        }
}

/* ---------------------------------------------------------------- descriptors */

static void parse_ac(struct ua *u, u8 *p, u8 *end)
{
    u8 *q;
    for (q = p + p[0]; q + 2 <= end && q[0] && q[1] != 4; q += q[0]) {
        struct ent *e;
        u8 st = q[2];
        int i;
        if (q[1] != 0x24 || st < 2 || u->nent >= MAXENT || q[0] < 4) continue;
        e = &u->ents[u->nent++];
        memset(e, 0, sizeof(*e));
        e->id = q[3];
        e->vol = e->mute = e->vol2 = 0xFF;
        if (u->uac2) {
            switch (st) {
            case 2: e->kind = E_IT; e->ttype = (u16)(q[4] | (q[5] << 8)); e->clock = q[7]; break;
            case 3: e->kind = E_OT; e->ttype = (u16)(q[4] | (q[5] << 8)); e->nsrc = 1; e->src[0] = q[7]; e->clock = q[8]; break;
            case 4: case 5: case 11:
                e->kind = st == 4 ? E_MIXER : st == 5 ? E_SEL : E_CLKSEL;
                e->nsrc = (u8)MIN(q[4], 8);
                for (i = 0; i < e->nsrc; i++) e->src[i] = q[5 + i];
                break;
            case 6:
            {
                int nch = (q[0] - 6) / 4;
                e->kind = E_FU;
                e->nsrc = 1;
                e->src[0] = q[4];
                for (i = 0; i < nch && i < 3; i++) {
                    u32 c = q[5 + i * 4] | ((u32)q[6 + i * 4] << 8);
                    if ((c & 3) && e->mute == 0xFF) e->mute = (u8)i;
                    if (c & 0x0C) {
                        if (e->vol == 0xFF) e->vol = (u8)i;
                        else if (e->vol == 1 && i == 2) e->vol2 = 2;
                    }
                }
                if (e->vol == 0) e->vol2 = 0xFF;
                break;
            }
            case 10: e->kind = E_CLKSRC; break;
            case 7:                                 /* effect unit: one source */
                e->kind = E_OTHER;
                e->nsrc = 1;
                e->src[0] = q[6];
                break;
            default:                                /* processing / extension unit */
                e->kind = E_OTHER;
                if (q[0] > 7) {
                    e->nsrc = (u8)MIN(q[6], 8);
                    for (i = 0; i < e->nsrc && 7 + i < q[0]; i++) e->src[i] = q[7 + i];
                }
            }
        } else {
            switch (st) {
            case 2: e->kind = E_IT; e->ttype = (u16)(q[4] | (q[5] << 8)); break;
            case 3: e->kind = E_OT; e->ttype = (u16)(q[4] | (q[5] << 8)); e->nsrc = 1; e->src[0] = q[7]; break;
            case 4: case 5:
                e->kind = st == 4 ? E_MIXER : E_SEL;
                e->nsrc = (u8)MIN(q[4], 8);
                for (i = 0; i < e->nsrc; i++) e->src[i] = q[5 + i];
                break;
            case 6:
            {
                int cs = q[5], nch;
                e->kind = E_FU;
                e->nsrc = 1;
                e->src[0] = q[4];
                if (!cs) break;
                nch = (q[0] - 7) / cs;
                for (i = 0; i < nch && i < 3; i++) {
                    u8 c = q[6 + i * cs];
                    if ((c & 1) && e->mute == 0xFF) e->mute = (u8)i;
                    if (c & 2) {
                        if (e->vol == 0xFF) e->vol = (u8)i;
                        else if (e->vol == 1 && i == 2) e->vol2 = 2;
                    }
                }
                if (e->vol == 0) e->vol2 = 0xFF;
                break;
            }
            default:
                e->kind = E_OTHER;
                if (q[0] > 6) {
                    e->nsrc = (u8)MIN(q[6], 8);
                    for (i = 0; i < e->nsrc && 7 + i < q[0]; i++) e->src[i] = q[7 + i];
                }
            }
        }
    }
}

/* does the path upstream of 'id' reach entity 'target'? */
static int reaches(struct ua *u, u8 id, u8 target, int depth)
{
    struct ent *e = find_ent(u, id);
    int i;
    if (id == target) return 1;
    if (!e || depth > 12) return 0;
    for (i = 0; i < e->nsrc; i++)
        if (reaches(u, e->src[i], target, depth + 1)) return 1;
    return 0;
}

/* first feature unit (with volume) going upstream from 'id' toward 'target' */
static u8 find_fu(struct ua *u, u8 id, u8 target, int depth)
{
    struct ent *e = find_ent(u, id);
    int i;
    if (!e || depth > 12 || id == target) return 0;
    if (e->kind == E_FU && e->vol != 0xFF && reaches(u, id, target, 0)) return id;
    for (i = 0; i < e->nsrc; i++) {
        u8 r = find_fu(u, e->src[i], target, depth + 1);
        if (r) return r;
    }
    return 0;
}

static u8 find_kind(struct ua *u, u8 id, int kind, int depth)
{
    struct ent *e = find_ent(u, id);
    int i;
    if (!e || depth > 12) return 0;
    if (e->kind == kind) return id;
    for (i = 0; i < e->nsrc; i++) {
        u8 r = find_kind(u, e->src[i], kind, depth + 1);
        if (r) return r;
    }
    return 0;
}

static u8 src_of_term(u16 tt)
{
    if (tt >= 0x0200 && tt < 0x0300) return AS_MIC;
    if (tt == 0x0603 || tt == 0x0602 || tt == 0x0601) return AS_LINE;
    if (tt == 0x0703) return AS_CD;
    return AS_AUX;
}

static int rate_ok(u32 r, const u32 *rates, int n, u32 lo, u32 hi)
{
    int i;
    if (lo && r >= lo && r <= hi) return 1;
    for (i = 0; i < n; i++) if (rates[i] == r) return 1;
    return 0;
}

static u32 pick_rate(const u32 *rates, int n, u32 lo, u32 hi)
{
    static const u32 pref[] = { 48000, 44100, 96000, 88200, 32000, 192000, 176400, 22050, 16000, 11025, 8000, 0 };
    int i;
    for (i = 0; pref[i]; i++) if (rate_ok(pref[i], rates, n, lo, hi)) return pref[i];
    if (n) return rates[0];
    return hi;
}

/* UAC2: the sample rates the clock source of a terminal offers */
static int clock_rates(struct ua *u, u8 term, u32 *rates, int max, u8 *clkid)
{
    struct ent *t = find_ent(u, term), *c;
    u8 b[2 + 12 * 8];
    int n, i, k = 0;
    if (!t) return 0;
    c = find_ent(u, t->clock);
    while (c && c->kind == E_CLKSEL && c->nsrc) c = find_ent(u, c->src[0]);
    if (!c || c->kind != E_CLKSRC) return 0;
    *clkid = c->id;
    if (ac_req(u, 0xA1, 0x02, 0x0100, c->id, b, 2) < 2) return 0;
    n = MIN(b[0] | (b[1] << 8), 8);
    if (ac_req(u, 0xA1, 0x02, 0x0100, c->id, b, (u16)(2 + 12 * n)) < 2 + 12 * n) return 0;
    for (i = 0; i < n && k < max; i++) {
        u8 *r = b + 2 + 12 * i;
        u32 lo = r[0] | (r[1] << 8) | ((u32)r[2] << 16) | ((u32)r[3] << 24);
        u32 hi = r[4] | (r[5] << 8) | ((u32)r[6] << 16) | ((u32)r[7] << 24);
        rates[k++] = lo;
        if (hi != lo && k < max) rates[k++] = hi;
    }
    return k;
}

/* best alternate setting of one streaming interface; score < 0 when unusable */
static int choose_alt(struct ua *u, u8 *ifd, u8 *end, struct ualt *best, int dir)
{
    u8 *p = ifd;
    int bestscore = -1;
    for (; p && p + 2 <= end && p[0]; p += p[0]) {
        u8 *q, *ep = NULL, *fb = NULL, ch = 0, sub = 0, bits = 0, term = 0, eprate = 0, clk = 0;
        u32 rates[16], lo = 0, hi = 0, rate;
        int nr = 0, pcm = 0, score, i;
        if (p[1] != 4) continue;
        if (p[2] != ifd[2]) break;                      /* another interface */
        if (p[3] == 0 || p[5] != 1 || p[6] != 2) continue;
        for (q = p + p[0]; q + 2 <= end && q[0] && q[1] != 4; q += q[0]) {
            if (q[1] == 0x24 && q[2] == 1) {
                term = q[3];
                if (u->uac2) {
                    pcm = q[0] >= 10 && (q[6] & 1) && q[5] == 1;
                    ch = q[10];
                } else {
                    pcm = (q[5] | (q[6] << 8)) == 1;
                }
            } else if (q[1] == 0x24 && q[2] == 2 && q[3] == 1) {
                if (u->uac2) {
                    sub = q[4];
                    bits = q[5];
                } else {
                    ch = q[4];
                    sub = q[5];
                    bits = q[6];
                    if (q[7] == 0 && q[0] >= 14) {
                        lo = q[8] | (q[9] << 8) | ((u32)q[10] << 16);
                        hi = q[11] | (q[12] << 8) | ((u32)q[13] << 16);
                    } else {
                        for (i = 0; i < q[7] && i < 16 && 8 + i * 3 + 2 < q[0]; i++)
                            rates[nr++] = q[8 + i * 3] | (q[9 + i * 3] << 8) | ((u32)q[10 + i * 3] << 16);
                    }
                }
            } else if (q[1] == 5 && (q[3] & 3) == 1) {
                if (((q[3] >> 4) & 3) == 1 || (ep && !fb && (q[2] & 0x80) && !(ep[2] & 0x80))) fb = q;
                else if (!ep) ep = q;
            } else if (q[1] == 0x25 && q[2] == 1 && !u->uac2) {
                eprate = q[3] & 1;
            }
        }
        if (!pcm || !ep || !ch || (sub != 2 && sub != 3 && sub != 4)) continue;
        if (((ep[2] & 0x80) != 0) != dir) continue;
        if (u->uac2) nr = clock_rates(u, term, rates, 16, &clk);
        rate = pick_rate(rates, nr, lo, hi);
        if (!rate) continue;
        score = (ch == 2 ? 100 : ch > 2 ? 50 : 30) + (sub == 2 ? 20 : sub == 3 ? 15 : 10) +
                (rate == 48000 ? 8 : rate == 44100 ? 6 : 2);
        if (score > bestscore) {
            bestscore = score;
            best->ifnum = p[2];
            best->alt = p[3];
            best->ch = ch;
            best->sub = sub;
            best->bits = bits;
            best->ep = ep;
            best->fbep = (((ep[3] >> 2) & 3) == 1 && !dir) ? fb : NULL;
            best->sync = (u8)((ep[3] >> 2) & 3);
            best->eprate = eprate;
            best->term = term;
            best->rate = rate;
            (void)clk;
        }
    }
    return bestscore;
}

/* ---------------------------------------------------------------- streaming */

static int frames_per_pkt(struct ustream *st)
{
    u32 step = st->fb ? st->fb : st->nominal, n;
    st->acc += step;
    n = st->acc >> 16;
    st->acc &= 0xFFFF;
    return (int)n;
}

static int iso_fill(struct usb_iso *s, u8 *dst, int max)
{
    struct ustream *st = s->priv;
    struct ahw *h = &st->u->a.out;
    int n = frames_per_pkt(st), i, c, fsz = st->alt.ch * st->alt.sub;
    if (n * fsz > max) n = max / fsz;
    for (i = 0; i < n; i++) {
        s16 *f = (s16 *)(h->buf + st->pos);
        s16 l = f[0], r = f[1];
        for (c = 0; c < st->alt.ch; c++) {
            s16 v = st->alt.ch == 1 ? (s16)(((s32)l + r) / 2) : c == 0 ? l : c == 1 ? r : 0;
            switch (st->alt.sub) {
            case 2: dst[0] = (u8)v; dst[1] = (u8)(v >> 8); break;
            case 3: dst[0] = 0; dst[1] = (u8)v; dst[2] = (u8)(v >> 8); break;
            default: dst[0] = 0; dst[1] = 0; dst[2] = (u8)v; dst[3] = (u8)(v >> 8);
            }
            dst += st->alt.sub;
        }
        st->pos = (st->pos + 4) % h->size;
    }
    return n * fsz;
}

static void iso_done(struct usb_iso *s, u8 *src, int len)
{
    struct ustream *st = s->priv;
    struct ahw *h = &st->u->a.in;
    int fsz = st->alt.ch * st->alt.sub, n = len / fsz, i;
    struct ustream *o = &st->u->out;
    if (o->implicit && o->active && o->iso.period_us) {
        u32 v = (u32)(((u64)(u32)n << 16) * o->iso.period_us / s->period_us);
        o->fb = o->fb ? (o->fb * 7 + v) / 8 : v;
    }
    for (i = 0; i < n; i++, src += fsz) {
        s16 *f = (s16 *)(h->buf + st->pos);
        u8 *r = src + (st->alt.ch > 1 ? st->alt.sub : 0);
        f[0] = (s16)(src[st->alt.sub - 2] | (src[st->alt.sub - 1] << 8));
        f[1] = (s16)(r[st->alt.sub - 2] | (r[st->alt.sub - 1] << 8));
        st->pos = (st->pos + 4) % h->size;
    }
}

/* feedback: frames per (micro)frame, 10.14 (3 bytes) or 16.16 (4 bytes) */
static void fb_done(struct usb_iso *s, u8 *src, int len)
{
    struct ustream *st = s->priv;
    u32 v, per, cand;
    int hs = st->u->d->speed >= USB_HS;
    if (len < 3) return;
    v = src[0] | (src[1] << 8) | ((u32)src[2] << 16) | (len >= 4 ? (u32)src[3] << 24 : 0);
    if (len == 3) v <<= 2;                                  /* 10.14 -> 16.16 */
    per = st->iso.period_us / (hs ? 125 : 1000);            /* data packet in (micro)frames */
    if (!per) per = 1;
    cand = v * per;
    /* accept only plausible values (some devices use the other format) */
    if (cand > st->nominal + st->nominal / 8 || cand < st->nominal - st->nominal / 8) {
        cand = (len == 3 ? v >> 2 : v >> 4) * per;          /* 16.16 in 3 bytes / 10.14 in 4 bytes */
        if (cand > st->nominal + st->nominal / 8 || cand < st->nominal - st->nominal / 8) return;
    }
    st->fb = cand;
}

static int set_rate(struct ua *u, struct ualt *al)
{
    u8 b[4];
    b[0] = (u8)al->rate;
    b[1] = (u8)(al->rate >> 8);
    b[2] = (u8)(al->rate >> 16);
    b[3] = (u8)(al->rate >> 24);
    if (u->uac2) {
        struct ent *t = find_ent(u, al->term), *c = t ? find_ent(u, t->clock) : NULL;
        while (c && c->kind == E_CLKSEL && c->nsrc) c = find_ent(u, c->src[0]);
        if (!c) return 0;
        return ac_req(u, 0x21, 0x01, 0x0100, c->id, b, 4) < 0 ? -1 : 0;
    }
    if (!al->eprate) return 0;
    return usb_control(u->d, 0x22, 0x01, 0x0100, al->ep[2], b, 3) < 0 ? -1 : 0;
}

static int stream_start(struct ua *u, int dir)
{
    struct ustream *st = dir ? &u->in : &u->out;
    if (!u->d || u->d->gone || st->active) return -1;
    if (usb_set_interface(u->d, st->alt.ifnum, st->alt.alt)) return -1;
    set_rate(u, &st->alt);
    if (usb_pipe_open(u->d, &st->pipe, st->alt.ep)) goto fail;
    memset(&st->iso, 0, sizeof(st->iso));
    st->iso.pipe = &st->pipe;
    st->iso.in = (u8)dir;
    st->iso.fill = iso_fill;
    st->iso.done = iso_done;
    st->iso.priv = st;
    if (usb_iso_start(&st->iso)) goto fail2;
    st->nominal = (u32)(((u64)st->alt.rate << 16) * st->iso.period_us / 1000000UL);
    st->fb = 0;
    st->acc = 0;
    st->pos = 0;
    if (st->alt.fbep && usb_pipe_open(u->d, &st->fbpipe, st->alt.fbep) == 0) {
        memset(&st->fbiso, 0, sizeof(st->fbiso));
        st->fbiso.pipe = &st->fbpipe;
        st->fbiso.in = 1;
        st->fbiso.done = fb_done;
        st->fbiso.priv = st;
        if (usb_iso_start(&st->fbiso)) usb_pipe_close(&st->fbpipe);
    }
    st->active = 1;
    return 0;
fail2:
    usb_pipe_close(&st->pipe);
fail:
    usb_set_interface(u->d, st->alt.ifnum, 0);
    return -1;
}

static void stream_stop(struct ua *u, int dir)
{
    struct ustream *st = dir ? &u->in : &u->out;
    if (!st->active) return;
    st->active = 0;
    if (st->fbiso.running) usb_iso_stop(&st->fbiso);
    usb_pipe_close(&st->fbpipe);
    usb_iso_stop(&st->iso);
    usb_pipe_close(&st->pipe);
    if (u->d && !u->d->gone) usb_set_interface(u->d, st->alt.ifnum, 0);
}

/* Implicit feedback: an asynchronous playback endpoint without a feedback endpoint takes
   its rate from the device's capture stream, which then runs along with playback. */
static int implicit_fb(struct ua *u)
{
    return u->out.alt.sync == 1 && !u->out.alt.fbep && (u->a.caps & WINAC_IN) && u->in.alt.rate == u->out.alt.rate;
}

static int ua_start(struct adev *a, int dir)
{
    struct ua *u = a->priv;
    if (dir) {
        if (!u->in.active && stream_start(u, 1)) return -1;
        u->inowner |= 1;
        u->in.pos = 0;
        return 0;
    }
    if (stream_start(u, 0)) return -1;
    u->out.implicit = 0;
    if (implicit_fb(u) && (u->in.active || stream_start(u, 1) == 0)) {
        u->inowner |= 2;
        u->out.implicit = 1;
    }
    return 0;
}

static void ua_stop(struct adev *a, int dir)
{
    struct ua *u = a->priv;
    if (!dir) {
        stream_stop(u, 0);
        if (u->out.implicit) {
            u->out.implicit = 0;
            u->inowner &= ~2;
            if (!u->inowner) stream_stop(u, 1);
        }
        return;
    }
    u->inowner &= ~1;
    if (!u->inowner) stream_stop(u, 1);
}

static u32 ua_pos(struct adev *a, int dir)
{
    struct ua *u = a->priv;
    return dir ? u->in.pos : u->out.pos;
}

/* ---------------------------------------------------------------- USB MIDI */

static void mq_put(struct ua *u, u8 cin, u8 b0, u8 b1, u8 b2)
{
    u16 n = (u16)((u->mqt + 4) % sizeof(u->mq));
    if (n == u->mqh) return;                        /* full: dropped */
    u->mq[u->mqt] = cin;                            /* cable 0 */
    u->mq[u->mqt + 1] = b0;
    u->mq[u->mqt + 2] = b1;
    u->mq[u->mqt + 3] = b2;
    u->mqt = n;
}

static void ua_midi(struct adev *a, const u8 *m, int len)
{
    struct ua *u = a->priv;
    int i;
    for (i = 0; i < len; i++) {
        u8 b = m[i];
        if (b >= 0xF8) {
            mq_put(u, 0x0F, b, 0, 0);
            continue;
        }
        if (b == 0xF0) {
            u->insx = 1;
            u->msx[0] = b;
            u->msxn = 1;
            continue;
        }
        if (u->insx) {
            if (b == 0xF7 || (b & 0x80)) {
                u->msx[u->msxn++] = 0xF7;
                mq_put(u, (u8)(4 + u->msxn), u->msx[0], u->msxn > 1 ? u->msx[1] : 0, u->msxn > 2 ? u->msx[2] : 0);
                u->insx = 0;
                u->msxn = 0;
                if (b == 0xF7) continue;
            } else {
                u->msx[u->msxn++] = b;
                if (u->msxn == 3) {
                    mq_put(u, 0x04, u->msx[0], u->msx[1], u->msx[2]);
                    u->msxn = 0;
                }
                continue;
            }
        }
        if (b & 0x80) {
            u8 hi = b >> 4;
            int need = (hi == 0xC || hi == 0xD || b == 0xF1 || b == 0xF3) ? 2 : (b == 0xF6) ? 1 : 3;
            u8 cin = b < 0xF0 ? hi : need == 1 ? 5 : need == 2 ? 2 : 3;
            if (i + need - 1 >= len) break;
            mq_put(u, cin, b, need > 1 ? m[i + 1] : 0, need > 2 ? m[i + 2] : 0);
            i += need - 1;
        }
    }
}

static void midi_done(struct usb_xfer *x)
{
    struct ua *u = x->priv;
    u->mbusy = 0;
}

static void midi_send(struct ua *u)
{
    u32 n = 0, max = MIN(u->mpipe.maxpkt ? u->mpipe.maxpkt : 64, 64);
    if (u->mbusy || u->mqh == u->mqt || !u->d || u->d->gone) return;
    while (u->mqh != u->mqt && n + 4 <= max) {
        memcpy(u->mbuf + n, &u->mq[u->mqh], 4);
        u->mqh = (u16)((u->mqh + 4) % sizeof(u->mq));
        n += 4;
    }
    memset(&u->mx, 0, sizeof(u->mx));
    u->mx.pipe = &u->mpipe;
    u->mx.buf = u->mbuf;
    u->mx.len = n;
    u->mx.cb = midi_done;
    u->mx.priv = u;
    u->mbusy = 1;
    if (usb_submit(&u->mx)) u->mbusy = 0;
}

static void ua_shutdown(struct adev *a)
{
    (void)a;
}

static struct adevops ua_ops = { ua_start, ua_stop, ua_pos, ua_set_vol, ua_set_src, NULL, ua_shutdown, ua_midi };

/* ---------------------------------------------------------------- probe */

static struct ua *ua_slot(struct usb_dev *d, int midi)
{
    int i;
    u16 vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8)), pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
    /* the same device plugged in again keeps its device number */
    for (i = 0; i < MAXUA; i++)
        if (uas[i] && !uas[i]->used && uas[i]->midi == midi && uas[i]->a.vid == vid && uas[i]->a.pid == pid)
            return uas[i];
    for (i = 0; i < MAXUA; i++)
        if (!uas[i]) {
            uas[i] = kmalloc(sizeof(struct ua));
            if (uas[i]) memset(uas[i], 0, sizeof(struct ua));
            return uas[i];
        }
    for (i = 0; i < MAXUA; i++)
        if (!uas[i]->used) return uas[i];
    return NULL;
}

static void name_dev(struct ua *u, struct usb_dev *d, const char *what)
{
    if (d->product[0]) ksnprintf(u->a.name, sizeof(u->a.name), "%s%s", d->product, what);
    else ksnprintf(u->a.name, sizeof(u->a.name), "USB Audio %04x:%04x%s", u->a.vid, u->a.pid, what);
}

static int probe_midi(struct usb_dev *d, u8 *ifd, u8 *end)
{
    struct ua *u;
    u8 *q;
    for (q = ifd + ifd[0]; q + 2 <= end && q[0] && q[1] != 4; q += q[0])
        if (q[1] == 5 && (q[3] & 3) == 2 && !(q[2] & 0x80)) break;
    if (!(q + 2 <= end && q[0] && q[1] == 5)) return -1;
    if (!(u = ua_slot(d, 1))) return -1;
    u->d = d;
    u->used = 1;
    u->midi = 1;
    u->midiif = ifd[2];
    u->a.vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8));
    u->a.pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
    if (usb_pipe_open(d, &u->mpipe, q)) {
        u->used = 0;
        return -1;
    }
    if (!u->mbuf) u->mbuf = kmalloc_align(64, 64);
    u->mqh = u->mqt = 0;
    u->mbusy = 0;
    u->insx = 0;
    name_dev(u, d, "");
    u->a.ops = &ua_ops;
    u->a.priv = u;
    u->a.type = AT_USB;
    u->a.caps = WINAC_MIDI;
    u->a.midiout = 1;
    adev_register(&u->a);
    if (d->driver == UD_NONE) d->driver = UD_MIDI;
    kprintf("uaudio: dev %u MIDI output on interface %u\n", d->id, ifd[2]);
    return 0;
}

static int ua_probe(struct usb_dev *d, u8 *ifd, u8 *end)
{
    struct ua *u;
    u8 *p, *cfg = d->cdesc, *cend = d->cdesc + d->cdesc_len;
    struct ualt al;
    int got = 0;
    if (ifd[5] != 1) return -1;
    if (ifd[6] == 3) return probe_midi(d, ifd, end);
    if (ifd[6] != 1) return -1;
    if (!(u = ua_slot(d, 0))) return -1;
    u->d = d;
    u->acif = ifd[2];
    u->uac2 = ifd[7] == 0x20;
    u->nent = 0;
    u->ofu = u->ifu = u->sel = 0;
    u->inowner = 0;
    u->nselin = 0;
    memset(&u->out, 0, sizeof(u->out));
    memset(&u->in, 0, sizeof(u->in));
    u->out.u = u->in.u = u;
    u->in.dir = 1;
    u->a.vid = (u16)(d->ddesc[8] | (d->ddesc[9] << 8));
    u->a.pid = (u16)(d->ddesc[10] | (d->ddesc[11] << 8));
    parse_ac(u, ifd, end);
    /* streaming interfaces of this function: the audio interfaces after the control one */
    for (p = cfg; p && p + 2 <= cend && p[0]; p += p[0]) {
        if (p[1] != 4 || p[3] != 0 || p[5] != 1 || p[6] != 2 || p[2] <= u->acif) continue;
        memset(&al, 0, sizeof(al));
        if (!u->out.alt.ep && choose_alt(u, p, cend, &al, 0) >= 0) {
            u->out.alt = al;
            got |= 1;
        } else if (!u->in.alt.ep && choose_alt(u, p, cend, &al, 1) >= 0) {
            u->in.alt = al;
            got |= 2;
        }
    }
    if (!got) return -1;
    u->used = 1;
    if (!u->rings) u->rings = kmalloc_align(RINGSZ * 2, 4096);
    if (!u->rings) return -1;
    u->a.caps = 0;
    if (got & 1) {
        int i;
        u->a.caps |= WINAC_OUT;
        u->a.out.buf = u->rings;
        u->a.out.size = RINGSZ;
        u->a.out.period = RINGPER;
        u->a.out.rate = u->out.alt.rate;
        u->a.out.noflush = 1;
        u->a.out.present = 1;
        /* playback volume: feature unit between the streaming terminal and an output */
        for (i = 0; i < u->nent && !u->ofu; i++)
            if (u->ents[i].kind == E_OT && u->ents[i].ttype != 0x0101)
                u->ofu = find_fu(u, u->ents[i].id, u->out.alt.term, 0);
    }
    if (got & 2) {
        struct ent *ot = find_ent(u, u->in.alt.term);
        u->a.caps |= WINAC_IN;
        u->a.in.buf = u->rings + RINGSZ;
        u->a.in.size = RINGSZ;
        u->a.in.period = RINGPER;
        u->a.in.rate = u->in.alt.rate;
        u->a.in.noflush = 1;
        u->a.in.present = 1;
        if (ot) {
            int i;
            u->ifu = find_fu(u, ot->src[0], 0xFF, 0);
            u->sel = find_kind(u, ot->src[0], E_SEL, 0);
            if (u->sel) {
                struct ent *s = find_ent(u, u->sel);
                u->nselin = s->nsrc;
                for (i = 0; i < s->nsrc; i++) {
                    struct ent *t = find_ent(u, s->src[i]);
                    u8 it = t ? find_kind(u, t->id, E_IT, 0) : 0;
                    struct ent *te = it ? find_ent(u, it) : NULL;
                    u->selmap[i] = te ? src_of_term(te->ttype) : AS_AUX;
                    u->a.insrcs |= u->selmap[i];
                }
            } else {
                u8 it = find_kind(u, ot->src[0], E_IT, 0);
                struct ent *te = it ? find_ent(u, it) : NULL;
                u->a.insrcs = te ? src_of_term(te->ttype) : AS_MIC;
            }
            u->a.insrc = u->a.insrcs & (u8)-(s8)u->a.insrcs;   /* lowest */
        }
    }
    if (u->ofu) {
        struct ent *f = find_ent(u, u->ofu);
        if (fu_range(u, u->ofu, f->vol, &u->vmin[0], &u->vmax[0])) u->ofu = 0;
    }
    if (u->ifu) {
        struct ent *f = find_ent(u, u->ifu);
        if (fu_range(u, u->ifu, f->vol, &u->vmin[1], &u->vmax[1])) u->ifu = 0;
    }
    u->a.ops = &ua_ops;
    u->a.priv = u;
    u->a.type = AT_USB;
    u->a.hwvol = (u8)(u->ofu != 0);
    u->a.midiout = 0;
    name_dev(u, d, "");
    d->driver = UD_AUDIO;
    d->drvinfo = (u8)(u->uac2 ? 2 : 1);
    kprintf("uaudio: dev %u UAC%d%s%s\n", d->id, u->uac2 ? 2 : 1, (got & 1) ? ", playback" : "",
            (got & 2) ? ", recording" : "");
    if (got & 1)
        kprintf("uaudio:   play: if %u alt %u, %u ch %u-bit, %lu Hz, %s%s\n", u->out.alt.ifnum, u->out.alt.alt,
                u->out.alt.ch, u->out.alt.bits, u->out.alt.rate,
                u->out.alt.sync == 1 ? "asynchronous" : u->out.alt.sync == 2 ? "adaptive" : "synchronous",
                u->out.alt.fbep ? " with feedback" : implicit_fb(u) ? " with implicit feedback" : "");
    if (got & 2)
        kprintf("uaudio:   record: if %u alt %u, %u ch %u-bit, %lu Hz\n", u->in.alt.ifnum, u->in.alt.alt,
                u->in.alt.ch, u->in.alt.bits, u->in.alt.rate);
    adev_register(&u->a);
    return 0;
}

static void ua_detach(struct usb_dev *d)
{
    int i;
    for (i = 0; i < MAXUA; i++) {
        struct ua *u = uas[i];
        if (!u || !u->used || u->d != d) continue;
        if (u->midi) {
            if (u->mbusy) usb_cancel(&u->mx);
            usb_pipe_close(&u->mpipe);
        } else {
            ua_stop(&u->a, 0);
            ua_stop(&u->a, 1);
            u->a.out.running = u->a.in.running = 0;
        }
        adev_unregister(&u->a);
        u->used = 0;
        u->d = NULL;
        kprintf("uaudio: %s removed\n", u->a.name);
    }
}

void uaudio_tick(void)
{
    int i;
    for (i = 0; i < MAXUA; i++)
        if (uas[i] && uas[i]->used && uas[i]->midi) midi_send(uas[i]);
    aeng_poll();
}

struct usb_class uaudio_class = { "audio", ua_probe, ua_detach, NULL };
