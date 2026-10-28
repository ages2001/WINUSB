/* winmix.c - WINMIX.EXE: volume control for the sound devices of WINAUDIO.SYS and WINUSB.SYS
   (master, wave, synthesizer, CD, line, recording level and recording source).
   Windows 3.0 and 3.1. */
#include <windows.h>
#include "winapi.h"

DWORD WINAPI GlobalDosAlloc(DWORD cb);
UINT WINAPI GlobalDosFree(UINT sel);

#define MK_FP(s, o) ((void __far *)(((DWORD)(s) << 16) | (WORD)(o)))
#define NCORE   2
#define MAXDEV  16
#define NCTL    6
#define ID_DEV  100
#define ID_SRC  101
#define ID_BAR  200
#define ID_LOCK 300
#define ID_TXT  400
#define RANGE   100

struct mdev {
    BYTE core, id, caps, insrcs, insrc, hwvol, type, gone;
    char name[48];
};

static const BYTE mplexes[NCORE] = { WIN_MPLEX_AUDIO, WIN_MPLEX_USB };
static const WORD vxdids[NCORE] = { WIN_VXD_AUDIO, WIN_VXD_USB };
static BYTE present[NCORE];
static DWORD vxdentry[NCORE];
static WORD reqsel, reqseg;
static struct winapireq FAR *rq;
static struct mdev devs[MAXDEV], nd2[MAXDEV];
static int ndev, cur;
static HWND hmain, hdev, hsrc, hbar[NCTL], htxt[NCTL], hlock;
static DWORD vals[NCTL];
static const char *const ctlnames[NCTL] = { "Master", "Wave", "Synth", "Rec level", "CD", "Line" };
static const char *const srcnames[5] = { "Microphone", "Line in", "CD", "Aux", "Stereo mix" };
static HINSTANCE hinst;
static HBRUSH hface;

static void fmemset(void FAR *d, BYTE v, WORD n)
{
    BYTE FAR *p = d;
    while (n--) *p++ = v;
}

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

static int same(const struct mdev *a, const struct mdev *b)
{
    return a->core == b->core && a->id == b->id && a->gone == b->gone && a->caps == b->caps &&
           !lstrcmp(a->name, b->name);
}

/* device list of both drivers; returns 1 when it changed (USB audio plugged in or removed) */
static int list_devs(void)
{
    int k, i, n = 0, changed;
    for (k = 0; k < NCORE; k++) {
        int nd;
        struct winauddev FAR *d;
        if (!present[k] || api(k, WINF_AUD_DEVS, 0, 0, 0, ((DWORD)reqseg << 16) | 64, 1024 - 64)) continue;
        nd = (int)(rq->retlen / sizeof(struct winauddev));
        d = MK_FP(reqsel, 64);
        for (i = 0; i < nd && n < MAXDEV; i++, d++) {
            struct mdev *m = &nd2[n++];
            m->core = (BYTE)k;
            m->id = d->id;
            m->caps = d->caps;
            m->insrcs = d->insrcs;
            m->insrc = d->insrc;
            m->hwvol = d->hwvol;
            m->type = d->type;
            m->gone = d->gone;
            lstrcpy(m->name, d->name);
        }
    }
    changed = n != ndev;
    for (i = 0; i < n && !changed; i++)
        if (!same(&nd2[i], &devs[i])) changed = 1;
    for (i = 0; i < n; i++) devs[i] = nd2[i];
    ndev = n;
    return changed;
}

static void scan(void)
{
    int k;
    DWORD r = GlobalDosAlloc(1024);
    realmode = !(GetWinFlags() & WF_PMODE);
    if (!r) return;
    reqsel = LOWORD(r);
    reqseg = HIWORD(r);
    rq = MK_FP(reqsel, 0);
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
        if (r_al == 0xFF && r_bx == 0x3357) present[k] = 1;
        if (GetWinFlags() & WF_ENHANCED) {
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
                _asm {                  /* Windows 95/98/ME: the VxD may run without the DOS driver */
                    xor ax, ax
                    xor bx, bx
                    call dword ptr ent
                    mov started, bx
                }
                if (started) present[k] = 1;
            }
        }
    }
    list_devs();
}

