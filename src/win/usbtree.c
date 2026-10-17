/* usbtree.c - USBTREE.EXE: USB device tree and storage viewer for WINUSB / WINDISK
   (Windows 3.0 real/standard/386 enhanced mode and Windows 3.1, Win16).
   Left: the device tree, right: details of the selected entry. */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "winfmt.h"
#include "winfs.h"

#define ID_REFRESH  101
#define ID_EJECT    102
#define ID_LOG      103
#define ID_SAVE     104
#define ID_AUTO     105
#define NBTN        5
#define ID_LIST     200
#define ID_TEXT     201
#define ID_FNAME    100
#define MAXNODES    128
#define MAXVIS      (MAXNODES + 1)
#define DOSBUF      49152U
#define TEXTSZ      30000U
#define IND         14          /* tree indent per level, pixels */
#define V_DISKS     (-1)        /* list entry "Disks and partitions" */

struct rmcs {
    unsigned long edi, esi, ebp, rsv, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
};

static HINSTANCE hInst;
static HWND hMain, hList, hText, hBtn[NBTN];
static WORD dos_sel, dos_seg;
static struct winusbnode far nodes[MAXNODES];
static int nnodes;
static int vis[MAXVIS];             /* list item -> node index (V_DISKS for the disk overview) */
static unsigned vlines[MAXVIS];     /* tree line bits per list item */
static int nvis;
static unsigned long treesig;
static char *text;
static int have_usb, have_disk;
static unsigned char cur_mplex = WIN_MPLEX_USB;
static UINT timer_id;
static int itemh, btnh;
static int realmode;
static HBRUSH face;
static unsigned char far blob[4096];

/* ---- driver calls ---- */


/* Windows 95/98/ME run the VxD without the DOS driver: its protected-mode entry (INT 2Fh
   AX=1684h) takes the same requests (AX=3, CX:DX = real-mode address of the request) */
static unsigned long vxd_ent[2];        /* [0] WINUSB.386, [1] WINDISK.386 */

static unsigned long vxd_find(unsigned vid)
{
    unsigned es_ = 0, di_ = 0, started = 0;
    unsigned long ent;
    if (!(GetWinFlags() & WF_ENHANCED)) return 0;
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
    if (!es_ && !di_) return 0;
    ent = ((unsigned long)es_ << 16) | di_;
    _asm {
        xor ax, ax
        xor bx, bx
        call dword ptr ent
        mov started, bx
    }
    return started ? ent : 0;
}

/* INT 2Fh AH=cur_mplex AL=01h: request at offset 0 of the DOS buffer, data at offset 64 */
static int wincall(unsigned fn, unsigned long a0, unsigned long a1, unsigned long a2, unsigned long *retlen)
{
    struct rmcs rm;
    struct winapireq far *r = (struct winapireq far *)MK_FP(dos_sel, 0);
    union REGS regs;
    struct SREGS sr;
    if (!dos_sel) return WINE_NOCORE;
    _fmemset(r, 0, sizeof(*r));
    r->fn = fn;
    r->arg0 = a0;
    r->arg1 = a1;
    r->arg2 = a2;
    r->buflen = DOSBUF - 64;
    r->buf = ((unsigned long)dos_seg << 16) | 64;
    segread(&sr);
    if (vxd_ent[cur_mplex == WIN_MPLEX_USB ? 0 : 1]) {
        unsigned long ent = vxd_ent[cur_mplex == WIN_MPLEX_USB ? 0 : 1];
        unsigned rseg = dos_seg;
        _asm {
            mov ax, 3
            mov cx, rseg
            xor dx, dx
            call dword ptr ent
        }
    } else if (realmode) {
        /* Windows 3.0 real mode: the driver is called directly */
        regs.x.ax = ((unsigned)cur_mplex << 8) | 0x01;
        regs.x.bx = 0;
        sr.es = dos_seg;
        int86x(0x2F, &regs, &regs, &sr);
    } else {
        _fmemset(&rm, 0, sizeof(rm));
        rm.eax = ((unsigned long)cur_mplex << 8) | 0x01;
        rm.es = dos_seg;
        regs.x.ax = 0x0300;
        regs.x.bx = 0x2F;
        regs.x.cx = 0;
        sr.es = FP_SEG((void far *)&rm);
        regs.x.di = FP_OFF((void far *)&rm);
        int86x(0x31, &regs, &regs, &sr);
    }
    if (retlen) *retlen = r->retlen;
    return r->status;
}

