/* winbeep.c - WINBEEP.DRV: the Windows "SOUND" driver (sound.drv= in SYSTEM.INI) for
   Windows 3.0. It does what the Multimedia Extensions' MMSOUND.DRV does (loads MMSYSTEM in
   protected mode, system sounds through sndPlaySound) and adds what works in every mode,
   real mode included:
     - MessageBeep through the MIDI synthesizer of WINAUDIO.SYS / WINUSB.SYS
       (PC speaker when neither is loaded),
     - the voice API of the original SOUND.DRV (OpenSound, SetVoiceNote, StartSound, ...)
       played on the synthesizer, four voices.
   The music is timed by a SYSTEM.DRV timer; the core is called only from that timer. */
#include <windows.h>
#include "winapi.h"

DWORD WINAPI GlobalDosAlloc(DWORD cb);
UINT WINAPI GlobalDosFree(UINT sel);
WORD WINAPI CreateSystemTimer(WORD ms, FARPROC proc);
WORD WINAPI KillSystemTimer(WORD id);
DWORD WINAPI GetSystemMSecCount(void);

#define MK_FP(s, o)  ((void __far *)(((DWORD)(s) << 16) | (WORD)(o)))
#define NVOICE       4
#define QMAX         64
#define TICK         10          /* timer period, ms */
#define NPEND        64
#define CH_BEEP      14          /* MIDI channel of MessageBeep */
#define CH_NOISE     9           /* percussion */
#define PROG_SQUARE  80          /* GM "Lead 1 (square)": the PC speaker sound */

/* ---------------------------------------------------------------- core */

static const BYTE mplexes[2] = { WIN_MPLEX_AUDIO, WIN_MPLEX_USB };
static const WORD vxdids[2] = { WIN_VXD_AUDIO, WIN_VXD_USB };
static int core = -1;               /* 0 WINAUDIO, 1 WINUSB, -1 none */
static DWORD vxdentry;
static WORD reqsel, reqseg;
static struct winapireq FAR *rq;
static int realmode, enhanced;

static void cli(void);
#pragma aux cli = "cli";
static void sti(void);
#pragma aux sti = "sti";
static WORD irqsave(void);
#pragma aux irqsave = "pushf" "pop ax" "cli" value [ax];
static void irqrestore(WORD f);
#pragma aux irqrestore = "push ax" "popf" parm [ax];

static void fmemset(void FAR *d, BYTE v, WORD n)
{
    BYTE FAR *p = d;
    while (n--) *p++ = v;
}

