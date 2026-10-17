/* wincam.c - WINCAM.EXE: USB camera viewer for WINUSB (Windows 3.0 real/standard/386
   enhanced mode and Windows 3.1, Win16). Live picture, photos (24-bit BMP) and videos
   (AVI: Motion-JPEG as the camera sends it, or uncompressed 24-bit frames).
   Command line (for batch files): WINCAM [/MODE n] [/SNAP] [/REC seconds] [/QUIT]
     /MODE n   picture size n of the list (0 = first)   /SNAP  a photo of the first picture
     /REC s    a video of s seconds                      /QUIT  close when that is done
   The frames come from WINUSB.SYS through INT 2Fh (WINF_CAM_*), in pieces of the DOS
   transfer buffer, into a global memory block. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
#include <i86.h>
#include "winapi.h"
#include "camjpeg.h"

#define ID_CAM      101
#define ID_MODE     102
#define ID_START    103
#define ID_PHOTO    104
#define ID_REC      105
#define ID_FOLDER   106
#define ID_STATUS   107
#define ID_FNAME    100
#define NCTL        7
#define DOSBUF      49152U
#define DATAOFF     64
#define MAXCAMS     8
#define MAXMODES    32
#define STRIPMAX    32000U
#define MAXSTRIP    64
#define POLLMS      40
#define LISTMS      2000

struct rmcs {
    unsigned long edi, esi, ebp, rsv, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
};

static HINSTANCE hInst;
static HWND hMain, hCtl[NCTL];
static WORD dos_sel, dos_seg;
static int realmode, have_usb;
static int ctlh, barh;

/* cameras */
static struct wincamdev cams[MAXCAMS];
static int ncams;
static struct wincammode modes[MAXMODES];
static int nmodes;
static int cam = -1;                /* index into cams of the selected camera */
static int mode = -1;
static int streaming;
static unsigned long camsig;

/* the current frame */
static HGLOBAL hFrame;
static unsigned long framecap, framelen;
static struct winframe fhdr;
static WORD ahincr;
static unsigned char __far *fbase;  /* locked frame block */
static int have_frame;

/* picture: preview size, scale, strips of a bottom-up DIB */
static int pw, ph, pk = 1, pf = 1;  /* preview = frame / pk * pf */
static int bpp;                     /* 8: palette display, 24 */
static unsigned stride, spr;        /* bytes per preview row, rows per strip */
static int nstrip;
static unsigned char __far *strip[MAXSTRIP];
static HPALETTE hPal;
static struct {
    BITMAPINFOHEADER h;
    RGBQUAD c[256];
} bmi;
static unsigned char dith[16][256]; /* 0..5 per dither cell */
static unsigned char far *vga_lut;  /* 4096: r4 g4 b4 -> VGA color */
static signed char bayer4[16];      /* dither offsets */
static const unsigned char vga[16][3] = {
    { 0, 0, 0 }, { 128, 0, 0 }, { 0, 128, 0 }, { 128, 128, 0 }, { 0, 0, 128 }, { 128, 0, 128 }, { 0, 128, 128 },
    { 192, 192, 192 }, { 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 }, { 0, 0, 255 },
    { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 }
};
static RECT prect;                  /* preview area */

/* statistics and recording */
static DWORD fps_t0;
static unsigned fps_n;
static unsigned fps10;              /* frames per second * 10 */
static char folder[128];
static char msg[128];
static int derr;                    /* msg is a decoding error of the last picture */
static int stream_raw;              /* the last YUY2 picture went through without a frame buffer */
static int photo_req;               /* photo of the next streamed picture */
static FILE *avi;
static char aviname[160];
static unsigned long avi_frames, avi_movi, avi_maxchunk, avi_bytes;
static DWORD avi_t0, avi_t1;
static HGLOBAL hIdx;
static unsigned long idxcap;
static int avi_mjpg;

/* command line */
static int opt_mode = -1, opt_snap, opt_rec, opt_quit, opt_dump, opt_test;

/* color conversion: YUY2 / NV12 (BT.601, 16..235) */
static short ytab[256], vrt[256], ugt[256], vgt[256], ubt[256];
static unsigned char clampt[1024];
#define CLP(v)  clampt[(v) + 384]

static int stream_frame(unsigned long ret);
static void test_log(void);

/* ---------------------------------------------------------------- memory */

/* buffers that keep their address: fixed global blocks (in Windows 3.0 real mode
   moveable memory, the local heap included, may move whenever memory is allocated) */
static void far *galloc(unsigned long n)
{
    HGLOBAL h = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, n);
    return h ? GlobalLock(h) : NULL;
}

static void gfree(void far *p)
{
    HGLOBAL h;
    if (!p) return;
    h = (HGLOBAL)LOWORD(GlobalHandle(FP_SEG(p)));
    GlobalUnlock(h);
    GlobalFree(h);
}

/* ---------------------------------------------------------------- driver calls */


/* Windows 95/98/ME run the VxD without the DOS driver: its protected-mode entry (INT 2Fh
   AX=1684h) takes the same requests (AX=3, CX:DX = real-mode address of the request) */
static unsigned long vxd_ent[1];

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

static int wincall(unsigned fn, unsigned long a0, unsigned long a1, unsigned long *retlen)
{
    struct rmcs rm;
    struct winapireq far *r = (struct winapireq far *)MK_FP(dos_sel, 0);
    union REGS regs;
    struct SREGS sr;
    if (!dos_sel) return WINE_NOCORE;
    _fmemset(r, 0, sizeof(*r));
    r->fn = fn;
    r->status = 0xFFFF;
    r->arg0 = a0;
    r->arg1 = a1;
    r->buflen = DOSBUF - DATAOFF;
    r->buf = ((unsigned long)dos_seg << 16) | DATAOFF;
    segread(&sr);
    if (vxd_ent[0]) {
        unsigned long ent = vxd_ent[0];
        unsigned rseg = dos_seg;
        _asm {
            mov ax, 3
            mov cx, rseg
            xor dx, dx
            call dword ptr ent
        }
    } else if (realmode) {
        regs.x.ax = ((unsigned)WIN_MPLEX_USB << 8) | 0x01;
        regs.x.bx = 0;
        sr.es = dos_seg;
        int86x(0x2F, &regs, &regs, &sr);
    } else {
        _fmemset(&rm, 0, sizeof(rm));
        rm.eax = ((unsigned long)WIN_MPLEX_USB << 8) | 0x01;
        rm.es = dos_seg;
        regs.x.ax = 0x0300;
        regs.x.bx = 0x2F;
        regs.x.cx = 0;
        sr.es = FP_SEG((void far *)&rm);
        regs.x.di = FP_OFF((void far *)&rm);
        int86x(0x31, &regs, &regs, &sr);
    }
    if (retlen) *retlen = r->retlen;
    return r->status == 0xFFFF ? WINE_NOCORE : r->status;
}

