/* synth.c - General MIDI synthesizer: 32 voices of two-operator FM, 128 instruments and a
   drum kit on channel 10. Integer only. It renders at half the device rate (22.05/24 kHz)
   and interpolates up, which halves the work on slow machines. */
#include "audio.h"
#include "synthtab.h"

#define NVOICE      32
#define PEAK        6500            /* full-scale amplitude of one voice */
#define ENV_ONE     (1L << 24)
#define F_BREATH    1               /* a little noise over the tone */
#define F_NOISE     2               /* noise instead of the tone */
#define F_NOISETONE 3

struct patch {
    u8 mmul, cmul;                  /* frequency multiples, in halves */
    u8 idx, fb;                     /* modulation depth 0..127, modulator feedback 0..7 */
    u8 matk, mdec, msus;            /* modulator envelope */
    u8 catk, cdec, csus, crel;      /* carrier envelope; times 2^(t/6) ms */
    u8 flags;
};

static const struct patch patches[128] = {
    /* piano */
    { 2, 2, 40, 1, 0, 48, 16, 0, 72, 0, 36, 0 }, { 2, 2, 52, 1, 0, 48, 24, 0, 70, 0, 36, 0 },
    { 2, 2, 46, 2, 0, 44, 20, 0, 70, 0, 36, 0 }, { 2, 2, 44, 3, 0, 46, 20, 0, 68, 0, 36, 0 },
    { 14, 2, 26, 0, 0, 40, 8, 0, 72, 0, 40, 0 }, { 2, 2, 36, 0, 0, 50, 10, 0, 70, 0, 40, 0 },
    { 8, 2, 44, 3, 0, 42, 24, 0, 62, 0, 30, 0 }, { 2, 2, 60, 4, 0, 36, 30, 0, 56, 0, 26, 0 },
    /* chromatic percussion */
    { 8, 2, 24, 0, 0, 48, 0, 0, 62, 0, 40, 0 }, { 7, 4, 30, 0, 0, 40, 0, 0, 64, 0, 44, 0 },
    { 8, 2, 30, 0, 0, 40, 0, 0, 62, 0, 40, 0 }, { 8, 2, 16, 0, 0, 50, 0, 0, 72, 0, 48, 0 },
    { 8, 2, 24, 0, 0, 24, 0, 0, 52, 0, 30, 0 }, { 6, 2, 30, 0, 0, 24, 0, 0, 48, 0, 28, 0 },
    { 7, 2, 36, 0, 0, 60, 0, 0, 80, 0, 50, 0 }, { 2, 2, 40, 2, 0, 46, 10, 0, 64, 0, 36, 0 },
    /* organ */
    { 4, 2, 26, 2, 0, 0, 127, 6, 0, 127, 30, 0 }, { 6, 2, 34, 2, 0, 40, 60, 4, 0, 127, 28, 0 },
    { 2, 2, 40, 4, 0, 0, 127, 6, 0, 127, 30, 0 }, { 4, 2, 30, 3, 30, 0, 127, 30, 0, 127, 48, 0 },
    { 2, 2, 30, 5, 24, 0, 127, 24, 0, 127, 40, 0 }, { 2, 2, 34, 5, 30, 0, 127, 30, 0, 127, 40, 0 },
    { 2, 2, 36, 4, 30, 0, 127, 30, 0, 127, 36, 0 }, { 2, 2, 32, 5, 30, 0, 127, 30, 0, 127, 40, 0 },
    /* guitar */
    { 2, 2, 30, 1, 0, 40, 10, 0, 64, 0, 36, 0 }, { 2, 2, 40, 2, 0, 42, 14, 0, 66, 0, 36, 0 },
    { 2, 2, 26, 1, 0, 44, 10, 0, 64, 0, 36, 0 }, { 2, 2, 32, 2, 0, 44, 16, 0, 66, 0, 36, 0 },
    { 2, 2, 36, 2, 0, 30, 0, 0, 40, 0, 24, 0 }, { 2, 2, 56, 6, 0, 60, 90, 0, 72, 60, 30, 0 },
    { 2, 2, 64, 7, 0, 60, 100, 0, 72, 70, 30, 0 }, { 4, 4, 20, 0, 0, 50, 10, 0, 64, 0, 36, 0 },
    /* bass */
    { 2, 2, 36, 0, 0, 36, 10, 0, 60, 0, 30, 0 }, { 2, 2, 40, 1, 0, 40, 20, 0, 62, 0, 30, 0 },
    { 2, 2, 50, 2, 0, 34, 20, 0, 62, 0, 30, 0 }, { 2, 2, 30, 1, 6, 48, 30, 6, 66, 0, 36, 0 },
    { 2, 2, 64, 3, 0, 30, 20, 0, 58, 0, 30, 0 }, { 2, 2, 70, 3, 0, 30, 24, 0, 58, 0, 30, 0 },
    { 2, 2, 60, 5, 0, 40, 40, 0, 60, 40, 30, 0 }, { 2, 2, 50, 4, 0, 44, 50, 0, 62, 50, 30, 0 },
    /* strings */
    { 2, 2, 30, 3, 36, 0, 110, 36, 0, 120, 40, 0 }, { 2, 2, 30, 3, 38, 0, 110, 38, 0, 120, 42, 0 },
    { 2, 2, 32, 3, 38, 0, 110, 38, 0, 120, 42, 0 }, { 2, 2, 34, 3, 40, 0, 110, 40, 0, 120, 42, 0 },
    { 2, 2, 28, 3, 36, 0, 110, 36, 0, 120, 42, 0 }, { 2, 2, 30, 1, 0, 36, 0, 0, 48, 0, 30, 0 },
    { 2, 2, 24, 0, 0, 44, 0, 0, 66, 0, 40, 0 }, { 2, 1, 30, 0, 0, 40, 0, 0, 64, 0, 40, 0 },
    /* ensemble */
    { 2, 2, 24, 3, 42, 0, 110, 42, 0, 120, 50, 0 }, { 2, 2, 22, 3, 48, 0, 110, 48, 0, 120, 52, 0 },
    { 2, 2, 30, 5, 40, 0, 110, 40, 0, 120, 50, 0 }, { 2, 2, 26, 4, 46, 0, 110, 46, 0, 120, 52, 0 },
    { 2, 2, 16, 2, 44, 0, 100, 44, 0, 120, 50, 0 }, { 2, 2, 10, 1, 42, 0, 100, 42, 0, 120, 50, 0 },
    { 2, 2, 18, 2, 40, 0, 100, 40, 0, 120, 48, 0 }, { 2, 2, 60, 5, 0, 40, 0, 0, 54, 0, 36, 0 },
    /* brass */
    { 2, 2, 44, 4, 24, 40, 90, 18, 0, 120, 30, 0 }, { 2, 2, 40, 4, 26, 40, 90, 20, 0, 120, 32, 0 },
    { 2, 2, 36, 3, 26, 40, 80, 22, 0, 120, 32, 0 }, { 2, 2, 30, 3, 20, 40, 60, 18, 0, 110, 28, 0 },
    { 2, 2, 26, 2, 30, 40, 70, 26, 0, 120, 36, 0 }, { 2, 2, 42, 4, 26, 40, 90, 22, 0, 120, 34, 0 },
    { 2, 2, 50, 5, 24, 44, 80, 18, 0, 120, 34, 0 }, { 2, 2, 40, 4, 30, 48, 70, 24, 0, 120, 36, 0 },
    /* reed */
    { 2, 2, 40, 4, 20, 0, 100, 18, 0, 120, 30, 0 }, { 2, 2, 42, 4, 20, 0, 100, 18, 0, 120, 30, 0 },
    { 2, 2, 44, 4, 20, 0, 100, 18, 0, 120, 30, 0 }, { 2, 2, 46, 4, 20, 0, 100, 18, 0, 120, 30, 0 },
    { 4, 2, 30, 3, 20, 0, 90, 18, 0, 120, 30, 0 }, { 4, 2, 28, 3, 22, 0, 90, 20, 0, 120, 32, 0 },
    { 2, 2, 34, 4, 22, 0, 90, 20, 0, 120, 30, 0 }, { 6, 2, 30, 2, 20, 0, 90, 18, 0, 120, 30, 0 },
    /* pipe */
    { 2, 2, 8, 0, 22, 0, 60, 22, 0, 120, 30, F_BREATH }, { 2, 2, 10, 0, 24, 0, 60, 24, 0, 120, 32, F_BREATH },
    { 2, 2, 12, 0, 20, 0, 60, 20, 0, 120, 30, F_BREATH }, { 2, 2, 8, 0, 26, 0, 60, 26, 0, 120, 34, F_BREATH },
    { 2, 2, 6, 0, 28, 0, 60, 28, 0, 120, 34, F_BREATH }, { 2, 2, 10, 1, 26, 0, 60, 26, 0, 120, 34, F_BREATH },
    { 2, 2, 4, 0, 22, 0, 40, 22, 0, 120, 30, 0 }, { 2, 2, 6, 0, 20, 0, 60, 20, 0, 120, 30, 0 },
    /* synth lead */
    { 4, 2, 44, 6, 0, 0, 127, 4, 0, 127, 28, 0 }, { 2, 2, 48, 7, 0, 0, 127, 4, 0, 127, 28, 0 },
    { 2, 2, 20, 2, 18, 0, 100, 18, 0, 120, 32, F_BREATH }, { 2, 2, 30, 3, 0, 30, 40, 6, 0, 120, 30, 0 },
    { 2, 2, 60, 7, 0, 50, 100, 4, 0, 110, 30, 0 }, { 2, 2, 16, 2, 30, 0, 100, 30, 0, 120, 40, 0 },
    { 3, 2, 40, 4, 6, 0, 120, 6, 0, 127, 30, 0 }, { 2, 2, 50, 6, 0, 40, 90, 4, 0, 120, 30, 0 },
    /* synth pad */
    { 4, 2, 24, 2, 40, 60, 40, 40, 0, 110, 60, 0 }, { 2, 2, 20, 2, 48, 0, 100, 48, 0, 120, 60, 0 },
    { 2, 2, 34, 5, 30, 50, 70, 30, 0, 110, 54, 0 }, { 2, 2, 14, 2, 50, 0, 100, 50, 0, 120, 60, 0 },
    { 2, 2, 22, 3, 54, 0, 100, 54, 0, 110, 60, 0 }, { 7, 2, 30, 2, 40, 0, 90, 40, 0, 110, 60, 0 },
    { 2, 2, 16, 1, 54, 0, 100, 54, 0, 120, 66, 0 }, { 2, 2, 40, 3, 60, 0, 127, 50, 0, 120, 60, 0 },
    /* synth effects */
    { 14, 2, 20, 0, 0, 50, 0, 30, 66, 0, 60, F_BREATH }, { 2, 2, 24, 3, 54, 0, 100, 54, 0, 110, 66, 0 },
    { 7, 2, 26, 0, 0, 56, 10, 0, 72, 0, 60, 0 }, { 2, 2, 30, 2, 30, 60, 30, 30, 70, 40, 60, 0 },
    { 8, 2, 30, 2, 20, 60, 40, 30, 0, 110, 60, 0 }, { 3, 2, 40, 5, 60, 0, 100, 60, 0, 110, 60, 0 },
    { 2, 2, 30, 3, 40, 60, 40, 40, 72, 40, 66, 0 }, { 5, 2, 40, 4, 40, 0, 100, 40, 0, 110, 60, 0 },
    /* ethnic */
    { 2, 2, 50, 5, 0, 54, 30, 0, 70, 0, 40, 0 }, { 2, 2, 44, 2, 0, 36, 10, 0, 58, 0, 30, 0 },
    { 2, 2, 48, 3, 0, 34, 10, 0, 56, 0, 30, 0 }, { 2, 2, 36, 2, 0, 40, 10, 0, 62, 0, 34, 0 },
    { 8, 2, 20, 0, 0, 36, 0, 0, 56, 0, 34, 0 }, { 2, 2, 40, 6, 18, 0, 120, 18, 0, 127, 30, 0 },
    { 2, 2, 34, 4, 30, 0, 110, 30, 0, 120, 40, 0 }, { 4, 2, 36, 5, 20, 0, 110, 20, 0, 120, 32, 0 },
    /* percussive */
    { 7, 2, 30, 0, 0, 40, 0, 0, 62, 0, 40, 0 }, { 6, 2, 26, 0, 0, 30, 0, 0, 50, 0, 30, 0 },
    { 5, 2, 30, 0, 0, 40, 0, 0, 58, 0, 36, 0 }, { 6, 2, 20, 0, 0, 18, 0, 0, 30, 0, 20, 0 },
    { 2, 2, 40, 0, 0, 30, 0, 0, 54, 0, 36, F_BREATH }, { 2, 2, 30, 0, 0, 30, 0, 0, 54, 0, 36, 0 },
    { 2, 2, 36, 2, 0, 30, 0, 0, 52, 0, 34, 0 }, { 2, 2, 0, 0, 0, 0, 0, 66, 0, 127, 24, F_NOISE },
    /* sound effects */
    { 2, 2, 10, 0, 0, 20, 0, 0, 30, 0, 20, F_NOISETONE }, { 2, 2, 0, 0, 0, 0, 0, 24, 40, 0, 30, F_NOISE },
    { 2, 2, 0, 0, 0, 0, 0, 60, 0, 100, 66, F_NOISE }, { 6, 4, 40, 0, 0, 30, 60, 6, 0, 110, 30, 0 },
    { 8, 4, 30, 0, 0, 0, 127, 0, 0, 127, 24, 0 }, { 1, 2, 60, 7, 0, 0, 127, 30, 0, 110, 50, F_NOISETONE },
    { 2, 2, 0, 0, 0, 0, 0, 50, 0, 100, 60, F_NOISE }, { 2, 2, 0, 0, 0, 0, 0, 0, 48, 0, 30, F_NOISE },
};