static int api_midi(DWORD msg)
{
    WORD rseg = reqseg, mx = ((WORD)mplexes[core] << 8) | 1;
    DWORD ent = vxdentry;
    fmemset(rq, 0, sizeof(*rq));
    rq->fn = WINF_AUD_MIDI;
    rq->status = 0xFFFF;
    rq->arg0 = 0xFF;                /* the synthesizer */
    rq->arg1 = msg;
    if (ent) {
        _asm {
            mov ax, 3
            mov cx, rseg
            xor dx, dx
            call dword ptr ent
        }
    } else if (realmode) {
        _asm {
            push es
            mov ax, mx
            mov es, rseg
            xor bx, bx
            int 2Fh
            pop es
        }
    } else {
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

/* finds WINAUDIO.SYS (else WINUSB.SYS) and its VxD */
static void find_core(void)
{
    int k;
    DWORD r;
    if (rq) return;
    realmode = !(GetWinFlags() & WF_PMODE);
    enhanced = (GetWinFlags() & WF_ENHANCED) != 0;
    for (k = 0; k < 2 && core < 0; k++) {
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
        if (r_al != 0xFF || r_bx != 0x3357) continue;
        core = k;
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
            if (es_ || di_) vxdentry = ((DWORD)es_ << 16) | di_;
        }
    }
    if (core < 0) return;
    r = GlobalDosAlloc(64);
    if (!r) {
        core = -1;
        return;
    }
    reqsel = LOWORD(r);
    reqseg = HIWORD(r);
    rq = MK_FP(reqsel, 0);
}

/* ---------------------------------------------------------------- MIDI out queue */

static DWORD pend[NPEND];
static volatile BYTE ph, pt;

static void midi(DWORD msg)
{
    BYTE n;
    WORD f = irqsave();
    n = (BYTE)((pt + 1) % NPEND);
    if (n != ph) {
        pend[pt] = msg;
        pt = n;
    }
    irqrestore(f);
}

static void flush_midi(void)
{
    while (ph != pt) {
        if (api_midi(pend[ph]) != WINE_OK && rq->status == 0xFFFF) return;   /* core busy: next tick */
        ph = (BYTE)((ph + 1) % NPEND);
    }
}

#define NOTEON(ch, n, v)  midi(0x90UL | (ch) | ((DWORD)(n) << 8) | ((DWORD)(v) << 16))
#define NOTEOFF(ch, n)    midi(0x80UL | (ch) | ((DWORD)(n) << 8) | (64UL << 16))
#define PROGRAM(ch, p)    midi(0xC0UL | (ch) | ((DWORD)(p) << 8))
#define BEND(ch, b)       midi(0xE0UL | (ch) | ((DWORD)((b) & 0x7F) << 8) | ((DWORD)(((b) >> 7) & 0x7F) << 16))
#define CTL(ch, c, v)     midi(0xB0UL | (ch) | ((DWORD)(c) << 8) | ((DWORD)(v) << 16))

/* ---------------------------------------------------------------- voices */

#define QE_NOTE     1
#define QE_SOUND    2
#define QE_SYNC     3

struct qe {
    BYTE type, note, vel, rsv;
    WORD bend;
    WORD ms;                        /* length */
    WORD gate;                      /* sounding part */
};

struct voice {
    struct qe q[QMAX];
    BYTE qh, qn, cap;
    BYTE sounding;                  /* MIDI note playing, 0 none */
    BYTE insync;                    /* waiting at a sync mark */
    BYTE thresh;
    WORD tempo, vol, mode, pitch;
    long left, gleft;               /* ms left of the entry and of its sounding part */
};

static struct voice voices[NVOICE];
static int opened, running, threshold_event;
static WORD timer;
static int beep_note = -1;
static long beep_left;
static int noise_note = -1;
static long noise_left;
static int busy;
static volatile DWORD now;          /* ms, counted by the timer */

static const unsigned long ftab[128] = {
    131UL, 139UL, 147UL, 156UL, 165UL, 175UL, 185UL, 196UL,
    208UL, 220UL, 233UL, 247UL, 262UL, 277UL, 294UL, 311UL,
    330UL, 349UL, 370UL, 392UL, 415UL, 440UL, 466UL, 494UL,
    523UL, 554UL, 587UL, 622UL, 659UL, 698UL, 740UL, 784UL,
    831UL, 880UL, 932UL, 988UL, 1047UL, 1109UL, 1175UL, 1245UL,
    1319UL, 1397UL, 1480UL, 1568UL, 1661UL, 1760UL, 1865UL, 1976UL,
    2093UL, 2217UL, 2349UL, 2489UL, 2637UL, 2794UL, 2960UL, 3136UL,
    3322UL, 3520UL, 3729UL, 3951UL, 4186UL, 4435UL, 4699UL, 4978UL,
    5274UL, 5588UL, 5920UL, 6272UL, 6645UL, 7040UL, 7459UL, 7902UL,
    8372UL, 8870UL, 9397UL, 9956UL, 10548UL, 11175UL, 11840UL, 12544UL,
    13290UL, 14080UL, 14917UL, 15804UL, 16744UL, 17740UL, 18795UL, 19912UL,
    21096UL, 22351UL, 23680UL, 25088UL, 26580UL, 28160UL, 29834UL, 31609UL,
    33488UL, 35479UL, 37589UL, 39824UL, 42192UL, 44701UL, 47359UL, 50175UL,
    53159UL, 56320UL, 59669UL, 63217UL, 66976UL, 70959UL, 75178UL, 79649UL,
    84385UL, 89402UL, 94719UL, 100351UL, 106318UL, 112640UL, 119338UL, 126434UL,
    133952UL, 141918UL, 150356UL, 159297UL, 168769UL, 178805UL, 189437UL, 200702UL,
};

/* MIDI note and pitch bend (+-2 semitones around 8192) of a frequency in Hz */
static int freq_note(unsigned long hz, WORD *bend)
{
    unsigned long f = hz * 16;
    int n;
    long cents;
    for (n = 0; n < 127 && ftab[n + 1] <= f; n++)
        ;
    if (n < 127 && f - ftab[n] > ftab[n + 1] - f) n++;
    /* cents ~ 1731 * (f - fn) / fn for small differences */
    cents = (long)(((long)f - (long)ftab[n]) * 1731L / (long)ftab[n]);
    if (cents > 100) cents = 100;
    if (cents < -100) cents = -100;
    *bend = (WORD)(8192 + cents * 8192L / 200);
    return n;
}

static void all_off(void)
{
    int v;
    for (v = 0; v < NVOICE; v++) {
        if (voices[v].sounding) NOTEOFF(v, voices[v].sounding);
        voices[v].sounding = 0;
        CTL(v, 123, 0);
    }
}

static void voice_defaults(struct voice *v)
{
    v->qh = v->qn = 0;
    v->cap = 32;                    /* 192 bytes, 6 per note */
    v->sounding = 0;
    v->insync = 0;
    v->thresh = 0;
    v->tempo = 120;
    v->vol = 128;
    v->mode = S_NORMAL;
    v->pitch = 0;
    v->left = v->gleft = 0;
}

static void start_entry(int i, struct voice *v)
{
    struct qe *e = &v->q[v->qh];
    v->left = e->ms;
    v->gleft = e->gate;
    if (e->type == QE_SYNC) {
        v->insync = 1;
        v->left = 0;
        return;
    }
    if (e->note) {
        if (e->type == QE_SOUND) BEND(i, e->bend);
        NOTEON(i, e->note, e->vel);
        v->sounding = e->note;
    }
}

static void next_entry(int i, struct voice *v)
{
    if (v->sounding) {
        NOTEOFF(i, v->sounding);
        if (v->q[v->qh].type == QE_SOUND) BEND(i, 8192);
        v->sounding = 0;
    }
    v->qh = (BYTE)((v->qh + 1) % QMAX);
    v->qn--;
    if (v->thresh && v->qn < v->thresh) threshold_event |= 1 << i;
    if (v->qn) start_entry(i, v);
}

static void music_tick(long dt)
{
    int i, waiting = 0, all = 1;
    for (i = 0; i < NVOICE; i++) {
        struct voice *v = &voices[i];
        if (!v->qn || v->insync) continue;
        v->gleft -= dt;
        if (v->gleft <= 0 && v->sounding) {
            NOTEOFF(i, v->sounding);
            v->sounding = 0;
        }
        v->left -= dt;
        if (v->left <= 0) next_entry(i, v);
    }
    /* sync marks: a voice waits until every voice with notes has reached its mark */
    for (i = 0; i < NVOICE; i++) {
        if (voices[i].insync) waiting = 1;
        else if (voices[i].qn) all = 0;
    }
    if (waiting && all)
        for (i = 0; i < NVOICE; i++)
            if (voices[i].insync) {
                voices[i].insync = 0;
                next_entry(i, &voices[i]);
            }
}

/* SYSTEM.DRV timer, interrupt time. The timer of Windows 3.0 may come only every 55 ms
   whatever rate is asked for: the time that really passed is used. */
static DWORD last;

void __far __loadds __saveregs TimerTick(void)
{
    DWORD t = GetSystemMSecCount();
    long dt = (long)(t - last);
    last = t;
    if (dt < 0 || dt > 500) dt = TICK;
    now += dt;
    if (busy) return;
    busy = 1;
    if (running) music_tick(dt);
    if (beep_note >= 0 && (beep_left -= dt) <= 0) {
        NOTEOFF(CH_BEEP, beep_note);
        beep_note = -1;
    }
    if (noise_note >= 0 && (noise_left -= dt) <= 0) {
        NOTEOFF(CH_NOISE, noise_note);
        noise_note = -1;
    }
    if (core >= 0) flush_midi();
    busy = 0;
}

static int need_timer(void)
{
    find_core();
    if (core < 0) return 0;
    if (!timer) {
        last = GetSystemMSecCount();
        timer = CreateSystemTimer(TICK, (FARPROC)TimerTick);
    }
    return timer != 0;
}

/* ---------------------------------------------------------------- voice API */

int WINAPI __loadds OpenSound(void)
{
    int v;
    if (opened) return S_SERDVNA;
    if (!need_timer()) return S_SERDVNA;
    running = 0;
    for (v = 0; v < NVOICE; v++) {
        voice_defaults(&voices[v]);
        PROGRAM(v, PROG_SQUARE);
        BEND(v, 8192);
    }
    threshold_event = 0;
    opened = 1;
    return NVOICE;
}

int WINAPI __loadds MyOpenSound(void)
{
    return OpenSound();
}

void WINAPI __loadds CloseSound(void)
{
    if (!opened) return;
    running = 0;
    all_off();
    opened = 0;
}

static struct voice *vp(int n)
{
    return (opened && n >= 1 && n <= NVOICE) ? &voices[n - 1] : NULL;
}

int WINAPI __loadds SetVoiceQueueSize(int n, int bytes)
{
    struct voice *v = vp(n);
    if (!v) return S_SERDVNA;
    if (running && v->qn) return S_SERMACT;
    if (bytes < 6) return S_SEROFM;
    v->cap = (BYTE)(bytes / 6 > QMAX ? QMAX : bytes / 6);
    return 0;
}

static int queue(struct voice *v, struct qe *e)
{
    int i = (int)(v - voices);
    cli();
    if (v->qn >= v->cap) {
        sti();
        return S_SERQFUL;
    }
    v->q[(v->qh + v->qn) % QMAX] = *e;
    v->qn++;
    if (v->qn == 1 && running) start_entry(i, v);
    sti();
    return 0;
}

int WINAPI __loadds SetVoiceNote(int n, int value, int length, int cdots)
{
    struct voice *v = vp(n);
    struct qe e;
    long whole, d, add;
    int k;
    if (!v) return S_SERDVNA;
    if (value < 0 || value > 84) return S_SERBDNT;
    if (length < 1 || length > 256) return S_SERDLN;
    if (cdots < 0 || cdots > 8) return S_SERDCC;
    whole = 240000L / v->tempo;     /* four quarter notes */
    d = add = whole / length;
    for (k = 0; k < cdots; k++) d += (add /= 2);
    if (d > 60000L) d = 60000L;
    e.type = QE_NOTE;
    e.note = (BYTE)(value ? value + 23 + v->pitch : 0);   /* note 37 = middle C */
    if (e.note > 127) e.note = 127;
    e.vel = (BYTE)(v->vol / 2 ? v->vol / 2 : 1);
    e.bend = 8192;
    e.ms = (WORD)d;
    e.gate = (WORD)(v->mode == S_LEGATO ? d : v->mode == S_STACCATO ? d * 3 / 4 : d * 7 / 8);
    return queue(v, &e);
}

int WINAPI __loadds SetVoiceAccent(int n, int tempo, int volume, int mode, int pitch)
{
    struct voice *v = vp(n);
    if (!v) return S_SERDVNA;
    if (tempo < 32 || tempo > 255) return S_SERDTP;
    if (volume < 0 || volume > 255) return S_SERDVL;
    if (mode < S_NORMAL || mode > S_STACCATO) return S_SERDMD;
    if (pitch < 0 || pitch > 83) return S_SERDPT;
    v->tempo = tempo;
    v->vol = volume;
    v->mode = mode;
    v->pitch = pitch;
    return 0;
}

int WINAPI __loadds SetVoiceEnvelope(int n, int shape, int repeat)
{
    (void)shape;
    (void)repeat;
    return vp(n) ? 0 : S_SERDVNA;
}

int WINAPI __loadds SetSoundNoise(int source, int duration)
{
    if (!opened) return S_SERDVNA;
    if (source < S_PERIOD512 || source > S_WHITEVOICE) return S_SERDSR;
    if (duration < 0) return S_SERDDR;
    cli();
    if (noise_note >= 0) NOTEOFF(CH_NOISE, noise_note);
    noise_note = source >= S_WHITE512 ? 38 + (source - S_WHITE512) : 42 + (source - S_PERIOD512) * 2;
    NOTEON(CH_NOISE, noise_note, 100);
    noise_left = duration * 55L;
    sti();
    return 0;
}

int WINAPI __loadds SetVoiceSound(int n, DWORD freq, int duration)
{
    struct voice *v = vp(n);
    struct qe e;
    WORD hz = HIWORD(freq);
    if (!v) return S_SERDVNA;
    if (duration < 0) return S_SERDDR;
    if (hz && (hz < 16 || hz > 12000)) return S_SERDFQ;
    e.type = QE_SOUND;
    e.note = hz ? (BYTE)freq_note(hz, &e.bend) : 0;
    e.vel = (BYTE)(v->vol / 2 ? v->vol / 2 : 1);
    if (!hz) e.bend = 8192;
    e.ms = (WORD)(duration > 1000 ? 55000U : duration * 55U);
    e.gate = e.ms;
    return queue(v, &e);
}

int WINAPI __loadds StartSound(void)
{
    int i;
    if (!opened) return S_SERDVNA;
    cli();
    if (!running) {
        running = 1;
        for (i = 0; i < NVOICE; i++)
            if (voices[i].qn) start_entry(i, &voices[i]);
    }
    sti();
    return 0;
}

int WINAPI __loadds StopSound(void)
{
    int i;
    if (!opened) return S_SERDVNA;
    cli();
    running = 0;
    all_off();
    for (i = 0; i < NVOICE; i++) {
        voices[i].qh = voices[i].qn = 0;
        voices[i].insync = 0;
    }
    sti();
    return 0;
}

static int queues_empty(void)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (voices[i].qn) return 0;
    return 1;
}