static int windetect(unsigned char id)
{
    struct rmcs rm;
    union REGS regs;
    struct SREGS sr;
    segread(&sr);
    if (realmode) {
        regs.x.ax = (unsigned)id << 8;
        regs.x.bx = 0;
        int86x(0x2F, &regs, &regs, &sr);
        return regs.h.al == 0xFF && regs.x.bx == 0x3357;
    }
    _fmemset(&rm, 0, sizeof(rm));
    rm.eax = (unsigned long)id << 8;
    regs.x.ax = 0x0300;
    regs.x.bx = 0x2F;
    regs.x.cx = 0;
    sr.es = FP_SEG((void far *)&rm);
    regs.x.di = FP_OFF((void far *)&rm);
    int86x(0x31, &regs, &regs, &sr);
    return (rm.eax & 0xFF) == 0xFF && (rm.ebx & 0xFFFF) == 0x3357;
}

static void real_int(int n, unsigned ax, unsigned bx)
{
    struct rmcs rm;
    union REGS regs;
    struct SREGS sr;
    segread(&sr);
    if (realmode) {
        regs.x.ax = ax;
        regs.x.bx = bx;
        int86x(n, &regs, &regs, &sr);
        return;
    }
    _fmemset(&rm, 0, sizeof(rm));
    rm.eax = ax;
    rm.ebx = bx;
    regs.x.ax = 0x0300;
    regs.x.bx = n;
    regs.x.cx = 0;
    sr.es = FP_SEG((void far *)&rm);
    regs.x.di = FP_OFF((void far *)&rm);
    int86x(0x31, &regs, &regs, &sr);
}

static unsigned char far *dosdata(void)
{
    return (unsigned char far *)MK_FP(dos_sel, 64);
}

/* ---- disks, partitions, file systems ---- */

struct rdctx {
    unsigned char mplex;
    int disk;
    unsigned long ssize;
};

static int rd_sect(void *ctx, unsigned long lba, unsigned cnt, unsigned char *buf, unsigned size)
{
    struct rdctx *c = (struct rdctx *)ctx;
    unsigned char save = cur_mplex;
    unsigned long len;
    int rc;
    if ((unsigned long)cnt * c->ssize > size || (unsigned long)cnt * c->ssize > DOSBUF - 64) return -1;
    cur_mplex = c->mplex;
    rc = wincall(WINF_RAWREAD, (unsigned long)c->disk, lba, cnt, &len);
    cur_mplex = save;
    if (rc != WINE_OK || len < (unsigned long)cnt * c->ssize) return -1;
    _fmemcpy(buf, dosdata(), (unsigned)len);
    return 0;
}

static struct windiskinfo dk[24];
static struct winunit units[WIN_MAXUNITS];

/* disks of driver mplex; only the disks of USB device usbdev when it is not 0 */
static int disks_of(struct sbuf *s, unsigned char mplex, unsigned usbdev)
{
    unsigned long len;
    struct wininfo in;
    int n, i, nunits = 0, shown = 0;
    unsigned char save = cur_mplex;
    cur_mplex = mplex;
    if (wincall(WINF_INFO, 0, 0, 0, &len) != WINE_OK) {
        cur_mplex = save;
        return 0;
    }
    _fmemcpy(&in, dosdata(), sizeof(in));
    if (wincall(WINF_UNITS, 0, 0, 0, &len) == WINE_OK) {
        nunits = (int)(len / sizeof(struct winunit));
        if (nunits > WIN_MAXUNITS) nunits = WIN_MAXUNITS;
        _fmemcpy(units, dosdata(), nunits * sizeof(struct winunit));
    }
    if (wincall(WINF_DISKS, 0, 0, 0, &len) != WINE_OK) {
        cur_mplex = save;
        return 0;
    }
    n = (int)(len / sizeof(struct windiskinfo));
    if (n > 24) n = 24;
    _fmemcpy(dk, dosdata(), n * sizeof(struct windiskinfo));
    for (i = 0; i < n; i++) {
        static const char *kind[] = { "?", "AHCI", "NVMe", "USB", "IDE", "SD", "MMC" };
        struct fsdisk fd;
        struct rdctx rc;
        char sz[24];
        if (usbdev && dk[i].usbdev != usbdev) continue;
        shown++;
        win_size(sz, dk[i].sectors_lo, dk[i].sectors_hi, dk[i].ssize);
        sb_printf(s, "\r\nDisk %u (%s): %s\r\n", dk[i].index, kind[dk[i].kind <= 6 ? dk[i].kind : 0], dk[i].model);
        if (!dk[i].present) {
            sb_printf(s, "No medium\r\n");
            continue;
        }
        sb_printf(s, "Capacity:\t%s, %lu-byte sectors\r\n", sz, dk[i].ssize);
        if (dk[i].serial[0]) sb_printf(s, "Serial number:\t%s\r\n", dk[i].serial);
        sb_printf(s, "Activity:\t%lu read, %lu written, %lu errors\r\n", dk[i].reads, dk[i].writes, dk[i].errors);
        if (dk[i].shared) sb_printf(s, "Also a BIOS disk\r\n");
        rc.mplex = mplex;
        rc.disk = dk[i].index;
        rc.ssize = dk[i].ssize;
        fd.read = rd_sect;
        fd.ctx = &rc;
        fd.ssize = dk[i].ssize;
        fd.sectors = dk[i].sectors_hi ? 0xFFFFFFFFUL : dk[i].sectors_lo;
        fd.index = dk[i].index;
        fd.units = units;
        fd.nunits = nunits;
        fd.firstdrive = in.firstdrive;
        win_partitions(s, &fd);
    }
    cur_mplex = save;
    return shown;
}

