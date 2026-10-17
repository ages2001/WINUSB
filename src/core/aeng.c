/* aeng.c - sound engine: streams in shared rings, mixing, sample rate conversion, capture.
   Shared by WINAUDIO (HDA, AC'97) and WINUSB (USB Audio). No floating point: the core runs
   in a VxD, where the FPU belongs to the virtual machines. */
#include "audio.h"
#include "srctab.h"

#define MAXADEV     8
#define MAXSTREAM   16
#define CHUNK       256             /* output frames per mixing step */
#define MAXIN       (CHUNK * 4 + 8) /* input frames for one step (down to 1/4) */

struct rsmp {
    u32 step;                       /* input frames per output frame, 16.16 */
    u32 frac;                       /* 16.16; >= 1.0: the next input frame is needed */
    int hp;                         /* oldest history entry */
    s16 h[SRC_TAPS][2];
    const s16 (*tab)[SRC_TAPS];
    u8 pass;                        /* same rate: copy */
};

struct astream {
    u8 used, dir, running, dev;
    u32 ring;                       /* winring, seg:off in the system VM */
    u32 rate, bits, ch, flt, fsz;
    u32 vl, vr;                     /* 0..65536 */
    u32 lastfill;                   /* device byte count where the stream's data ends */
    struct rsmp rs;
};

static struct adev *adevs[MAXADEV];
static struct astream streams[MAXSTREAM];
static s32 acc[CHUNK * 2];
static s16 tin[MAXIN * 2];
static s16 tout[CHUNK * 4 * 2];
static int flush_mode;              /* 0 unknown, 1 clflush, 2 wbinvd */
static int synth_dev = -1;          /* device the synthesizer plays on */

/* ---------------------------------------------------------------- helpers */