struct drum {
    u16 f0, f1;                     /* tone frequency: start, end (Hz) */
    u8 sweep;                       /* time of the pitch fall */
    u8 tone, noise;                 /* levels 0..127 */
    u8 hp;                          /* bright (high-passed) noise */
    u8 dec;                         /* decay time */
};

static const struct drum drums[47] = {     /* notes 35..81 */
    { 120, 45, 24, 127, 10, 0, 54 }, { 140, 50, 22, 127, 14, 0, 52 }, { 800, 600, 10, 60, 80, 1, 24 },
    { 200, 160, 20, 60, 110, 0, 46 }, { 1000, 1000, 0, 0, 120, 1, 36 }, { 220, 180, 18, 70, 120, 1, 44 },
    { 90, 65, 36, 127, 10, 0, 54 }, { 0, 0, 0, 0, 100, 1, 28 }, { 105, 75, 36, 127, 10, 0, 54 },
    { 0, 0, 0, 0, 90, 1, 30 }, { 120, 85, 36, 127, 10, 0, 54 }, { 0, 0, 0, 0, 100, 1, 50 },
    { 140, 100, 36, 127, 10, 0, 54 }, { 160, 115, 36, 127, 10, 0, 54 }, { 0, 0, 0, 0, 110, 1, 70 },
    { 185, 130, 36, 127, 10, 0, 52 }, { 0, 0, 0, 0, 80, 1, 64 }, { 0, 0, 0, 0, 110, 1, 64 },
    { 2400, 2400, 0, 60, 50, 1, 54 }, { 0, 0, 0, 0, 90, 1, 40 }, { 0, 0, 0, 0, 110, 1, 56 },
    { 560, 560, 0, 100, 0, 0, 40 }, { 0, 0, 0, 0, 110, 1, 72 }, { 0, 0, 0, 0, 80, 1, 54 },
    { 0, 0, 0, 0, 80, 1, 62 }, { 420, 380, 20, 110, 10, 0, 36 }, { 300, 270, 20, 110, 10, 0, 38 },
    { 340, 320, 10, 110, 10, 0, 30 }, { 340, 300, 20, 110, 10, 0, 44 }, { 230, 210, 20, 110, 10, 0, 46 },
    { 500, 440, 20, 100, 30, 0, 40 }, { 380, 340, 20, 100, 30, 0, 42 }, { 900, 900, 0, 100, 0, 0, 40 },
    { 660, 660, 0, 100, 0, 0, 42 }, { 0, 0, 0, 0, 80, 1, 34 }, { 0, 0, 0, 0, 80, 1, 26 },
    { 2500, 2500, 0, 80, 0, 0, 30 }, { 2300, 2300, 0, 80, 0, 0, 46 }, { 0, 0, 0, 0, 70, 1, 32 },
    { 0, 0, 0, 0, 70, 1, 44 }, { 2500, 2500, 0, 110, 0, 0, 24 }, { 1300, 1300, 0, 110, 0, 0, 24 },
    { 1000, 1000, 0, 110, 0, 0, 26 }, { 600, 700, 10, 90, 10, 0, 30 }, { 500, 350, 30, 90, 10, 0, 40 },
    { 4000, 4000, 0, 70, 0, 0, 30 }, { 4000, 4000, 0, 70, 0, 0, 60 },
};

