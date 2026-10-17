/* winwheel.c - WINWHEEL.EXE: the helper of WINMOUSE.DRV. It turns wheel / tilt events into
   scroll messages for programs that do not know the wheel, sets the back / forward buttons
   option, and scrolls windows by moving the mouse after a click of the wheel (autoscroll).

   WIN.INI [WinWheel]:
     Lines=3            lines per wheel notch
     Invert=0           1: reverse the wheel
     XButtons=1         back / forward buttons type Alt+Left / Alt+Right
     AutoScroll=1       wheel click in a window with scroll bars starts autoscroll
     AutoScrollSpeed=100   percent
     NoAutoScroll=...   window classes that get the middle button themselves (comma separated) */
#include <windows.h>
#include <string.h>

#define WM_WINWHEEL   (WM_USER + 0x3D3)
#define WM_WINTILT    (WM_USER + 0x3D4)
#define WM_WINMBUTTON (WM_USER + 0x3D5)       /* wParam 1 middle button pressed, 0 released */
#define WM_WINASEND   (WM_USER + 0x3D6)       /* a click ended autoscroll */

#define WO_XBUTTONS   0x01
#define WO_MIDDLE     0x02

typedef void (FAR PASCAL *SETWHEELPROC)(HWND);
typedef void (FAR PASCAL *SETOPTPROC)(WORD);
typedef DWORD (FAR PASCAL *AUTOSCROLLPROC)(WORD);
typedef void (FAR PASCAL *MIDDLEPROC)(WORD);

static HINSTANCE hInst;
static int lines_per_notch = 3;
static int invert;
static SETOPTPROC set_opts;
static AUTOSCROLLPROC auto_scroll;
static MIDDLEPROC middle_button;

/* programs with their own wheel-click handling (autoscroll, open link in a new window ...) */
static const char def_noauto[] = "Internet Explorer_Server,_WwG,EXCEL7,MozillaWindowClass,OperaWindowClass";
static char noauto[256];

/* ---- autoscroll ---- */
#define AS_IDLE     0
#define AS_PASS     1           /* the middle button was handed to the program */
#define AS_PENDING  2           /* pressed over a window that scrolls: click or drag? */
#define AS_CLICK    3           /* running after a click, the next click ends it */
#define AS_HOLD     4           /* running while the button is held after a drag */
#define AS_TIMER    1
#define AS_TICK     30          /* ms */
#define AS_DEAD     8           /* mouse counts around the origin that do not scroll */
#define AS_MOVE     4           /* pixels that make a press a drag */

static int as_state;
static int as_speed = 100;
static HWND as_vt, as_vbar, as_ht, as_hbar;    /* vertical / horizontal target and scroll bar control */
static POINT as_press;
static DWORD as_last;
static long as_acc_v, as_acc_h;                 /* thousandths of a line */
static HCURSOR as_cur[5];                       /* origin, up, down, left, right */
static int as_ci;                               /* the one shown */
static HWND as_mark;                            /* small window under the held cursor */
static BYTE as_keys[256];

static BOOL has_style(HWND h, DWORD st)
{
    return (GetWindowLong(h, GWL_STYLE) & st) != 0;
}

static BOOL is_class(HWND h, const char *name)
{
    char cls[32];
    GetClassName(h, cls, sizeof(cls));
    return lstrcmpi(cls, name) == 0;
}

/* find a scroll bar control among the children of h */
static HWND find_scrollbar(HWND h, BOOL vertical)
{
    HWND c;
    for (c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c)) continue;
        if (is_class(c, "ScrollBar")) {
            DWORD st = GetWindowLong(c, GWL_STYLE);
            if (vertical ? (st & SBS_VERT) : !(st & SBS_VERT)) return c;
        }
    }
    return 0;
}

/* the window that scrolls for the window h: one with a built-in scroll bar (walking up to the
   top level), a list box, or the parent of a separate scroll bar control (*bar) */
