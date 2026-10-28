/* aeng.c - sound engine: streams in shared rings, mixing, sample rate conversion, capture.
   Shared by WINAUDIO (HDA, AC'97) and WINUSB (USB Audio). No floating point: the core runs
   in a VxD, where the FPU belongs to the virtual machines. */
#include "audio.h"
#include "srctab.h"
#ifdef WIN_AUDIO
#include "sbemu.h"
#define SB_ON() sb_active()
#else
#define SB_ON() 0
#endif

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
    struct winring *direct;         /* internal stream (Sound Blaster emulation): its ring */
    u8 rt;                          /* produced in real time: never limits how far ahead we mix */
    u8 volctl;                      /* MX_* that scales it */
};

static struct adev *adevs[MAXADEV];
static struct astream streams[MAXSTREAM];
static s32 acc[CHUNK * 2];
static s16 tin[MAXIN * 2];
static s16 tout[CHUNK * 4 * 2];
static int flush_mode;              /* 0 unknown, 1 clflush, 2 wbinvd */
static int synth_dev = -1;          /* device the synthesizer plays on */
static int rt_dev = -1;             /* device of the internal streams */
static u8 lowlat;                   /* DOS: serviced often (RTC), short latency */

/* ---------------------------------------------------------------- helpers */

static s16 hard16(s32 v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

/* soft limit above -1.2 dB: loud mixes of several streams round off instead of clipping */
static s16 clip16(s32 v)
{
    if (v > 28672) {
        if (v > 2000000L) return 32767;
        return (s16)(32767 - (s32)(16769025L / (v - 28672 + 4095)));
    }
    if (v < -28672) {
        if (v < -2000000L) return -32767;
        return (s16)-(32767 - (s32)(16769025L / (-v - 28672 + 4095)));
    }
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
    struct winring *r = s->direct ? s->direct : (struct winring *)plat_sysrm(s->ring);
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
        out[p * 2] = hard16(l >> 15);
        out[p * 2 + 1] = hard16(rr >> 15);
        p++;
        r->frac += r->step;
    }
done:
    *used = u;
    return p;
}

/* Windows volumes are linear in amplitude (half way: -6 dB); hardware amplifiers count in dB
   steps. The attenuation in 1/100 dB: 20 * log10(65535 / v), 0xFFFFFFFF for silence. */
u32 aeng_att_cdb(u32 v)
{
    u32 x, l2 = 0, bit;
    if (!v) return 0xFFFFFFFFUL;
    if (v >= 65535) return 0;
    x = (65535UL << 15) / v;                    /* Q15, >= 1.0 */
    while (x >= 65536UL) {
        x >>= 1;
        l2 += 65536UL;
    }
    for (bit = 32768UL; bit; bit >>= 1) {
        x = (x * x) >> 15;
        if (x >= 65536UL) {
            x >>= 1;
            l2 |= bit;
        }
    }
    return (u32)(((u64)l2 * 60206UL) >> 16) / 100;     /* 6.0206 dB per octave */
}

/* ---------------------------------------------------------------- devices */

/* MX_* value for the hardware: the master is 0 while muted */
static u32 hwval(struct adev *a, int ctl)
{
    return ctl == MX_MASTER && a->mute ? 0 : a->vol[ctl];
}

int adev_register(struct adev *a)
{
    int i, j;
    for (i = 0; i < MAXADEV; i++)
        if (adevs[i] == a) {            /* plugged in again: same number, same settings */
            a->gone = 0;
            a->out.running = a->in.running = 0;
            if (a->ops->set_vol)
                for (j = 0; j < MX_COUNT; j++)
                    if (j != MX_MASTER || a->hwvol) a->ops->set_vol(a, j, hwval(a, j));
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
                if (j != MX_MASTER || a->hwvol) a->ops->set_vol(a, j, hwval(a, j));
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
    u32 wl = a->vol[s->volctl] & 0xFFFF, wr = a->vol[s->volctl] >> 16;
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
            if (r->wr == r->rd && s->lastfill && !s->rt) r->flags |= RF_UNDERRUN;
            break;
        }
    }
}