#define ST_ATK  0
#define ST_DEC  1
#define ST_SUS  2
#define ST_REL  3

struct voice {
    u8 on;                          /* 0 free, 1 key down, 2 released, 3 held by the pedal */
    u8 ch, note, vel;
    u32 age;
    const struct patch *p;
    const struct drum *d;
    u32 cph, mph;
    u32 base;                       /* phase increment of the note at unit multiple */
    u32 cinc, minc;
    s32 cenv, menv;
    u8 cst, mst;
    s16 fb1, fb2;
    s32 gl, gr;                     /* gain left / right (Q15, PEAK included) */
    s32 cdelta;                     /* attack step */
    s32 mdelta;
    u32 dinc0, dinc1;               /* drum tone sweep */
    s32 dsw;                        /* sweep position (ENV_ONE -> 0) */
    s16 nprev;
    u8 ramp;                        /* first samples fade in: no click at the note start */
};

struct chan {
    u8 prog, vol, expr, pan, sustain, mod, rpnl, rpnh;
    s16 bend;
    u8 range;                       /* bend range, semitones */
    u8 rsv;
};

static struct voice vo[NVOICE];
static struct chan chs[16];
static u32 irate;                   /* internal rate */
static u32 blk;                     /* samples per envelope step */
static u32 blkpos;
static u32 agec;
static u32 noise = 0x12345678UL;
static u32 last_ms;
static u32 lfo;                     /* vibrato phase */
static s32 ibuf[64 * 2];
static int ilen, ipos;
static s32 prevl, prevr;
static u8 half;
static u8 running_status;
static u8 sysex;
static u8 mbuf[3];
static int mlen;
static int disabled;