int WINAPI __loadds WaitSoundState(int state)
{
    DWORD t0;
    int i, ok;
    if (!opened) return S_SERDVNA;
    if (state < S_QUEUEEMPTY || state > S_ALLTHRESHOLD) return S_SERDST;
    if (!running) return 0;
    t0 = now;
    for (;;) {
        if (state == S_QUEUEEMPTY) ok = queues_empty();
        else {
            ok = state == S_ALLTHRESHOLD;
            for (i = 0; i < NVOICE; i++) {
                int below = voices[i].qn < voices[i].thresh || !voices[i].qn;
                if (state == S_THRESHOLD && below) ok = 1;
                if (state == S_ALLTHRESHOLD && !below) ok = 0;
            }
        }
        if (ok) break;
        if (now - t0 > 600000UL) break;
    }
    if (state == S_QUEUEEMPTY) {
        /* the last notes have sounded completely */
        t0 = now;
        while (now - t0 < 30)
            ;
    }
    return 0;
}

int WINAPI __loadds SyncAllVoices(void)
{
    int i;
    struct qe e;
    if (!opened) return S_SERDVNA;
    for (i = 0; i < NVOICE; i++)
        if (voices[i].qn >= voices[i].cap) return S_SERQFUL;
    fmemset(&e, 0, sizeof(e));
    e.type = QE_SYNC;
    for (i = 0; i < NVOICE; i++) queue(&voices[i], &e);
    return 0;
}