static int windetect(void)
{
    struct rmcs rm;
    union REGS regs;
    struct SREGS sr;
    segread(&sr);
    if (realmode) {
        regs.x.ax = (unsigned)WIN_MPLEX_USB << 8;
        regs.x.bx = 0;
        int86x(0x2F, &regs, &regs, &sr);
        return regs.h.al == 0xFF && regs.x.bx == 0x3357;
    }
    _fmemset(&rm, 0, sizeof(rm));
    rm.eax = (unsigned long)WIN_MPLEX_USB << 8;
    regs.x.ax = 0x0300;
    regs.x.bx = 0x2F;
    regs.x.cx = 0;
    sr.es = FP_SEG((void far *)&rm);
    regs.x.di = FP_OFF((void far *)&rm);
    int86x(0x31, &regs, &regs, &sr);
    return (rm.eax & 0xFF) == 0xFF && (rm.ebx & 0xFFFF) == 0x3357;
}

static unsigned char far *dosdata(void)
{
    return (unsigned char far *)MK_FP(dos_sel, DATAOFF);
}

/* ---------------------------------------------------------------- frame memory */

/* pointer to byte 'off' of the locked frame block (more than 64K: selector tiling) */
static unsigned char __far *fptr(unsigned long off)
{
    return (unsigned char __far *)MK_FP(FP_SEG(fbase) + (unsigned)(off >> 16) * ahincr, (unsigned)off);
}

/* bytes from 'off' up to the end of its 64K piece */
static unsigned long fpiece(unsigned long off)
{
    return 0x10000UL - (off & 0xFFFF);
}

static void fcopy_in(unsigned long off, const unsigned char far *src, unsigned long n)
{
    while (n) {
        unsigned long k = fpiece(off);
        if (k > n) k = n;
        _fmemcpy(fptr(off), src, (unsigned)k);
        src += (unsigned)k;
        off += k;
        n -= k;
    }
}

static void fcopy_out(unsigned char far *dst, unsigned long off, unsigned n)
{
    while (n) {
        unsigned long k = fpiece(off);
        if (k > n) k = n;
        _fmemcpy(dst, fptr(off), (unsigned)k);
        dst += (unsigned)k;
        off += k;
        n -= (unsigned)k;
    }
}

static int frame_mem(unsigned long size)
{
    if (size <= framecap && hFrame) return 0;
    if (hFrame) {
        GlobalFree(hFrame);
        hFrame = 0;
        framecap = 0;
    }
    size = (size + 0xFFF) & ~0xFFFUL;
    hFrame = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hFrame) return -1;
    framecap = size;
    return 0;
}

static void frame_lock(void)
{
    fbase = (unsigned char __far *)GlobalLock(hFrame);
}

static void frame_unlock(void)
{
    if (fbase) GlobalUnlock(hFrame);
    fbase = NULL;
}

/* fetch the newest frame: WINE_OK, WINE_BUSY (none new) or an error */
static int fetch_frame(void)
{
    unsigned long ret, off, size;
    int r;
    r = wincall(WINF_CAM_FRAME, cams[cam].id, 0, &ret);
    if (r != WINE_OK) return r;
    if (ret < sizeof(struct winframe)) return WINE_IO;
    _fmemcpy(&fhdr, dosdata(), sizeof(fhdr));
    size = fhdr.size;
    stream_raw = 0;
    if (frame_mem(size)) {
        /* too little memory for the whole frame (Windows 3.0 real mode): YUY2 rows are
           used as they arrive */
        if (fhdr.type == CAM_YUY2) return stream_frame(ret);
        return WINE_SMALL;
    }
    frame_lock();
    if (!fbase) return WINE_SMALL;
    off = ret - sizeof(struct winframe);
    if (off > size) off = size;
    fcopy_in(0, dosdata() + sizeof(struct winframe), off);
    while (off < size) {
        r = wincall(WINF_CAM_FRAME, cams[cam].id, off, &ret);
        if (r != WINE_OK || !ret) break;
        if (ret > size - off) ret = size - off;
        fcopy_in(off, dosdata(), ret);
        off += ret;
    }
    frame_unlock();
    framelen = off;
    have_frame = off == size;
    return have_frame ? WINE_OK : WINE_IO;
}

/* ---------------------------------------------------------------- decoding */

static void make_tables(void)
{
    int i, k;
    static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    for (i = 0; i < 256; i++) {
        long y = i - 16, c = i - 128;
        ytab[i] = (short)((y * 76284L + 32768L) >> 16);        /* 1.164 */
        vrt[i] = (short)((c * 104597L + 32768L) >> 16);        /* 1.596 */
        ugt[i] = (short)((-c * 25675L + 32768L) >> 16);        /* 0.392 */
        vgt[i] = (short)((-c * 53279L + 32768L) >> 16);        /* 0.813 */
        ubt[i] = (short)((c * 132201L + 32768L) >> 16);        /* 2.017 */
    }
    for (i = 0; i < 1024; i++) clampt[i] = (unsigned char)(i < 384 ? 0 : i > 384 + 255 ? 255 : i - 384);
    /* 16 colors: nearest VGA color of each 4-bit RGB, dither offsets */
    vga_lut = (unsigned char far *)galloc(4096);
    if (vga_lut)
        for (i = 0; i < 4096; i++) {
            int r = (i >> 8) * 17, g = ((i >> 4) & 15) * 17, b = (i & 15) * 17, c, best = 0;
            long bd = 0x7FFFFFFFL;
            for (c = 0; c < 16; c++) {
                long dr = r - vga[c][0], dg = g - vga[c][1], db = b - vga[c][2];
                long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bd) {
                    bd = d;
                    best = c;
                }
            }
            vga_lut[i] = (unsigned char)best;
        }
    for (k = 0; k < 16; k++) bayer4[k] = (signed char)((bayer[k] - 8) * 3);
    /* ordered dither to 6 levels per channel (8-bit displays) */
    for (k = 0; k < 16; k++)
        for (i = 0; i < 256; i++) {
            int v = (i * 5 * 16 + bayer[k] * 255 / 1) / (255 * 16);
            dith[k][i] = (unsigned char)(v > 5 ? 5 : v);
        }
}

typedef void (*rowsink)(void *ctx, int y, const unsigned char far *bgr);

static unsigned char far *rowbuf;   /* source row (raw formats) */
static unsigned char far *rgbrow;   /* converted row */

static void yuv_row(unsigned char far *o, const unsigned char far *py, const unsigned char far *puv, int w,
                    int yuy2)
{
    int x;
    for (x = 0; x < w; x += 2, o += 6) {
        int y0, y1, u, v, r, g, b;
        if (yuy2) {
            y0 = ytab[py[0]];
            u = py[1];
            y1 = ytab[py[2]];
            v = py[3];
            py += 4;
        } else {
            y0 = ytab[py[0]];
            y1 = ytab[py[1]];
            py += 2;
            u = puv[0];
            v = puv[1];
            puv += 2;
        }
        r = vrt[v];
        g = ugt[u] + vgt[v];
        b = ubt[u];
        o[0] = CLP(y0 + b);
        o[1] = CLP(y0 + g);
        o[2] = CLP(y0 + r);
        if (x + 1 < fhdr.w) {
            o[3] = CLP(y1 + b);
            o[4] = CLP(y1 + g);
            o[5] = CLP(y1 + r);
        }
    }
}

