/* audtest.c - AUDTEST.EXE: DOS test and diagnostic tool for WINAUDIO.SYS (and the USB audio
   devices of WINUSB.SYS): device list, test tone, WAV playback and recording, MIDI test,
   mixer settings, driver log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include <bios.h>
#include <malloc.h>
#include <conio.h>
#include "winapi.h"

#define RINGDATA 32768U
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))

static struct winapireq req;
static unsigned char mplex;
static unsigned char __far *ringmem;
static struct winring __far *ring;
static struct winauddev devs[8];
static int ndev;
static char logbuf[16384];

static int call_api(unsigned fn, unsigned long a0, unsigned long a1, unsigned long a2, void __far *buf,
                    unsigned len)
{
    union REGS r;
    struct SREGS sr;
    memset(&req, 0, sizeof(req));
    req.fn = fn;
    req.arg0 = a0;
    req.arg1 = a1;
    req.arg2 = a2;
    req.buflen = len;
    req.buf = buf ? ((unsigned long)FP_SEG(buf) << 16) | FP_OFF(buf) : 0;
    segread(&sr);
    r.h.ah = mplex;
    r.h.al = 0x01;
    sr.es = FP_SEG((void __far *)&req);
    r.x.bx = FP_OFF((void __far *)&req);
    int86x(0x2F, &r, &r, &sr);
    return req.status;
}

static int detect(unsigned char id)
{
    union REGS r;
    struct SREGS sr;
    segread(&sr);
    r.h.ah = id;
    r.h.al = 0;
    r.x.bx = 0;
    int86x(0x2F, &r, &r, &sr);
    return r.h.al == 0xFF && r.x.bx == 0x3357;
}

static unsigned long ticks(void)
{
    return *(volatile unsigned long __far *)MK_FP(0x40, 0x6C);
}

static int keyhit(void)
{
    if (!_bios_keybrd(_KEYBRD_READY)) return 0;
    _bios_keybrd(_KEYBRD_READ);
    return 1;
}

static const char *const ename[] = { "ok", "bad function", "no device", "I/O error", "buffer too small",
                                      "busy", "core not active" };

static void die(const char *what, int st)
{
    printf("AUDTEST: %s: %s\n", what, st >= 0 && st <= 6 ? ename[st] : "error");
    exit(1);
}

static void load_devs(void)
{
    int st = call_api(WINF_AUD_DEVS, 0, 0, 0, (void __far *)devs, sizeof(devs));
    if (st) die("device list", st);
    ndev = (int)(req.retlen / sizeof(devs[0]));
}

static const char *tname(int t)
{
    return t == 1 ? "HD Audio" : t == 2 ? "AC'97" : t == 3 ? "USB Audio" : "?";
}

static void srcnames(unsigned s, char *out)
{
    static const char *const n[] = { "mic", "line", "cd", "aux", "mix" };
    int i;
    out[0] = 0;
    for (i = 0; i < 5; i++)
        if (s & (1 << i)) {
            if (out[0]) strcat(out, ",");
            strcat(out, n[i]);
        }
    if (!out[0]) strcpy(out, "-");
}

static void list(void)
{
    int i;
    char s[40], cur[40];
    if (!ndev) {
        printf("No sound devices.\n");
        return;
    }
    for (i = 0; i < ndev; i++) {
        struct winauddev *d = &devs[i];
        srcnames(d->insrcs, s);
        srcnames(d->insrc, cur);
        printf("%u: %s [%s] %04X:%04X, %lu Hz%s%s%s%s%s\n", d->id, d->name, tname(d->type), d->vid, d->pid,
               d->rate, (d->caps & WINAC_OUT) ? ", play" : "", (d->caps & WINAC_IN) ? ", record" : "",
               (d->caps & WINAC_SYNTH) ? ", synth" : "", (d->caps & WINAC_MIDI) ? ", MIDI out" : "",
               d->gone ? ", REMOVED" : "");
        if (d->caps & WINAC_IN) printf("   inputs: %s (selected: %s)\n", s, cur);
    }
}

/* ---------------------------------------------------------------- Sound Blaster emulation */

