/* winsound.c - WINSOUND.DRV: MMSYSTEM sound driver for Windows 3.1 and Windows 3.0 with the
   Multimedia Extensions. Wave output and input, the MIDI synthesizer (and USB MIDI ports)
   and auxiliary volumes of the WINAUDIO (HD Audio, AC'97) and WINUSB (USB Audio) drivers.

   Every open stream has a ring buffer in conventional memory (GlobalDosAlloc) that the
   driver core reads (playback) or writes (recording) on its own. A periodic multimedia
   timer moves data between the application's buffers and the rings and returns finished
   buffers. Requests go to the core through the VxD's protected-mode entry (enhanced mode)
   or a simulated real-mode INT 2Fh (standard mode). */
#include <windows.h>
#include <mmsystem.h>
#include "winapi.h"

/* ---- installable driver and MMDDK definitions ---- */
#define DRV_LOAD            0x0001
#define DRV_ENABLE          0x0002
#define DRV_OPEN            0x0003
#define DRV_CLOSE           0x0004
#define DRV_DISABLE         0x0005
#define DRV_FREE            0x0006
#define DRV_CONFIGURE       0x0007
#define DRV_QUERYCONFIGURE  0x0008
#define DRV_INSTALL         0x0009
#define DRV_REMOVE          0x000A
#define DRVCNF_RESTART      0x0002

#define DCB_TYPEMASK        0x0007

#define WODM_GETNUMDEVS     3
#define WODM_GETDEVCAPS     4
#define WODM_OPEN           5
#define WODM_CLOSE          6
#define WODM_PREPARE        7
#define WODM_UNPREPARE      8
#define WODM_WRITE          9
#define WODM_PAUSE          10
#define WODM_RESTART        11
#define WODM_RESET          12
#define WODM_GETPOS         13
#define WODM_GETPITCH       14
#define WODM_SETPITCH       15
#define WODM_GETVOLUME      16
#define WODM_SETVOLUME      17
#define WODM_GETPLAYBACKRATE 18
#define WODM_SETPLAYBACKRATE 19
#define WODM_BREAKLOOP      20

#define WIDM_GETNUMDEVS     50
#define WIDM_GETDEVCAPS     51
#define WIDM_OPEN           52
#define WIDM_CLOSE          53
#define WIDM_PREPARE        54
#define WIDM_UNPREPARE      55
#define WIDM_ADDBUFFER      56
#define WIDM_START          57
#define WIDM_STOP           58
#define WIDM_RESET          59
#define WIDM_GETPOS         60

#define MODM_GETNUMDEVS     1
#define MODM_GETDEVCAPS     2
#define MODM_OPEN           3
#define MODM_CLOSE          4
#define MODM_PREPARE        5
#define MODM_UNPREPARE      6
#define MODM_DATA           7
#define MODM_LONGDATA       8
#define MODM_RESET          9
#define MODM_GETVOLUME      10
#define MODM_SETVOLUME      11
#define MODM_CACHEPATCHES   12
#define MODM_CACHEDRUMPATCHES 13

#define MIDM_GETNUMDEVS     53

#define AUXDM_GETNUMDEVS    3
#define AUXDM_GETDEVCAPS    4
#define AUXDM_GETVOLUME     5
#define AUXDM_SETVOLUME     6

typedef struct {
    HWAVE hWave;
    const WAVEFORMAT FAR *lpFormat;
    DWORD dwCallback;
    DWORD dwInstance;
} WAVEOPENDESC, FAR *LPWAVEOPENDESC;

typedef struct {
    HMIDI hMidi;
    DWORD dwCallback;
    DWORD dwInstance;
} MIDIOPENDESC, FAR *LPMIDIOPENDESC;

BOOL WINAPI DriverCallback(DWORD dwCallback, UINT uFlags, HANDLE hDevice, UINT uMessage, DWORD dwUser,
                           DWORD dwParam1, DWORD dwParam2);
DWORD WINAPI GlobalDosAlloc(DWORD cb);
UINT WINAPI GlobalDosFree(UINT sel);

#define WAVE_FORMAT_IEEE_FLOAT  3
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MK_FP(s, o) ((void __far *)(((DWORD)(s) << 16) | (WORD)(o)))

/* ---- devices ---- */
#define NCORE       2
#define MAXDEV      16
#define MAXOPEN     12
#define REQBLK      2048        /* DOS block: request, device list, MIDI buffer */
#define RQ_DEVS     64
#define RQ_MIDI     1024
#define MIDICHUNK   1024

struct sdev {
    BYTE core, id, type, caps, insrcs, gone, ctl, rsv;
    DWORD rate;
    char name[32];
};

static const BYTE mplexes[NCORE] = { WIN_MPLEX_AUDIO, WIN_MPLEX_USB };
static const WORD vxdids[NCORE] = { WIN_VXD_AUDIO, WIN_VXD_USB };
static BYTE present[NCORE];
static DWORD vxdentry[NCORE];
static WORD reqsel, reqseg;
static struct winapireq FAR *rq;
static int scanned, enhanced;

static struct sdev wouts[MAXDEV], wins[MAXDEV], mouts[MAXDEV], auxs[MAXDEV];
static int nwout, nwin, nmout, naux;