static int jmore(struct jin *in)
{
    unsigned long k = fpiece(in->pos);
    if (in->pos >= in->size) return 0;
    if (k > in->size - in->pos) k = in->size - in->pos;
    if (k > 0xFFF0UL) k = 0xFFF0UL;
    in->p = fptr(in->pos);
    in->left = (unsigned)k;
    return 1;
}

/* decode the current frame row by row into 'sink'; 0 or an error text */
/* step: every step-th row and pixel; up: raw formats from the last row up (bottom-up
   files are then written in order) */
static const char *decode(rowsink sink, void *ctx, int step, int up)
{
    int y, w = fhdr.w, h = fhdr.h, r, x;
    const char *err = NULL;
    if (!have_frame) return "no picture";
    frame_lock();
    if (!fbase) return "out of memory";
    if (fhdr.type == CAM_MJPEG) {
        struct jin in;
        memset(&in, 0, sizeof(in));
        in.size = framelen;
        in.more = jmore;
        in.ystep = in.xstep = step;
        r = jpg_decode(&in, (jrow_fn)sink, ctx);
        if (r == JPG_UNSUP) err = "unsupported JPEG";
        else if (r == JPG_MEM) err = "out of memory";
        else if (r) err = "damaged JPEG frame";
    } else {
        int k;
        for (k = 0; k < h; k += step) {
            unsigned long off;
            y = up ? h - 1 - k : k;
            if (fhdr.type == CAM_NV12) {
                unsigned long uv = (unsigned long)w * h + (unsigned long)(y / 2) * w;
                off = (unsigned long)y * w;
                if ((unsigned long)w * h * 3 / 2 > framelen) break;
                fcopy_out(rowbuf, off, w);
                fcopy_out(rowbuf + 4096, uv, w);
                yuv_row(rgbrow, rowbuf, rowbuf + 4096, w, 0);
            } else {
                off = (unsigned long)y * w * 2;
                if (off + (unsigned long)w * 2 > framelen) break;
                fcopy_out(rowbuf, off, w * 2);
                yuv_row(rgbrow, rowbuf, NULL, w, 1);
            }
            if (step > 1)                               /* every step-th pixel */
                for (x = step; x < w; x += step) _fmemcpy(rgbrow + x / step * 3, rgbrow + x * 3, 3);
            sink(ctx, y, rgbrow);
        }
    }
    frame_unlock();
    return err;
}

/* ---------------------------------------------------------------- preview */

static void free_strips(void)
{
    int i;
    for (i = 0; i < nstrip; i++) {
        gfree(strip[i]);
        strip[i] = NULL;
    }
    nstrip = 0;
    pw = ph = 0;
}

/* preview layout for a w x h frame in the preview area */
static void layout(int w, int h)
{
    int aw = prect.right - prect.left, ah = prect.bottom - prect.top, i;
    unsigned rows;
    free_strips();
    if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return;
    pk = 1;
    while ((w / pk > aw || h / pk > ah) && pk < 16) pk++;
    pf = 1;
    while (pk == 1 && w * (pf + 1) <= aw && h * (pf + 1) <= ah && pf < 4) pf++;
    pw = w / pk;
    ph = h / pk;
    stride = (unsigned)(((long)pw * bpp / 8 + 3) & ~3L);
    spr = STRIPMAX / stride;
    if (!spr) spr = 1;
    for (i = 0, rows = 0; rows < (unsigned)ph && i < MAXSTRIP; i++, rows += spr) {
        unsigned n = (unsigned)ph - rows < spr ? (unsigned)ph - rows : spr;
        strip[i] = (unsigned char far *)galloc((unsigned long)stride * n);
        if (!strip[i]) break;
        _fmemset(strip[i], 0, stride * n);
    }
    nstrip = i;
    if ((unsigned long)nstrip * spr < (unsigned long)ph) ph = (int)((unsigned long)nstrip * spr);
}

static void preview_row(void *ctx, int y, const unsigned char far *bgr)
{
    int oy, s, r, n, x;
    unsigned char far *o;
    (void)ctx;
    if (y % pk) return;
    oy = y / pk;
    if (oy >= ph) return;
    s = oy / spr;
    r = oy % spr;
    n = (s == nstrip - 1) ? ph - s * spr : spr;
    o = strip[s] + (unsigned)(n - 1 - r) * stride;
    if (bpp == 24) {
        _fmemcpy(o, bgr, pw * 3);
    } else if (bpp == 8) {
        const unsigned char *d0;
        int row4 = (oy & 3) * 4;
        for (x = 0; x < pw; x++, bgr += 3) {
            d0 = dith[row4 + (x & 3)];
            *o++ = (unsigned char)(d0[bgr[2]] * 36 + d0[bgr[1]] * 6 + d0[bgr[0]]);
        }
    } else {
        /* 16 colors: ordered dither, nearest VGA color from 4 bits per channel */
        const signed char *b = bayer4 + (oy & 3) * 4;
        for (x = 0; x < pw; x++, bgr += 3) {
            int t = b[x & 3], r = CLP(bgr[2] + t), g = CLP(bgr[1] + t), bl = CLP(bgr[0] + t);
            unsigned char c = vga_lut[((r >> 4) << 8) | ((g >> 4) << 4) | (bl >> 4)];
            if (x & 1) *o++ |= c;
            else *o = (unsigned char)(c << 4);
        }
    }
}