int WINAPI __loadds CountVoiceNotes(int n)
{
    struct voice *v = vp(n);
    int i, c = 0;
    if (!v) return 0;
    for (i = 0; i < v->qn; i++)
        if (v->q[(v->qh + i) % QMAX].type != QE_SYNC) c++;
    return c;
}

int FAR * WINAPI __loadds GetThresholdEvent(void)
{
    return &threshold_event;
}

int WINAPI __loadds GetThresholdStatus(void)
{
    int r;
    cli();
    r = threshold_event;
    threshold_event = 0;
    sti();
    return r;
}

int WINAPI __loadds SetVoiceThreshold(int n, int notes)
{
    struct voice *v = vp(n);
    if (!v) return 1;
    v->thresh = (BYTE)(notes < 0 ? 0 : notes > QMAX ? QMAX : notes);
    return 0;
}

/* ---------------------------------------------------------------- MessageBeep */

typedef BOOL (FAR PASCAL *SNDPLAY)(LPCSTR, UINT);
typedef LRESULT (FAR PASCAL *DRVPROC)(DWORD, HANDLE, UINT, LPARAM, LPARAM);

static HINSTANCE hmmsys;            /* MMSYSTEM loaded by this driver (Windows 3.0 MME) */
static SNDPLAY sndplay;
static const char *const sysnames[5] = { "SystemDefault", "SystemHand", "SystemQuestion", "SystemExclamation",
                                         "SystemAsterisk" };