/* ---- open streams ---- */
#define WQLEN       32
#define QAT(w, i)   ((w)->q[((w)->qh + (i)) % WQLEN])
struct wstream {
    BYTE used, dir, core, dev, started, paused, gone, loopon;
    WORD sid;
    WORD ringsel, ringseg;
    DWORD size;                 /* ring data bytes */
    WORD align;                 /* bytes per frame */
    DWORD rate;
    HWAVE hwave;
    DWORD cb, inst;
    WORD cbflags;
    /* Queue of the application's headers. Kept here: the MMSYSTEM of Windows 3.0 with
       Multimedia Extensions uses lpNext and reserved of WAVEHDR itself. */
    LPWAVEHDR q[WQLEN];
    DWORD qend[WQLEN];          /* playback: ring position where the buffer ends */
    BYTE qh, qn;                /* first entry, number of entries */
    BYTE cdone;                 /* playback: entries completely copied into the ring */
    signed char lsi;            /* entry that begins the loop, -1 none */
    DWORD curoff;               /* playback: bytes of entry cdone copied */
    DWORD loops;
    DWORD base;                 /* ring position of byte 0 */
};

struct mstream {
    BYTE used, core, dev, status;
    HMIDI hmidi;
    DWORD cb, inst;
    WORD cbflags;
};

static struct wstream ws[MAXOPEN];
static struct mstream ms[MAXOPEN];
static int nopen;
static UINT timer_id;
static volatile int busy;

/* ---------------------------------------------------------------- helpers */

static void fmemcpy(void FAR *d, const void FAR *s, WORD n)
{
    BYTE FAR *dd = d;
    const BYTE FAR *ss = s;
    while (n--) *dd++ = *ss++;
}

static void fmemset(void FAR *d, BYTE v, WORD n)
{
    BYTE FAR *dd = d;
    while (n--) *dd++ = v;
}

static void fstrncpy(char FAR *d, const char FAR *s, int n)
{
    while (--n > 0 && *s) *d++ = *s++;
    *d = 0;
}

static void cli(void);
#pragma aux cli = "cli";
static void sti(void);
#pragma aux sti = "sti";

/* ---------------------------------------------------------------- core API */

static int realmode;          /* Windows 3.0 real mode: no DPMI */

static int api(int k, WORD fn, DWORD a0, DWORD a1, DWORD a2, DWORD buf, DWORD len)
{
    WORD rseg = reqseg, mx = ((WORD)mplexes[k] << 8) | 1;
    DWORD ent = vxdentry[k];
    if (!rq || !present[k]) return WINE_NOCORE;
    fmemset(rq, 0, sizeof(*rq));
    rq->fn = fn;
    rq->status = 0xFFFF;
    rq->arg0 = a0;
    rq->arg1 = a1;
    rq->arg2 = a2;
    rq->buf = buf;
    rq->buflen = len;
    if (ent) {
        _asm {
            mov ax, 3
            mov cx, rseg
            xor dx, dx
            call dword ptr ent
        }
    } else if (realmode) {
        /* real mode (Windows 3.0 WIN /R): the driver is called directly, ES:BX -> request */
        _asm {
            push es
            mov ax, mx
            mov es, rseg
            xor bx, bx
            int 2Fh
            pop es
        }
    } else {
        /* DPMI: simulate real-mode INT 2Fh, ES:BX -> request */
        static struct {
            DWORD edi, esi, ebp, rsv, ebx, edx, ecx, eax;
            WORD flags, es, ds, fs, gs, ip, cs, sp, ss;
        } rmc;
        fmemset(&rmc, 0, sizeof(rmc));
        rmc.eax = mx;
        rmc.es = rseg;
        _asm {
            push es
            push di
            push ds
            pop es
            lea di, rmc
            mov ax, 0300h
            mov bx, 002Fh
            xor cx, cx
            int 31h
            pop di
            pop es
        }
    }
    return rq->status;
}

#define RQBUF(off) (((DWORD)reqseg << 16) | (off))

static void add_dev(struct sdev *tab, int *n, int core, struct winauddev FAR *d, const char *suffix)
{
    struct sdev *s;
    char FAR *p;
    int i = 0;
    if (*n >= MAXDEV) return;
    s = &tab[(*n)++];
    s->core = (BYTE)core;
    s->id = d->id;
    s->type = d->type;
    s->caps = d->caps;
    s->insrcs = d->insrcs;
    s->rate = d->rate;
    s->gone = d->gone;
    s->ctl = 0;
    s->name[0] = 'W';
    s->name[1] = 'I';
    s->name[2] = 'N';
    s->name[3] = ' ';
    p = d->name;
    for (i = 4; i < 31 && *p; i++) s->name[i] = *p++;
    if (suffix) {
        const char *q = suffix;
        if (i > 31 - lstrlen(suffix)) i = 31 - lstrlen(suffix);
        while (*q && i < 31) s->name[i++] = *q++;
    }
    s->name[i] = 0;
}

static void set_name(struct sdev *s, const char *name)
{
    fstrncpy(s->name, name, 32);
}

static void scan(void);

/* finds the entry of core device (k, id) in tab; ctl 1 = the synthesizer of core k */
static struct sdev *find_dev(struct sdev *tab, int n, int k, int id, int ctl)
{
    int i;
    for (i = 0; i < n; i++)
        if (tab[i].core == k && tab[i].ctl == ctl && (ctl == 1 || tab[i].id == id)) return &tab[i];
    return NULL;
}

/* MMSYSTEM counts the devices of a driver only when it loads the driver. USB audio devices
   plugged in later take one of these reserved entries of the USB core; an entry that is
   not connected fails to open, so WAVE_MAPPER and sndPlaySound move on to the next device. */
#define USBSLOTS    2
#define NOID        0xFF