static u32 rnd(void)
{
    noise = noise * 1664525UL + 1013904223UL;
    return noise;
}

/* phase increment for a pitch of note*64 + cents/64 above notehz[note] */
static u32 pitch_inc(u32 base, s32 sub)        /* sub: 1/64 semitone units */
{
    s32 oct = 0;
    u32 f;
    while (sub < 0) {
        sub += 768;
        oct--;
    }
    while (sub >= 768) {
        sub -= 768;
        oct++;
    }
    f = (u32)(((u64)base * expotab[sub]) >> 16);
    return oct >= 0 ? f << oct : f >> -oct;
}

static u32 note_inc(int note)
{
    return (u32)(((u64)notehz[note] << 24) / irate);
}

static void voice_gain(struct voice *v)
{
    struct chan *c = &chs[v->ch];
    s32 x = (s32)v->vel * c->vol * c->expr / 63, g;
    if (x > 32767) x = 32767;
    g = (s32)(((s64)x * x) >> 15);
    g = g * PEAK >> 15;
    v->gl = MIN(g, g * (127 - c->pan) / 90);       /* center: -3 dB on both sides */
    v->gr = MIN(g, g * c->pan / 90);
}

static void voice_pitch(struct voice *v)
{
    struct chan *c = &chs[v->ch];
    s32 sub = (s32)c->bend * c->range * 64 / 8192;
    if (c->mod) {
        /* vibrato up to +-50 cents at 5.5 Hz */
        s32 s = sintab[(lfo >> 22) & 1023];
        sub += s * c->mod / 127 * 32 / 32767;
    }
    v->cinc = pitch_inc(v->base, sub) / 2 * v->p->cmul;
    v->minc = pitch_inc(v->base, sub) / 2 * v->p->mmul;
}