static volatile WORD spin;

static void speaker_beep(void)
{
    WORD old = 0, i;
    _asm {
        mov al, 0B6h
        out 43h, al
        in al, 42h
        mov ah, al
        in al, 42h
        xchg al, ah
        mov old, ax
        mov al, 0B6h
        out 43h, al
        mov ax, 1591                ; 1193180 / 750 Hz
        out 42h, al
        mov al, ah
        out 42h, al
        in al, 61h
        or al, 3
        out 61h, al
    }
    for (i = 0; i < 0xF000; i++)
        spin++;
    _asm {
        in al, 61h
        and al, 0FCh
        out 61h, al
        mov al, 0B6h
        out 43h, al
        mov ax, old
        out 42h, al
        mov al, ah
        out 42h, al
    }
}

/* MessageBeep(type): type is the MB_ICON* value or -1 */
void WINAPI __loadds DoBeep(int type)
{
    /* the multimedia system sounds when MMSYSTEM is there (protected mode) */
    if (type != -1 && !realmode) {
        if (!sndplay) {
            HINSTANCE h = hmmsys ? hmmsys : GetModuleHandle("MMSYSTEM");
            if (h) sndplay = (SNDPLAY)GetProcAddress(h, "sndPlaySound");
        }
        if (sndplay) {
            int k = (type & 0xF0) >> 4;
            if (k > 4) k = 0;
            if (sndplay(sysnames[k], 1 /* SND_ASYNC */)) return;
        }
    }
    if (need_timer()) {
        static const BYTE notes[5] = { 78, 66, 81, 83, 85 };   /* 740 Hz like the speaker; others by icon */
        int k = type == -1 ? 0 : (type & 0xF0) >> 4;
        if (k > 4) k = 0;
        cli();
        if (beep_note >= 0) NOTEOFF(CH_BEEP, beep_note);
        PROGRAM(CH_BEEP, PROG_SQUARE);
        beep_note = notes[k];
        NOTEON(CH_BEEP, beep_note, 100);
        beep_left = 150;
        sti();
        return;
    }
    speaker_beep();
}