static HWND find_target(HWND h, BOOL vertical, HWND *bar)
{
    HWND target, w, sb = 0;
    DWORD need = vertical ? WS_VSCROLL : WS_HSCROLL;
    *bar = 0;
    for (target = h; target; target = GetParent(target)) {
        if (has_style(target, need)) return target;
        if (vertical && (is_class(target, "ListBox") || is_class(target, "ComboLBox"))) return target;
    }
    for (w = h; w && !sb; w = GetParent(w)) sb = find_scrollbar(w, vertical);
    if (!sb) return 0;
    *bar = sb;
    return GetParent(sb);
}

/* n lines: positive up / left, negative down / right */
static void scroll_lines(HWND target, HWND bar, BOOL vertical, int n)
{
    UINT msg = vertical ? WM_VSCROLL : WM_HSCROLL;
    WPARAM code = n > 0 ? SB_LINEUP : SB_LINEDOWN;
    int i;
    for (i = 0; i < (n < 0 ? -n : n); i++)
        SendMessage(target, msg, code, MAKELPARAM(0, bar));
    SendMessage(target, msg, SB_ENDSCROLL, MAKELPARAM(0, bar));
}

static void scroll(int notches, BOOL vertical)
{
    POINT pt;
    HWND h, target, bar;
    int n;
    GetCursorPos(&pt);
    h = WindowFromPoint(pt);
    if (!h) return;
    target = find_target(h, vertical, &bar);
    if (!target) return;
    n = notches * lines_per_notch;
    if (invert) n = -n;
    scroll_lines(target, bar, vertical, n);
}

/* h or one of its parents has a class of the NoAutoScroll list */
static BOOL excluded(HWND h)
{
    char cls[64];
    for (; h; h = GetParent(h)) {
        const char *p = noauto;
        int n = GetClassName(h, cls, sizeof(cls));
        while (*p) {
            const char *e = p;
            int len;
            while (*e && *e != ',') e++;
            while (*p == ' ') p++;
            len = (int)(e - p);
            while (len > 0 && p[len - 1] == ' ') len--;
            if (len == n && len > 0) {
                char one[64];
                memcpy(one, p, len);
                one[len] = 0;
                if (!lstrcmpi(one, cls)) return TRUE;
            }
            p = *e ? e + 1 : e;
        }
    }
    return FALSE;
}

static void as_set_cursor(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, i = 0;
    if (ax > AS_DEAD || ay > AS_DEAD) {
        if (ay >= ax) i = dy < 0 ? 1 : 2;
        else i = dx < 0 ? 3 : 4;
    }
    as_ci = i;
    if (as_cur[i]) SetCursor(as_cur[i]);
}

/* The cursor stays where autoscroll started (WINMOUSE.DRV holds the motion back). A small
   topmost window of WINWHEEL under it makes Windows ask WINWHEEL for the cursor shape: on
   Windows 95/98/ME the cursor over a window of another program (32-bit ones above all) is
   that program's, whatever WINWHEEL sets. */
LRESULT CALLBACK _export MarkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETCURSOR:
        if (as_cur[as_ci]) SetCursor(as_cur[as_ci]);
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void mark_show(BOOL on)
{
    POINT pt;
    if (!on) {
        if (as_mark) ShowWindow(as_mark, SW_HIDE);
        return;
    }
    GetCursorPos(&pt);
    if (!as_mark)
        as_mark = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT, "WinWheelMark", "", WS_POPUP, pt.x - 16,
                                 pt.y - 16, 32, 32, NULL, NULL, hInst, NULL);
    if (!as_mark) return;
    SetWindowPos(as_mark, HWND_TOPMOST, pt.x - 16, pt.y - 16, 32, 32, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    /* a mouse move: the window under the cursor (the mark) sets the cursor now */
    SetCursorPos(pt.x, pt.y);
}