static void paint_picture(HDC dc)
{
    int i, x0, y0, rows;
    HPALETTE op = NULL;
    if (!nstrip) return;
    if (hPal) {
        op = SelectPalette(dc, hPal, FALSE);
        RealizePalette(dc);
    }
    x0 = prect.left + ((prect.right - prect.left) - pw * pf) / 2;
    y0 = prect.top + ((prect.bottom - prect.top) - ph * pf) / 2;
    for (i = 0; i < nstrip; i++) {
        rows = (i == nstrip - 1) ? ph - i * (int)spr : (int)spr;
        bmi.h.biWidth = pw;
        bmi.h.biHeight = rows;
        bmi.h.biSizeImage = (DWORD)stride * rows;
        if (pf == 1)
            SetDIBitsToDevice(dc, x0, y0 + i * (int)spr, pw, rows, 0, 0, 0, rows, strip[i],
                              (BITMAPINFO FAR *)&bmi, DIB_RGB_COLORS);
        else
            StretchDIBits(dc, x0, y0 + i * (int)spr * pf, pw * pf, rows * pf, 0, 0, pw, rows, strip[i],
                          (BITMAPINFO FAR *)&bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    if (op) SelectPalette(dc, op, FALSE);
}

static void show_frame(void)
{
    HDC dc;
    const char *err;
    if (fhdr.w / pk != pw || fhdr.h / pk != ph || !nstrip) {
        layout(fhdr.w, fhdr.h);
        InvalidateRect(hMain, &prect, TRUE);
    }
    if (!nstrip) return;
    err = decode(preview_row, NULL, pk, 0);
    if (err) {
        lstrcpy(msg, err);
        derr = 1;
        return;
    }
    if (derr) {
        msg[0] = 0;
        derr = 0;
    }
    dc = GetDC(hMain);
    paint_picture(dc);
    ReleaseDC(hMain, dc);
}

/* ---------------------------------------------------------------- files */

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
    put16(p, (unsigned)v);
    put16(p + 2, (unsigned)(v >> 16));
}

static void fput32(FILE *f, unsigned long v)
{
    unsigned char b[4];
    put32(b, v);
    fwrite(b, 1, 4, f);
}

static void fput4cc(FILE *f, const char *s)
{
    fwrite(s, 1, 4, f);
}

/* a free name <folder>\<base>NNN.<ext> */
static int free_name(char *out, const char *base, const char *ext)
{
    int n, l;
    OFSTRUCT ofs;
    lstrcpy(out, folder);
    l = lstrlen(out);
    if (l && out[l - 1] != '\\' && out[l - 1] != ':') out[l++] = '\\';
    for (n = 1; n < 1000; n++) {
        sprintf(out + l, "%s%03d.%s", base, n, ext);
        if (OpenFile(out, &ofs, OF_EXIST) == -1) return 0;
    }
    return -1;
}

struct fsink {
    FILE *f;
    int next;                   /* row that follows in the file */
    long base;
    int h;
    unsigned stride;
    int w;
};

/* rows to a bottom-up 24-bit DIB in a file */
static void file_row(void *ctx, int y, const unsigned char far *bgr)
{
    struct fsink *s = (struct fsink *)ctx;
    static const unsigned char pad[4];
    if (y >= s->h) return;
    /* rows in file order need no seek (raw formats come bottom-up) */
    if (y != s->next) fseek(s->f, s->base + (long)(s->h - 1 - y) * s->stride, SEEK_SET);
    s->next = y - 1;
    fwrite(bgr, 1, s->w * 3, s->f);
    if (s->stride > (unsigned)s->w * 3) fwrite(pad, 1, s->stride - s->w * 3, s->f);
}

static void dib_header(unsigned char *b, int w, int h, unsigned long comp)
{
    memset(b, 0, 40);
    put32(b, 40);
    put32(b + 4, w);
    put32(b + 8, h);
    put16(b + 12, 1);
    put16(b + 14, 24);
    put32(b + 16, comp);
    put32(b + 20, comp ? (unsigned long)w * h * 3 : (((unsigned long)w * 3 + 3) & ~3UL) * h);
}

static char photoname[160];

/* a BMP file for the current picture size; 0 or -1 (msg says why) */
static int photo_open(struct fsink *s)
{
    unsigned char hdr[54];
    if (free_name(photoname, "PHOTO", "BMP")) {
        lstrcpy(msg, "No free file name in the folder");
        return -1;
    }
    s->f = fopen(photoname, "wb");
    if (!s->f) {
        sprintf(msg, "Cannot create %s", photoname);
        return -1;
    }
    s->next = -2;
    s->base = 54;
    s->h = fhdr.h;
    s->w = fhdr.w;
    s->stride = (unsigned)(((unsigned long)fhdr.w * 3 + 3) & ~3UL);
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + (unsigned long)s->stride * s->h);
    put32(hdr + 10, 54);
    dib_header(hdr + 14, fhdr.w, fhdr.h, 0);
    fwrite(hdr, 1, 54, s->f);
    return 0;
}

static void photo_close(struct fsink *s, const char *err)
{
    if (ferror(s->f)) err = "write error (disk full?)";
    fclose(s->f);
    if (err) sprintf(msg, "%s: %s", photoname, err);
    else sprintf(msg, "Saved %s", photoname);
    test_log();
}

static void photo(void)
{
    struct fsink s;
    if (!have_frame) {
        if (streaming && stream_raw) {
            photo_req = 1;                              /* taken from the next picture */
            return;
        }
        lstrcpy(msg, "No picture yet");
        return;
    }
    if (photo_open(&s)) return;
    photo_close(&s, decode(file_row, &s, 1, 1));
}

/* ---- AVI ---- */

#define AVI_HDR     224UL       /* RIFF, hdrl list, movi list header: first chunk */

static void avi_header(void)
{
    unsigned char b[AVI_HDR];
    unsigned long us = 0, rate = 1000000UL, len = avi_frames;
    int w = fhdr.w, h = fhdr.h;
    if (avi_frames > 1 && avi_t1 > avi_t0) us = (avi_t1 - avi_t0) * 1000UL / (avi_frames - 1);
    if (!us && mode >= 0) us = modes[mode].interval / 10;
    if (!us) us = 66666UL;
    memset(b, 0, sizeof(b));
    memcpy(b, "RIFF", 4);
    put32(b + 4, AVI_HDR - 8 + avi_bytes + (avi_frames ? 8 + avi_frames * 16 : 8));
    memcpy(b + 8, "AVI LIST", 8);
    put32(b + 16, 192);
    memcpy(b + 20, "hdrlavih", 8);
    put32(b + 28, 56);
    put32(b + 32, us);                                      /* dwMicroSecPerFrame */
    put32(b + 36, avi_maxchunk / (us / 1000 + 1) * 1000);  /* dwMaxBytesPerSec */
    put32(b + 44, 0x10);                                    /* AVIF_HASINDEX */
    put32(b + 48, len);
    put32(b + 56, 1);                                       /* streams */
    put32(b + 60, avi_maxchunk);
    put32(b + 64, w);
    put32(b + 68, h);
    memcpy(b + 88, "LIST", 4);
    put32(b + 92, 116);
    memcpy(b + 96, "strlstrh", 8);
    put32(b + 104, 56);
    memcpy(b + 108, "vids", 4);
    memcpy(b + 112, avi_mjpg ? "MJPG" : "DIB ", 4);
    put32(b + 128, us);                                     /* dwScale */
    put32(b + 132, rate);                                   /* dwRate */
    put32(b + 140, len);
    put32(b + 144, avi_maxchunk);
    put32(b + 148, 0xFFFFFFFFUL);                           /* dwQuality */
    put16(b + 160, w);
    put16(b + 162, h);
    memcpy(b + 164, "strf", 4);
    put32(b + 168, 40);
    dib_header(b + 172, w, h, avi_mjpg ? 0x47504A4DUL : 0);   /* 'MJPG' */
    memcpy(b + 212, "LIST", 4);
    put32(b + 216, 4 + avi_bytes);
    memcpy(b + 220, "movi", 4);
    fseek(avi, 0, SEEK_SET);
    fwrite(b, 1, sizeof(b), avi);
}