static struct winsb sb;

static int sb_load(void)
{
    if (mplex != WIN_MPLEX_AUDIO) return 0;
    if (call_api(WINF_AUD_SB, 0, 0, 0, (void __far *)&sb, sizeof(sb))) return 0;
    return req.retlen >= 48;
}

static void sb_show(void)
{
    if (!sb_load()) return;
    if (sb.flags & SBF_OFF) {
        printf("Sound Blaster emulation: off (/NOSB)\n");
        return;
    }
    if (!(sb.flags & SBF_ON)) {
        printf("Sound Blaster emulation: not running (no sound device)\n");
        return;
    }
    printf("Sound Blaster 16 emulation: SET BLASTER=A%X I%u D%u H%u", sb.base, sb.irq, sb.dma8,
           sb.dma16 ? sb.dma16 : sb.dma8);
    if (sb.mpu) printf(" P%X", sb.mpu);
    printf(" T6\n");
    printf("   DSP %Xh-%Xh, OPL3 FM %s, MPU-401 %s, IRQ %u, DMA %u (8-bit) and %u (16-bit)\n",
           sb.base, sb.base + 15, (sb.flags & SBF_NOOPL) ? "off" : "388h and base", sb.mpu ? "UART" : "off",
           sb.irq, sb.dma8, sb.dma16 ? sb.dma16 : sb.dma8);
    if (sb.flags & SBF_VXD)
        printf("   Windows: emulated by the VxD in every MS-DOS box\n");
    else if (sb.flags & SBF_V86) {
        union REGS r;
        struct SREGS sr;
        struct winvm __far *vm;
        segread(&sr);
        r.x.ax = 0xD500;
        int86x(0x2F, &r, &r, &sr);
        vm = (struct winvm __far *)MK_FP(sr.es, r.x.di + WIN_VMOFF);
        printf("   DOS: ports trapped by the built-in V86 monitor (%s), RTC interrupt 1024 Hz,\n"
               "   %lu port accesses, %u interrupt(s) so far; DPMI host %s (%u client(s)), VCPI %s\n",
               (vm->flags & VMF_VME) ? "VME" : "no VME", vm->traps, sb.irqs,
               (vm->flags & VMF_NODPMI) ? "off" : "on", vm->clients,
               (vm->flags & VMF_NOVCPI) ? "off" : (vm->flags & VMF_VCPI) ? "for all programs" :
               "for WINDRV and Windows Standard Mode");
    } else if (sb.flags & SBF_DOSTRAP)
        printf("   DOS: %u ports trapped through the QPI, %s, %u interrupt(s) so far\n", sb.ntraps,
               (sb.flags & SBF_RTC) ? "RTC interrupt 1024 Hz" : "timer interrupt only", sb.irqs);
    if ((sb.flags & SBF_RTC) && !(sb.flags & SBF_VXD)) {
        /* the RTC periodic interrupt drives the emulation in DOS: does it arrive? */
        unsigned long t0, t, n0;
        sb_load();
        n0 = sb.tticks;
        t0 = ticks();
        while ((t = ticks()) - t0 < 10) ;
        sb_load();
        printf("   RTC interrupt (IRQ 8): %lu per second measured\n", (sb.tticks - n0) * 2UL * 182UL / ((t - t0) * 10UL));
        if (sb.tticks == n0)
            printf("   The RTC periodic interrupt does not arrive (HPET legacy replacement mode, or IRQ 8\n"
                   "   routed elsewhere): the emulation runs from the timer tick only, with interrupts\n"
                   "   of the card coming late. Please report the machine.\n");
    }
    if (!(sb.flags & (SBF_VXD | SBF_V86 | SBF_DOSTRAP)))
        printf("   DOS: not available - remove EMM386 (WINAUDIO.SYS traps the ports itself without\n"
               "   it), or load QEMM, or JEMM386 / JEMMEX with QPIEMU.DLL before WINAUDIO.SYS.\n"
               "   Windows MS-DOS boxes always have the emulation.\n");
}