static struct voice *alloc_voice(void)
{
    struct voice *best = NULL;
    int i;
    for (i = 0; i < NVOICE; i++)
        if (!vo[i].on) return &vo[i];
    /* steal: the quietest released voice, else the oldest */
    for (i = 0; i < NVOICE; i++)
        if (vo[i].on == 2 && (!best || vo[i].cenv < best->cenv)) best = &vo[i];
    if (best) return best;
    for (i = 0; i < NVOICE; i++)
        if (!best || vo[i].age < best->age) best = &vo[i];
    return best;
}

static void note_on(int ch, int note, int vel)
{
    struct voice *v;
    int i;
    if (!irate) return;
    last_ms = plat_ms() | 1;
    /* the same key again: retrigger that voice */
    for (i = 0; i < NVOICE; i++)
        if (vo[i].on && vo[i].ch == ch && vo[i].note == note) {
            vo[i].on = 2;
            vo[i].cst = vo[i].mst = ST_REL;
        }
    if (ch == 9) {
        const struct drum *d;
        if (note < 35 || note > 81) return;
        d = &drums[note - 35];
        v = alloc_voice();
        memset(v, 0, sizeof(*v));
        v->on = 2;
        v->d = d;
        v->p = &patches[0];
        v->dinc0 = (u32)(((u64)d->f0 << 32) / irate);
        v->dinc1 = (u32)(((u64)d->f1 << 32) / irate);
        v->dsw = ENV_ONE;
        v->cinc = v->dinc0;
        v->cenv = ENV_ONE;
        v->cst = ST_DEC;
    } else {
        v = alloc_voice();
        memset(v, 0, sizeof(*v));
        v->on = 1;
        v->p = &patches[chs[ch].prog];
        v->base = note_inc(note);
        v->cst = v->p->catk ? ST_ATK : ST_DEC;
        v->mst = v->p->matk ? ST_ATK : ST_DEC;
        v->cenv = v->p->catk ? 0 : ENV_ONE;
        v->menv = v->p->matk ? 0 : ENV_ONE;
        v->cdelta = ENV_ONE / (s32)tsteps[v->p->catk];
        v->mdelta = ENV_ONE / (s32)tsteps[v->p->matk];
    }
    v->ch = (u8)ch;
    v->note = (u8)note;
    v->vel = (u8)vel;
    v->age = ++agec;
    voice_gain(v);
    if (!v->d) voice_pitch(v);
}