/* ---- tree list ---- */

static void node_label(char *out, const struct winusbnode *n)
{
    struct sbuf s;
    int i;
    sb_init(&s, out, 160);
    if (n->type == UN_HC) {
        sb_printf(&s, "%s controller %02X:%02X.%u", win_hctype(n->hctype), (unsigned)(n->pciloc >> 8),
                  (unsigned)((n->pciloc >> 3) & 0x1F), (unsigned)(n->pciloc & 7));
        return;
    }
    if (n->type == UN_PORT) {
        sb_printf(&s, "Port %u: %s", n->port, win_portstate(n->portstatus));
        return;
    }
    if (n->product[0]) sb_printf(&s, "%u: %s", n->port, n->product);
    else if (n->driver) sb_printf(&s, "%u: USB %s", n->port, win_driver(n->driver));
    else sb_printf(&s, "%u: %s device", n->port, win_class(n->cls, n->sub, n->proto));
    if (n->letters[0]) {
        sb_printf(&s, " (");
        for (i = 0; i < 8 && n->letters[i]; i++) sb_printf(&s, "%s%c:", i ? " " : "", n->letters[i]);
        sb_printf(&s, ")");
    }
    if (n->portstatus & 0x00800000UL) sb_printf(&s, " - safe to remove");
}

static COLORREF node_color(const struct winusbnode *n)
{
    if (n->type == UN_HC) return RGB(0, 0, 128);
    if (n->type == UN_PORT) return RGB(128, 0, 0);
    switch (n->driver) {
    case UD_HUB: return RGB(128, 128, 128);
    case UD_BOT: case UD_UAS: return RGB(0, 128, 0);
    case UD_KBD: case UD_MOUSE: case UD_TABLET: case UD_HIDOTHER: return RGB(128, 0, 128);
    case UD_AUDIO: case UD_MIDI: return RGB(0, 128, 128);
    }
    return RGB(255, 255, 255);
}

static int vis_depth(int i)
{
    return vis[i] == V_DISKS ? 0 : nodes[vis[i]].depth;
}

static unsigned vis_key(int i)
{
    if (i < 0 || i >= nvis) return 0;
    return vis[i] == V_DISKS ? 0xFFFF : nodes[vis[i]].type == UN_HC ? 0x4000 | nodes[vis[i]].id : nodes[vis[i]].id;
}

/* empty ports in a normal state are not listed */
static int listed(const struct winusbnode *n)
{
    if (n->type != UN_PORT) return 1;
    return (n->portstatus & 1) || ((n->portstatus >> 24) & 0x7F) != 0;
}

static void build_lines(void)
{
    int i, j, l;
    for (i = 0; i < nvis; i++) {
        int d = vis_depth(i);
        vlines[i] = 0;
        for (l = 0; l < d && l < 16; l++) {
            for (j = i + 1; j < nvis; j++) {
                int dj = vis_depth(j);
                if (dj <= l) break;
                if (dj == l + 1) {
                    vlines[i] |= 1U << l;
                    break;
                }
            }
        }
    }
}