static volatile unsigned sbirqs;
static void (__interrupt __far *sb_oldvec)(void);

static void __interrupt __far sb_isr(void)
{
    inp(sb.base + 0xE);
    inp(sb.base + 0xF);
    sbirqs++;
    outp(0x20, 0x20);
}

static int dsp_w(unsigned char v)
{
    long n = 100000L;
    while ((inp(sb.base + 0xC) & 0x80) && --n) ;
    outp(sb.base + 0xC, v);
    return n != 0;
}

static int dsp_r(void)
{
    long n = 100000L;
    while (!(inp(sb.base + 0xE) & 0x80) && --n) ;
    if (!n) return -1;
    return inp(sb.base + 0xA);
}

static void opl_w(int r, int v)
{
    int i;
    outp(0x388, r);
    for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, v);
    for (i = 0; i < 35; i++) inp(0x388);
}

static int sb_check(int ok, const char *what)
{
    printf("  %-34s %s\n", what, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

/* the emulated card as a DOS program sees it: DSP, interrupt, DMA sound, FM, MPU-401 */
static void sbtest(void)
{
    static unsigned char raw[8192 + 16];
    static const unsigned char page8[4] = { 0x87, 0x83, 0x81, 0x82 };
    unsigned char __far *buf;
    unsigned long lin, t;
    unsigned i, v1, v2, cnt;
    int vec, bad = 0, dch;
    if (!sb_load() || !(sb.flags & SBF_ON)) {
        printf("AUDTEST: the Sound Blaster emulation is not running\n");
        return;
    }
    if (!(sb.flags & (SBF_VXD | SBF_DOSTRAP))) {
        sb_show();
        return;
    }
    printf("Sound Blaster 16 at %Xh, IRQ %u, DMA %u:\n", sb.base, sb.irq, sb.dma8);
    outp(sb.base + 6, 1);
    for (i = 0; i < 100; i++) inp(sb.base + 6);
    outp(sb.base + 6, 0);
    bad += sb_check(dsp_r() == 0xAA, "DSP reset");
    dsp_w(0xE1);
    v1 = dsp_r();
    v2 = dsp_r();
    bad += sb_check(v1 == 4, "DSP version 4.xx");
    vec = sb.irq < 8 ? sb.irq + 8 : sb.irq - 8 + 0x70;
    sb_oldvec = _dos_getvect(vec);
    _dos_setvect(vec, sb_isr);
    if (sb.irq < 8) outp(0x21, inp(0x21) & ~(1 << sb.irq));
    sbirqs = 0;
    dsp_w(0xF2);
    t = ticks();
    while (!sbirqs && ticks() - t < 10) ;
    bad += sb_check(sbirqs == 1, "interrupt");
    /* 8-bit DMA: 1 second of a 689 Hz tone at 11025 Hz */
    lin = ((unsigned long)FP_SEG(raw) << 4) + FP_OFF(raw);
    lin = (lin + 4095) & ~4095UL;
    buf = MK_FP((unsigned)(lin >> 4), (unsigned)(lin & 15));
    for (i = 0; i < 4096; i++) {
        static const signed char s16[16] = { 0, 49, 90, 117, 127, 117, 90, 49, 0, -49, -90, -117, -127, -117, -90, -49 };
        buf[i] = (unsigned char)(128 + s16[i & 15] / 2);
    }
    dch = sb.dma8;
    outp(0x0A, 4 | dch);
    outp(0x0C, 0);
    outp(0x0B, 0x58 | dch);
    outp(dch * 2, (unsigned char)lin);
    outp(dch * 2, (unsigned char)(lin >> 8));
    outp(page8[dch], (unsigned char)(lin >> 16));
    outp(dch * 2 + 1, 0xFF);
    outp(dch * 2 + 1, 0x0F);
    outp(0x0A, dch);
    sbirqs = 0;
    dsp_w(0xD1);
    dsp_w(0x40);
    dsp_w((unsigned char)(256 - 1000000L / 11025));
    dsp_w(0x48);
    dsp_w(0xFF);
    dsp_w(0x07);
    t = ticks();
    dsp_w(0x1C);
    while (ticks() - t < 9) ;
    outp(0x0C, 0);
    cnt = inp(dch * 2 + 1);
    cnt |= inp(dch * 2 + 1) << 8;
    while (ticks() - t < 18) ;
    dsp_w(0xDA);
    printf("  8-bit DMA sound, 1 s: %u interrupts (expected 5), DMA count %04Xh\n", sbirqs, cnt);
    bad += sb_check(sbirqs >= 4 && sbirqs <= 7, "DMA interrupt rate");
    t = ticks();
    while (ticks() - t < 4) ;
    /* FM: timer detection and a note */
    opl_w(4, 0x60);
    opl_w(4, 0x80);
    v1 = inp(0x388) & 0xE0;
    opl_w(2, 0xFF);
    opl_w(4, 0x21);
    t = ticks();
    while (ticks() - t < 2) ;
    v2 = inp(0x388) & 0xE0;
    opl_w(4, 0x60);
    opl_w(4, 0x80);
    bad += sb_check(v1 == 0 && v2 == 0xC0, "FM (AdLib detection)");
    opl_w(0x20, 0x01); opl_w(0x40, 0x10); opl_w(0x60, 0xF0); opl_w(0x80, 0x77);
    opl_w(0x23, 0x01); opl_w(0x43, 0x00); opl_w(0x63, 0xF0); opl_w(0x83, 0x77);
    opl_w(0xC0, 0x32);
    opl_w(0xA0, 0x44);
    opl_w(0xB0, 0x32);
    t = ticks();
    while (ticks() - t < 9) ;
    opl_w(0xB0, 0x12);
    /* MPU-401: a note on the synthesizer */
    if (sb.mpu) {
        outp(sb.mpu + 1, 0xFF);
        for (t = 0; (inp(sb.mpu + 1) & 0x80) && t < 60000UL; t++) ;
        v1 = inp(sb.mpu);
        outp(sb.mpu + 1, 0x3F);
        for (t = 0; (inp(sb.mpu + 1) & 0x80) && t < 60000UL; t++) ;
        v2 = inp(sb.mpu);
        bad += sb_check(v1 == 0xFE && v2 == 0xFE, "MPU-401 UART");
        outp(sb.mpu, 0x90);
        outp(sb.mpu, 60);
        outp(sb.mpu, 100);
        t = ticks();
        while (ticks() - t < 9) ;
        outp(sb.mpu, 0x80);
        outp(sb.mpu, 60);
        outp(sb.mpu, 0);
        outp(sb.mpu + 1, 0xFF);
    }
    if (sb.irq < 8) outp(0x21, inp(0x21) | (1 << sb.irq));
    _dos_setvect(vec, sb_oldvec);
    printf("%s\n", bad ? "Sound Blaster emulation: errors" : "Sound Blaster emulation works.");
}

static int ring_init(void)
{
    if (!ringmem) {
        ringmem = (unsigned char __far *)_fmalloc(RINGDATA + 32 + 16);
        if (!ringmem) die("memory", 4);
        /* the header must not wrap around a segment: start at offset 0 */
        ring = (struct winring __far *)MK_FP(FP_SEG(ringmem) + (FP_OFF(ringmem) + 15) / 16, 0);
    }
    _fmemset(ring, 0, 32);
    ring->magic = WINRING_MAGIC;
    return 0;
}

static unsigned char __far *rdata(void)
{
    return (unsigned char __far *)ring + 32;
}

static int open_stream(int dev, int dir, unsigned long rate, int bits, int ch, int flt)
{
    int st;
    unsigned fsz = (unsigned)(bits / 8 * ch);
    ring_init();
    ring->size = RINGDATA - RINGDATA % fsz;
    st = call_api(WINF_AUD_OPEN, (unsigned long)dev | ((unsigned long)dir << 8), rate,
                  (unsigned long)bits | ((unsigned long)ch << 8) | ((unsigned long)flt << 16), ring, 0);
    if (st) die("open", st);
    return (int)req.retlen;
}

static void ctl(int sid, int c, unsigned long v)
{
    call_api(WINF_AUD_CTL, (unsigned long)sid, (unsigned long)c, v, NULL, 0);
}

/* copy n bytes into the playback ring */
static void put(const unsigned char __far *p, unsigned n)
{
    unsigned long sz = ring->size;
    while (n) {
        unsigned off = (unsigned)(ring->wr % sz), k = (unsigned)MIN_U(n, sz - off);
        _fmemcpy(rdata() + off, p, k);
        ring->wr += k;
        p += k;
        n -= k;
    }
}

static unsigned long ring_free(void)
{
    return ring->size - (ring->wr - ring->rd);
}

static void wait_drain(void)
{
    unsigned long t0 = ticks();
    while (ring->pos != ring->wr && ticks() - t0 < 18 * 10) {
        if (keyhit()) break;
    }
}

/* ---------------------------------------------------------------- tone */

static short sine(unsigned ph)          /* ph 0..65535 -> -32767..32767, parabolic approximation */
{
    long x = (long)(ph & 0x7FFF) - 16384;       /* -16384..16383 */
    long y = 32767L - (x * x) / 8192L;         /* 0..32767 */
    return (short)((ph & 0x8000) ? -y : y);
}

static void tone(int dev, unsigned long rate, unsigned hz, unsigned secs)
{
    static short buf[1024];
    unsigned ph = 0, step = (unsigned)(((unsigned long)hz << 16) / rate);
    unsigned long total = rate * secs, done = 0;
    int sid = open_stream(dev, 0, rate, 16, 2, 0), i;
    printf("Playing %u Hz on device %d (%lu Hz 16-bit stereo, %u s), any key stops.\n", hz, dev, rate, secs);
    ctl(sid, AC_START, 0);
    while (done < total) {
        if (keyhit()) break;
        if (ring_free() < sizeof(buf)) continue;
        for (i = 0; i < 512; i++) {
            short v = (short)(sine(ph) / 4);
            buf[i * 2] = v;
            buf[i * 2 + 1] = (done / rate) & 1 ? (short)0 : v;   /* right channel every other second */
            ph += step;
        }
        put((unsigned char __far *)buf, sizeof(buf));
        done += 512;
    }
    wait_drain();
    if (ring->flags & RF_UNDERRUN) printf("(underrun reported)\n");
    ctl(sid, AC_CLOSE, 0);
}

/* ---------------------------------------------------------------- WAV */

struct wavfmt {
    unsigned short tag, ch;
    unsigned long rate, bps;
    unsigned short align, bits;
};

static int wav_open(FILE *f, struct wavfmt *fm, unsigned long *len)
{
    unsigned char h[12];
    unsigned char ck[8];
    int havefmt = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    while (fread(ck, 1, 8, f) == 8) {
        unsigned long n = *(unsigned long *)(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char b[40];
            unsigned r = (unsigned)(n < sizeof(b) ? n : sizeof(b));
            fread(b, 1, r, f);
            if (n > r) fseek(f, (long)(n - r), SEEK_CUR);
            memcpy(fm, b, 16);
            if (fm->tag == 0xFFFE && r >= 26) fm->tag = *(unsigned short *)(b + 24);
            havefmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            *len = n;
            return havefmt ? 0 : -1;
        } else {
            fseek(f, (long)((n + 1) & ~1UL), SEEK_CUR);
        }
    }
    return -1;
}

static void play(int dev, const char *name)
{
    static unsigned char buf[4096];
    struct wavfmt fm;
    unsigned long len, done = 0;
    FILE *f = fopen(name, "rb");
    int sid, flt;
    if (!f) die(name, 2);
    if (wav_open(f, &fm, &len)) {
        printf("AUDTEST: %s is not a WAV file\n", name);
        exit(1);
    }
    flt = fm.tag == 3;
    if ((fm.tag != 1 && fm.tag != 3) || !fm.ch) {
        printf("AUDTEST: format tag %u is not supported (PCM and IEEE float only)\n", fm.tag);
        exit(1);
    }
    sid = open_stream(dev, 0, fm.rate, fm.bits, fm.ch, flt);
    printf("%s: %lu Hz, %u bit%s, %u channel(s), %lu bytes. Any key stops.\n", name, fm.rate, fm.bits,
           flt ? " float" : "", fm.ch, len);
    while (done < len) {
        unsigned n;
        if (keyhit()) break;
        if (ring_free() < sizeof(buf)) {
            ctl(sid, AC_START, 0);      /* the ring is full: start (again) */
            continue;
        }
        n = (unsigned)(len - done < sizeof(buf) ? len - done : sizeof(buf));
        n -= n % (unsigned)(fm.bits / 8 * fm.ch);
        if (!n || fread(buf, 1, n, f) != n) break;
        put((unsigned char __far *)buf, n);
        done += n;
    }
    ctl(sid, AC_START, 0);
    wait_drain();
    printf("played %lu bytes%s\n", ring->pos, (ring->flags & RF_UNDERRUN) ? " (underruns)" : "");
    ctl(sid, AC_CLOSE, 0);
    fclose(f);
}

static void wav_header(FILE *f, unsigned long rate, int bits, int ch, unsigned long len)
{
    unsigned char h[44];
    unsigned long bps = rate * (unsigned long)(bits / 8 * ch);
    memcpy(h, "RIFF", 4);
    *(unsigned long *)(h + 4) = 36 + len;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(unsigned long *)(h + 16) = 16;
    *(unsigned short *)(h + 20) = 1;
    *(unsigned short *)(h + 22) = (unsigned short)ch;
    *(unsigned long *)(h + 24) = rate;
    *(unsigned long *)(h + 28) = bps;
    *(unsigned short *)(h + 32) = (unsigned short)(bits / 8 * ch);
    *(unsigned short *)(h + 34) = (unsigned short)bits;
    memcpy(h + 36, "data", 4);
    *(unsigned long *)(h + 40) = len;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void record(int dev, const char *name, unsigned secs, unsigned long rate)
{
    static unsigned char buf[4096];
    unsigned long want = rate * 4UL * secs, got = 0, last;
    FILE *f = fopen(name, "wb");
    int sid;
    if (!f) die(name, 3);
    wav_header(f, rate, 16, 2, 0);
    sid = open_stream(dev, 1, rate, 16, 2, 0);
    printf("Recording %u s at %lu Hz 16-bit stereo to %s, any key stops.\n", secs, rate, name);
    ctl(sid, AC_START, 0);
    last = ticks();
    while (got < want) {
        unsigned long avail = ring->wr - ring->rd;
        unsigned n, off, k;
        if (keyhit()) break;
        if (avail < sizeof(buf)) {
            if (ticks() - last > 18 * 3) {
                printf("AUDTEST: no data from the device for 3 seconds\n");
                break;
            }
            continue;
        }
        last = ticks();
        n = sizeof(buf);
        off = (unsigned)(ring->rd % ring->size);
        k = (unsigned)MIN_U(n, ring->size - off);
        _fmemcpy((void __far *)buf, rdata() + off, k);
        if (k < n) _fmemcpy((void __far *)(buf + k), rdata(), n - k);
        ring->rd += n;
        fwrite(buf, 1, n, f);
        got += n;
    }
    ctl(sid, AC_CLOSE, 0);
    wav_header(f, rate, 16, 2, got);
    fclose(f);
    printf("%lu bytes recorded%s\n", got, (ring->flags & RF_OVERRUN) ? " (overruns)" : "");
}

/* ---------------------------------------------------------------- MIDI */

static int mdev = 0xFF;

static void midi(unsigned char a, unsigned char b, unsigned char c)
{
    int st = call_api(WINF_AUD_MIDI, (unsigned long)mdev, (unsigned long)a | ((unsigned long)b << 8) |
                      ((unsigned long)c << 16), 0, NULL, 0);
    if (st) die("MIDI", st);
}

static void wait_ticks(unsigned n)
{
    unsigned long t0 = ticks();
    while (ticks() - t0 < n) ;
}

static void miditest(int dev)
{
    static const unsigned char scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    static const unsigned char progs[] = { 0, 24, 40, 56, 73, 19, 81, 11 };
    int i, k;
    mdev = dev;
    printf("MIDI test on %s: scales with 8 instruments, chords, drums. Any key stops.\n",
           dev == 0xFF ? "the synthesizer" : "the selected device");
    for (k = 0; k < 8; k++) {
        midi(0xC0, progs[k], 0);
        for (i = 0; i < 8; i++) {
            midi(0x90, scale[i], 100);
            wait_ticks(3);
            midi(0x80, scale[i], 0);
            if (keyhit()) goto out;
        }
    }
    midi(0xC0, 48, 0);
    midi(0xC1, 32, 0);
    for (k = 0; k < 4; k++) {
        static const unsigned char ch[4][3] = { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } };
        for (i = 0; i < 3; i++) midi(0x90, ch[k][i], 90);
        midi(0x91, (unsigned char)(ch[k][0] - 24), 110);
        for (i = 0; i < 8; i++) {
            midi(0x99, (i & 3) == 0 ? 36 : (i & 3) == 2 ? 38 : 42, 110);
            midi(0x99, 42, 70);
            wait_ticks(4);
            if (keyhit()) goto out;
        }
        for (i = 0; i < 3; i++) midi(0x80, ch[k][i], 0);
        midi(0x81, (unsigned char)(ch[k][0] - 24), 0);
    }
    midi(0x99, 49, 120);
    midi(0x99, 36, 120);
    wait_ticks(30);
out:
    for (i = 0; i < 16; i++) midi((unsigned char)(0xB0 + i), 123, 0);
}

/* ---------------------------------------------------------------- mixer */

static const char *const mxname[] = { "master", "wave", "synth", "input", "cd", "line" };

static void mixer_show(int dev)
{
    int i;
    for (i = 0; i < MX_COUNT; i++) {
        unsigned long v;
        if (call_api(WINF_AUD_MIX, (unsigned long)dev, (unsigned long)i, 0, NULL, 0)) continue;
        v = req.retlen;
        printf("  %-7s %3lu%% / %3lu%%\n", mxname[i], ((v & 0xFFFF) * 100 + 32767) / 65535,
               ((v >> 16) * 100 + 32767) / 65535);
    }
}

static void mixer_set(int dev, const char *what, const char *val)
{
    static const char *const srcs[] = { "MIC", "LINE", "CD", "AUX", "MIX" };
    int i, st;
    unsigned long v;
    if (!stricmp(what, "SRC")) {
        for (i = 0; i < 5 && stricmp(val, srcs[i]); i++) ;
        if (i == 5) die("source (MIC, LINE, CD, AUX or MIX)", 1);
        st = call_api(WINF_AUD_MIX, (unsigned long)dev, MX_SRC | 0x80000000UL, 1UL << i, NULL, 0);
        if (st) die("record source", st);
        if (req.retlen != (1UL << i)) printf("AUDTEST: the device has no %s input\n", srcs[i]);
        return;
    }
    for (i = 0; i < MX_COUNT && stricmp(what, mxname[i]); i++) ;
    if (i == MX_COUNT) die("control (MASTER, WAVE, SYNTH, INPUT, CD, LINE, SRC)", 1);
    v = (unsigned long)atoi(val);
    if (v > 100) v = 100;
    v = v * 65535UL / 100;
    st = call_api(WINF_AUD_MIX, (unsigned long)dev, (unsigned long)i | 0x80000000UL, v | (v << 16), NULL, 0);
    if (st) die("mixer", st);
}

static void showlog(void)
{
    int st = call_api(WINF_LOG, 0, 0, 0, (void __far *)logbuf, sizeof(logbuf) - 1);
    if (st) die("log", st);
    logbuf[req.retlen < sizeof(logbuf) ? (unsigned)req.retlen : sizeof(logbuf) - 1] = 0;
    fputs(logbuf, stdout);
}

static void usage(void)
{
    printf("AUDTEST 1.1 - WINAUDIO test tool\n\n"
           "  AUDTEST                      list sound devices\n"
           "  AUDTEST TONE [hz] [dev]      2-second test tone (left, then both channels)\n"
           "  AUDTEST PLAY file.wav [dev]  play a WAV file (PCM 8..32 bit, float, any rate)\n"
           "  AUDTEST REC file.wav [s] [dev] [rate]   record (16-bit stereo, default 5 s 44100 Hz)\n"
           "  AUDTEST MIDI [dev]           MIDI synthesizer test\n"
           "  AUDTEST MIX [dev]            show the mixer\n"
           "  AUDTEST MIX dev control 0..100   control: MASTER WAVE SYNTH INPUT CD LINE\n"
           "  AUDTEST MIX dev SRC MIC|LINE|CD|AUX|MIX  record source\n"
           "  AUDTEST SB                   test the Sound Blaster 16 emulation (DSP, IRQ, DMA, FM, MPU)\n"
           "  AUDTEST LOG                  driver log\n"
           "  /USB                         use the USB audio devices of WINUSB.SYS\n");
}

int main(int argc, char **argv)
{
    int i, n = 0;
    char *a[8];
    mplex = WIN_MPLEX_AUDIO;
    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "/USB")) mplex = WIN_MPLEX_USB;
        else if (!strcmp(argv[i], "/?")) {
            usage();
            return 0;
        } else if (n < 8) a[n++] = argv[i];
    }
    if (!detect(mplex)) {
        printf("AUDTEST: %s is not loaded.\n", mplex == WIN_MPLEX_USB ? "WINUSB.SYS" : "WINAUDIO.SYS");
        return 1;
    }
    if (n && !stricmp(a[0], "LOG")) {
        showlog();
        return 0;
    }
    load_devs();
    if (!n) {
        list();
        sb_show();
        return 0;
    }
    if (!stricmp(a[0], "TONE")) tone(n > 2 ? atoi(a[2]) : 0, 44100, n > 1 ? atoi(a[1]) : 440, 2);
    else if (!stricmp(a[0], "PLAY") && n > 1) play(n > 2 ? atoi(a[2]) : 0, a[1]);
    else if (!stricmp(a[0], "REC") && n > 1)
        record(n > 3 ? atoi(a[3]) : 0, a[1], n > 2 ? atoi(a[2]) : 5, n > 4 ? atol(a[4]) : 44100);
    else if (!stricmp(a[0], "MIDI")) miditest(n > 1 ? atoi(a[1]) : 0xFF);
    else if (!stricmp(a[0], "SB")) sbtest();
    else if (!stricmp(a[0], "MIX") && n < 3) {
        int d = n > 1 ? atoi(a[1]) : 0;
        printf("Mixer of device %d:\n", d);
        mixer_show(d);
    } else if (!stricmp(a[0], "MIX") && n >= 4) mixer_set(atoi(a[1]), a[2], a[3]);
    else usage();
    return 0;
}