static void note_off(int ch, int note)
{
    int i;
    for (i = 0; i < NVOICE; i++) {
        struct voice *v = &vo[i];
        if (v->on != 1 || v->ch != ch || v->note != note || v->d) continue;
        if (chs[ch].sustain) {
            v->on = 3;
            continue;
        }
        v->on = 2;
        v->cst = v->mst = ST_REL;
    }
}

static void release_pedal(int ch)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vo[i].on == 3 && vo[i].ch == ch) {
            vo[i].on = 2;
            vo[i].cst = vo[i].mst = ST_REL;
        }
}

static void all_off(int ch, int hard)
{
    int i;
    for (i = 0; i < NVOICE; i++) {
        if (!vo[i].on || vo[i].ch != ch) continue;
        if (hard) vo[i].on = 0;
        else if (!vo[i].d) {
            vo[i].on = 2;
            vo[i].cst = vo[i].mst = ST_REL;
        }
    }
}

static void reset_ctl(int ch)
{
    struct chan *c = &chs[ch];
    c->expr = 127;
    c->sustain = 0;
    c->mod = 0;
    c->bend = 0;
    c->rpnl = c->rpnh = 127;
    release_pedal(ch);
}

void synth_reset(void)
{
    int i;
    memset(vo, 0, sizeof(vo));
    for (i = 0; i < 16; i++) {
        memset(&chs[i], 0, sizeof(chs[i]));
        chs[i].vol = 100;
        chs[i].pan = 64;
        chs[i].range = 2;
        reset_ctl(i);
    }
    running_status = 0;
    sysex = 0;
    mlen = 0;
}

static void update_chan(int ch, int pitch)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vo[i].on && vo[i].ch == ch) {
            voice_gain(&vo[i]);
            if (pitch && !vo[i].d) voice_pitch(&vo[i]);
        }
}

static void control(int ch, int cc, int val)
{
    struct chan *c = &chs[ch];
    switch (cc) {
    case 1: c->mod = (u8)val; update_chan(ch, 1); break;
    case 6:
        if (c->rpnh == 0 && c->rpnl == 0) c->range = (u8)(val > 24 ? 24 : val);
        break;
    case 7: c->vol = (u8)val; update_chan(ch, 0); break;
    case 10: c->pan = (u8)val; update_chan(ch, 0); break;
    case 11: c->expr = (u8)val; update_chan(ch, 0); break;
    case 64:
        c->sustain = (u8)(val >= 64);
        if (!c->sustain) release_pedal(ch);
        break;
    case 98: case 99: c->rpnl = c->rpnh = 127; break;       /* NRPN: ignore data entry */
    case 100: c->rpnl = (u8)val; break;
    case 101: c->rpnh = (u8)val; break;
    case 120: all_off(ch, 1); break;
    case 121: reset_ctl(ch); update_chan(ch, 1); break;
    case 123: case 124: case 125: case 126: case 127: all_off(ch, 0); break;
    }
}

static void message(const u8 *m)
{
    int ch = m[0] & 15;
    switch (m[0] & 0xF0) {
    case 0x80: note_off(ch, m[1]); break;
    case 0x90: if (m[2]) note_on(ch, m[1], m[2]); else note_off(ch, m[1]); break;
    case 0xB0: control(ch, m[1], m[2]); break;
    case 0xC0: chs[ch].prog = m[1]; break;
    case 0xE0:
        chs[ch].bend = (s16)(((m[2] << 7) | m[1]) - 8192);
        update_chan(ch, 1);
        break;
    }
}