static void as_start(HWND hwnd, int state)
{
    int i;
    auto_scroll(1);
    as_state = state;
    as_last = GetTickCount();
    as_acc_v = as_acc_h = 0;
    for (i = 8; i < 256; i++) as_keys[i] = (BYTE)((GetAsyncKeyState(i) & 0x8000) != 0);
    as_set_cursor(0, 0);
    mark_show(TRUE);
    SetTimer(hwnd, AS_TIMER, AS_TICK, NULL);
}

static void as_stop(HWND hwnd)
{
    POINT pt;
    int was = as_state;
    as_state = AS_IDLE;
    KillTimer(hwnd, AS_TIMER);
    if (was == AS_CLICK || was == AS_HOLD) {
        mark_show(FALSE);
        auto_scroll(0);
        /* the window under the cursor sets its own cursor again */
        GetCursorPos(&pt);
        SetCursorPos(pt.x, pt.y);
    }
}

/* lines to scroll for an offset of the mouse from the origin over ms milliseconds */
static int as_lines(long *acc, int off, DWORD ms)
{
    int a = off < 0 ? -off : off, n;
    if (a <= AS_DEAD) {
        *acc = 0;
        return 0;
    }
    /* 0.4 lines per second for every count beyond the dead zone */
    *acc += (long)(a - AS_DEAD) * (long)ms * 2L * as_speed / 500L;
    n = (int)(*acc / 1000L);
    *acc -= (long)n * 1000L;
    if (n > 25) n = 25;
    return off < 0 ? n : -n;
}

static void as_tick(HWND hwnd)
{
    DWORD now = GetTickCount(), ms = now - as_last, r;
    int dx, dy, i, n;
    as_last = now;
    if (ms > 200) ms = 200;
    if ((!as_vt || !IsWindow(as_vt)) && (!as_ht || !IsWindow(as_ht))) {
        as_stop(hwnd);
        return;
    }
    /* any key ends it */
    for (i = 8; i < 256; i++) {
        BYTE down = (BYTE)((GetAsyncKeyState(i) & 0x8000) != 0);
        if (down && !as_keys[i]) {
            as_stop(hwnd);
            return;
        }
        as_keys[i] = down;
    }
    r = auto_scroll(2);
    dx = (int)LOWORD(r);
    dy = (int)HIWORD(r);
    if (as_vt && IsWindow(as_vt) && (n = as_lines(&as_acc_v, dy, ms)) != 0) scroll_lines(as_vt, as_vbar, TRUE, n);
    if (as_ht && IsWindow(as_ht) && (n = as_lines(&as_acc_h, dx, ms)) != 0) scroll_lines(as_ht, as_hbar, FALSE, n);
    as_set_cursor(dx, dy);
}

/* middle button pressed: autoscroll candidate when the window under the cursor scrolls and
   does not handle the middle button itself */
static BOOL as_target(void)
{
    POINT pt;
    HWND h;
    GetCursorPos(&pt);
    h = WindowFromPoint(pt);
    if (!h || excluded(h)) return FALSE;
    as_vt = find_target(h, TRUE, &as_vbar);
    as_ht = find_target(h, FALSE, &as_hbar);
    return as_vt || as_ht;
}

static void middle(HWND hwnd, BOOL down)
{
    if (down) {
        if (as_state == AS_CLICK || as_state == AS_HOLD) {
            as_stop(hwnd);
            return;
        }
        if (auto_scroll && as_target()) {
            as_state = AS_PENDING;
            GetCursorPos(&as_press);
            SetTimer(hwnd, AS_TIMER, AS_TICK, NULL);
        } else {
            as_state = AS_PASS;
            if (middle_button) middle_button(1);
        }
        return;
    }
    switch (as_state) {
    case AS_PASS:
        as_state = AS_IDLE;
        if (middle_button) middle_button(0);
        break;
    case AS_PENDING:
        KillTimer(hwnd, AS_TIMER);
        as_start(hwnd, AS_CLICK);
        break;
    case AS_HOLD:
        as_stop(hwnd);
        break;
    }
}