static void load_values(void)
{
    struct mdev *m = &devs[cur];
    int i, s, sel = -1, idx = 0, ok = ndev > 0 && !m->gone;
    char t[32];
    if (!ok) {
        for (i = 0; i < NCTL; i++) {
            EnableWindow(hbar[i], FALSE);
            SetWindowText(htxt[i], "-");
        }
        SendMessage(hsrc, CB_RESETCONTENT, 0, 0);
        EnableWindow(hsrc, FALSE);
        return;
    }
    for (i = 0; i < NCTL; i++) {
        int en = 1;
        vals[i] = 0;
        if (api(m->core, WINF_AUD_MIX, m->id, i, 0, 0, 0) == 0) vals[i] = rq->retlen;
        if ((i == 3 && !(m->caps & WINAC_IN)) || ((i == 4 || i == 5) && m->type != 2)) en = 0;
        if (i == 0 && !(m->caps & WINAC_OUT)) en = 0;
        SetScrollPos(hbar[i], SB_CTL, RANGE - (int)((LOWORD(vals[i]) * (DWORD)RANGE + 32767) / 65535), TRUE);
        EnableWindow(hbar[i], en);
        wsprintf(t, "%d%%", (int)((LOWORD(vals[i]) * 100UL + 32767) / 65535));
        SetWindowText(htxt[i], en ? t : "-");
    }
    SendMessage(hsrc, CB_RESETCONTENT, 0, 0);
    if (api(m->core, WINF_AUD_MIX, m->id, MX_SRC, 0, 0, 0) == 0) m->insrc = (BYTE)rq->retlen;
    for (s = 0; s < 5; s++)
        if (m->insrcs & (1 << s)) {
            SendMessage(hsrc, CB_ADDSTRING, 0, (LPARAM)(LPSTR)srcnames[s]);
            if (m->insrc & (1 << s)) sel = idx;
            idx++;
        }
    SendMessage(hsrc, CB_SETCURSEL, sel, 0);
    EnableWindow(hsrc, idx > 1);
}

/* the volumes of the device as WINSOUND.DRV keeps them (SYSTEM.INI [winsound.drv]): set again
   when Windows starts */