static int msglen(u8 st)
{
    switch (st & 0xF0) {
    case 0xC0: case 0xD0: return 2;
    case 0xF0: return st == 0xF1 || st == 0xF3 ? 2 : st == 0xF2 ? 3 : 1;
    }
    return 3;
}

/* a stream of MIDI bytes (running status and system exclusive allowed) */
void synth_midi(const u8 *msg, int len)
{
    static u8 sx[8];
    static int sxn;
    int i;
    if (disabled) return;
    for (i = 0; i < len; i++) {
        u8 b = msg[i];
        if (b >= 0xF8) {
            if (b == 0xFF) synth_reset();
            continue;
        }
        if (sysex) {
            if (b == 0xF7) {
                sysex = 0;
                /* GM System On, GS reset, XG System On */
                if ((sxn >= 5 && sx[0] == 0x7E && sx[2] == 9 && sx[3] == 1) ||
                    (sxn >= 7 && sx[0] == 0x41 && sx[3] == 0x12 && sx[4] == 0x40 && sx[5] == 0 && sx[6] == 0x7F) ||
                    (sxn >= 6 && sx[0] == 0x43 && sx[2] == 0x4C && sx[3] == 0 && sx[4] == 0 && sx[5] == 0x7E))
                    synth_reset();
                continue;
            }
            if (b & 0x80) sysex = 0;
            else {
                if (sxn < 8) sx[sxn++] = b;
                continue;
            }
        }
        if (b == 0xF0) {
            sysex = 1;
            sxn = 0;
            running_status = 0;
            continue;
        }
        if (b & 0x80) {
            mbuf[0] = b;
            mlen = 1;
            running_status = b < 0xF0 ? b : 0;
        } else {
            if (!mlen) {
                if (!running_status) continue;
                mbuf[0] = running_status;
                mlen = 1;
            }
            mbuf[mlen++] = b;
        }
        if (mlen >= msglen(mbuf[0])) {
            if (mbuf[0] < 0xF0) message(mbuf);
            mlen = 0;
        }
    }
}

/* ---------------------------------------------------------------- rendering */

static s32 env_step(s32 e, u8 *st, u8 atk, u8 dec, u8 sus, u8 rel, s32 delta, int drum)
{
    s32 sl = (s32)sus << 17;        /* 127 -> almost ENV_ONE */
    (void)atk;
    switch (*st) {
    case ST_ATK:
        e += delta;
        if (e >= ENV_ONE) {
            e = ENV_ONE;
            *st = ST_DEC;
        }
        break;
    case ST_DEC:
        if (!drum && sus >= 127) {
            *st = ST_SUS;
            break;
        }
        if (!dec) {
            e = sl;
            *st = ST_SUS;
            break;
        }
        e = sl + (s32)(((s64)(e - sl) * decf[dec]) >> 16);
        if (e - sl < 256) {
            e = sl;
            *st = ST_SUS;
        }
        break;
    case ST_SUS:
        break;
    case ST_REL:
        e = (s32)(((s64)e * decf[rel]) >> 16);
        break;
    }
    return e;
}

static void control_step(void)
{
    int i;
    lfo += (u32)(((u64)55 << 32) / 10 / ENV_RATE);
    for (i = 0; i < NVOICE; i++) {
        struct voice *v = &vo[i];
        if (!v->on) continue;
        if (v->d) {
            const struct drum *d = v->d;
            v->cenv = (s32)(((s64)v->cenv * decf[d->dec]) >> 16);
            if (d->sweep) v->dsw = (s32)(((s64)v->dsw * decf[d->sweep]) >> 16);
            else v->dsw = 0;
            v->cinc = v->dinc1 + (u32)(s32)(((s64)(s32)(v->dinc0 - v->dinc1) * v->dsw) >> 24);
        } else {
            const struct patch *p = v->p;
            v->cenv = env_step(v->cenv, &v->cst, p->catk, p->cdec, p->csus, p->crel, v->cdelta, 0);
            v->menv = env_step(v->menv, &v->mst, p->matk, p->mdec, p->msus, p->crel, v->mdelta, 0);
            if (chs[v->ch].mod) voice_pitch(v);
        }
        if (v->cenv < (1L << 12) && v->cst != ST_ATK) v->on = 0;
    }
}