static s16 clip16(s32 v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

/* IEEE single <-> 16-bit PCM without the FPU */
static s16 f2s(u32 f)
{
    int e = (int)((f >> 23) & 0xFF);
    u32 m = (f & 0x7FFFFFUL) | 0x800000UL;
    s32 v;
    if (e < 127 - 16) return 0;
    if (e >= 127) v = 32767;
    else v = (s32)(m >> (135 - e));
    if (v > 32767) v = 32767;
    return (s16)((f & 0x80000000UL) ? -v : v);
}

static u32 s2f(s16 s)
{
    u32 a, sign = 0;
    int p = 15;
    if (!s) return 0;
    if (s < 0) {
        sign = 0x80000000UL;
        a = (u32)(-(s32)s);
    } else {
        a = (u32)s;
    }
    while (!(a & (1UL << p))) p--;
    return sign | ((u32)(p - 15 + 127) << 23) | ((a << (23 - p)) & 0x7FFFFFUL);
}

static s16 dec1(const u8 *p, u32 bits, u32 flt)
{
    switch (bits) {
    case 8: return (s16)(((s16)p[0] - 128) << 8);
    case 16: return (s16)(p[0] | (p[1] << 8));
    case 24: return (s16)(p[1] | (p[2] << 8));
    default:
        if (flt) return f2s((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
        return (s16)(p[2] | (p[3] << 8));
    }
}

static void enc1(u8 *p, s16 v, u32 bits, u32 flt)
{
    u32 f;
    switch (bits) {
    case 8: p[0] = (u8)((v >> 8) + 128); break;
    case 16: p[0] = (u8)v; p[1] = (u8)(v >> 8); break;
    case 24: p[0] = 0; p[1] = (u8)v; p[2] = (u8)(v >> 8); break;
    default:
        f = flt ? s2f(v) : ((u32)(s32)v << 16);
        p[0] = (u8)f; p[1] = (u8)(f >> 8); p[2] = (u8)(f >> 16); p[3] = (u8)(f >> 24);
    }
}

static struct winring *ringp(struct astream *s)
{
    struct winring *r = (struct winring *)plat_sysrm(s->ring);
    if (!r || r->magic != WINRING_MAGIC || !r->size) return NULL;
    return r;
}

/* ---------------------------------------------------------------- resampler */

static void rs_init(struct rsmp *r, u32 in, u32 out)
{
    u32 ratio = (u32)(((u64)in * 100) / out);
    memset(r, 0, sizeof(*r));
    r->pass = (u8)(in == out);
    r->step = (u32)(((u64)in << 16) / out);
    r->tab = src_tab[ratio <= 100 ? 0 : ratio <= 170 ? 1 : ratio <= 300 ? 2 : 3];
    r->frac = 0x10000UL;
}

/* n input frames -> up to max output frames; *used = input frames taken */
static int rs_run(struct rsmp *r, const s16 *in, int n, s16 *out, int max, int *used)
{
    int p = 0, u = 0;
    if (r->pass) {
        int k = MIN(n, max);
        memcpy(out, in, (u32)k * 4);
        *used = k;
        return k;
    }
    while (p < max) {
        const s16 *c;
        s32 l = 0, rr = 0;
        int k, j;
        while (r->frac >= 0x10000UL) {
            if (u >= n) goto done;
            r->h[r->hp][0] = in[u * 2];
            r->h[r->hp][1] = in[u * 2 + 1];
            r->hp = (r->hp + 1) & (SRC_TAPS - 1);
            u++;
            r->frac -= 0x10000UL;
        }
        c = r->tab[r->frac >> 10];
        for (k = 0, j = r->hp; k < SRC_TAPS; k++, j = (j + 1) & (SRC_TAPS - 1)) {
            l += (s32)r->h[j][0] * c[k];
            rr += (s32)r->h[j][1] * c[k];
        }
        out[p * 2] = clip16(l >> 15);
        out[p * 2 + 1] = clip16(rr >> 15);
        p++;
        r->frac += r->step;
    }
done:
    *used = u;
    return p;
}

/* ---------------------------------------------------------------- devices */

int adev_register(struct adev *a)
{
    int i, j;
    for (i = 0; i < MAXADEV; i++)
        if (adevs[i] == a) {            /* plugged in again: same number, same settings */
            a->gone = 0;
            a->out.running = a->in.running = 0;
            if (a->ops->set_vol)
                for (j = 0; j < MX_COUNT; j++)
                    if (j != MX_MASTER || a->hwvol) a->ops->set_vol(a, j, a->vol[j]);
            if (a->insrc && a->ops->set_src) a->ops->set_src(a, a->insrc);
            kprintf("audio %d: %s is back\n", i, a->name);
            return i;
        }
    for (i = 0; i < MAXADEV; i++) {
        if (adevs[i]) continue;
        adevs[i] = a;
        a->index = i;
        for (j = 0; j < MX_COUNT; j++) a->vol[j] = 0xC000C000UL;
        a->vol[MX_MASTER] = 0xFFFFFFFFUL;
        a->vol[MX_WAVE] = 0xFFFFFFFFUL;
        a->vol[MX_SYNTH] = 0xFFFFFFFFUL;
        a->vol[MX_LINE] = 0;                    /* line-in pass-through off */
        if (a->caps & WINAC_OUT) a->caps |= WINAC_SYNTH;
        if (a->ops->set_vol)
            for (j = 0; j < MX_COUNT; j++)
                if (j != MX_MASTER || a->hwvol) a->ops->set_vol(a, j, a->vol[j]);
        kprintf("audio %d: %s%s%s\n", i, a->name, (a->caps & WINAC_OUT) ? ", playback" : "",
                (a->caps & WINAC_IN) ? ", recording" : "");
        return i;
    }
    return -1;
}

void adev_unregister(struct adev *a)
{
    int i;
    for (i = 0; i < MAXSTREAM; i++)
        if (streams[i].used && streams[i].dev == a->index) {
            struct winring *r = ringp(&streams[i]);
            if (r) r->flags |= RF_GONE;
            streams[i].running = 0;
        }
    a->gone = 1;
    if (synth_dev == a->index) synth_dev = -1;
}

struct adev *adev_get(int i)
{
    return (i >= 0 && i < MAXADEV) ? adevs[i] : NULL;
}

int adev_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAXADEV; i++) if (adevs[i]) n++;
    return n;
}

/* ---------------------------------------------------------------- DMA coherency */

void aeng_flush_dma(void *p, u32 n)
{
    u8 *a = (u8 *)((u32)p & ~31UL), *e = (u8 *)p + n;
    if (!flush_mode) flush_mode = (cpu_features() & (1UL << 19)) ? 1 : 2;
    if (flush_mode == 2) {
        cpu_wbinvd();
        return;
    }
    for (; a < e; a += 32) cpu_clflush(a);
}

/* ---------------------------------------------------------------- playback */

/* reads up to n frames from the ring without taking them */
static int peek(struct astream *s, struct winring *r, s16 *dst, int n)
{
    u32 avail = (r->wr - r->rd) / s->fsz, off, i, bs = s->bits / 8;
    u8 *d = (u8 *)r + 32;
    if ((u32)n > avail) n = (int)avail;
    off = r->rd % r->size;
    for (i = 0; i < (u32)n; i++) {
        u8 *f = d + off;
        s16 l = dec1(f, s->bits, s->flt);
        dst[i * 2] = l;
        dst[i * 2 + 1] = s->ch > 1 ? dec1(f + bs, s->bits, s->flt) : l;
        off += s->fsz;
        if (off >= r->size) off = 0;
    }
    return n;
}

static void render(struct adev *a, struct astream *s, struct winring *r, int frames, u32 fill)
{
    u32 wl = a->vol[MX_WAVE] & 0xFFFF, wr = a->vol[MX_WAVE] >> 16;
    s32 gl = (s32)(((u64)s->vl * (wl + 1)) >> 16), gr = (s32)(((u64)s->vr * (wr + 1)) >> 16);
    int got = 0;
    while (got < frames) {
        int want = MIN(frames - got, CHUNK * 4);
        int need = s->rs.pass ? want : (int)(((u64)want * s->rs.step) >> 16) + 2;
        int nin, used, prod, i;
        if (need > MAXIN) need = MAXIN;
        nin = peek(s, r, tin, need);
        prod = rs_run(&s->rs, tin, nin, tout, want, &used);
        r->rd += (u32)used * s->fsz;
        for (i = 0; i < prod; i++) {
            acc[(got + i) * 2] += (tout[i * 2] * gl) >> 16;
            acc[(got + i) * 2 + 1] += (tout[i * 2 + 1] * gr) >> 16;
        }
        got += prod;
        if (prod) s->lastfill = fill + (u32)got * 4;
        if (!prod && !used) {
            if (r->wr == r->rd && s->lastfill) r->flags |= RF_UNDERRUN;
            break;
        }
    }
}

static void mix(struct adev *a, s16 *dst, int frames, u32 fill)
{
    int i, live = 0;
    u32 ml = a->vol[MX_MASTER] & 0xFFFF, mr = a->vol[MX_MASTER] >> 16;
    memset(acc, 0, (u32)frames * 8);
    for (i = 0; i < MAXSTREAM; i++) {
        struct astream *s = &streams[i];
        struct winring *r;
        if (!s->used || !s->running || s->dir || s->dev != a->index) continue;
        if (!(r = ringp(s))) continue;
        render(a, s, r, frames, fill);
        live++;
    }
    if (a->index == synth_dev) synth_render(acc, frames, a->out.rate, a->vol[MX_SYNTH]);
    if (a->hwvol) ml = mr = 0xFFFF;
    for (i = 0; i < frames; i++) {
        dst[i * 2] = clip16((s32)(((s64)acc[i * 2] * (ml + 1)) >> 16));
        dst[i * 2 + 1] = clip16((s32)(((s64)acc[i * 2 + 1] * (mr + 1)) >> 16));
    }
}

/* playback position of each stream: what was taken minus what still waits in the device */
static void out_positions(struct adev *a)
{
    struct ahw *h = &a->out;
    int i;
    for (i = 0; i < MAXSTREAM; i++) {
        struct astream *s = &streams[i];
        struct winring *r;
        s32 ahead;
        u32 back, pos;
        if (!s->used || s->dir || s->dev != a->index || !(r = ringp(s))) continue;
        ahead = (s32)(s->lastfill - h->hwtotal) / 4;
        if (ahead < 0) ahead = 0;
        back = (u32)(((u64)(u32)ahead * s->rs.step) >> 16) * s->fsz;
        pos = r->rd - (back < r->rd ? back : r->rd);
        if ((s32)(pos - r->pos) > 0) r->pos = pos;
        if (!ahead && r->rd == r->wr) r->pos = r->rd;
    }
}

static u32 latency_bytes(struct ahw *h)
{
    /* the VxD services every 10 ms, the DOS core every timer tick (55 ms, and polls can
       be skipped while DOS is busy) */
    u32 ms = plat_mode == PM_VXD ? 70 : 400;
    u32 b = (h->rate / 1000 * ms) * 4;
    if (b > h->size - h->period) b = h->size - h->period;
    return b & ~3UL;
}

/* output frames the running streams can supply now (0xFFFFFFFF: no limit) */
static u32 stream_limit(struct adev *a)
{
    u32 lim = 0xFFFFFFFFUL;
    int i;
    for (i = 0; i < MAXSTREAM; i++) {
        struct astream *s = &streams[i];
        struct winring *r;
        u32 in, outf;
        if (!s->used || !s->running || s->dir || s->dev != a->index || !(r = ringp(s))) continue;
        in = (r->wr - r->rd) / s->fsz;
        outf = s->rs.pass ? in : (u32)(((u64)in << 16) / s->rs.step);
        outf = outf > 2 ? outf - 2 : 0;
        if (outf < lim) lim = outf;
    }
    return lim;
}

static void out_service(struct adev *a)
{
    struct ahw *h = &a->out;
    u32 pos, d, target, lim, floor;
    if (!h->running) return;
    pos = a->ops->pos(a, 0);
    if (pos >= h->size) pos = 0;
    d = (pos + h->size - h->lastpos) % h->size;
    h->lastpos = pos;
    h->hwtotal += d;
    if ((s32)(h->fill - h->hwtotal) < 0) {                          /* we were late */
        if (!h->late++ || !(h->late & 63))
            kprintf("audio %d: playback ran dry %lu time(s) (%lu bytes)\n", a->index, h->late, h->hwtotal - h->fill);
        h->fill = h->hwtotal;
    }
    /* as far ahead as the streams have data, up to the latency; never less than the safety
       margin (then a starving stream gets a gap of silence instead of stale sound) */
    target = h->hwtotal + latency_bytes(h);
    floor = h->hwtotal + latency_bytes(h) / 3;
    lim = stream_limit(a);
    if (lim != 0xFFFFFFFFUL && (s32)(target - (h->fill + lim * 4)) > 0) {
        target = h->fill + lim * 4;
        if ((s32)(floor - target) > 0) target = floor;
    }
    while ((s32)(target - h->fill) > 0) {
        u32 off = h->fill % h->size, n = target - h->fill;
        if (n > h->size - off) n = h->size - off;
        if (n > CHUNK * 4) n = CHUNK * 4;
        n &= ~3UL;
        if (!n) break;
        mix(a, (s16 *)(h->buf + off), (int)(n / 4), h->fill);
        if (!h->noflush) aeng_flush_dma(h->buf + off, n);
        h->fill += n;
    }
    out_positions(a);
}

/* ---------------------------------------------------------------- capture */

static void capture(struct astream *s, struct winring *r, const s16 *in, int n)
{
    u32 bs = s->bits / 8;
    while (n > 0) {
        int used, prod, i;
        prod = rs_run(&s->rs, in, n, tout, CHUNK * 4, &used);
        for (i = 0; i < prod; i++) {
            u8 *d;
            if (r->wr - r->rd + s->fsz > r->size) {
                r->flags |= RF_OVERRUN;
                break;
            }
            d = (u8 *)r + 32 + (r->wr % r->size);
            if (s->ch == 1) {
                enc1(d, (s16)(((s32)tout[i * 2] + tout[i * 2 + 1]) / 2), s->bits, s->flt);
            } else {
                u32 c;
                enc1(d, tout[i * 2], s->bits, s->flt);
                enc1(d + bs, tout[i * 2 + 1], s->bits, s->flt);
                for (c = 2; c < s->ch; c++) enc1(d + c * bs, 0, s->bits, s->flt);
            }
            r->wr += s->fsz;
            r->pos = r->wr;
        }
        if (!used && !prod) break;
        in += used * 2;
        n -= used;
    }
}

static void in_service(struct adev *a)
{
    struct ahw *h = &a->in;
    u32 pos, d;
    int i;
    if (!h->running) return;
    pos = a->ops->pos(a, 1);
    if (pos >= h->size) pos = 0;
    d = (pos + h->size - h->lastpos) % h->size;
    h->lastpos = pos;
    while (d) {
        u32 off = h->fill % h->size, n = d;
        if (n > h->size - off) n = h->size - off;
        if (!h->noflush) aeng_flush_dma(h->buf + off, n);      /* drop stale cache lines */
        for (i = 0; i < MAXSTREAM; i++) {
            struct astream *s = &streams[i];
            struct winring *r;
            if (!s->used || !s->running || !s->dir || s->dev != a->index || !(r = ringp(s))) continue;
            capture(s, r, (const s16 *)(h->buf + off), (int)(n / 4));
        }
        h->fill += n;
        d -= n;
    }
}

/* ---------------------------------------------------------------- service */

static int dev_busy(struct adev *a, int dir)
{
    int i;
    for (i = 0; i < MAXSTREAM; i++)
        if (streams[i].used && streams[i].dev == a->index && streams[i].dir == dir) return 1;
    return 0;
}

static void hw_start(struct adev *a, int dir)
{
    struct ahw *h = dir ? &a->in : &a->out;
    int i;
    if (h->running || a->gone) return;
    memset(h->buf, 0, h->size);
    if (!h->noflush) aeng_flush_dma(h->buf, h->size);
    h->fill = h->hwtotal = h->lastpos = 0;
    if (!dir) h->fill = h->period;  /* playback starts on a period of silence */
    for (i = 0; i < MAXSTREAM; i++)
        if (streams[i].dev == a->index) streams[i].lastfill = 0;
    if (a->ops->start(a, dir) == 0) h->running = 1;
}

static void hw_stop(struct adev *a, int dir)
{
    struct ahw *h = dir ? &a->in : &a->out;
    if (!h->running) return;
    a->ops->stop(a, dir);
    h->running = 0;
}

/* a direction nobody used for 2 s is stopped (the last sound has been heard) */
static void idle_stop(struct adev *a, int dir)
{
    struct ahw *h = dir ? &a->in : &a->out;
    int busy;
    if (!h->running) return;
    busy = dev_busy(a, dir) || (!dir && a->index == synth_dev && synth_busy());
    if (busy) {
        h->idle = 0;
        return;
    }
    if (!h->idle) h->idle = plat_ms() | 1;
    else if (plat_ms() - h->idle > 2000) hw_stop(a, dir);
}

void aeng_poll(void)
{
    int i;
    for (i = 0; i < MAXADEV; i++) {
        struct adev *a = adevs[i];
        if (!a || a->gone) continue;
        if (a->ops->poll) a->ops->poll(a);
        if (a->index == synth_dev && synth_busy() && !a->out.running) hw_start(a, 0);
        out_service(a);
        in_service(a);
        idle_stop(a, 0);
        idle_stop(a, 1);
    }
}

void aeng_init(void)
{
    memset(streams, 0, sizeof(streams));
    synth_init();
}

void aeng_shutdown(void)
{
    int i;
    for (i = 0; i < MAXADEV; i++) {
        struct adev *a = adevs[i];
        if (!a) continue;
        hw_stop(a, 0);
        hw_stop(a, 1);
        if (a->ops->shutdown) a->ops->shutdown(a);
        adevs[i] = NULL;
    }
    memset(streams, 0, sizeof(streams));
}

void aeng_remove(struct adev *a)
{
    int i;
    for (i = 0; i < MAXADEV; i++)
        if (adevs[i] == a) {
            hw_stop(a, 0);
            hw_stop(a, 1);
            if (a->ops->shutdown) a->ops->shutdown(a);
            adev_unregister(a);
            kprintf("audio %d: %s removed\n", i, a->name);
            return;
        }
}

/* ---------------------------------------------------------------- API */

static int api_open(struct winapireq *rq)
{
    struct adev *a = adev_get((int)(rq->arg0 & 0xFF));
    u32 dir = (rq->arg0 >> 8) & 1, bits = rq->arg2 & 0xFF, ch = (rq->arg2 >> 8) & 0xFF;
    u32 flt = (rq->arg2 >> 16) & 1, rate = rq->arg1;
    struct astream *s = NULL;
    struct winring *r;
    int i;
    if (!a || a->gone) return WINE_NODEV;
    if (!(a->caps & (dir ? WINAC_IN : WINAC_OUT))) return WINE_NODEV;
    if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) || (flt && bits != 32)) return WINE_BADFN;
    if (!ch || ch > 8 || rate < 2000 || rate > 384000) return WINE_BADFN;
    for (i = 0; i < MAXSTREAM; i++)
        if (!streams[i].used) {
            s = &streams[i];
            break;
        }
    if (!s) return WINE_BUSY;
    memset(s, 0, sizeof(*s));
    s->ring = rq->buf;
    s->dir = (u8)dir;
    s->dev = (u8)a->index;
    s->rate = rate;
    s->bits = bits;
    s->ch = ch;
    s->flt = flt;
    s->fsz = bits / 8 * ch;
    r = ringp(s);
    if (!r || r->size % s->fsz) return WINE_BADFN;
    s->vl = s->vr = 0x10000UL;
    if (dir) rs_init(&s->rs, a->in.rate, rate);
    else rs_init(&s->rs, rate, a->out.rate);
    s->used = 1;
    hw_start(a, (int)dir);
    rq->retlen = (u32)(s - streams) + 1;
    kprintf("audio %d: %s stream %u: %lu Hz, %lu bit%s, %lu ch\n", a->index, dir ? "record" : "play",
            rq->retlen, rate, bits, flt ? " float" : "", ch);
    return WINE_OK;
}