static void save(struct mdev *m)
{
    static const WORD ctls[NCTL + 2] = { 0, 1, 2, 3, 4, 5, MX_SRC, MX_MUTE };
    char key[16], val[100], old[100];
    DWORD v[NCTL + 2];
    int i;
    for (i = 0; i < NCTL + 2; i++) {
        if (api(m->core, WINF_AUD_MIX, m->id, ctls[i], 0, 0, 0)) return;
        v[i] = rq->retlen;
    }
    wsprintf(val, "%lX %lX %lX %lX %lX %lX %lX %lX", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    wsprintf(key, "Volume%d.%d", m->core, m->id);
    GetPrivateProfileString("winsound.drv", key, "", old, sizeof(old), "SYSTEM.INI");
    if (lstrcmp(old, val)) WritePrivateProfileString("winsound.drv", key, val, "SYSTEM.INI");
}

static void set_value(int i, int pos)
{
    struct mdev *m = &devs[cur];
    DWORD l = (DWORD)(RANGE - pos) * 65535 / RANGE, r = l;
    char t[16];
    if (!SendMessage(hlock, BM_GETCHECK, 0, 0)) {
        /* keep the balance: scale both sides */
        DWORD ol = LOWORD(vals[i]), or = HIWORD(vals[i]), mx = ol > or ? ol : or;
        if (mx) {
            l = ol * l / mx;
            r = or * r / mx;
        }
    }
    vals[i] = l | (r << 16);
    api(m->core, WINF_AUD_MIX, m->id, (DWORD)i | 0x80000000L, vals[i], 0, 0);
    save(m);
    wsprintf(t, "%d%%", (int)((DWORD)(RANGE - pos) * 100 / RANGE));
    SetWindowText(htxt[i], t);
}

static void set_source(void)
{
    struct mdev *m = &devs[cur];
    int sel = (int)SendMessage(hsrc, CB_GETCURSEL, 0, 0), s, idx = 0;
    for (s = 0; s < 5; s++)
        if (m->insrcs & (1 << s)) {
            if (idx++ == sel) {
                api(m->core, WINF_AUD_MIX, m->id, MX_SRC | 0x80000000L, 1UL << s, 0, 0);
                save(m);
                break;
            }
        }
}

static void fill_devices(void)
{
    int i;
    SendMessage(hdev, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < ndev; i++) {
        char t[80];
        wsprintf(t, "%d: %s%s%s", i, (LPSTR)devs[i].name, (LPSTR)(devs[i].core ? " (USB)" : ""),
                 (LPSTR)(devs[i].gone ? " - unplugged" : ""));
        SendMessage(hdev, CB_ADDSTRING, 0, (LPARAM)(LPSTR)t);
    }
    if (!ndev) SendMessage(hdev, CB_ADDSTRING, 0, (LPARAM)(LPSTR)"No sound device");
    if (cur >= ndev) cur = 0;
    SendMessage(hdev, CB_SETCURSEL, cur, 0);
}

LRESULT CALLBACK __export WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    switch (msg) {
    case WM_CREATE:
    {
        HFONT f = GetStockObject(ANSI_VAR_FONT);
        hdev = CreateWindow("combobox", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            8, 8, 368, 150, hw, (HMENU)ID_DEV, hinst, NULL);
        fill_devices();
        for (i = 0; i < NCTL; i++) {
            HWND l = CreateWindow("static", ctlnames[i], WS_CHILD | WS_VISIBLE | SS_CENTER, 8 + i * 62, 40, 60, 16,
                                  hw, NULL, hinst, NULL);
            SendMessage(l, WM_SETFONT, (WPARAM)f, 0);
            hbar[i] = CreateWindow("scrollbar", NULL, WS_CHILD | WS_VISIBLE | SBS_VERT, 26 + i * 62, 60, 22, 150,
                                   hw, (HMENU)(ID_BAR + i), hinst, NULL);
            SetScrollRange(hbar[i], SB_CTL, 0, RANGE, FALSE);
            htxt[i] = CreateWindow("static", "", WS_CHILD | WS_VISIBLE | SS_CENTER, 8 + i * 62, 214, 60, 16, hw,
                                   (HMENU)(ID_TXT + i), hinst, NULL);
            SendMessage(htxt[i], WM_SETFONT, (WPARAM)f, 0);
        }
        hlock = CreateWindow("button", "Same level left and right", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             8, 238, 220, 18, hw, (HMENU)ID_LOCK, hinst, NULL);
        SendMessage(hlock, WM_SETFONT, (WPARAM)f, 0);
        SendMessage(hlock, BM_SETCHECK, 1, 0);
        {
            HWND l = CreateWindow("static", "Recording source:", WS_CHILD | WS_VISIBLE, 8, 266, 120, 16, hw, NULL,
                                  hinst, NULL);
            SendMessage(l, WM_SETFONT, (WPARAM)f, 0);
        }
        hsrc = CreateWindow("combobox", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 130, 262,
                            160, 120, hw, (HMENU)ID_SRC, hinst, NULL);
        SendMessage(hdev, WM_SETFONT, (WPARAM)f, 0);
        SendMessage(hsrc, WM_SETFONT, (WPARAM)f, 0);
        load_values();
        SetTimer(hw, 1, 2000, NULL);
        return 0;
    }
    case WM_TIMER:
    {
        /* follow USB audio devices being plugged in and removed */
        BYTE oc = ndev ? devs[cur].core : 0xFF, oi = ndev ? devs[cur].id : 0xFF;
        if (list_devs()) {
            cur = 0;
            for (i = 0; i < ndev; i++)
                if (devs[i].core == oc && devs[i].id == oi) cur = i;
            fill_devices();
            load_values();
        }
        return 0;
    }
    case WM_VSCROLL:
    {
        HWND b = (HWND)HIWORD(lp);
        int pos;
        for (i = 0; i < NCTL && hbar[i] != b; i++) ;
        if (i == NCTL) break;
        pos = GetScrollPos(b, SB_CTL);
        switch (wp) {
        case SB_LINEUP: pos -= 2; break;
        case SB_LINEDOWN: pos += 2; break;
        case SB_PAGEUP: pos -= 10; break;
        case SB_PAGEDOWN: pos += 10; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = RANGE; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = LOWORD(lp); break;
        default: return 0;
        }
        if (pos < 0) pos = 0;
        if (pos > RANGE) pos = RANGE;
        SetScrollPos(b, SB_CTL, pos, TRUE);
        set_value(i, pos);
        return 0;
    }
    case WM_COMMAND:
        if (wp == ID_DEV && HIWORD(lp) == CBN_SELCHANGE) {
            cur = (int)SendMessage(hdev, CB_GETCURSEL, 0, 0);
            if (cur < 0) cur = 0;
            load_values();
        } else if (wp == ID_SRC && HIWORD(lp) == CBN_SELCHANGE) {
            set_source();
        }
        return 0;
    case WM_CTLCOLOR:
        if (HIWORD(lp) == CTLCOLOR_STATIC || HIWORD(lp) == CTLCOLOR_BTN) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)hface;
        }
        break;
    case WM_DESTROY:
        KillTimer(hw, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hw, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    MSG m;
    (void)cmd;
    hinst = hi;
    hface = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    scan();
    if (!present[0] && !present[1]) {
        MessageBox(NULL, "Neither WINAUDIO.SYS nor WINUSB.SYS is loaded.", "WINMIX", MB_OK | MB_ICONINFORMATION);
        if (rq) GlobalDosFree(reqsel);
        return 0;
    }
    if (!hprev) {
        fmemset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hi;
        wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "WINMIX";
        RegisterClass(&wc);
    }
    hmain = CreateWindow("WINMIX", "WIN Volume Control", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, 0, 392, 320, NULL, NULL, hi, NULL);
    ShowWindow(hmain, show);
    UpdateWindow(hmain);
    while (GetMessage(&m, NULL, 0, 0)) {
        if (!IsDialogMessage(hmain, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    GlobalDosFree(reqsel);
    DeleteObject(hface);
    return m.wParam;
}