static void mix(struct adev *a, s16 *dst, int frames, u32 fill)
{
    int i, live = 0;
    u32 ml = hwval(a, MX_MASTER) & 0xFFFF, mr = hwval(a, MX_MASTER) >> 16;
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
    u32 ms = h->latms ? h->latms : plat_mode == PM_VXD ? 70 : lowlat ? 40 : 400;
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
        if (!s->used || !s->running || s->dir || s->dev != a->index || s->rt || !(r = ringp(s))) continue;
        in = (r->wr - r->rd) / s->fsz;
        outf = s->rs.pass ? in : (u32)(((u64)in << 16) / s->rs.step);
        outf = outf > 2 ? outf - 2 : 0;
        if (outf < lim) lim = outf;
    }
    return lim;
}

/* Silence after the written data: when the service comes late (DOS busy with the disk,
   interrupts off for long) the device plays silence there, not the sound of one ring turn
   ago (which is heard as a loop) */
static void zero_ahead(struct ahw *h)
{
    u32 guard = h->size / 2, end, from;
    if ((s32)(h->zeroed - h->fill) < 0) h->zeroed = h->fill;
    if (h->fill - h->hwtotal + guard + h->period > h->size) guard = h->size - (h->fill - h->hwtotal) - h->period;
    if ((s32)guard <= 0) return;
    end = h->fill + guard;
    from = h->zeroed;
    while ((s32)(end - from) > 0) {
        u32 off = from % h->size, n = end - from;
        if (n > h->size - off) n = h->size - off;
        memset(h->buf + off, 0, n);
        if (!h->noflush) aeng_flush_dma(h->buf + off, n);
        from += n;
    }
    h->zeroed = end;
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
        /* resume a little ahead: the controller has already fetched the bytes at its position */
        h->fill = h->hwtotal + ((h->rate / 1000 * 4) * 4 & ~3UL);
    }
#ifdef WIN_AUDIO
    if (a->index == rt_dev && sb_active()) sb_produce(d / 4, h->rate);
#endif
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
    zero_ahead(h);
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
    /* playback starts on silence that outlasts the first service (and the controller's prefetch) */
    if (!dir) {
        u32 lead = latency_bytes(h);
        h->fill = h->zeroed = lead > h->period ? lead : h->period;
    }
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
    busy = dev_busy(a, dir) || (!dir && a->index == synth_dev && synth_busy()) ||
           (!dir && a->index == rt_dev && SB_ON());
    if (busy) {
        h->idle = 0;
        return;
    }
    if (!h->idle) h->idle = plat_ms() | 1;
    else if (plat_ms() - h->idle > 2000) hw_stop(a, dir);
}

#ifdef WIN_AUDIO
static void br_poll(void);
#else
static void brc_poll(void);
static void brc_close(void);
#endif