static int api_ctl(struct winapireq *rq)
{
    struct astream *s;
    struct adev *a;
    struct winring *r;
    u32 n = rq->arg0;
    if (!n || n > MAXSTREAM || !streams[n - 1].used) return WINE_BADFN;
    s = &streams[n - 1];
    a = adev_get(s->dev);
    r = ringp(s);
    switch (rq->arg1) {
    case AC_START:
        if (a && !a->gone) hw_start(a, s->dir);
        s->running = 1;
        break;
    case AC_PAUSE:
        s->running = 0;
        break;
    case AC_RESET:
        s->running = 0;
        if (r) {
            if (s->dir) r->wr = r->rd;
            else r->rd = r->wr;
            r->pos = s->dir ? r->wr : r->rd;
            r->flags &= ~(RF_UNDERRUN | RF_OVERRUN);
        }
        if (a) {
            if (s->dir) rs_init(&s->rs, a->in.rate, s->rate);
            else rs_init(&s->rs, s->rate, a->out.rate);
        }
        break;
    case AC_CLOSE:
        s->used = 0;
        s->running = 0;
        break;
    case AC_VOLUME:
        s->vl = (rq->arg2 & 0xFFFF) + 1;
        s->vr = (rq->arg2 >> 16) + 1;
        break;
    default:
        return WINE_BADFN;
    }
    return WINE_OK;
}