static void rec_start(void)
{
    if (!streaming || (!have_frame && !stream_raw)) {
        lstrcpy(msg, "Start the camera first");
        return;
    }
    if (free_name(aviname, "VIDEO", "AVI")) {
        lstrcpy(msg, "No free file name in the folder");
        return;
    }
    avi = fopen(aviname, "w+b");
    if (!avi) {
        sprintf(msg, "Cannot create %s", aviname);
        return;
    }
    setvbuf(avi, NULL, _IOFBF, 16384);
    avi_mjpg = fhdr.type == CAM_MJPEG;
    avi_frames = avi_bytes = avi_maxchunk = 0;
    avi_movi = AVI_HDR - 4;
    idxcap = 0;
    avi_header();
    sprintf(msg, "Recording %s", aviname);
}

static void rec_stop(void)
{
    unsigned long i;
    if (!avi) return;
    fseek(avi, 0, SEEK_END);
    fput4cc(avi, "idx1");
    fput32(avi, avi_frames * 16);
    if (hIdx && avi_frames) {
        unsigned char __far *p = (unsigned char __far *)GlobalLock(hIdx);
        for (i = 0; i < avi_frames; i++) {
            unsigned long e = i * 8;
            unsigned char __far *q = (unsigned char __far *)MK_FP(FP_SEG(p) + (unsigned)(e >> 16) * ahincr,
                                                                  (unsigned)e);
            fput4cc(avi, avi_mjpg ? "00dc" : "00db");
            fput32(avi, 0x10);                              /* AVIIF_KEYFRAME */
            fwrite(q, 1, 8, avi);
        }
        GlobalUnlock(hIdx);
    }
    avi_header();
    if (ferror(avi)) sprintf(msg, "%s: write error (disk full?)", aviname);
    else sprintf(msg, "Saved %s, %lu frames", aviname, avi_frames);
    fclose(avi);
    avi = NULL;
    if (hIdx) GlobalFree(hIdx);
    hIdx = 0;
}

static int idx_add(unsigned long off, unsigned long size)
{
    unsigned long e = avi_frames * 8;
    unsigned char __far *p, __far *q;
    if (e + 8 > idxcap) {
        HGLOBAL n = hIdx ? GlobalReAlloc(hIdx, idxcap + 16384, GMEM_MOVEABLE) : GlobalAlloc(GMEM_MOVEABLE, 16384);
        if (!n) return -1;
        hIdx = n;
        idxcap += 16384;
    }
    p = (unsigned char __far *)GlobalLock(hIdx);
    q = (unsigned char __far *)MK_FP(FP_SEG(p) + (unsigned)(e >> 16) * ahincr, (unsigned)e);
    put32((unsigned char *)q, off);
    put32((unsigned char *)q + 4, size);
    GlobalUnlock(hIdx);
    return 0;
}

static unsigned long rec_open_dib(struct fsink *s);
static void rec_close(unsigned long size);

static void rec_frame(void)
{
    unsigned long size, pos, start = AVI_HDR + avi_bytes;
    struct fsink s;
    int dht = 1;
    if (!avi || !have_frame) return;
    if (avi_mjpg != (fhdr.type == CAM_MJPEG)) return;
    fseek(avi, start, SEEK_SET);
    if (avi_mjpg) {
        struct jin in;
        frame_lock();
        if (!fbase) return;
        memset(&in, 0, sizeof(in));
        in.size = framelen;
        in.more = jmore;
        dht = jpg_has_dht(&in);
        size = framelen + (dht ? 0 : sizeof(jpg_std_dht));
        fput4cc(avi, "00dc");
        fput32(avi, size);
        /* cameras leave out the Huffman tables: put the standard ones behind SOI */
        fwrite(fbase, 1, 2, avi);
        if (!dht) fwrite(jpg_std_dht, 1, sizeof(jpg_std_dht), avi);
        for (pos = 2; pos < framelen; ) {
            unsigned long k = fpiece(pos);
            if (k > framelen - pos) k = framelen - pos;
            if (k > 0x8000UL) k = 0x8000UL;
            fwrite(fptr(pos), 1, (unsigned)k, avi);
            pos += k;
        }
        frame_unlock();
    } else {
        size = rec_open_dib(&s);
        decode(file_row, &s, 1, 1);
    }
    rec_close(size);
}

/* YUY2 without a frame buffer: the rows go to the preview, a requested photo and the
   movie while the frame is fetched */
static int stream_frame(unsigned long ret)
{
    struct fsink ps, rs;
    unsigned long off, size = fhdr.size, rsize = 0;
    unsigned rowbytes = fhdr.w * 2, carry = 0;
    int doph = photo_req, dorec = avi && !avi_mjpg, y = 0, r = WINE_OK, x;
    unsigned char far *src;
    unsigned long n;
    if (!fhdr.w || rowbytes > 8192) return WINE_SMALL;
    stream_raw = 1;
    photo_req = 0;
    if (fhdr.w / pk != pw || fhdr.h / pk != ph || !nstrip) {
        layout(fhdr.w, fhdr.h);
        InvalidateRect(hMain, &prect, TRUE);
    }
    if (doph && photo_open(&ps)) doph = 0;
    if (dorec) rsize = rec_open_dib(&rs);
    off = 0;
    src = dosdata() + sizeof(struct winframe);
    n = ret - sizeof(struct winframe);
    for (;;) {
        if (n > size - off) n = size - off;
        off += n;
        while (n) {
            unsigned k = rowbytes - carry;
            if (k > n) k = (unsigned)n;
            _fmemcpy(rowbuf + carry, src, k);
            carry += k;
            src += k;
            n -= k;
            if (carry < rowbytes) break;
            carry = 0;
            yuv_row(rgbrow, rowbuf, NULL, fhdr.w, 1);
            if (doph) file_row(&ps, y, rgbrow);
            if (dorec) file_row(&rs, y, rgbrow);
            if (nstrip && y % pk == 0) {
                if (pk > 1)
                    for (x = pk; x < fhdr.w; x += pk) _fmemcpy(rgbrow + x / pk * 3, rgbrow + x * 3, 3);
                preview_row(NULL, y, rgbrow);
            }
            y++;
        }
        if (off >= size) break;
        r = wincall(WINF_CAM_FRAME, cams[cam].id, off, &ret);
        if (r != WINE_OK || !ret) {
            if (r == WINE_OK) r = WINE_IO;
            break;
        }
        src = dosdata();
        n = ret;
    }
    if (doph) photo_close(&ps, y < fhdr.h ? "picture incomplete" : NULL);
    if (dorec) rec_close(rsize);
    if (y >= fhdr.h && nstrip) {
        HDC dc = GetDC(hMain);
        paint_picture(dc);
        ReleaseDC(hMain, dc);
    }
    return y >= fhdr.h ? WINE_OK : r;
}