static void placeholder(struct sdev *tab, int *n, const char *name, int num, int ctl)
{
    struct sdev *e;
    if (*n >= MAXDEV) return;
    e = &tab[(*n)++];
    fmemset(e, 0, sizeof(*e));
    e->core = 1;
    e->id = NOID;
    e->gone = 1;
    e->ctl = (BYTE)ctl;
    e->type = 3;
    e->caps = WINAC_OUT | WINAC_IN;
    e->rate = 48000;
    wsprintf(e->name, num ? "%s %d" : "%s", (LPSTR)name, num);
}

static void reserve_usb(void)
{
    int i, have;
    if (!present[1]) return;
    for (i = have = 0; i < nwout; i++) have += wouts[i].core == 1;
    for (i = have; i < USBSLOTS; i++) placeholder(wouts, &nwout, "WIN USB Audio", i + 1, 0);
    for (i = have = 0; i < nwin; i++) have += wins[i].core == 1;
    for (i = have; i < USBSLOTS; i++) placeholder(wins, &nwin, "WIN USB Audio In", i + 1, 0);
    for (i = have = 0; i < nmout; i++) have += mouts[i].core == 1 && mouts[i].ctl == 0;
    for (i = have; i < USBSLOTS; i++) placeholder(mouts, &nmout, "WIN USB MIDI", i + 1, 0);
    for (i = have = 0; i < nmout; i++) have += mouts[i].core == 1 && mouts[i].ctl == 1;
    if (!have) placeholder(mouts, &nmout, "WIN USB GM Synth", 0, 1);
}

/* entry for core device d: the known one, else a new one (while the driver loads), else
   a free reserved entry of the core */
static struct sdev *bind_dev(struct sdev *tab, int *n, int k, struct winauddev FAR *d, const char *suffix,
                             int loading)
{
    struct sdev *e = find_dev(tab, *n, k, d->id, 0);
    int i;
    if (e) return e;
    if (loading) {
        add_dev(tab, n, k, d, suffix);
        return NULL;
    }
    for (i = 0; i < *n; i++)
        if (tab[i].core == k && tab[i].ctl == 0 && tab[i].id == NOID) break;
    if (i == *n)
        for (i = 0; i < *n; i++)
            if (tab[i].core == k && tab[i].ctl == 0 && tab[i].gone) break;
    if (i == *n) return NULL;
    tab[i].id = d->id;
    tab[i].type = d->type;
    tab[i].caps = d->caps;
    tab[i].insrcs = d->insrcs;
    tab[i].rate = d->rate;
    return &tab[i];
}

/* Device list of core k: adds the devices found while the driver loads, later binds USB
   devices plugged in to reserved entries and updates the removed state. Entries are never
   taken away, so the device numbers MMSYSTEM gives out stay valid. */
static void merge(int k, int loading)
{
    int nd, i, synth = 0;
    struct winauddev FAR *d;
    struct sdev *e;
    if (!present[k] || api(k, WINF_AUD_DEVS, 0, 0, 0, RQBUF(RQ_DEVS), REQBLK - RQ_DEVS - 64)) return;
    nd = (int)(rq->retlen / sizeof(struct winauddev));
    d = MK_FP(reqsel, RQ_DEVS);
    for (i = 0; i < nmout; i++)
        if (mouts[i].core == k && mouts[i].ctl == 1) mouts[i].gone = 1;
    for (i = 0; i < nd; i++, d++) {
        if ((d->caps & WINAC_OUT) && (e = bind_dev(wouts, &nwout, k, d, NULL, loading)) != NULL) e->gone = d->gone;
        if ((d->caps & WINAC_IN) && (e = bind_dev(wins, &nwin, k, d, NULL, loading)) != NULL) e->gone = d->gone;
        if ((d->caps & WINAC_MIDI) && (e = bind_dev(mouts, &nmout, k, d, " MIDI", loading)) != NULL) e->gone = d->gone;
        if ((d->caps & WINAC_SYNTH) && !d->gone && !synth) {
            /* the synthesizer plays on the first present device of the core */
            synth = 1;
            if ((e = find_dev(mouts, nmout, k, d->id, 1)) == NULL && loading && nmout < MAXDEV) {
                add_dev(mouts, &nmout, k, d, NULL);
                e = &mouts[nmout - 1];
                set_name(e, k ? "WIN USB GM Synth" : "WIN GM Synth");
                e->ctl = 1;
            }
            if (e) {
                e->id = d->id;
                e->gone = 0;
            }
        }
        if (loading && (d->caps & WINAC_OUT) && k == 0) {
            add_dev(auxs, &naux, k, d, NULL);
            set_name(&auxs[naux - 1], "WIN Master Volume");
            auxs[naux - 1].ctl = MX_MASTER;
            if (d->type == 2) {     /* AC'97: analog CD and line inputs */
                add_dev(auxs, &naux, k, d, NULL);
                set_name(&auxs[naux - 1], "WIN CD Audio");
                auxs[naux - 1].ctl = MX_CD;
                add_dev(auxs, &naux, k, d, NULL);
                set_name(&auxs[naux - 1], "WIN Line In");
                auxs[naux - 1].ctl = MX_LINE;
            }
        }
    }
}

/* device list again (GETNUMDEVS): picks up USB audio devices plugged in or removed */
static void rescan(void)
{
    int k;
    scan();
    if (busy) return;
    busy++;
    for (k = 0; k < NCORE; k++) merge(k, 0);
    busy--;
}