void aeng_poll(void)
{
    int i;
#ifdef WIN_AUDIO
    br_poll();
#else
    brc_poll();
#endif
    for (i = 0; i < MAXADEV; i++) {
        struct adev *a = adevs[i];
        if (!a || a->gone) continue;
        if (a->ops->poll) a->ops->poll(a);
        if (a->index == synth_dev && synth_busy() && !a->out.running) hw_start(a, 0);
        if (a->index == rt_dev && SB_ON() && !a->out.running) hw_start(a, 0);
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
#ifdef WIN_AUDIO
    sb_stop();
#else
    brc_close();
    ((struct winbridge *)((u8 *)cb + WIN_BROFF))->state = 0;
    ((struct winbridge *)((u8 *)cb + WIN_BROFF))->outs = 0;
#endif
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

/* ---------------------------------------------------------------- internal streams */

/* the device the synthesizer and the emulated Sound Blaster play on */
static int default_dev(void)
{
    int d, pass;
    if (synth_dev >= 0 && adevs[synth_dev] && !adevs[synth_dev]->gone) return synth_dev;
    /* a sound device of this driver first; the bridge to WINUSB's USB Audio when there is
       none, or first with /USBSB */
    for (pass = 0; pass < 2; pass++)
        for (d = 0; d < MAXADEV; d++)
            if (adevs[d] && !adevs[d]->gone && (adevs[d]->caps & WINAC_SYNTH) &&
                (pass || adevs[d]->hidden == ((cb->opts2 & OPT2_USBSB) != 0)))
                return d;
    return -1;
}

void aeng_midi(const u8 *m, int len)
{
    int d = default_dev();
    struct adev *a;
    if (d < 0) return;
    a = adevs[d];
    if (synth_dev != d) {
        if (synth_dev >= 0) synth_reset();
        synth_dev = d;
    }
    synth_midi(m, len);
    if (!a->out.running) hw_start(a, 0);
}

void aeng_lowlat(int on)
{
    lowlat = (u8)on;
}

/* a 16-bit stereo stream in a ring of the core, produced in real time */
int aeng_rt_open(struct winring *r, u32 rate, int volctl)
{
    int i, d = default_dev();
    struct astream *s;
    if (d < 0) return -1;
    for (i = 0; i < MAXSTREAM; i++)
        if (!streams[i].used) break;
    if (i == MAXSTREAM) return -1;
    s = &streams[i];
    memset(s, 0, sizeof(*s));
    s->direct = r;
    s->rt = 1;
    s->dev = (u8)d;
    s->rate = rate;
    s->bits = 16;
    s->ch = 2;
    s->fsz = 4;
    s->vl = s->vr = 0x10000UL;
    s->volctl = (u8)volctl;
    rs_init(&s->rs, rate, adevs[d]->out.rate);
    s->used = 1;
    rt_dev = d;
    return i;
}

static struct astream *rt_get(int id)
{
    if (id < 0 || id >= MAXSTREAM || !streams[id].used || !streams[id].direct) return NULL;
    return &streams[id];
}

/* running: silence first, as much as the device plays before the producer has had its
   first period, plus a cushion against the jitter of the service times */
void aeng_rt_run(int id, int run)
{
    struct astream *s = rt_get(id);
    struct adev *a;
    struct winring *r;
    u32 lead, want, n, cushion;
    if (!s) return;
    a = adev_get(s->dev);
    s->running = (u8)run;
    if (!run || !a || a->gone) return;
    if (!a->out.running) hw_start(a, 0);
    r = s->direct;
    lead = a->out.fill - a->out.hwtotal;
    want = latency_bytes(&a->out);
    cushion = plat_mode == PM_VXD ? 25 : lowlat ? 15 : 120;
    n = want > lead ? (want - lead) / 4 : 0;
    n = (u32)(((u64)n * s->rate) / a->out.rate) + s->rate * cushion / 1000;
    if (n * 4 > r->size / 2) n = r->size / 8;
    memset((u8 *)r + 32, 0, r->size);
    r->rd = 0;
    r->wr = r->pos = n * 4;
    rs_init(&s->rs, s->rate, a->out.rate);
}

void aeng_rt_rate(int id, u32 rate)
{
    struct astream *s = rt_get(id);
    struct adev *a;
    if (!s || s->rate == rate) return;
    a = adev_get(s->dev);
    s->rate = rate;
    if (a) rs_init(&s->rs, rate, a->out.rate);
}

void aeng_rt_vol(int id, u32 vl, u32 vr)
{
    struct astream *s = rt_get(id);
    if (!s) return;
    s->vl = vl;
    s->vr = vr;
}

void aeng_rt_close(int id)
{
    struct astream *s = rt_get(id);
    if (s) memset(s, 0, sizeof(*s));
}

/* DOS: the Sound Blaster emulation between timer polls (RTC) */
void aeng_tick(void)
{
    struct adev *a = adev_get(rt_dev);
    if (!a || a->gone || !SB_ON()) return;
    if (!a->out.running) hw_start(a, 0);
    out_service(a);
}

/* ---------------------------------------------------------------- sound bridge */

/* WINAUDIO and WINUSB (struct winbridge in winapi.h): WINAUDIO's bridge device mixes the
   Sound Blaster emulation and the synthesizer into a ring of its own memory, WINUSB plays
   that ring on its first USB Audio output as a stream. The bridge device's position is the
   consumer's read position while WINUSB serves the ring, else it follows the clock (the
   emulated card keeps its pace with nobody listening). */
#define BR_SIZE     65536UL
#define BR_RATE     48000UL

static struct winbridge *br_own(void)
{
    return (struct winbridge *)((u8 *)cb + WIN_BROFF);
}

static int br_peer_ok(struct winbridge *p)
{
    return p && p->magic == WINBR_MAGIC;
}

#ifdef WIN_AUDIO
static struct adev br_dev;
static struct winring *br_ring;
static u32 br_vtot, br_us, br_frac, br_beat, br_beatms, br_probems;
static s32 br_off;
static u8 br_lock;
static u8 br_up;

static int br_start(struct adev *a, int dir)
{
    struct winbridge *o = br_own();
    (void)a;
    if (dir) return -1;
    o->state = 0;
    br_ring->rd = br_ring->wr = br_ring->pos = 0;
    br_ring->flags = 0;
    br_vtot = br_frac = 0;
    br_lock = 0;
    br_us = plat_us();
    o->seq++;
    o->state = 1;
    return 0;
}

static void br_stop(struct adev *a, int dir)
{
    (void)a;
    if (!dir) br_own()->state = 0;
}

static u32 br_pos(struct adev *a, int dir)
{
    struct winbridge *p = plat_bridge_peer();
    u32 now = plat_ms(), us = plat_us(), rd = br_ring->rd, t, add, old;
    int alive = 0;
    (void)dir;
    br_ring->wr = a->out.fill;              /* what has been mixed so far */
    if (br_peer_ok(p)) {
        if (p->beat != br_beat) {
            br_beat = p->beat;
            br_beatms = now;
        }
        alive = p->state && now - br_beatms < 250;
    }
    /* the clock moves the position smoothly (WINUSB takes the data in bursts, and the
       emulated card's interrupts must not come in bursts); it follows WINUSB's reading */
    t = us - br_us;
    if (t > 1000000UL) t = 1000000UL;
    t = t * (BR_RATE / 1000) * 4 + br_frac;
    br_us = us;
    add = (t / 1000) & ~3UL;
    old = br_vtot;
    br_vtot += add;
    br_frac = t - add * 1000;
    /* drift: the distance to what the USB device has played (the stream position WINUSB
       keeps in the ring) stays as it was when the device began, within 2 % of the rate */
    if (alive && br_ring->pos) {
        s32 e = (s32)(br_ring->pos - br_vtot);
        if (!br_lock) {
            br_off = e;
            br_lock = 1;
        } else {
            s32 c = (e - br_off) / 64, lim = (s32)(add / 50) + 4;
            if (c > lim) c = lim;
            if (c < -lim) c = -lim;
            br_vtot += (u32)c & ~3UL;
        }
    }
    /* never past what WINUSB has read: the mixing ahead would overwrite unread data */
    if (alive && (s32)(br_vtot - rd) > 0) br_vtot = (s32)(old - rd) > 0 ? old : rd;
    return br_vtot % BR_SIZE;
}

static void br_shutdown(struct adev *a)
{
    (void)a;
    br_own()->state = 0;
    br_up = 0;
}

static struct adevops br_ops = { br_start, br_stop, br_pos, NULL, NULL, NULL, br_shutdown, NULL };

/* registers the bridge device once WINUSB has a USB Audio output */
void aeng_bridge_probe(void)
{
    struct winbridge *o = br_own(), *p;
    if (br_up) return;
    o->magic = WINBR_MAGIC;
    plat_bridge_publish(o);
    p = plat_bridge_peer();
    if (!br_peer_ok(p) || !p->outs) return;
    if (!br_ring && !(br_ring = kmalloc_align(BR_SIZE + 32, 4096))) return;
    memset(br_ring, 0, BR_SIZE + 32);
    br_ring->magic = WINRING_MAGIC;
    br_ring->size = BR_SIZE;
    o->state = 0;
    o->phys = vphys(br_ring);
    o->lin = (u32)br_ring;
    o->size = BR_SIZE;
    o->rate = BR_RATE;
    memset(&br_dev, 0, sizeof(br_dev));
    br_dev.ops = &br_ops;
    br_dev.type = AT_USB;
    br_dev.caps = WINAC_OUT;
    br_dev.hidden = 1;
    br_dev.irq = 0xFF;
    br_dev.loc = 0xFFFF;
    strlcpy(br_dev.name, "USB Audio of WINUSB (sound bridge)", sizeof(br_dev.name));
    br_dev.out.buf = (u8 *)br_ring + 32;
    br_dev.out.size = BR_SIZE;
    br_dev.out.period = 4096;
    br_dev.out.rate = BR_RATE;
    br_dev.out.present = 1;
    br_dev.out.noflush = 1;
    /* ahead of what WINUSB schedules on the USB device (DOS 150 ms, VxD tens of ms) */
    br_dev.out.latms = plat_mode == PM_VXD ? 150 : 300;
    if (adev_register(&br_dev) < 0) return;
    br_up = 1;
}

/* why the bridge is not there (DOS start message), NULL when it is */
const char *aeng_bridge_why(void)
{
    struct winbridge *p;
    if (br_up) return NULL;
    p = plat_bridge_peer();
    if (!br_peer_ok(p)) return "WINUSB.SYS is not loaded before WINAUDIO.SYS";
    if (!(((struct wincb *)((u8 *)p - WIN_BROFF))->opts & OPT_USB)) return "WINUSB.SYS runs without /USB (USB in DOS)";
    if (!p->outs) return "WINUSB has found no USB Audio output";
    return "not started";
}

static void br_poll(void)
{
    u32 now = plat_ms();
    if (br_up || now - br_probems < 1000) return;
    br_probems = now;
    aeng_bridge_probe();
}
#else
/* WINUSB: plays the ring of WINAUDIO's bridge device */
static int brc_sid = -1;
static u16 brc_seq;
static u8 brc_dev;

static void brc_close(void)
{
    if (brc_sid >= 0) streams[brc_sid].used = 0;
    brc_sid = -1;
}

static void brc_poll(void)
{
    struct winbridge *o = br_own(), *p;
    struct winring *r;
    struct astream *s;
    int i, n = 0, d = -1;
    o->magic = WINBR_MAGIC;
    plat_bridge_publish(o);
    for (i = 0; i < MAXADEV; i++)
        if (adevs[i] && !adevs[i]->gone && (adevs[i]->caps & WINAC_OUT)) {
            n++;
            if (d < 0) d = i;
        }
    o->outs = (u8)n;
    p = plat_bridge_peer();
    if (!br_peer_ok(p) || p->state != 1 || d < 0) {
        brc_close();
        o->state = 0;
        return;
    }
    if (brc_sid >= 0 && (p->seq != brc_seq || brc_dev != d)) brc_close();
    if (brc_sid < 0) {
        r = plat_bridge_ring(p);
        if (!r || r->magic != WINRING_MAGIC || r->size != p->size) return;
        for (i = 0; i < MAXSTREAM && streams[i].used; i++) ;
        if (i == MAXSTREAM) return;
        s = &streams[i];
        memset(s, 0, sizeof(*s));
        s->direct = r;
        s->dev = (u8)d;
        s->rate = p->rate;
        s->bits = 16;
        s->ch = 2;
        s->fsz = 4;
        s->vl = s->vr = 0x10000UL;
        s->volctl = MX_WAVE;
        rs_init(&s->rs, p->rate, adevs[d]->out.rate);
        s->used = 1;
        s->running = 1;
        hw_start(adevs[d], 0);
        brc_sid = i;
        brc_seq = p->seq;
        brc_dev = (u8)d;
        kprintf("audio %d: plays the sound bridge of WINAUDIO\n", d);
    }
    o->state = 1;
    o->beat++;
}
#endif

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
    s->volctl = MX_WAVE;
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
    if (!n || n > MAXSTREAM || !streams[n - 1].used || streams[n - 1].direct) return WINE_BADFN;
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
    if (ctl == MX_MUTE) {
        if (set) {
            a->mute = rq->arg2 != 0;
            if (a->ops->set_vol && !a->gone) a->ops->set_vol(a, MX_MASTER, hwval(a, MX_MASTER));
        }
        rq->retlen = a->mute;
        return WINE_OK;
    }
    if (ctl >= MX_COUNT) return WINE_BADFN;
    if (set) {
        a->vol[ctl] = rq->arg2;
        if (a->ops->set_vol && !a->gone) a->ops->set_vol(a, (int)ctl, hwval(a, (int)ctl));
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
            if (!a || a->hidden) continue;
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
            if (m[0] < 0x80 || (m[0] & 0xF0) == 0xC0 || (m[0] & 0xF0) == 0xD0 || m[0] == 0xF1 || m[0] == 0xF3) len = 2;
            else if (m[0] >= 0xF4 && m[0] != 0xF2) len = 1;         /* system common and real time */
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
