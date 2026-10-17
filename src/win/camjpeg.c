/* camjpeg.c - baseline JPEG decoder for WINCAM: Motion-JPEG frames of USB cameras.
   Huffman coded 8-bit baseline / extended sequential, 1 or 3 components, any sampling
   factors up to 4, restart intervals; frames without Huffman tables (as cameras send them)
   use the standard tables of the JPEG specification (K.3). The image comes out one row at
   a time (BGR) so that no frame sized buffer is needed. Works in 16-bit code (int is 16
   bits, all wide arithmetic in long). */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "camjpeg.h"

typedef unsigned char u8;
typedef unsigned short u16;
#if UINT_MAX >= 0xFFFFFFFFUL
typedef unsigned int u32;       /* the bit buffer must be exactly 32 bits */
#else
typedef unsigned long u32;
#endif

#define MAXW        2048
#define FAST        9

struct huff {
    u16 fast[1 << FAST];        /* length << 8 | value, 0xFFFF: longer code */
    long maxcode[18];
    long delta[17];             /* value index minus first code of each length */
    u8 val[256];
    u8 used;
};

struct comp {
    int id, h, v, tq, td, ta;
    int bw, bh;                 /* blocks of the plane per MCU row */
    int dc;
    u8 *plane;                  /* one MCU row: bw * 8 by bh * 8 samples */
};

struct jdec {
    struct jin *in;
    u16 q[4][64];
    struct huff hd[4], ha[4];
    struct comp c[3];
    int nc, w, h, hmax, vmax, mcux, mcuy, ri;
    int ns, sc[3];              /* components of the scan */
    u32 bits;
    int nbits;
    int marker;                 /* marker found in the entropy data */
    int eof;
    short blk[64];
};

static const u8 dezig[64 + 16] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* run past the end on damaged data */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

const unsigned char jpg_std_dht[420] = {
    0xFF, 0xC4, 0x01, 0xA2, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00,
    0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51,
    0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52,
    0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF8, 0xF9, 0xFA, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01,
    0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07,
    0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33,
    0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19,
    0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85,
    0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
    0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8,
    0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF8, 0xF9, 0xFA
};

static void FAR *def_alloc(unsigned long n) { return malloc((size_t)n); }
static void def_free(void FAR *p) { free(p); }
void FAR *(*jpg_alloc)(unsigned long n) = def_alloc;
void (*jpg_free)(void FAR *p) = def_free;

/* ---------------------------------------------------------------- input */

static int getb(struct jin *in)
{
    if (!in->left && (in->pos >= in->size || !in->more(in) || !in->left)) return -1;
    in->left--;
    in->pos++;
    return *in->p++;
}

static int get16(struct jin *in)
{
    int a = getb(in), b = getb(in);
    if (a < 0 || b < 0) return -1;
    return (a << 8) | b;
}

static void skip(struct jin *in, unsigned n)
{
    while (n--) if (getb(in) < 0) return;
}

/* next marker: skips fill bytes and data */
static int next_marker(struct jin *in)
{
    int b;
    for (;;) {
        do {
            b = getb(in);
            if (b < 0) return -1;
        } while (b != 0xFF);
        do b = getb(in); while (b == 0xFF);
        if (b < 0) return -1;
        if (b) return b;
    }
}

/* ---------------------------------------------------------------- tables */