static void scan(void)
{
    int k;
    if (scanned) return;
    scanned = 1;
    enhanced = (GetWinFlags() & WF_ENHANCED) != 0;
    realmode = !(GetWinFlags() & WF_PMODE);
    if (!rq) {
        DWORD r = GlobalDosAlloc(REQBLK);
        if (!r) return;
        reqsel = LOWORD(r);
        reqseg = HIWORD(r);
        rq = MK_FP(reqsel, 0);
    }
    for (k = 0; k < NCORE; k++) {
        BYTE id = mplexes[k], r_al = 0;
        WORD r_bx = 0, vid = vxdids[k], es_ = 0, di_ = 0;
        _asm {
            push es
            push di
            mov ah, id
            mov al, 0
            xor bx, bx
            int 2Fh
            mov r_al, al
            mov r_bx, bx
            pop di
            pop es
        }
        present[k] = 0;
        vxdentry[k] = 0;
        if (r_al == 0xFF && r_bx == 0x3357) present[k] = 1;
        if (enhanced) {
            _asm {
                push es
                push di
                xor di, di
                mov es, di
                mov ax, 1684h
                mov bx, vid
                int 2Fh
                mov es_, es
                mov di_, di
                pop di
                pop es
            }
            if (es_ || di_) {
                DWORD ent = ((DWORD)es_ << 16) | di_;
                WORD started = 0;
                vxdentry[k] = ent;
                /* Windows 95/98/ME without the DOS driver: the VxD runs on its own (AX=0: BX = started) */
                _asm {
                    xor ax, ax
                    xor bx, bx
                    call dword ptr ent
                    mov started, bx
                }
                if (started) present[k] = 1;
            }
        }
        if (!present[k]) continue;
        merge(k, 1);
    }
    reserve_usb();
}

/* ---------------------------------------------------------------- timer */

static void wave_service(struct wstream *w);

void CALLBACK __loadds TimerProc(UINT id, UINT msg, DWORD user, DWORD d1, DWORD d2)
{
    int i;
    (void)id; (void)msg; (void)user; (void)d1; (void)d2;
    if (busy) return;
    busy = 1;
    for (i = 0; i < MAXOPEN; i++)
        if (ws[i].used) wave_service(&ws[i]);
    busy = 0;
}

static void opened(void)
{
    if (nopen++ == 0 && !timer_id) {
        timeBeginPeriod(10);
        timer_id = timeSetEvent(10, 5, (LPTIMECALLBACK)TimerProc, 0, TIME_PERIODIC);
    }
}

static void closed(void)
{
    if (--nopen == 0 && timer_id) {
        timeKillEvent(timer_id);
        timeEndPeriod(10);
        timer_id = 0;
    }
}

/* ---------------------------------------------------------------- wave */

static void callback(struct wstream *w, UINT msg, DWORD p1)
{
    if (w->cbflags) DriverCallback(w->cb, w->cbflags, (HANDLE)w->hwave, msg, w->inst, p1, 0);
}

static struct winring FAR *ringp(struct wstream *w)
{
    return MK_FP(w->ringsel, 0);
}

/* removes the first queue entry */
static LPWAVEHDR qpop(struct wstream *w)
{
    LPWAVEHDR h = w->q[w->qh];
    w->qh = (BYTE)((w->qh + 1) % WQLEN);
    w->qn--;
    if (w->cdone) w->cdone--;
    if (w->lsi >= 0) w->lsi--;
    h->dwFlags &= ~WHDR_INQUEUE;
    h->dwFlags |= WHDR_DONE;
    return h;
}

/* playback: copy queued data into the ring */
static void out_fill(struct wstream *w)
{
    struct winring FAR *r = ringp(w);
    BYTE FAR *data = (BYTE FAR *)r + 32;
    while (w->cdone < w->qn) {
        int i = w->cdone;
        LPWAVEHDR h = QAT(w, i);
        DWORD left = h->dwBufferLength - w->curoff, space = w->size - (r->wr - r->rd);
        WORD n, off;
        if (h->dwFlags & WHDR_BEGINLOOP && w->curoff == 0 && w->lsi < 0) {
            w->lsi = (signed char)i;
            w->loops = h->dwLoops;
            w->loopon = w->loops > 1;
        }
        if (!left) {
            w->qend[(w->qh + i) % WQLEN] = r->wr;   /* fully queued: done when played up to here */
            w->curoff = 0;
            if ((h->dwFlags & WHDR_ENDLOOP) && w->lsi >= 0) {
                if (w->loops > 1) {
                    w->loops--;
                    w->cdone = (BYTE)w->lsi;
                    if (w->loops == 1) w->loopon = 0;
                    continue;
                }
                w->lsi = -1;
                w->loopon = 0;
            }
            w->cdone++;
            continue;
        }
        if (space < w->align) break;
        off = (WORD)(r->wr % w->size);
        n = (WORD)MIN(left, space);
        if (n > w->size - off) n = (WORD)(w->size - off);
        if (n > 0x8000) n = 0x8000;
        n -= n % w->align;
        if (!n) break;
        fmemcpy(data + off, h->lpData + w->curoff, n);
        w->curoff += n;
        r->wr += n;
    }
}

static void out_done(struct wstream *w)
{
    struct winring FAR *r = ringp(w);
    if (r->flags & RF_GONE) w->gone = 1;
    if (w->gone && w->cdone < w->qn) {
        /* removed device: everything is "played" */
        w->cdone = w->qn;
        w->curoff = 0;
        w->loopon = 0;
    }
    while (w->cdone) {
        if (w->loopon && w->lsi == 0) break;
        if (!w->gone && (long)(r->pos - w->qend[w->qh]) < 0) break;
        callback(w, WOM_DONE, (DWORD)qpop(w));
    }
}