/* an uncompressed frame chunk at the end of the movie: header, the rows go to 's' */
static unsigned long rec_open_dib(struct fsink *s)
{
    unsigned long start = AVI_HDR + avi_bytes, size;
    s->f = avi;
    s->next = -2;
    s->w = fhdr.w;
    s->h = fhdr.h;
    s->stride = (unsigned)(((unsigned long)fhdr.w * 3 + 3) & ~3UL);
    s->base = (long)start + 8;
    size = (unsigned long)s->stride * s->h;
    fseek(avi, start, SEEK_SET);
    fput4cc(avi, "00db");
    fput32(avi, size);
    return size;
}

/* the chunk of 'size' bytes is written: index, totals */
static void rec_close(unsigned long size)
{
    unsigned long start = AVI_HDR + avi_bytes;
    fseek(avi, start + 8 + size, SEEK_SET);
    if (size & 1) fputc(0, avi);
    if (idx_add(start - avi_movi, size)) {
        rec_stop();
        lstrcpy(msg, "Recording stopped: out of memory");
        return;
    }
    if (!avi_frames) avi_t0 = GetTickCount();
    avi_t1 = GetTickCount();
    avi_frames++;
    avi_bytes += 8 + ((size + 1) & ~1UL);
    if (size > avi_maxchunk) avi_maxchunk = size;
    if (ferror(avi)) {
        rec_stop();
        lstrcpy(msg, "Recording stopped: write error (disk full?)");
    }
}

/* ---------------------------------------------------------------- cameras */

static void status(void)
{
    char s[200];
    if (!have_usb) {
        SetWindowText(hCtl[6], "WINUSB.SYS is not loaded.");
        return;
    }
    if (!ncams) {
        SetWindowText(hCtl[6], "No camera connected.");
        return;
    }
    s[0] = 0;
    if (streaming && mode >= 0)
        sprintf(s, "%ux%u %s, %u.%u fps", modes[mode].w, modes[mode].h,
                modes[mode].type == CAM_MJPEG ? "MJPEG" : modes[mode].type == CAM_NV12 ? "NV12" : "YUY2",
                fps10 / 10, fps10 % 10);
    else
        lstrcpy(s, "Stopped");
    if (avi) {
        DWORD t = avi_frames ? (avi_t1 - avi_t0) / 1000 : 0;
        sprintf(s + strlen(s), "  |  REC %02lu:%02lu, %lu frames", t / 60, t % 60, avi_frames);
    }
    if (msg[0]) sprintf(s + strlen(s), "  |  %s", msg);
    SetWindowText(hCtl[6], s);
}

static void enable_ctls(void)
{
    EnableWindow(hCtl[0], ncams > 0 && !avi);
    EnableWindow(hCtl[1], nmodes > 0 && !avi);
    EnableWindow(hCtl[2], ncams > 0 && mode >= 0);
    EnableWindow(hCtl[3], have_frame || (streaming && stream_raw));
    EnableWindow(hCtl[4], streaming);
    SetWindowText(hCtl[2], streaming ? "Stop" : "Start");
    SetWindowText(hCtl[4], avi ? "Stop rec." : "Record");
}

static void cam_stop(void)
{
    if (avi) rec_stop();
    if (streaming && cam >= 0) wincall(WINF_CAM_STOP, cams[cam].id, 0, NULL);
    streaming = 0;
    fps10 = 0;
}

static const char *type_name(int t)
{
    return t == CAM_MJPEG ? "MJPEG" : t == CAM_NV12 ? "NV12" : "YUY2";
}

static void fill_modes(void)
{
    unsigned long ret;
    int i, best = -1;
    char s[64];
    SendMessage(hCtl[1], CB_RESETCONTENT, 0, 0);
    nmodes = 0;
    mode = -1;
    if (cam < 0) return;
    if (wincall(WINF_CAM_MODES, cams[cam].id, 0, &ret) == WINE_OK) {
        nmodes = (int)(ret / sizeof(struct wincammode));
        if (nmodes > MAXMODES) nmodes = MAXMODES;
        _fmemcpy(modes, dosdata(), nmodes * sizeof(struct wincammode));
    }
    for (i = 0; i < nmodes; i++) {
        struct wincammode *m = &modes[i];
        unsigned long f10 = m->interval ? 100000000UL / m->interval : 0;
        sprintf(s, "%ux%u %s %lu.%lu fps%s", m->w, m->h, type_name(m->type), f10 / 10, f10 % 10,
                m->maxsize ? "" : " (too large)");
        SendMessage(hCtl[1], CB_ADDSTRING, 0, (LPARAM)(LPSTR)s);
        /* default: the largest picture up to 640x480 */
        if (m->maxsize && m->w <= 640 && (best < 0 || (long)m->w * m->h > (long)modes[best].w * modes[best].h ||
            ((long)m->w * m->h == (long)modes[best].w * modes[best].h && m->type == CAM_MJPEG)))
            best = i;
    }
    mode = best;
    if (mode >= 0) SendMessage(hCtl[1], CB_SETCURSEL, mode, 0);
}

/* the camera list; returns nonzero when it changed */
static int list_cams(void)
{
    unsigned long ret, sig = 0;
    int i, n = 0, keep = -1;
    char s[64];
    struct wincamdev old;
    memset(&old, 0, sizeof(old));
    if (!have_usb) return 0;
    if (wincall(WINF_CAM_DEVS, 0, 0, &ret) == WINE_OK) {
        n = (int)(ret / sizeof(struct wincamdev));
        if (n > MAXCAMS) n = MAXCAMS;
    }
    for (i = 0; i < n; i++) {
        struct wincamdev far *d = (struct wincamdev far *)dosdata() + i;
        sig = sig * 31 + d->id * 7 + d->gone + 1;
    }
    sig += n;
    if (sig == camsig) return 0;
    camsig = sig;
    if (cam >= 0) old = cams[cam];
    _fmemcpy(cams, dosdata(), n * sizeof(struct wincamdev));
    for (i = 0; i < n; i++)
        if (cam >= 0 && cams[i].id == old.id && !cams[i].gone) keep = i;
    if (cam >= 0 && keep < 0) {
        /* the camera went away */
        if (avi) rec_stop();
        streaming = 0;
        lstrcpy(msg, "Camera removed");
    }
    ncams = n;
    SendMessage(hCtl[0], CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        sprintf(s, "%d: %s%s", i + 1, (char far *)cams[i].name, cams[i].gone ? " (removed)" : "");
        SendMessage(hCtl[0], CB_ADDSTRING, 0, (LPARAM)(LPSTR)s);
    }
    if (keep >= 0) {
        cam = keep;
        SendMessage(hCtl[0], CB_SETCURSEL, cam, 0);
    } else {
        for (cam = 0; cam < n && cams[cam].gone; cam++) ;
        if (cam == n) cam = -1;
        if (cam >= 0) SendMessage(hCtl[0], CB_SETCURSEL, cam, 0);
        fill_modes();
    }
    return 1;
}