/* ---------------------------------------------------------------- driver */

/* USER calls Enable when it loads the driver. Windows 3.0 has no installable drivers:
   like MMSOUND.DRV, this driver loads MMSYSTEM there (protected mode only). */
void WINAPI __loadds Enable(void)
{
    HINSTANCE u = GetModuleHandle("USER");
    realmode = !(GetWinFlags() & WF_PMODE);
    if (realmode || (u && GetProcAddress(u, "OpenDriver")) || hmmsys) return;
    {
        OFSTRUCT of;
        HINSTANCE h;
        if (OpenFile("MMSYSTEM.DLL", &of, OF_EXIST) == -1) return;
        h = LoadLibrary("MMSYSTEM.DLL");
        if ((UINT)h < 32) return;
        hmmsys = h;
        {
            DRVPROC dp = (DRVPROC)GetProcAddress(h, "DriverProc");
            if (dp && dp(0, 0, DRV_LOAD, 0, 0)) dp(0, 0, DRV_ENABLE, 0, 0);
        }
    }
}

void WINAPI __loadds Disable(void)
{
    if (timer) {
        KillSystemTimer(timer);
        timer = 0;
    }
    if (hmmsys) {
        DRVPROC dp = (DRVPROC)GetProcAddress(hmmsys, "DriverProc");
        if (dp) dp(0, 0, DRV_DISABLE, 0, 0);
    }
}