LRESULT CALLBACK _export WheelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_WINWHEEL:
        scroll((int)wp, TRUE);
        return 0;
    case WM_WINTILT:
        scroll(-(int)wp, FALSE);
        return 0;
    case WM_WINMBUTTON:
        middle(hwnd, wp != 0);
        return 0;
    case WM_WINASEND:
        if (as_state == AS_CLICK || as_state == AS_HOLD) as_stop(hwnd);
        return 0;
    case WM_TIMER:
        if (wp != AS_TIMER) break;
        if (as_state == AS_PENDING) {
            POINT pt;
            GetCursorPos(&pt);
            /* dragged with the button held: scroll until it is released */
            if (pt.x - as_press.x > AS_MOVE || as_press.x - pt.x > AS_MOVE ||
                pt.y - as_press.y > AS_MOVE || as_press.y - pt.y > AS_MOVE)
                as_start(hwnd, AS_HOLD);
        } else if (as_state == AS_CLICK || as_state == AS_HOLD) {
            as_tick(hwnd);
        } else {
            KillTimer(hwnd, AS_TIMER);
        }
        return 0;
    case WM_DESTROY:
        if (as_state == AS_CLICK || as_state == AS_HOLD) as_stop(hwnd);
        if (as_mark) DestroyWindow(as_mark);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG m;
    HMODULE hm;
    SETWHEELPROC setw;
    WORD opts = 0;
    int i;

    (void)show;
    if (hprev) {
        /* second start: /U unloads the running copy */
        if (cmd && (cmd[0] == '/' || cmd[0] == '-') && (cmd[1] == 'u' || cmd[1] == 'U')) {
            HWND old = FindWindow("WinWheelClass", NULL);
            if (old) PostMessage(old, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    hInst = hi;
    lines_per_notch = GetProfileInt("WinWheel", "Lines", 3);
    invert = GetProfileInt("WinWheel", "Invert", 0);
    if (lines_per_notch < 1) lines_per_notch = 1;
    if (lines_per_notch > 20) lines_per_notch = 20;
    if (GetProfileInt("WinWheel", "XButtons", 1)) opts |= WO_XBUTTONS;
    if (GetProfileInt("WinWheel", "AutoScroll", 1)) opts |= WO_MIDDLE;
    as_speed = GetProfileInt("WinWheel", "AutoScrollSpeed", 100);
    if (as_speed < 10) as_speed = 10;
    if (as_speed > 1000) as_speed = 1000;
    GetProfileString("WinWheel", "NoAutoScroll", def_noauto, noauto, sizeof(noauto));
    hm = GetModuleHandle("MOUSE");
    setw = hm ? (SETWHEELPROC)GetProcAddress(hm, "SetWheelWindow") : NULL;
    if (!setw) {
        MessageBox(NULL, "WINMOUSE.DRV is not the active mouse driver.\n"
                         "Set mouse.drv=winmouse.drv in SYSTEM.INI [boot].",
                   "WINWHEEL", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    set_opts = (SETOPTPROC)GetProcAddress(hm, "SetWheelOptions");
    middle_button = (MIDDLEPROC)GetProcAddress(hm, "MiddleButton");
    if (opts & WO_MIDDLE) auto_scroll = (AUTOSCROLLPROC)GetProcAddress(hm, "AutoScroll");
    if (!auto_scroll) opts &= ~WO_MIDDLE;
    for (i = 0; i < 5; i++) as_cur[i] = LoadCursor(hi, MAKEINTRESOURCE(i + 1));
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WheelWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = "WinWheelClass";
    if (!RegisterClass(&wc)) return 0;
    wc.lpfnWndProc = MarkWndProc;
    wc.hCursor = as_cur[0];
    wc.lpszClassName = "WinWheelMark";
    RegisterClass(&wc);
    hwnd = CreateWindow("WinWheelClass", "WIN Wheel", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!hwnd) return 0;
    setw(hwnd);
    if (set_opts) set_opts(opts);
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    setw(NULL);
    return 0;
}