static void cam_start(void)
{
    int r;
    if (cam < 0 || mode < 0) return;
    cam_stop();
    r = wincall(WINF_CAM_START, cams[cam].id, mode, NULL);
    if (r == WINE_OK) {
        streaming = 1;
        msg[0] = 0;
        fps_t0 = GetTickCount();
        fps_n = 0;
        have_frame = 0;
    } else {
        sprintf(msg, r == WINE_SMALL ? "Not enough driver memory for this picture size" :
                     r == WINE_NODEV ? "Camera removed" : "Camera start failed (%d)", r);
    }
}

/* /TEST: every picture size in turn, a photo and a 2 second video of each */
static void test_log(void)
{
    FILE *f;
    if (!opt_test) return;
    f = fopen("C:\\WINCAMT.TXT", "a");
    if (!f) return;
    fprintf(f, "mode %d %ux%u %s: %s\n", mode, modes[mode].w, modes[mode].h, type_name(modes[mode].type), msg);
    fclose(f);
}

static void test_next(void)
{
    while (++mode < nmodes) {
        SendMessage(hCtl[1], CB_SETCURSEL, mode, 0);
        cam_start();
        if (streaming) {
            opt_snap = 1;
            opt_rec = 2;
            return;
        }
        test_log();
    }
    opt_test = 0;
    opt_quit = 1;
    PostMessage(hMain, WM_CLOSE, 0, 0);
}

static void poll(void)
{
    int r;
    DWORD now;
    if (!streaming) return;
    r = fetch_frame();
    if (r == WINE_BUSY) return;
    if (r == WINE_NODEV) {
        list_cams();
        cam_stop();
        lstrcpy(msg, "Camera removed");
        enable_ctls();
        status();
        return;
    }
    if (r == WINE_SMALL) {
        cam_stop();
        lstrcpy(msg, "Out of memory for the frame");
        if (opt_test) {
            test_log();
            opt_snap = opt_rec = 0;
            test_next();
        }
        enable_ctls();
        status();
        return;
    }
    if (r != WINE_OK) return;
    if (!stream_raw) {
        if (avi) rec_frame();
        show_frame();
    }
    if (opt_dump) {
        /* debugging: the raw frame */
        FILE *f = fopen("C:\\FRAME.RAW", "wb");
        opt_dump = 0;
        if (f) {
            unsigned long o;
            frame_lock();
            fwrite(&fhdr, 1, sizeof(fhdr), f);
            for (o = 0; o < framelen; o += 1024) fwrite(fptr(o), 1, (unsigned)(framelen - o < 1024UL ? framelen - o : 1024UL), f);
            frame_unlock();
            fclose(f);
        }
    }
    if (opt_snap) {
        opt_snap = 0;
        photo();
    }
    if (opt_rec > 0 && !avi) {
        rec_start();
        opt_rec = -opt_rec;
    } else if (opt_rec < 0 && avi && avi_frames && avi_t1 - avi_t0 >= (DWORD)-opt_rec * 1000UL) {
        rec_stop();
        opt_rec = 0;
        test_log();
        if (opt_test) test_next();
    }
    if (opt_quit && !opt_snap && opt_rec <= 0 && !avi) {
        opt_quit = 0;
        PostMessage(hMain, WM_CLOSE, 0, 0);
    }
    enable_ctls();
    fps_n++;
    now = GetTickCount();
    if (now - fps_t0 >= 2000) {
        fps10 = (unsigned)((unsigned long)fps_n * 10000UL / (now - fps_t0));
        fps_t0 = now;
        fps_n = 0;
    }
    status();
}

/* ---------------------------------------------------------------- folder dialog */