/* recording: move captured data into the application's buffers */
static void in_take(struct wstream *w)
{
    struct winring FAR *r = ringp(w);
    BYTE FAR *data = (BYTE FAR *)r + 32;
    if (r->flags & RF_GONE) w->gone = 1;
    while (w->qn) {
        LPWAVEHDR h = QAT(w, 0);
        DWORD avail = r->wr - r->rd, room = h->dwBufferLength - h->dwBytesRecorded;
        WORD n, off;
        if (room && avail) {
            off = (WORD)(r->rd % w->size);
            n = (WORD)MIN(avail, room);
            if (n > w->size - off) n = (WORD)(w->size - off);
            if (n > 0x8000) n = 0x8000;
            fmemcpy(h->lpData + h->dwBytesRecorded, data + off, n);
            h->dwBytesRecorded += n;
            r->rd += n;
            continue;
        }
        if (room && !w->gone) break;
        callback(w, WIM_DATA, (DWORD)qpop(w));
    }
}

static void wave_service(struct wstream *w)
{
    if (w->dir) {
        if (w->started) in_take(w);
    } else {
        out_fill(w);
        out_done(w);
    }
}

static int check_format(const WAVEFORMAT FAR *f, WORD *bits)
{
    WORD b;
    if (f->wFormatTag != WAVE_FORMAT_PCM && f->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) return 0;
    b = ((const PCMWAVEFORMAT FAR *)f)->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && b != 32) return 0;
    if (b != 8 && b != 16 && b != 24 && b != 32) return 0;
    if (!f->nChannels || f->nChannels > 8) return 0;
    if (f->nSamplesPerSec < 2000 || f->nSamplesPerSec > 192000L) return 0;
    if (f->nBlockAlign != f->nChannels * (b / 8)) return 0;
    *bits = b;
    return 1;
}