/* fetches the tree; returns 1 when anything visible changed */
static int fetch_tree(void)
{
    unsigned long len, sig = 0;
    int i, k;
    nnodes = 0;
    if (have_usb && wincall(WINF_USBTREE, 0, 0, 0, &len) == WINE_OK) {
        nnodes = (int)(len / sizeof(struct winusbnode));
        if (nnodes > MAXNODES) nnodes = MAXNODES;
        _fmemcpy(nodes, dosdata(), nnodes * sizeof(struct winusbnode));
    }
    for (i = 0; i < nnodes; i++) {
        const unsigned char *p = (const unsigned char *)&nodes[i];
        sig = sig * 31 + nodes[i].id + nodes[i].type + nodes[i].driver + (nodes[i].portstatus & 0x7F800001UL);
        for (k = 0; k < 8; k++) sig = sig * 7 + (unsigned char)nodes[i].letters[k];
        (void)p;
    }
    sig = sig * 31 + nnodes;
    if (sig == treesig) return 0;
    treesig = sig;
    return 1;
}

static void fill_list(void)
{
    unsigned key = vis_key((int)SendMessage(hList, LB_GETCURSEL, 0, 0));
    int i, sel = 0, w, maxw = 0;
    char line[160];
    HDC dc;
    HFONT of;
    nvis = 0;
    if (have_usb || have_disk) vis[nvis++] = V_DISKS;
    for (i = 0; i < nnodes && nvis < MAXVIS; i++)
        if (listed(&nodes[i])) vis[nvis++] = i;
    build_lines();
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    dc = GetDC(hList);
    of = SelectObject(dc, GetStockObject(SYSTEM_FONT));
    for (i = 0; i < nvis; i++) {
        if (vis[i] == V_DISKS) strcpy(line, "Disks and partitions");
        else node_label(line, &nodes[vis[i]]);
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)(LPSTR)line);
        w = LOWORD(GetTextExtent(dc, line, strlen(line))) + 4 + (vis_depth(i) + 1) * IND + 8;
        if (w > maxw) maxw = w;
        if (vis_key(i) == key) sel = i;
    }
    SelectObject(dc, of);
    ReleaseDC(hList, dc);
    SendMessage(hList, LB_SETHORIZONTALEXTENT, maxw, 0);
    if (nvis) SendMessage(hList, LB_SETCURSEL, sel, 0);
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, TRUE);
}