/* one internal sample of all voices */
static void render1(s32 *l, s32 *r)
{
    s32 sl = 0, sr = 0;
    int i;
    for (i = 0; i < NVOICE; i++) {
        struct voice *v = &vo[i];
        s32 o, a;
        if (!v->on) continue;
        a = v->cenv >> 9;                                   /* Q15 */
        if (v->d) {
            const struct drum *d = v->d;
            s32 n = (s32)(s16)(rnd() >> 16), t = 0;
            if (d->hp) {
                s32 x = n;
                n = (n - v->nprev) / 2;
                v->nprev = (s16)x;
            }
            if (d->tone) {
                t = sintab[v->cph >> 22];
                v->cph += v->cinc;
            }
            o = (t * d->tone + n * d->noise) / 128;
        } else {
            const struct patch *p = v->p;
            if (p->flags == F_NOISE) {
                o = (s32)(s16)(rnd() >> 16) / 2;
            } else {
                u32 mi = (u32)p->idx * (u32)(v->menv >> 16);  /* 0..32512 */
                s32 m;
                u32 fbo = 0;
                if (p->fb) fbo = (u32)(((s32)v->fb1 + v->fb2) / 2) << (p->fb + 11);
                m = sintab[((v->mph + fbo) >> 22) & 1023];
                v->fb2 = v->fb1;
                v->fb1 = (s16)m;
                v->mph += v->minc;
                o = sintab[((v->cph + (u32)(m * (s32)mi) * 4) >> 22) & 1023];
                v->cph += v->cinc;
                if (p->flags == F_BREATH) o += (s32)(s16)(rnd() >> 16) / 24;
                else if (p->flags == F_NOISETONE) o = (o + (s32)(s16)(rnd() >> 16)) / 2;
            }
        }
        o = o * a >> 15;
        if (v->ramp < 32) o = o * v->ramp++ >> 5;
        sl += o * v->gl >> 15;
        sr += o * v->gr >> 15;
    }
    *l = sl;
    *r = sr;
}

static void render_block(void)
{
    int n;
    for (n = 0; n < 64; n++) {
        if (!blkpos) control_step();
        render1(&ibuf[n * 2], &ibuf[n * 2 + 1]);
        if (++blkpos >= blk) blkpos = 0;
    }
    ilen = 64;
    ipos = 0;
}

static void set_rate(u32 rate)
{
    irate = rate >= 32000 ? rate / 2 : rate;
    blk = irate / ENV_RATE;
    if (!blk) blk = 1;
    blkpos = 0;
    ilen = ipos = 0;
}

int synth_busy(void)
{
    int i;
    for (i = 0; i < NVOICE; i++) if (vo[i].on) return 1;
    return last_ms && plat_ms() - last_ms < 1000;
}

void synth_render(s32 *acc, int frames, u32 rate, u32 vol)
{
    u32 vl = (vol & 0xFFFF) + 1, vr = (vol >> 16) + 1;
    int i, up;
    if (disabled) return;
    if ((rate >= 32000 ? rate / 2 : rate) != irate) {
        u32 old = irate;
        set_rate(rate);
        if (old) {          /* running notes keep their pitch */
            for (i = 0; i < NVOICE; i++)
                if (vo[i].on) {
                    if (vo[i].d) {
                        vo[i].on = 0;
                        continue;
                    }
                    vo[i].base = note_inc(vo[i].note);
                    voice_pitch(&vo[i]);
                }
        }
    }
    for (i = 0; i < NVOICE && !vo[i].on; i++) ;
    if (i == NVOICE && !ilen) return;
    up = irate != rate;
    for (i = 0; i < frames; i++) {
        s32 l, r;
        if (!up || !half) {
            if (ipos >= ilen) render_block();
            l = ibuf[ipos * 2];
            r = ibuf[ipos * 2 + 1];
            ipos++;
            if (up) {
                s32 t = l;
                l = (prevl + l) / 2;
                prevl = t;
                t = r;
                r = (prevr + r) / 2;
                prevr = t;
            }
        } else {
            l = prevl;
            r = prevr;
        }
        half ^= 1;
        acc[i * 2] += (s32)(((s64)l * vl) >> 16);
        acc[i * 2 + 1] += (s32)(((s64)r * vr) >> 16);
    }
    if (ipos >= ilen) {
        for (i = 0; i < NVOICE && !vo[i].on; i++) ;
        if (i == NVOICE) ilen = ipos = 0;
    }
}

void synth_init(void)
{
    disabled = (cb->opts & OPT_NOSYNTH) != 0;
    irate = 0;
    synth_reset();
    set_rate(48000);
}