static DWORD wave_open(int dir, struct sdev *dev, LPWAVEOPENDESC od, DWORD flags, DWORD FAR *user)
{
    struct wstream *w = NULL;
    const WAVEFORMAT FAR *f = od->lpFormat;
    WORD bits;
    DWORD bps, size, r;
    int i, st;
    if (!check_format(f, &bits)) return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (dev->gone) return MMSYSERR_NODRIVER;
    for (i = 0; i < MAXOPEN; i++)
        if (!ws[i].used) {
            w = &ws[i];
            break;
        }
    if (!w) return MMSYSERR_ALLOCATED;
    fmemset(w, 0, sizeof(*w));
    w->lsi = -1;
    w->align = f->nBlockAlign;
    w->rate = f->nSamplesPerSec;
    /* ring: about 250 ms, 8..60 KB */
    bps = f->nSamplesPerSec * f->nBlockAlign;
    size = bps / 4;
    if (size < 8192) size = 8192;
    if (size > 61440L) size = 61440L;
    size -= size % w->align;
    r = GlobalDosAlloc(size + 32);
    if (!r) return MMSYSERR_NOMEM;
    w->ringsel = LOWORD(r);
    w->ringseg = HIWORD(r);
    w->size = size;
    fmemset(MK_FP(w->ringsel, 0), 0, 32);
    ringp(w)->magic = WINRING_MAGIC;
    ringp(w)->size = size;
    st = api(dev->core, WINF_AUD_OPEN, (DWORD)dev->id | ((DWORD)dir << 8), f->nSamplesPerSec,
             (DWORD)bits | ((DWORD)f->nChannels << 8) | (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? 0x10000L : 0),
             (DWORD)w->ringseg << 16, 0);
    if (st) {
        GlobalDosFree(w->ringsel);
        return st == WINE_BUSY ? MMSYSERR_ALLOCATED : MMSYSERR_ERROR;
    }
    w->sid = (WORD)rq->retlen;
    w->dir = (BYTE)dir;
    w->core = dev->core;
    w->dev = dev->id;
    w->hwave = od->hWave;
    w->cb = od->dwCallback;
    w->inst = od->dwInstance;
    w->cbflags = HIWORD(flags) & DCB_TYPEMASK;
    w->used = 1;
    *user = (DWORD)(WORD)(w - ws) + 1;
    opened();
    callback(w, dir ? WIM_OPEN : WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static struct wstream *wfrom(DWORD user, int dir)
{
    if (!user || user > MAXOPEN || !ws[(WORD)user - 1].used || ws[(WORD)user - 1].dir != dir) return NULL;
    return &ws[(WORD)user - 1];
}

static void wave_ctl(struct wstream *w, int c)
{
    api(w->core, WINF_AUD_CTL, w->sid, (DWORD)c, 0, 0, 0);
}

/* reset: return every buffer */
static void wave_reset(struct wstream *w)
{
    struct winring FAR *r = ringp(w);
    busy++;
    wave_ctl(w, AC_RESET);
    if (w->dir) in_take(w);
    while (w->qn) callback(w, w->dir ? WIM_DATA : WOM_DONE, (DWORD)qpop(w));
    w->cdone = 0;
    w->curoff = 0;
    w->lsi = -1;
    w->loopon = 0;
    w->started = 0;
    w->base = w->dir ? r->wr : r->rd;
    busy--;
}

static DWORD wave_close(struct wstream *w)
{
    if (w->qn) return WAVERR_STILLPLAYING;
    wave_ctl(w, AC_CLOSE);
    callback(w, w->dir ? WIM_CLOSE : WOM_CLOSE, 0);
    GlobalDosFree(w->ringsel);
    w->used = 0;
    closed();
    return MMSYSERR_NOERROR;
}

/* Buffers are used at interrupt time (timer callback): the header and the data are page
   locked here. The MMSYSTEM of Windows 3.0 with Multimedia Extensions does not do that for
   the driver, and a page fault at interrupt time stops Windows. */
static DWORD wave_prepare(LPWAVEHDR h, int lock)
{
    if (lock) {
        if (h->dwFlags & WHDR_PREPARED) return MMSYSERR_NOERROR;
        GlobalPageLock((HGLOBAL)(WORD)((DWORD)(void FAR *)(h) >> 16));
        GlobalPageLock((HGLOBAL)(WORD)((DWORD)(void FAR *)(h->lpData) >> 16));
        h->dwFlags |= WHDR_PREPARED;
    } else {
        if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
        if (!(h->dwFlags & WHDR_PREPARED)) return MMSYSERR_NOERROR;
        GlobalPageUnlock((HGLOBAL)(WORD)((DWORD)(void FAR *)(h->lpData) >> 16));
        GlobalPageUnlock((HGLOBAL)(WORD)((DWORD)(void FAR *)(h) >> 16));
        h->dwFlags &= ~WHDR_PREPARED;
    }
    return MMSYSERR_NOERROR;
}

static int queue(struct wstream *w, LPWAVEHDR h)
{
    int r = 0;
    busy++;
    if (w->qn >= WQLEN) {
        r = -1;
    } else {
        h->dwFlags &= ~WHDR_DONE;
        h->dwFlags |= WHDR_INQUEUE;
        w->q[(w->qh + w->qn) % WQLEN] = h;
        w->qn++;
    }
    busy--;
    return r;
}

static DWORD getpos(struct wstream *w, MMTIME FAR *t, DWORD size)
{
    struct winring FAR *r = ringp(w);
    DWORD b = r->pos - w->base;
    if (size < 6) return MMSYSERR_INVALPARAM;
    if (t->wType == TIME_SAMPLES) t->u.sample = b / w->align;
    else if (t->wType == TIME_MS) {
        DWORD f = b / w->align;
        t->u.ms = f / w->rate * 1000 + f % w->rate * 1000 / w->rate;
    }
    else {
        t->wType = TIME_BYTES;
        t->u.cb = b;
    }
    return MMSYSERR_NOERROR;
}

static DWORD vol_get(struct sdev *d, int ctl, DWORD FAR *v)
{
    if (api(d->core, WINF_AUD_MIX, d->id, (DWORD)ctl, 0, 0, 0)) return MMSYSERR_ERROR;
    *v = rq->retlen;
    return MMSYSERR_NOERROR;
}

static DWORD vol_set(struct sdev *d, int ctl, DWORD v)
{
    return api(d->core, WINF_AUD_MIX, d->id, (DWORD)ctl | 0x80000000L, v, 0, 0) ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}

DWORD CALLBACK __loadds wodMessage(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    struct wstream *w;
    scan();
    if (msg == WODM_GETNUMDEVS || msg == WODM_OPEN || msg == WODM_GETDEVCAPS) rescan();
    if (msg == WODM_GETNUMDEVS) return nwout;
    if (id >= (UINT)nwout && msg != WODM_CLOSE) return MMSYSERR_BADDEVICEID;
    switch (msg) {
    case WODM_GETDEVCAPS:
    {
        WAVEOUTCAPS c;
        fmemset(&c, 0, sizeof(c));
        c.wMid = 0x6033;
        c.wPid = (WORD)(0x10 + wouts[id].type);
        c.vDriverVersion = 0x0100;
        fstrncpy(c.szPname, wouts[id].name, MAXPNAMELEN);
        c.dwFormats = 0x0FFF;
        c.wChannels = 2;
        c.dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
        fmemcpy((void FAR *)p1, &c, (WORD)MIN(p2, sizeof(c)));
        return MMSYSERR_NOERROR;
    }
    case WODM_OPEN:
        return wave_open(0, &wouts[id], (LPWAVEOPENDESC)p1, p2, (DWORD FAR *)user);
    case WODM_GETVOLUME:
        return vol_get(&wouts[id], MX_WAVE, (DWORD FAR *)p1);
    case WODM_SETVOLUME:
        return vol_set(&wouts[id], MX_WAVE, p1);
    }
    if (!(w = wfrom(user, 0))) return MMSYSERR_INVALHANDLE;
    switch (msg) {
    case WODM_CLOSE:
        return wave_close(w);
    case WODM_PREPARE:
    case WODM_UNPREPARE:
        return wave_prepare((LPWAVEHDR)p1, msg == WODM_PREPARE);
    case WODM_WRITE:
    {
        LPWAVEHDR h = (LPWAVEHDR)p1;
        if (!(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
        if (queue(w, h)) return MMSYSERR_NOMEM;
        busy++;
        out_fill(w);
        busy--;
        if (!w->started && !w->paused) {
            wave_ctl(w, AC_START);
            w->started = 1;
        }
        return MMSYSERR_NOERROR;
    }
    case WODM_PAUSE:
        w->paused = 1;
        if (w->started) wave_ctl(w, AC_PAUSE);
        return MMSYSERR_NOERROR;
    case WODM_RESTART:
        if (w->paused) {
            w->paused = 0;
            wave_ctl(w, AC_START);
            w->started = 1;
        }
        return MMSYSERR_NOERROR;
    case WODM_RESET:
        wave_reset(w);
        w->paused = 0;
        return MMSYSERR_NOERROR;
    case WODM_BREAKLOOP:
        busy++;
        if (w->lsi >= 0) {
            w->loops = 1;
            w->loopon = 0;
        }
        busy--;
        return MMSYSERR_NOERROR;
    case WODM_GETPOS:
        return getpos(w, (MMTIME FAR *)p1, p2);
    }
    return MMSYSERR_NOTSUPPORTED;
}

DWORD CALLBACK __loadds widMessage(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    struct wstream *w;
    scan();
    if (msg == WIDM_GETNUMDEVS || msg == WIDM_OPEN || msg == WIDM_GETDEVCAPS) rescan();
    if (msg == WIDM_GETNUMDEVS) return nwin;
    if (id >= (UINT)nwin && msg != WIDM_CLOSE) return MMSYSERR_BADDEVICEID;
    switch (msg) {
    case WIDM_GETDEVCAPS:
    {
        WAVEINCAPS c;
        fmemset(&c, 0, sizeof(c));
        c.wMid = 0x6033;
        c.wPid = (WORD)(0x20 + wins[id].type);
        c.vDriverVersion = 0x0100;
        fstrncpy(c.szPname, wins[id].name, MAXPNAMELEN);
        c.dwFormats = 0x0FFF;
        c.wChannels = 2;
        fmemcpy((void FAR *)p1, &c, (WORD)MIN(p2, sizeof(c)));
        return MMSYSERR_NOERROR;
    }
    case WIDM_OPEN:
        return wave_open(1, &wins[id], (LPWAVEOPENDESC)p1, p2, (DWORD FAR *)user);
    }
    if (!(w = wfrom(user, 1))) return MMSYSERR_INVALHANDLE;
    switch (msg) {
    case WIDM_CLOSE:
        return wave_close(w);
    case WIDM_PREPARE:
    case WIDM_UNPREPARE:
        return wave_prepare((LPWAVEHDR)p1, msg == WIDM_PREPARE);
    case WIDM_ADDBUFFER:
    {
        LPWAVEHDR h = (LPWAVEHDR)p1;
        if (!(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
        h->dwBytesRecorded = 0;
        if (queue(w, h)) return MMSYSERR_NOMEM;
        return MMSYSERR_NOERROR;
    }
    case WIDM_START:
        if (!w->started) {
            struct winring FAR *r = ringp(w);
            wave_ctl(w, AC_START);
            if (!r->pos) w->base = r->wr;
            w->started = 1;
        }
        return MMSYSERR_NOERROR;
    case WIDM_STOP:
        if (w->started) {
            busy++;
            wave_ctl(w, AC_PAUSE);
            w->started = 0;
            in_take(w);
            if (w->qn && QAT(w, 0)->dwBytesRecorded)    /* the current buffer is returned */
                callback(w, WIM_DATA, (DWORD)qpop(w));
            busy--;
        }
        return MMSYSERR_NOERROR;
    case WIDM_RESET:
        wave_reset(w);
        return MMSYSERR_NOERROR;
    case WIDM_GETPOS:
        return getpos(w, (MMTIME FAR *)p1, p2);
    }
    return MMSYSERR_NOTSUPPORTED;
}

/* ---------------------------------------------------------------- MIDI */

static void mcallback(struct mstream *m, UINT msg, DWORD p1)
{
    if (m->cbflags) DriverCallback(m->cb, m->cbflags, (HANDLE)m->hmidi, msg, m->inst, p1, 0);
}

static void midi_short(struct mstream *m, DWORD msg)
{
    BYTE b0 = LOBYTE(LOWORD(msg));
    if (b0 & 0x80) {
        if (b0 < 0xF0) m->status = b0;
        else if (b0 < 0xF8) m->status = 0;
    } else {
        if (!m->status) return;
        msg = (msg << 8) | m->status;           /* running status */
    }
    api(m->core, WINF_AUD_MIDI, m->dev, msg, 0, 0, 0);
}

static void midi_long(struct mstream *m, LPMIDIHDR h)
{
    DWORD done = 0;
    while (done < h->dwBufferLength) {
        WORD n = (WORD)MIN(h->dwBufferLength - done, MIDICHUNK);
        fmemcpy(MK_FP(reqsel, RQ_MIDI), h->lpData + done, n);
        api(m->core, WINF_AUD_MIDI, m->dev, 0, 0, RQBUF(RQ_MIDI), n);
        done += n;
    }
    m->status = 0;
}

static void midi_alloff(struct mstream *m)
{
    int c;
    for (c = 0; c < 16; c++) {
        api(m->core, WINF_AUD_MIDI, m->dev, 0x0040B0L | c, 0, 0, 0);     /* sustain off */
        api(m->core, WINF_AUD_MIDI, m->dev, 0x007BB0L | c, 0, 0, 0);     /* all notes off */
    }
}

DWORD CALLBACK __loadds modMessage(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    struct mstream *m;
    scan();
    if (msg == MODM_GETNUMDEVS || msg == MODM_OPEN || msg == MODM_GETDEVCAPS) rescan();
    if (msg == MODM_GETNUMDEVS) return nmout;
    if (id >= (UINT)nmout && msg != MODM_CLOSE) return MMSYSERR_BADDEVICEID;
    switch (msg) {
    case MODM_GETDEVCAPS:
    {
        MIDIOUTCAPS c;
        fmemset(&c, 0, sizeof(c));
        c.wMid = 0x6033;
        c.wPid = (WORD)(0x30 + mouts[id].ctl);
        c.vDriverVersion = 0x0100;
        fstrncpy(c.szPname, mouts[id].name, MAXPNAMELEN);
        c.wTechnology = mouts[id].ctl ? MOD_SYNTH : MOD_MIDIPORT;
        c.wVoices = mouts[id].ctl ? 32 : 0;
        c.wNotes = mouts[id].ctl ? 32 : 0;
        c.wChannelMask = 0xFFFF;
        c.dwSupport = mouts[id].ctl ? MIDICAPS_VOLUME | MIDICAPS_LRVOLUME : 0;
        fmemcpy((void FAR *)p1, &c, (WORD)MIN(p2, sizeof(c)));
        return MMSYSERR_NOERROR;
    }
    case MODM_OPEN:
    {
        LPMIDIOPENDESC od = (LPMIDIOPENDESC)p1;
        int i;
        m = NULL;
        for (i = 0; i < MAXOPEN; i++)
            if (!ms[i].used) {
                m = &ms[i];
                break;
            }
        if (!m) return MMSYSERR_ALLOCATED;
        if (mouts[id].gone) return MMSYSERR_NODRIVER;
        fmemset(m, 0, sizeof(*m));
        m->used = 1;
        m->core = mouts[id].core;
        m->dev = mouts[id].id;
        m->hmidi = od->hMidi;
        m->cb = od->dwCallback;
        m->inst = od->dwInstance;
        m->cbflags = HIWORD(p2) & DCB_TYPEMASK;
        *(DWORD FAR *)user = (DWORD)(WORD)(m - ms) + 1;
        mcallback(m, MOM_OPEN, 0);
        return MMSYSERR_NOERROR;
    }
    case MODM_GETVOLUME:
        return vol_get(&mouts[id], MX_SYNTH, (DWORD FAR *)p1);
    case MODM_SETVOLUME:
        return vol_set(&mouts[id], MX_SYNTH, p1);
    }
    if (!user || user > MAXOPEN || !ms[(WORD)user - 1].used) return MMSYSERR_INVALHANDLE;
    m = &ms[(WORD)user - 1];
    switch (msg) {
    case MODM_CLOSE:
        midi_alloff(m);
        mcallback(m, MOM_CLOSE, 0);
        m->used = 0;
        return MMSYSERR_NOERROR;
    case MODM_DATA:
        midi_short(m, p1);
        return MMSYSERR_NOERROR;
    case MODM_LONGDATA:
    {
        LPMIDIHDR h = (LPMIDIHDR)p1;
        if (!(h->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
        midi_long(m, h);
        h->dwFlags |= MHDR_DONE;
        mcallback(m, MOM_DONE, (DWORD)h);
        return MMSYSERR_NOERROR;
    }
    case MODM_RESET:
        midi_alloff(m);
        m->status = 0;
        return MMSYSERR_NOERROR;
    case MODM_CACHEPATCHES:
    case MODM_CACHEDRUMPATCHES:
        return MMSYSERR_NOERROR;
    }
    return MMSYSERR_NOTSUPPORTED;
}

DWORD CALLBACK __loadds midMessage(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    (void)id; (void)user; (void)p1; (void)p2;
    if (msg == MIDM_GETNUMDEVS) return 0;
    return MMSYSERR_NOTSUPPORTED;
}

/* ---------------------------------------------------------------- aux */

DWORD CALLBACK __loadds auxMessage(UINT id, UINT msg, DWORD user, DWORD p1, DWORD p2)
{
    (void)user;
    scan();
    if (msg == AUXDM_GETNUMDEVS) return naux;
    if (id >= (UINT)naux) return MMSYSERR_BADDEVICEID;
    switch (msg) {
    case AUXDM_GETDEVCAPS:
    {
        AUXCAPS c;
        fmemset(&c, 0, sizeof(c));
        c.wMid = 0x6033;
        c.wPid = (WORD)(0x40 + auxs[id].ctl);
        c.vDriverVersion = 0x0100;
        fstrncpy(c.szPname, auxs[id].name, MAXPNAMELEN);
        c.wTechnology = auxs[id].ctl == MX_CD ? AUXCAPS_CDAUDIO : AUXCAPS_AUXIN;
        c.dwSupport = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME;
        fmemcpy((void FAR *)p1, &c, (WORD)MIN(p2, sizeof(c)));
        return MMSYSERR_NOERROR;
    }
    case AUXDM_GETVOLUME:
        return vol_get(&auxs[id], auxs[id].ctl, (DWORD FAR *)p1);
    case AUXDM_SETVOLUME:
        return vol_set(&auxs[id], auxs[id].ctl, p1);
    }
    return MMSYSERR_NOTSUPPORTED;
}

/* ---------------------------------------------------------------- DriverProc */

LRESULT CALLBACK __loadds DriverProc(DWORD id, HANDLE hdrv, UINT msg, LPARAM p1, LPARAM p2)
{
    (void)id; (void)hdrv; (void)p1; (void)p2;
    switch (msg) {
    case DRV_LOAD:
        scan();
        return 1;
    case DRV_FREE:
        if (timer_id) {
            timeKillEvent(timer_id);
            timeEndPeriod(10);
            timer_id = 0;
        }
        if (rq) {
            GlobalDosFree(reqsel);
            rq = NULL;
        }
        return 1;
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_QUERYCONFIGURE:
        return 0;
    case DRV_INSTALL:
        return DRVCNF_RESTART;
    case DRV_REMOVE:
        return 1;
    }
    return 0;
}