static void draw_item(const DRAWITEMSTRUCT FAR *di)
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int i = (int)di->itemID, d, l, x, my;
    char line[160];
    HBRUSH br;
    HPEN pen, op;
    COLORREF fg, bg;
    if (i < 0 || i >= nvis) return;
    if (di->itemAction == ODA_FOCUS) {
        DrawFocusRect(dc, &r);
        return;
    }
    bg = GetSysColor((di->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    fg = GetSysColor((di->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    br = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(dc, &r, br);
    DeleteObject(br);
    d = vis_depth(i);
    my = (r.top + r.bottom) / 2;
    pen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    op = SelectObject(dc, pen);
    for (l = 0; l < d; l++) {
        x = r.left + 4 + l * IND + IND / 2;
        if (l == d - 1) {
            MoveTo(dc, x, r.top);
            LineTo(dc, x, (vlines[i] & (1U << l)) ? r.bottom : my + 1);
            MoveTo(dc, x, my);
            LineTo(dc, x + IND / 2 + 1, my);
        } else if (vlines[i] & (1U << l)) {
            MoveTo(dc, x, r.top);
            LineTo(dc, x, r.bottom);
        }
    }
    SelectObject(dc, op);
    DeleteObject(pen);
    x = r.left + 4 + d * IND;
    if (vis[i] != V_DISKS) {
        RECT m;
        HBRUSH mb = CreateSolidBrush(node_color(&nodes[vis[i]]));
        SetRect(&m, x + 1, my - 4, x + 9, my + 4);
        FillRect(dc, &m, mb);
        DeleteObject(mb);
        FrameRect(dc, &m, GetStockObject(BLACK_BRUSH));
        x += 12;
    }
    SendMessage(hList, LB_GETTEXT, i, (LPARAM)(LPSTR)line);
    SetBkColor(dc, bg);
    SetTextColor(dc, fg);
    {
        RECT t;
        DWORD ext = GetTextExtent(dc, line, strlen(line));
        SetRect(&t, x, r.top, x + LOWORD(ext) + 4, r.bottom);
        ExtTextOut(dc, x + 2, r.top + 1, ETO_OPAQUE, &t, line, strlen(line), NULL);
        if (di->itemState & ODS_FOCUS) DrawFocusRect(dc, &t);
    }
}

/* ---- details ---- */

static void set_text(void)
{
    SetWindowText(hText, text);
}

static void show_message(const char *m)
{
    struct sbuf s;
    sb_init(&s, text, TEXTSZ);
    sb_printf(&s, "%s", m);
    set_text();
}

static void detail_hc(struct sbuf *s, const struct winusbnode *n)
{
    sb_printf(s, "%s host controller\r\n\r\n", win_hctype(n->hctype));
    if (n->product[0]) sb_printf(s, "Name:\t%s\r\n", n->product);
    sb_printf(s, "PCI ID:\t%04X:%04X\r\n", n->vid, n->pid);
    sb_printf(s, "PCI location:\tbus %u, device %u, function %u\r\n", (unsigned)(n->pciloc >> 8),
              (unsigned)((n->pciloc >> 3) & 0x1F), (unsigned)(n->pciloc & 7));
    sb_printf(s, "Registers:\t%08lX\r\n", n->mmio & ~0xFUL);
    if (n->irq == 0xFF) sb_printf(s, "Interrupt:\tnone (polled)\r\n");
    else sb_printf(s, "Interrupt:\tIRQ %u, %lu handled\r\n", n->irq, n->xfers);
    sb_printf(s, "Root ports:\t%u\r\n", n->nports);
    sb_printf(s, "Errors:\t%lu\r\n", n->errors);
}

static void detail_dev(struct sbuf *s, const struct winusbnode *n)
{
    unsigned long len;
    int i;
    sb_printf(s, "%s%s%s\r\n\r\n", n->manuf, n->manuf[0] ? " " : "", n->product[0] ? n->product : "USB device");
    sb_printf(s, "Vendor ID:\t%04X\r\n", n->vid);
    sb_printf(s, "Product ID:\t%04X\r\n", n->pid);
    sb_printf(s, "Revision:\t%X.%02X\r\n", n->bcd >> 8, n->bcd & 0xFF);
    if (n->serial[0]) sb_printf(s, "Serial number:\t%s\r\n", n->serial);
    sb_printf(s, "USB version:\t%X.%02X\r\n", n->bcdusb >> 8, n->bcdusb & 0xFF);
    sb_printf(s, "Speed:\t%s\r\n", win_speed(n->speed));
    sb_printf(s, "Class:\t%02X/%02X/%02X %s\r\n", n->cls, n->sub, n->proto, win_class(n->cls, n->sub, n->proto));
    sb_printf(s, "Driver:\t%s\r\n", win_driver(n->driver));
    sb_printf(s, "Port:\t%u, tier %u\r\n", n->port, n->depth);
    sb_printf(s, "Address:\t%u", n->addr);
    if (n->slot) sb_printf(s, ", xHCI slot %u", n->slot);
    sb_printf(s, "\r\nPower:\t%u mA, configuration %u\r\n", n->maxpower, n->config);
    sb_printf(s, "Interfaces:\t%u\r\n", n->iface_count);
    if (n->nports) sb_printf(s, "Hub ports:\t%u\r\n", n->nports);
    if (n->letters[0]) {
        sb_printf(s, "Drive letters:\t");
        for (i = 0; i < 8 && n->letters[i]; i++) sb_printf(s, "%c: ", n->letters[i]);
        sb_printf(s, "\r\n");
    }
    sb_printf(s, "Transfers:\t%lu, %lu errors\r\n", n->xfers, n->errors);
    if (n->portstatus & 0x00800000UL) sb_printf(s, "State:\tsafe to remove\r\n");
    if ((n->driver == UD_BOT || n->driver == UD_UAS) && !(n->portstatus & 0x00800000UL))
        disks_of(s, WIN_MPLEX_USB, n->id);
    if (wincall(WINF_USBDESC, n->id, 0, 0, &len) == WINE_OK) {
        if (len > sizeof(blob)) len = sizeof(blob);
        _fmemcpy(blob, dosdata(), (unsigned)len);
        win_descriptors(s, blob, (unsigned)len);
    }
}

static void show_disks(struct sbuf *s)
{
    int n = 0;
    sb_printf(s, "Disks and partitions\r\n");
    if (have_disk) {
        sb_printf(s, "\r\n== WINDISK ==\r\n");
        n += disks_of(s, WIN_MPLEX_DISK, 0);
    }
    if (have_usb) {
        sb_printf(s, "\r\n== WINUSB ==\r\n");
        n += disks_of(s, WIN_MPLEX_USB, 0);
    }
    if (!n) sb_printf(s, "\r\nNo disks.\r\n");
}

static void show_details(void)
{
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    struct sbuf s;
    const struct winusbnode *n;
    HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
    sb_init(&s, text, TEXTSZ);
    EnableWindow(hBtn[1], FALSE);
    if (sel < 0 || sel >= nvis) {
        if (!have_usb && !have_disk) sb_printf(&s, "WINUSB.SYS is not loaded.");
    } else if (vis[sel] == V_DISKS) {
        show_disks(&s);
    } else {
        n = &nodes[vis[sel]];
        if (n->type == UN_HC) detail_hc(&s, n);
        else if (n->type == UN_PORT) {
            sb_printf(&s, "Port %u\r\n\r\nState:\t%s\r\n", n->port, win_portstate(n->portstatus));
            sb_printf(&s, "Status:\t%s%s%s%s%s\r\n", (n->portstatus & 1) ? "connected " : "",
                      (n->portstatus & 2) ? "enabled " : "", (n->portstatus & 8) ? "powered " : "",
                      (n->portstatus & 0x10) ? "OVERCURRENT " : "", (n->portstatus & 0x80) ? "companion " : "");
        } else {
            detail_dev(&s, n);
            EnableWindow(hBtn[1], (n->driver == UD_BOT || n->driver == UD_UAS) && !(n->portstatus & 0x00800000UL));
        }
    }
    set_text();
    SetCursor(oc);
}

static void show_log(void)
{
    unsigned long len, i;
    unsigned o = 0;
    unsigned char far *d;
    if (wincall(WINF_LOG, 0, 0, 0, &len) != WINE_OK) return;
    d = dosdata();
    /* the tail that fits, LF -> CRLF */
    i = len > TEXTSZ / 2 - 8 ? len - (TEXTSZ / 2 - 8) : 0;
    for (; i < len && o < TEXTSZ - 4; i++) {
        if (d[i] == '\r') continue;
        if (d[i] == '\n') text[o++] = '\r';
        text[o++] = d[i];
    }
    text[o] = 0;
    set_text();
    SendMessage(hText, EM_SETSEL, 0, MAKELPARAM(o, o));
}

static void refresh(int force)
{
    if (fetch_tree() || force) {
        fill_list();
        show_details();
    }
}

static void eject(void)
{
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0), rc;
    char msg[160];
    const struct winusbnode *n;
    if (sel < 0 || sel >= nvis || vis[sel] == V_DISKS) return;
    n = &nodes[vis[sel]];
    if (n->type != UN_DEV) return;
    sprintf(msg, "Prepare \"%s %s\" for removal?\n\nAll files on its drives must be closed.", n->manuf, n->product);
    if (MessageBox(hMain, msg, "Safe removal", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    real_int(0x2F, 0x4A10, 0x0001);         /* SMARTDRV: commit */
    real_int(0x21, 0x0D00, 0);              /* DOS: flush buffers */
    rc = wincall(WINF_USBEJECT, n->id, 0, 0, NULL);
    if (rc == WINE_OK) MessageBox(hMain, "The device can now be removed safely.", "Safe removal", MB_OK | MB_ICONINFORMATION);
    else MessageBox(hMain, "Flushing the device failed.", "Safe removal", MB_OK | MB_ICONSTOP);
    refresh(1);
}

/* ---- report ---- */

static char fname[128] = "USBTREE.TXT";

BOOL CALLBACK _export NameDlgProc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hd, ID_FNAME, fname);
        SendDlgItemMessage(hd, ID_FNAME, EM_LIMITTEXT, sizeof(fname) - 1, 0);
        return TRUE;
    case WM_COMMAND:
        if (wp == IDOK) {
            GetDlgItemText(hd, ID_FNAME, fname, sizeof(fname));
            EndDialog(hd, 1);
            return TRUE;
        }
        if (wp == IDCANCEL) {
            EndDialog(hd, 0);
            return TRUE;
        }
    }
    return FALSE;
}

/* COMMDLG.DLL is not part of Windows 3.0: use it when it is there, else a simple dialog */
static int ask_name(void)
{
    typedef BOOL (FAR PASCAL *GSFN)(OPENFILENAME FAR *);
    OFSTRUCT ofs;
    HINSTANCE h;
    int ok = -1;
    if (!realmode && OpenFile("COMMDLG.DLL", &ofs, OF_EXIST) != -1) {
        h = LoadLibrary("COMMDLG.DLL");
        if ((UINT)h >= 32) {
            GSFN fn = (GSFN)GetProcAddress(h, "GetSaveFileName");
            if (fn) {
                OPENFILENAME of;
                memset(&of, 0, sizeof(of));
                of.lStructSize = sizeof(of);
                of.hwndOwner = hMain;
                of.lpstrFilter = "Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
                of.lpstrFile = fname;
                of.nMaxFile = sizeof(fname);
                of.lpstrDefExt = "txt";
                of.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                ok = fn(&of) ? 1 : 0;
            }
            FreeLibrary(h);
        }
    }
    if (ok < 0) {
        FARPROC p = MakeProcInstance((FARPROC)NameDlgProc, hInst);
        ok = DialogBox(hInst, MAKEINTRESOURCE(2), hMain, p) == 1;
        FreeProcInstance(p);
    }
    return ok;
}

static void save_report(void)
{
    FILE *f;
    int i;
    struct sbuf s;
    HCURSOR oc;
    if (!ask_name()) return;
    f = fopen(fname, "wb");
    if (!f) {
        MessageBox(hMain, "Cannot create the file.", "USBTREE", MB_OK | MB_ICONSTOP);
        return;
    }
    oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
    fprintf(f, "WINDRV USB / storage report\r\n===========================\r\n\r\n");
    for (i = 0; i < nnodes; i++) {
        char line[200];
        sb_init(&s, line, sizeof(line));
        win_node_line(&s, &nodes[i]);
        fprintf(f, "%s\r\n", line);
    }
    sb_init(&s, text, TEXTSZ);
    show_disks(&s);
    fprintf(f, "\r\n%s", text);
    for (i = 0; i < nnodes; i++) {
        if (nodes[i].type == UN_PORT) continue;
        sb_init(&s, text, TEXTSZ);
        sb_printf(&s, "\r\n----------------------------------------\r\n");
        if (nodes[i].type == UN_HC) detail_hc(&s, &nodes[i]);
        else detail_dev(&s, &nodes[i]);
        fputs(text, f);
    }
    fprintf(f, "\r\n\r\nEvent log\r\n---------\r\n");
    show_log();
    fputs(text, f);
    fclose(f);
    SetCursor(oc);
    show_message("Report saved.");
}

/* ---- window ---- */

static void layout(int w, int h)
{
    int i, x = 4, lw;
    HDC dc = GetDC(hMain);
    char t[40];
    for (i = 0; i < NBTN; i++) {
        int bw;
        GetWindowText(hBtn[i], t, sizeof(t));
        bw = LOWORD(GetTextExtent(dc, t, strlen(t))) + (i == NBTN - 1 ? 28 : 16);
        MoveWindow(hBtn[i], x, 4, bw, btnh, TRUE);
        x += bw + 4;
    }
    ReleaseDC(hMain, dc);
    lw = w * 2 / 5;
    if (lw < 150) lw = 150;
    MoveWindow(hList, 4, btnh + 8, lw, h - btnh - 12, TRUE);
    MoveWindow(hText, lw + 8, btnh + 8, w - lw - 12, h - btnh - 12, TRUE);
}

LRESULT CALLBACK _export MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static const char *labels[NBTN] = { "&Refresh", "Safe &removal", "&Log", "&Save report...", "&Auto refresh" };
    static int tabs[1] = { 64 };
    int i;
    switch (msg) {
    case WM_CREATE:
    {
        TEXTMETRIC tm;
        HDC dc = GetDC(hwnd);
        GetTextMetrics(dc, &tm);
        ReleaseDC(hwnd, dc);
        itemh = tm.tmHeight + 2;
        if (itemh < 12) itemh = 12;
        btnh = tm.tmHeight + 10;
        for (i = 0; i < NBTN; i++)
            hBtn[i] = CreateWindow("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | (i == NBTN - 1 ? BS_AUTOCHECKBOX : BS_PUSHBUTTON),
                                   0, 0, 10, 10, hwnd, (HMENU)(ID_REFRESH + i), hInst, NULL);
        hList = CreateWindow("LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY |
                             LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 0, 0, 10, 10, hwnd,
                             (HMENU)ID_LIST, hInst, NULL);
        hText = CreateWindow("EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd, (HMENU)ID_TEXT, hInst, NULL);
        SendMessage(hText, EM_SETTABSTOPS, 1, (LPARAM)(int FAR *)tabs);
        EnableWindow(hBtn[1], FALSE);
        SendMessage(hBtn[NBTN - 1], BM_SETCHECK, 1, 0);
        return 0;
    }
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT FAR *)lp)->itemHeight = itemh;
        return TRUE;
    case WM_DRAWITEM:
        /* Windows 3.0 passes 0 in wParam: the control is identified by CtlID */
        if (((const DRAWITEMSTRUCT FAR *)lp)->CtlID == ID_LIST) draw_item((const DRAWITEMSTRUCT FAR *)lp);
        return TRUE;
    case WM_CTLCOLOR:
        /* the check box on the button face colored window */
        if (HIWORD(lp) == CTLCOLOR_BTN) {
            if (!face) face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)(UINT)face;
        }
        break;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.x = 440;
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.y = 240;
        return 0;
    case WM_SIZE:
        if (wp != SIZEICONIC) layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_TIMER:
        refresh(0);
        return 0;
    case WM_COMMAND:
        switch (wp) {
        case ID_REFRESH: refresh(1); return 0;
        case ID_EJECT: eject(); return 0;
        case ID_LOG: show_log(); return 0;
        case ID_SAVE: save_report(); return 0;
        case ID_AUTO:
            if (SendMessage(hBtn[NBTN - 1], BM_GETCHECK, 0, 0)) {
                if (!timer_id) timer_id = SetTimer(hwnd, 1, 2000, NULL);
            } else if (timer_id) {
                KillTimer(hwnd, 1);
                timer_id = 0;
            }
            return 0;
        case ID_LIST:
            if (HIWORD(lp) == LBN_SELCHANGE) show_details();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (timer_id) KillTimer(hwnd, 1);
        if (face) DeleteObject(face);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    MSG m;
    DWORD d;
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN), w, h;
    (void)cmd;
    hInst = hi;
    realmode = !(GetWinFlags() & WF_PMODE);
    text = (char *)malloc(TEXTSZ);
    if (!text) return 0;
    if (!hprev) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = MainProc;
        wc.hInstance = hi;
        wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "WINUsbTree";
        if (!RegisterClass(&wc)) return 0;
    }
    d = GlobalDosAlloc(DOSBUF);
    dos_sel = LOWORD(d);
    dos_seg = HIWORD(d);
    vxd_ent[0] = vxd_find(WIN_VXD_USB);
    vxd_ent[1] = vxd_find(WIN_VXD_DISK);
    have_usb = dos_sel && (windetect(WIN_MPLEX_USB) || vxd_ent[0]);
    have_disk = dos_sel && (windetect(WIN_MPLEX_DISK) || vxd_ent[1]);
    w = cx - 40 > 760 ? 760 : cx - 40;
    h = cy - 60 > 520 ? 520 : cy - 60;
    hMain = CreateWindow("WINUsbTree", "USB Tree - WINDRV", WS_OVERLAPPEDWINDOW, (cx - w) / 2, (cy - h) / 3, w, h,
                         NULL, NULL, hi, NULL);
    ShowWindow(hMain, show);
    UpdateWindow(hMain);
    fetch_tree();
    fill_list();
    if (have_usb || have_disk) {
        /* start on the first device, or on the disk overview */
        int i;
        for (i = 0; i < nvis; i++)
            if (vis[i] != V_DISKS && nodes[vis[i]].type == UN_DEV && nodes[vis[i]].driver != UD_HUB) break;
        SendMessage(hList, LB_SETCURSEL, i < nvis ? i : 0, 0);
        show_details();
    } else {
        show_message("Neither WINUSB.SYS nor WINDISK.SYS is loaded. Load them from CONFIG.SYS.");
    }
    if (have_usb) timer_id = SetTimer(hMain, 1, 2000, NULL);
    else SendMessage(hBtn[NBTN - 1], BM_SETCHECK, 0, 0);
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    if (dos_sel) GlobalDosFree(dos_sel);
    return 0;
}