static int build_huff(struct huff *t, const u8 *counts, struct jin *in, const u8 *vals)
{
    int l, i, k, n = 0;
    long code = 0;
    for (l = 0; l < 16; l++) n += counts[l];
    if (n > 256) return -1;
    for (i = 0; i < n; i++) {
        int v = vals ? vals[i] : getb(in);
        if (v < 0) return -1;
        t->val[i] = (u8)v;
    }
    for (i = 0; i < (1 << FAST); i++) t->fast[i] = 0xFFFF;
    for (l = 1, k = 0; l <= 16; l++) {
        int cnt = counts[l - 1];
        t->delta[l] = k - code;
        for (i = 0; i < cnt; i++, k++, code++) {
            if (l <= FAST) {
                int s = (int)(code << (FAST - l)), m = 1 << (FAST - l), j;
                for (j = 0; j < m; j++) t->fast[s + j] = (u16)((l << 8) | t->val[k]);
            }
        }
        t->maxcode[l] = cnt ? code - 1 : -1;
        if (code > (1L << l)) return -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFFL;
    t->used = 1;
    return 0;
}

static int read_dht(struct jdec *j, struct jin *in, const u8 *src, int len)
{
    /* src: tables from memory (the standard ones), else from the input */
    int pos = 0;
    while (len > 0) {
        u8 counts[16];
        int tc, th, i, n = 0, r;
        struct huff *t;
        int b = src ? src[pos++] : getb(in);
        if (b < 0) return -1;
        tc = b >> 4;
        th = b & 15;
        if (tc > 1 || th > 3) return -1;
        for (i = 0; i < 16; i++) {
            int c = src ? src[pos++] : getb(in);
            if (c < 0) return -1;
            counts[i] = (u8)c;
            n += c;
        }
        t = tc ? &j->ha[th] : &j->hd[th];
        r = build_huff(t, counts, in, src ? src + pos : NULL);
        if (r) return -1;
        pos += n;
        len -= 17 + n;
    }
    return 0;
}

/* ---------------------------------------------------------------- entropy decoding */

static void fill(struct jdec *j)
{
    while (j->nbits <= 24) {
        int b = 0;
        if (!j->marker) {
            b = getb(j->in);
            if (b < 0) {
                j->eof = 1;
                b = 0;
            } else if (b == 0xFF) {
                int c = getb(j->in);
                while (c == 0xFF) c = getb(j->in);
                if (c) {
                    j->marker = c < 0 ? 0xD9 : c;
                    b = 0;
                }
            }
        }
        j->bits |= (u32)b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int decode_huff(struct jdec *j, struct huff *t)
{
    int l, f;
    long code;
    if (j->nbits < 16) fill(j);
    f = t->fast[(unsigned)(j->bits >> (32 - FAST))];
    if (f != (int)0xFFFF) {
        l = f >> 8;
        j->bits <<= l;
        j->nbits -= l;
        return f & 0xFF;
    }
    for (l = FAST + 1; l <= 16; l++) {
        code = (long)(j->bits >> (32 - l));
        if (code <= t->maxcode[l]) break;
    }
    if (l > 16) {
        j->nbits = 0;
        j->bits = 0;
        return -1;
    }
    j->bits <<= l;
    j->nbits -= l;
    return t->val[(int)(code + t->delta[l]) & 0xFF];
}

static int receive(struct jdec *j, int n)
{
    long v;
    if (!n) return 0;
    if (j->nbits < n) fill(j);
    v = (long)(j->bits >> (32 - n));
    j->bits <<= n;
    j->nbits -= n;
    if (v < (1L << (n - 1))) v -= (1L << n) - 1;              /* negative */
    return (int)v;
}

/* one 8x8 block, dequantized in natural order; returns the number of coefficients
   read (1: only DC) or -1 */
static int decode_block(struct jdec *j, struct comp *c)
{
    short *b = j->blk;
    u16 *q = j->q[c->tq];
    int s, k, r, last = 1;
    memset(b, 0, sizeof(j->blk));
    s = decode_huff(j, &j->hd[c->td]);
    if (s < 0 || s > 11) return -1;
    c->dc += receive(j, s);
    b[0] = (short)((long)c->dc * q[0]);
    for (k = 1; k < 64; ) {
        int rs = decode_huff(j, &j->ha[c->ta]);
        if (rs < 0) return -1;
        r = rs >> 4;
        s = rs & 15;
        if (!s) {
            if (r != 15) break;                         /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return -1;
        b[dezig[k]] = (short)((long)receive(j, s) * q[k]);
        k++;
        last = k;
    }
    return last;
}

/* ---------------------------------------------------------------- inverse DCT */

#define F2F(x)  ((long)((x) * 4096.0 + 0.5))
#define FSH(x)  ((long)(x) << 12)

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * F2F(0.5411961); \
    t2 = p1 + p3 * F2F(-1.847759065); \
    t3 = p1 + p2 * F2F(0.765366865); \
    p2 = s0; p3 = s4; \
    t0 = FSH(p2 + p3); \
    t1 = FSH(p2 - p3); \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * F2F(1.175875602); \
    t0 = t0 * F2F(0.298631336); \
    t1 = t1 * F2F(2.053119869); \
    t2 = t2 * F2F(3.072711026); \
    t3 = t3 * F2F(1.501321110); \
    p1 = p5 + p1 * F2F(-0.899976223); \
    p2 = p5 + p2 * F2F(-2.562915447); \
    p3 = p3 * F2F(-1.961570560); \
    p4 = p4 * F2F(-0.390180644); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static u8 clampv(long v)
{
    return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void idct(const short *d, u8 *out, int stride)
{
    long v[64], t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
    long *pv;
    int i;
    for (i = 0; i < 8; i++, d++) {
        pv = v + i;
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
            long dc = (long)d[0] * 4;
            pv[0] = pv[8] = pv[16] = pv[24] = pv[32] = pv[40] = pv[48] = pv[56] = dc;
            continue;
        }
        IDCT_1D((long)d[0], (long)d[8], (long)d[16], (long)d[24], (long)d[32], (long)d[40], (long)d[48], (long)d[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        pv[0] = (x0 + t3) >> 10;
        pv[56] = (x0 - t3) >> 10;
        pv[8] = (x1 + t2) >> 10;
        pv[48] = (x1 - t2) >> 10;
        pv[16] = (x2 + t1) >> 10;
        pv[40] = (x2 - t1) >> 10;
        pv[24] = (x3 + t0) >> 10;
        pv[32] = (x3 - t0) >> 10;
    }
    for (i = 0, pv = v; i < 8; i++, pv += 8, out += stride) {
        IDCT_1D(pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], pv[6], pv[7])
        x0 += 65536L + (128L << 17);
        x1 += 65536L + (128L << 17);
        x2 += 65536L + (128L << 17);
        x3 += 65536L + (128L << 17);
        out[0] = clampv((x0 + t3) >> 17);
        out[7] = clampv((x0 - t3) >> 17);
        out[1] = clampv((x1 + t2) >> 17);
        out[6] = clampv((x1 - t2) >> 17);
        out[2] = clampv((x2 + t1) >> 17);
        out[5] = clampv((x2 - t1) >> 17);
        out[3] = clampv((x3 + t0) >> 17);
        out[4] = clampv((x3 - t0) >> 17);
    }
}

static void idct_dc(const short *d, u8 *out, int stride)
{
    long v = ((long)d[0] + 4) >> 3;
    u8 b = clampv(v + 128);
    int i;
    for (i = 0; i < 8; i++, out += stride) memset(out, b, 8);
}

/* ---------------------------------------------------------------- color */

static short crr[256], cbb[256], crg[256], cbg[256];
static u8 clamptab[1024];
static int tabs_done;

static void make_tabs(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        long x = i - 128;
        crr[i] = (short)((x * 91881L + 32768L) >> 16);         /* 1.402 */
        cbb[i] = (short)((x * 116130L + 32768L) >> 16);        /* 1.772 */
        crg[i] = (short)((-x * 46802L + 32768L) >> 16);        /* 0.71414 */
        cbg[i] = (short)((-x * 22554L + 32768L) >> 16);        /* 0.34414 */
    }
    for (i = 0; i < 1024; i++) clamptab[i] = (u8)(i < 384 ? 0 : i > 384 + 255 ? 255 : i - 384);
    tabs_done = 1;
}

#define CL(v)   clamptab[(v) + 384]

/* ---------------------------------------------------------------- frame */

static int read_headers(struct jdec *j, int *sof)
{
    struct jin *in = j->in;
    int m, len, i, have_dht = 0;
    if (getb(in) != 0xFF || getb(in) != 0xD8) return JPG_BAD;
    *sof = 0;
    for (;;) {
        m = next_marker(in);
        if (m < 0 || m == 0xD9) return JPG_BAD;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        len = get16(in);
        if (len < 2) return JPG_BAD;
        len -= 2;
        switch (m) {
        case 0xDB:                                      /* quantization tables */
            while (len > 0) {
                int pq, tq, b = getb(in);
                if (b < 0) return JPG_BAD;
                pq = b >> 4;
                tq = b & 3;
                for (i = 0; i < 64; i++) j->q[tq][i] = (u16)(pq ? get16(in) : getb(in));
                len -= 1 + (pq ? 128 : 64);
            }
            break;
        case 0xC4:
            if (read_dht(j, in, NULL, len)) return JPG_BAD;
            have_dht = 1;
            break;
        case 0xDD:
            j->ri = get16(in);
            skip(in, (unsigned)(len - 2));
            break;
        case 0xC0: case 0xC1:
            if (getb(in) != 8) return JPG_UNSUP;
            j->h = get16(in);
            j->w = get16(in);
            j->nc = getb(in);
            if (j->nc != 1 && j->nc != 3) return JPG_UNSUP;
            if (j->w <= 0 || j->h <= 0 || j->w > MAXW) return JPG_UNSUP;
            j->hmax = j->vmax = 1;
            for (i = 0; i < j->nc; i++) {
                struct comp *c = &j->c[i];
                int hv;
                c->id = getb(in);
                hv = getb(in);
                c->tq = getb(in) & 3;
                c->h = hv >> 4;
                c->v = hv & 15;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return JPG_BAD;
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            *sof = 1;
            break;
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            return JPG_UNSUP;
        case 0xDA:                                      /* start of scan */
            if (!*sof) return JPG_BAD;
            j->ns = getb(in);
            if (j->ns < 1 || j->ns > 3) return JPG_UNSUP;
            for (i = 0; i < j->ns; i++) {
                int id = getb(in), t = getb(in), k;
                for (k = 0; k < j->nc && j->c[k].id != id; k++) ;
                if (k == j->nc) return JPG_BAD;
                j->sc[i] = k;
                j->c[k].td = (t >> 4) & 3;
                j->c[k].ta = t & 3;
            }
            skip(in, 3);
            if (j->ns != j->nc) return JPG_UNSUP;       /* non-interleaved scans */
            if (!have_dht && read_dht(j, NULL, jpg_std_dht + 4, sizeof(jpg_std_dht) - 4)) return JPG_BAD;
            return JPG_OK;
        default:
            skip(in, (unsigned)len);
        }
    }
}

int jpg_info(struct jin *in, int *w, int *h)
{
    struct jdec *j = (struct jdec *)jpg_alloc(sizeof(struct jdec));
    int sof, r;
    if (!j) return JPG_MEM;
    memset(j, 0, sizeof(*j));
    j->in = in;
    r = read_headers(j, &sof);
    *w = j->w;
    *h = j->h;
    jpg_free(j);
    return r;
}

int jpg_has_dht(struct jin *in)
{
    int m, len;
    if (getb(in) != 0xFF || getb(in) != 0xD8) return 0;
    for (;;) {
        m = next_marker(in);
        if (m < 0 || m == 0xDA || m == 0xD9) return 0;
        if (m == 0xC4) return 1;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        len = get16(in);
        if (len < 2) return 0;
        skip(in, (unsigned)(len - 2));
    }
}

static int restart(struct jdec *j)
{
    int i;
    j->bits = 0;
    j->nbits = 0;
    if (!j->marker) {
        int m = next_marker(j->in);
        if (m < 0) return -1;
        j->marker = m;
    }
    if (j->marker < 0xD0 || j->marker > 0xD7) return -1;
    j->marker = 0;
    for (i = 0; i < j->nc; i++) j->c[i].dc = 0;
    return 0;
}

int jpg_decode(struct jin *in, jrow_fn row, void *ctx)
{
    struct jdec *j;
    u8 *rgb = NULL;
    int sof, r, i, mx, my, n, todo, rows;
    if (!tabs_done) make_tabs();
    j = (struct jdec *)jpg_alloc(sizeof(struct jdec));
    if (!j) return JPG_MEM;
    memset(j, 0, sizeof(*j));
    j->in = in;
    r = read_headers(j, &sof);
    if (r) goto out;
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    r = JPG_MEM;
    for (i = 0; i < j->nc; i++) {
        struct comp *c = &j->c[i];
        c->bw = j->mcux * c->h;
        c->bh = c->v;
        if ((long)c->bw * 8 * c->bh * 8 > 60000L) goto out;
        c->plane = (u8 *)jpg_alloc((unsigned long)c->bw * 8 * c->bh * 8);
        if (!c->plane) goto out;
    }
    rgb = (u8 *)jpg_alloc((unsigned long)j->w * 3);
    if (!rgb) goto out;
    r = JPG_OK;
    todo = j->ri;
    for (my = 0; my < j->mcuy; my++) {
        for (mx = 0; mx < j->mcux; mx++) {
            if (j->ri && !todo) {
                if (restart(j)) {
                    r = JPG_BAD;
                    goto rows_out;
                }
                todo = j->ri;
            }
            for (i = 0; i < j->ns; i++) {
                struct comp *c = &j->c[j->sc[i]];
                int bx, by;
                for (by = 0; by < c->v; by++)
                    for (bx = 0; bx < c->h; bx++) {
                        int stride = c->bw * 8;
                        u8 *dst = c->plane + (by * 8) * stride + (mx * c->h + bx) * 8;
                        n = decode_block(j, c);
                        if (n < 0) {
                            r = JPG_BAD;
                            goto rows_out;
                        }
                        if (n <= 1) idct_dc(j->blk, dst, stride);
                        else idct(j->blk, dst, stride);
                    }
            }
            if (todo) todo--;
        }
rows_out:
        /* color conversion of the MCU row (chroma by sample replication) */
        rows = 8 * j->vmax;
        if (my * rows + rows > j->h) rows = j->h - my * rows;
        for (n = 0; n < rows; n++) {
            u8 *o = rgb;
            int x, xs = in->xstep > 1 ? in->xstep : 1;
            struct comp *cy = &j->c[0];
            u8 *py = cy->plane + (n * cy->v / j->vmax) * cy->bw * 8;
            if (in->ystep > 1 && (my * 8 * j->vmax + n) % in->ystep) continue;
            if (j->nc == 1) {
                for (x = 0; x < j->w; x += xs, o += 3) o[0] = o[1] = o[2] = py[x];
            } else {
                struct comp *cb = &j->c[1], *cr = &j->c[2];
                u8 *pb = cb->plane + (n * cb->v / j->vmax) * cb->bw * 8;
                u8 *pr = cr->plane + (n * cr->v / j->vmax) * cr->bw * 8;
                int fy = j->hmax / cy->h, fb = j->hmax / cb->h, fr = j->hmax / cr->h;
                if (fy == 1 && fb == 2 && fr == 2) {
                    /* 4:2:2 and 4:2:0, the camera formats */
                    for (x = 0; x < j->w; x += xs, o += 3) {
                        int yy = py[x], u = pb[x >> 1], v = pr[x >> 1];
                        o[0] = CL(yy + cbb[u]);
                        o[1] = CL(yy + cbg[u] + crg[v]);
                        o[2] = CL(yy + crr[v]);
                    }
                } else {
                    for (x = 0; x < j->w; x += xs, o += 3) {
                        int yy = py[x / fy], u = pb[x / fb], v = pr[x / fr];
                        o[0] = CL(yy + cbb[u]);
                        o[1] = CL(yy + cbg[u] + crg[v]);
                        o[2] = CL(yy + crr[v]);
                    }
                }
            }
            row(ctx, my * 8 * j->vmax + n, rgb);
        }
        if (r) break;
    }
out:
    for (i = 0; i < 3; i++) if (j->c[i].plane) jpg_free(j->c[i].plane);
    if (rgb) jpg_free(rgb);
    jpg_free(j);
    return r;
}