BOOL CALLBACK _export FolderDlgProc(HWND hd, UINT m, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemText(hd, ID_FNAME, folder);
        SendDlgItemMessage(hd, ID_FNAME, EM_LIMITTEXT, sizeof(folder) - 1, 0);
        return TRUE;
    case WM_COMMAND:
        if (wp == IDOK) {
            GetDlgItemText(hd, ID_FNAME, folder, sizeof(folder));
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

static void ask_folder(void)
{
    FARPROC p = MakeProcInstance((FARPROC)FolderDlgProc, hInst);
    if (DialogBox(hInst, MAKEINTRESOURCE(2), hMain, p) == 1) {
        WriteProfileString("WINCAM", "Folder", folder);
        sprintf(msg, "Photos and videos go to %s", folder);
    }
    FreeProcInstance(p);
}

/* ---------------------------------------------------------------- window */

static void place_ctls(int cx, int cy)
{
    static const int wid[6] = { 140, 176, 52, 52, 80, 64 };
    int i, x = 6;
    for (i = 0; i < 6; i++) {
        MoveWindow(hCtl[i], x, 6, wid[i], i < 2 ? ctlh * 8 : ctlh, TRUE);
        x += wid[i] + 6;
    }
    MoveWindow(hCtl[6], 6, cy - barh + 3, cx - 12, barh - 6, TRUE);
    prect.left = 4;
    prect.top = ctlh + 12;
    prect.right = cx - 4;
    prect.bottom = cy - barh - 2;
}

static void make_palette(HDC dc)
{
    int i;
    bpp = 24;
    if ((GetDeviceCaps(dc, RASTERCAPS) & RC_PALETTE) && GetDeviceCaps(dc, SIZEPALETTE) == 256) {
        LOGPALETTE *lp = (LOGPALETTE *)malloc(sizeof(LOGPALETTE) + 216 * sizeof(PALETTEENTRY));
        if (lp) {
            lp->palVersion = 0x300;
            lp->palNumEntries = 216;
            for (i = 0; i < 216; i++) {
                lp->palPalEntry[i].peRed = (BYTE)(i / 36 * 51);
                lp->palPalEntry[i].peGreen = (BYTE)(i / 6 % 6 * 51);
                lp->palPalEntry[i].peBlue = (BYTE)(i % 6 * 51);
                lp->palPalEntry[i].peFlags = 0;
                bmi.c[i].rgbRed = lp->palPalEntry[i].peRed;
                bmi.c[i].rgbGreen = lp->palPalEntry[i].peGreen;
                bmi.c[i].rgbBlue = lp->palPalEntry[i].peBlue;
                bmi.c[i].rgbReserved = 0;
            }
            hPal = CreatePalette(lp);
            free(lp);
            if (hPal) bpp = 8;
        }
    } else if (GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES) <= 4 && vga_lut) {
        bpp = 4;
        for (i = 0; i < 16; i++) {
            bmi.c[i].rgbRed = vga[i][0];
            bmi.c[i].rgbGreen = vga[i][1];
            bmi.c[i].rgbBlue = vga[i][2];
            bmi.c[i].rgbReserved = 0;
        }
    }
    memset(&bmi.h, 0, sizeof(bmi.h));
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = (WORD)bpp;
    bmi.h.biClrUsed = bpp == 8 ? 216 : bpp == 4 ? 16 : 0;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (nstrip && (have_frame || stream_raw)) {
        paint_picture(dc);
    } else {
        const char *t = !have_usb ? "WINUSB.SYS is not loaded. Load it from CONFIG.SYS." :
                        !ncams ? "No camera connected. Plug in a USB camera (UVC)." :
                        streaming ? "Waiting for the first picture..." : "Choose a picture size and press Start.";
        SetBkMode(dc, TRANSPARENT);
        DrawText(dc, t, -1, &prect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK _export MainProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        static const char *cls[NCTL] = { "COMBOBOX", "COMBOBOX", "BUTTON", "BUTTON", "BUTTON", "BUTTON", "STATIC" };
        static const char *txt[NCTL] = { "", "", "Start", "Photo", "Record", "Folder...", "" };
        static const DWORD sty[NCTL] = { CBS_DROPDOWNLIST | WS_VSCROLL, CBS_DROPDOWNLIST | WS_VSCROLL, BS_PUSHBUTTON,
                                         BS_PUSHBUTTON, BS_PUSHBUTTON, BS_PUSHBUTTON, SS_LEFT };
        TEXTMETRIC tm;
        HDC dc = GetDC(hwnd);
        int i;
        GetTextMetrics(dc, &tm);
        make_palette(dc);
        ReleaseDC(hwnd, dc);
        ctlh = tm.tmHeight + 10;
        barh = tm.tmHeight + 8;
        for (i = 0; i < NCTL; i++)
            hCtl[i] = CreateWindow(cls[i], txt[i], WS_CHILD | WS_VISIBLE | sty[i] | (i < 6 ? WS_TABSTOP : 0), 0, 0, 10,
                                   10, hwnd, (HMENU)(ID_CAM + i), hInst, NULL);
        return 0;
    }
    case WM_SIZE:
        place_ctls(LOWORD(lp), HIWORD(lp));
        if (have_frame) layout(fhdr.w, fhdr.h);
        if (have_frame && nstrip) show_frame();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.x = 440;
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.y = 240;
        return 0;
    case WM_COMMAND:
        switch (wp) {
        case ID_CAM:
            if (HIWORD(lp) == CBN_SELCHANGE) {
                int c = (int)SendMessage(hCtl[0], CB_GETCURSEL, 0, 0);
                if (c != cam && c >= 0) {
                    cam_stop();
                    cam = c;
                    fill_modes();
                }
            }
            break;
        case ID_MODE:
            if (HIWORD(lp) == CBN_SELCHANGE) {
                int k = (int)SendMessage(hCtl[1], CB_GETCURSEL, 0, 0);
                if (k >= 0 && k != mode) {
                    int was = streaming;
                    mode = k;
                    if (was) cam_start();
                }
            }
            break;
        case ID_START:
            if (streaming) cam_stop();
            else cam_start();
            break;
        case ID_PHOTO:
            photo();
            break;
        case ID_REC:
            if (avi) rec_stop();
            else rec_start();
            break;
        case ID_FOLDER:
            ask_folder();
            break;
        }
        enable_ctls();
        status();
        InvalidateRect(hwnd, &prect, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == 2) {
            if (list_cams()) {
                enable_ctls();
                status();
                InvalidateRect(hwnd, &prect, TRUE);
            }
        } else {
            poll();
        }
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_QUERYNEWPALETTE:
        if (hPal) {
            HDC dc = GetDC(hwnd);
            HPALETTE op = SelectPalette(dc, hPal, FALSE);
            int n = RealizePalette(dc);
            SelectPalette(dc, op, FALSE);
            ReleaseDC(hwnd, dc);
            if (n) InvalidateRect(hwnd, NULL, FALSE);
            return n;
        }
        break;
    case WM_PALETTECHANGED:
        if (hPal && (HWND)wp != hwnd) InvalidateRect(hwnd, &prect, FALSE);
        return 0;
    case WM_DESTROY:
        cam_stop();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, m, wp, lp);
}

static void parse_cmd(LPSTR p)
{
    char w[16];
    int n;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        for (n = 0; *p && *p != ' ' && *p != '\t'; p++)
            if (n < (int)sizeof(w) - 1) w[n++] = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        w[n] = 0;
        if (!strcmp(w, "/SNAP")) opt_snap = 1;
        else if (!strcmp(w, "/DUMP")) opt_dump = 1;
        else if (!strcmp(w, "/TEST")) {
            opt_test = opt_snap = 1;
            opt_rec = 2;
            opt_mode = 0;
        }
        else if (!strcmp(w, "/QUIT")) opt_quit = 1;
        else if (!strcmp(w, "/MODE") || !strcmp(w, "/REC")) {
            int v = 0, rec = w[1] == 'R';
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
            if (rec) opt_rec = v > 0 ? v : 10;
            else opt_mode = v;
        }
    }
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    MSG m;
    DWORD d;
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN), w, h;
    hInst = hi;
    parse_cmd(cmd);
    realmode = !(GetWinFlags() & WF_PMODE);
    ahincr = (WORD)(DWORD)GetProcAddress(GetModuleHandle("KERNEL"), "__AHINCR");
    if (!ahincr) ahincr = realmode ? 0x1000 : 8;
    make_tables();
    if (realmode) LockData(0);                          /* far pointers into DGROUP stay valid */
    jpg_alloc = galloc;
    jpg_free = gfree;
    rowbuf = (unsigned char far *)galloc(8192);
    rgbrow = (unsigned char far *)galloc(4096 * 3);
    if (!rowbuf || !rgbrow) return 0;
    GetProfileString("WINCAM", "Folder", "C:\\", folder, sizeof(folder));
    if (!hprev) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = MainProc;
        wc.hInstance = hi;
        wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "WINCam";
        if (!RegisterClass(&wc)) return 0;
    }
    d = GlobalDosAlloc(DOSBUF);
    dos_sel = LOWORD(d);
    dos_seg = HIWORD(d);
    vxd_ent[0] = vxd_find(WIN_VXD_USB);
    have_usb = dos_sel && (windetect() || vxd_ent[0]);
    w = cx - 40 > 700 ? 700 : cx - 40;
    h = cy - 40 > 560 ? 560 : cy - 40;
    hMain = CreateWindow("WINCam", "Camera - WINDRV", WS_OVERLAPPEDWINDOW, (cx - w) / 2, (cy - h) / 3, w, h, NULL,
                         NULL, hi, NULL);
    ShowWindow(hMain, show);
    UpdateWindow(hMain);
    list_cams();
    if (opt_mode >= 0 && opt_mode < nmodes) {
        mode = opt_mode;
        SendMessage(hCtl[1], CB_SETCURSEL, mode, 0);
    }
    if (cam >= 0 && mode >= 0) cam_start();
    enable_ctls();
    status();
    InvalidateRect(hMain, NULL, TRUE);
    SetTimer(hMain, 1, POLLMS, NULL);
    SetTimer(hMain, 2, LISTMS, NULL);
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    free_strips();
    if (hFrame) GlobalFree(hFrame);
    if (hPal) DeleteObject(hPal);
    if (dos_sel) GlobalDosFree(dos_sel);
    return 0;
}