static int api_mix(struct winapireq *rq)
{
    struct adev *a = adev_get((int)(rq->arg0 & 0xFF));
    u32 ctl = rq->arg1 & 0xFFFF, set = rq->arg1 & 0x80000000UL;
    if (!a) return WINE_NODEV;
    if (ctl == MX_SRC) {
        if (set && (rq->arg2 & a->insrcs)) {
            a->insrc = (u8)rq->arg2;
            if (a->ops->set_src) a->ops->set_src(a, (int)rq->arg2);
        }
        rq->retlen = a->insrc;
        return WINE_OK;
    }
    if (ctl >= MX_COUNT) return WINE_BADFN;
    if (set) {
        a->vol[ctl] = rq->arg2;
        if (a->ops->set_vol && !a->gone) a->ops->set_vol(a, (int)ctl, rq->arg2);
    }
    rq->retlen = a->vol[ctl];
    return WINE_OK;
}

int aeng_api(struct winapireq *rq, u8 *buf)
{
    int i, n = 0;
    switch (rq->fn) {
    case WINF_AUD_DEVS:
    {
        struct winauddev *d = (struct winauddev *)buf;
        for (i = 0; i < MAXADEV; i++) {
            struct adev *a = adevs[i];
            if (!a) continue;
            if ((u32)(n + 1) * sizeof(*d) > rq->buflen) break;
            memset(d, 0, sizeof(*d));
            d->id = (u8)i;
            d->type = a->type;
            d->caps = a->caps;
            d->insrcs = a->insrcs;
            d->insrc = a->insrc;
            d->rate = a->out.rate ? a->out.rate : a->in.rate;
            d->vid = a->vid;
            d->pid = a->pid;
            d->hwvol = a->hwvol;
            d->gone = a->gone;
            strlcpy(d->name, a->name, sizeof(d->name));
            d++;
            n++;
        }
        rq->retlen = (u32)n * sizeof(*d);
        return WINE_OK;
    }
    case WINF_AUD_OPEN:
        return api_open(rq);
    case WINF_AUD_CTL:
        return api_ctl(rq);
    case WINF_AUD_MIX:
        return api_mix(rq);
    case WINF_AUD_MIDI:
    {
        int d = (int)(rq->arg0 & 0xFF);
        struct adev *a;
        u8 m[3];
        const u8 *p = buf;
        int len = (int)rq->buflen;
        if (!buf || !len) {
            m[0] = (u8)rq->arg1;
            m[1] = (u8)(rq->arg1 >> 8);
            m[2] = (u8)(rq->arg1 >> 16);
            p = m;
            len = 3;
            if (m[0] < 0x80 || (m[0] & 0xF0) == 0xC0 || (m[0] & 0xF0) == 0xD0) len = 2;
        }
        if (d == 0xFF) {                        /* the synthesizer on the default device */
            for (d = 0; d < MAXADEV; d++)
                if (adevs[d] && !adevs[d]->gone && (adevs[d]->caps & WINAC_SYNTH)) break;
            if (d == MAXADEV) return WINE_NODEV;
        }
        a = adev_get(d);
        if (!a || a->gone) return WINE_NODEV;
        if (a->midiout && a->ops->midi) {
            a->ops->midi(a, p, len);
            return WINE_OK;
        }
        if (!(a->caps & WINAC_SYNTH)) return WINE_NODEV;
        if (synth_dev != d) {
            if (synth_dev >= 0) synth_reset();
            synth_dev = d;
        }
        synth_midi(p, len);
        if (!a->out.running) hw_start(a, 0);
        return WINE_OK;
    }
    }
    return WINE_BADFN;
}
