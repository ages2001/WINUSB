/* wingrp.c - WINGRP.EXE: creates the Program Manager groups that the DOS installers of
   WINDISK, WINUSB and WINAUDIO have requested. The installers cannot talk to Program
   Manager from DOS; they write the group into WIN.INI and start WINGRP once through the
   run= line:
       [WinDrv]
       Group.WINUSB=USB Devices|C:\WINDOWS\USBTREE.EXE;Volume Control|C:\WINDOWS\WINMIX.EXE
   WINGRP sends [CreateGroup()] and [AddItem()] to Program Manager over DDE (Windows 3.0
   and 3.1; the shell of Windows 95/98/ME answers the same commands with a Start Menu
   group), then removes the request and itself from WIN.INI. An item of the same name is
   deleted first, so that installing again does not make duplicates. */
#include <windows.h>
#include <dde.h>
#include <string.h>

#define SEC "WinDrv"

static HWND hserver;
static int acked;
static char keys[1024], val[1024], cmd[2048];

LRESULT CALLBACK GrpWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DDE_ACK:
        if (!hserver) {
            /* answer to WM_DDE_INITIATE: lp = application atom, topic atom */
            hserver = (HWND)wp;
            GlobalDeleteAtom(LOWORD(lp));
            GlobalDeleteAtom(HIWORD(lp));
        } else {
            /* answer to WM_DDE_EXECUTE: high word = the command memory */
            GlobalFree((HANDLE)HIWORD(lp));
            acked = 1;
        }
        return 0;
    case WM_DDE_TERMINATE:
        return 0;
    }
    return DefWindowProc(hw, msg, wp, lp);
}

static void pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

static int execute(HWND hw, const char *c)
{
    HANDLE h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, lstrlen(c) + 1);
    LPSTR p;
    DWORD t;
    if (!h)
        return -1;
    p = GlobalLock(h);
    lstrcpy(p, c);
    GlobalUnlock(h);
    acked = 0;
    if (!PostMessage(hserver, WM_DDE_EXECUTE, (WORD)hw, MAKELONG(0, h))) {
        GlobalFree(h);
        return -1;
    }
    for (t = GetTickCount(); !acked && GetTickCount() - t < 10000;)
        pump();
    return acked ? 0 : -1;
}

/* "[CreateGroup(name)][ShowGroup(name,1)][AddItem(path,title,WINGRP.EXE,icon)]..." from
   the request "title|path|icon;..." (the icons are the resources of WINGRP.EXE) */
static char self[128];

static const char *field(const char *p, char *out, int max, char end)
{
    int n = 0;
    while (*p && *p != end && *p != ';' && n < max - 1)
        out[n++] = *p++;
    out[n] = 0;
    while (*p && *p != end && *p != ';')
        p++;
    if (*p == end)
        p++;
    return p;
}

/* the group, then every item on its own: [DeleteItem()] fails harmlessly when there is no
   item of that name yet */
static void group(HWND hw, const char *name, const char *items)
{
    const char *p = items;
    char path[128], title[64], icon[8];
    int shell95 = (LOBYTE(LOWORD(GetVersion())) > 3 || HIBYTE(LOWORD(GetVersion())) >= 95);
    if (shell95)
        wsprintf(cmd, "[CreateGroup(%s)]", (LPSTR)name);
    else
        wsprintf(cmd, "[CreateGroup(%s)][ShowGroup(%s,1)]", (LPSTR)name, (LPSTR)name);
    if (execute(hw, cmd))
        return;
    while (*p) {
        p = field(p, title, sizeof(title), '|');
        p = field(p, path, sizeof(path), '|');
        p = field(p, icon, sizeof(icon), ';');
        if (!path[0] || lstrlen(path) + lstrlen(title) + lstrlen(self) + 24 >= (int)sizeof(cmd))
            continue;
        wsprintf(cmd, "[DeleteItem(%s)]", (LPSTR)title);
        execute(hw, cmd);
        lstrcpy(cmd, "[AddItem(");
        lstrcat(cmd, path);
        lstrcat(cmd, ",");
        lstrcat(cmd, title);
        if (icon[0] && self[0]) {
            lstrcat(cmd, ",");
            lstrcat(cmd, self);
            lstrcat(cmd, ",");
            lstrcat(cmd, icon);
        }
        lstrcat(cmd, ")]");
        execute(hw, cmd);
    }
}

/* removes WINGRP.EXE from the run= line of WIN.INI */
static void unrun(void)
{
    char line[512], out[512], tok[128];
    char *p = line;
    int n;
    GetProfileString("windows", "run", "", line, sizeof(line));
    out[0] = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (!*p)
            break;
        n = 0;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') {
            if (n < (int)sizeof(tok) - 1)
                tok[n++] = *p;
            p++;
        }
        tok[n] = 0;
        AnsiUpper(tok);
        if (n >= 10 && !lstrcmp(tok + n - 10, "WINGRP.EXE"))
            continue;
        if (n >= 6 && !lstrcmp(tok + n - 6, "WINGRP"))
            continue;
        if (out[0])
            lstrcat(out, " ");
        lstrcat(out, tok);
    }
    WriteProfileString("windows", "run", out);
}

int PASCAL WinMain(HANDLE hi, HANDLE hp, LPSTR cl, int show)
{
    WNDCLASS wc;
    HWND hw, hpm;
    ATOM app, topic;
    DWORD t0, t1;
    char *k;
    (void)cl;
    (void)show;
    if (!hp) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = GrpWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = "WinGrp";
        if (!RegisterClass(&wc))
            return 0;
    }
    GetModuleFileName(hi, self, sizeof(self));
    hw = CreateWindow("WinGrp", "WinGrp", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!hw)
        return 0;
    GetProfileString(SEC, NULL, "", keys, sizeof(keys));
    if (keys[0]) {
        /* the shell may still be starting: try for a while */
        for (t0 = GetTickCount(); !hserver && GetTickCount() - t0 < 20000;) {
            if (FindWindow("Progman", NULL)) {
                app = GlobalAddAtom("PROGMAN");
                topic = GlobalAddAtom("PROGMAN");
                SendMessage((HWND)-1, WM_DDE_INITIATE, (WORD)hw, MAKELONG(app, topic));
                GlobalDeleteAtom(app);
                GlobalDeleteAtom(topic);
            }
            if (!hserver)
                for (t1 = GetTickCount(); GetTickCount() - t1 < 500;)
                    pump();
        }
        for (k = keys; *k; k += lstrlen(k) + 1) {
            if (strnicmp(k, "Group.", 6))
                continue;
            GetProfileString(SEC, k, "", val, sizeof(val));
            if (hserver && val[0])
                group(hw, k + 6, val);
            WriteProfileString(SEC, k, NULL);
        }
        if (hserver)
            PostMessage(hserver, WM_DDE_TERMINATE, (WORD)hw, 0L);
        /* Windows 3.0 starts the shell minimized when run= has entries */
        if ((hpm = FindWindow("Progman", NULL)) != NULL && IsIconic(hpm))
            ShowWindow(hpm, SW_RESTORE);
    }
    unrun();
    DestroyWindow(hw);
    return 0;
}
