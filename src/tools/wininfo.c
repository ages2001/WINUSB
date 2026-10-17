/* wininfo.c - WININFO.EXE: DOS console tool for WINDISK.SYS / WINUSB.SYS (status, disks, drive letters,
   USB tree, descriptors, safe removal, log) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "winfmt.h"
#include "winfs.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static struct winapireq req;
static unsigned char __far *big;
#define BIGSZ 32768U
static char text[16384];
static struct wincb __far *cbp;
static unsigned char mplex;
static struct wincb __far *cbs[2];
static const unsigned char mplexes[2] = { WIN_MPLEX_DISK, WIN_MPLEX_USB };
static const char *names[2] = { "WINDISK.SYS", "WINUSB.SYS" };

static int call_api(unsigned fn, unsigned long a0, unsigned long a1, unsigned long a2, void __far *buf, unsigned len)
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

static struct wincb __far *detect(unsigned char id)
{
    union REGS r;
    struct SREGS sr;
    segread(&sr);
    r.h.ah = id;
    r.h.al = 0;
    r.x.bx = 0;
    int86x(0x2F, &r, &r, &sr);
    if (r.h.al != 0xFF || r.x.bx != 0x3357) return NULL;
    return MK_FP(sr.es, r.x.di);
}

/* make driver k (0 disk, 1 USB) the target of call_api */
static int use(int k)
{
    if (!cbs[k]) return 0;
    cbp = cbs[k];
    mplex = mplexes[k];
    return 1;
}

static void out(const char *s)
{
    const char *p;
    for (p = s; *p; p++) if (*p != '\r') putchar(*p);
}

static void show_units(void)
{
    int i;
    printf("Drive letters (first %c:)\n", 'A' + cbp->firstdrive);
    for (i = 0; i < cbp->nunits; i++) {
        struct winunit __far *u = &cbp->units[i];
        char lbl[12];
        _fmemcpy(lbl, u->label, 11);
        lbl[11] = 0;
        printf("  %c:  %-6s ", 'A' + cbp->firstdrive + i, u->type == UT_FIXED ? "fixed" : "slot");
        if (u->state & US_PRESENT)
            printf("disk%u part %u  start %lu  %lu MB  type %02X  \"%s\"%s\n", u->disk, u->part, u->lba,
                   u->sectors >> 11, u->ptype, lbl, (u->state & US_WPROT) ? " read-only" : "");
        else
            printf("empty\n");
    }
}

static unsigned long rd_ssize;

static int rd_sect(void *ctx, unsigned long lba, unsigned cnt, unsigned char *buf, unsigned size)
{
    unsigned long n = (unsigned long)cnt * rd_ssize;
    if (n > size || n > BIGSZ) return -1;
    if (call_api(WINF_RAWREAD, (unsigned long)(int)ctx, lba, cnt, big, BIGSZ) != WINE_OK || req.retlen < n) return -1;
    _fmemcpy(buf, big, (unsigned)n);
    return 0;
}

/* partitions and file systems of every disk of the current driver */
static void show_partitions(void)
{
    static struct windiskinfo dk[24];
    struct sbuf s;
    struct fsdisk fd;
    int n, i;
    if (call_api(WINF_DISKS, 0, 0, 0, big, BIGSZ) != WINE_OK) return;
    n = (int)(req.retlen / sizeof(struct windiskinfo));
    if (n > 24) n = 24;
    _fmemcpy(dk, big, n * sizeof(struct windiskinfo));
    for (i = 0; i < n; i++) {
        char sz[24];
        win_size(sz, dk[i].sectors_lo, dk[i].sectors_hi, dk[i].ssize);
        printf("\ndisk%u: %s, %s\n", dk[i].index, dk[i].model, sz);
        if (!dk[i].present) {
            printf("  no medium\n");
            continue;
        }
        rd_ssize = dk[i].ssize;
        fd.read = rd_sect;
        fd.ctx = (void *)(int)dk[i].index;
        fd.ssize = dk[i].ssize;
        fd.sectors = dk[i].sectors_hi ? 0xFFFFFFFFUL : dk[i].sectors_lo;
        fd.index = dk[i].index;
        fd.units = (const struct winunit *)0;
        fd.nunits = 0;
        fd.firstdrive = cbp->firstdrive;
        {
            static struct winunit u[WIN_MAXUNITS];
            int k;
            for (k = 0; k < cbp->nunits && k < WIN_MAXUNITS; k++) _fmemcpy(&u[k], &cbp->units[k], sizeof(u[k]));
            fd.units = u;
            fd.nunits = k;
        }
        sb_init(&s, text, sizeof(text));
        win_partitions(&s, &fd);
        out(text);
    }
}

static void flush_caches(void)
{
    union REGS r;
    r.x.ax = 0x4A10;            /* SMARTDRV: commit cache */
    r.x.bx = 0x0001;
    int86(0x2F, &r, &r);
    r.h.ah = 0x0D;              /* DOS: disk reset, writes dirty buffers */
    int86(0x21, &r, &r);
}

/* cameras: list; with a mode number: capture one frame into a file */
static int cameras(int mode, const char *file)
{
    static struct wincamdev cd[4];
    static struct wincammode cm[32];
    int n, i, k, nm;
    if (call_api(WINF_CAM_DEVS, 0, 0, 0, big, BIGSZ) != WINE_OK) {
        printf("No camera support (WINUSB.SYS not loaded or USB not active in DOS).\n");
        return 1;
    }
    n = (int)(req.retlen / sizeof(struct wincamdev));
    if (n > 4) n = 4;
    _fmemcpy(cd, big, n * sizeof(struct wincamdev));
    if (!n) printf("No USB camera.\n");
    for (i = 0; i < n; i++) {
        printf("camera %u: %s%s\n", cd[i].id, cd[i].name, cd[i].gone ? " (unplugged)" : "");
        if (call_api(WINF_CAM_MODES, cd[i].id, 0, 0, big, BIGSZ) != WINE_OK) continue;
        nm = (int)(req.retlen / sizeof(struct wincammode));
        if (nm > 32) nm = 32;
        _fmemcpy(cm, big, nm * sizeof(struct wincammode));
        for (k = 0; k < nm; k++)
            printf("  mode %2d: %-5s %4ux%-4u %3lu fps%s\n", k, cm[k].type == CAM_MJPEG ? "MJPEG" :
                   cm[k].type == CAM_NV12 ? "NV12" : "YUY2", cm[k].w, cm[k].h,
                   cm[k].interval ? 10000000UL / cm[k].interval : 0UL, cm[k].maxsize ? "" : "  (too large)");
    }
    if (mode >= 0 && n) {
        FILE *f;
        unsigned long off = 0, size = 0, t;
        int r, tries = 0;
        struct winframe fr;
        r = call_api(WINF_CAM_START, cd[0].id, mode, 0, NULL, 0);
        if (r != WINE_OK) {
            printf("start failed (%d)\n", r);
            return 1;
        }
        f = fopen(file, "wb");
        if (!f) return 1;
        for (;;) {
            r = call_api(WINF_CAM_FRAME, cd[0].id, off, 0, big, BIGSZ);
            if (r == WINE_BUSY && off == 0) {
                /* about 55 ms */
                t = *(unsigned long __far *)MK_FP(0x40, 0x6C);
                while (*(volatile unsigned long __far *)MK_FP(0x40, 0x6C) == t) ;
                if (++tries > 200) break;
                continue;
            }
            if (r != WINE_OK) break;
            if (off == 0) {
                _fmemcpy(&fr, big, sizeof(fr));
                size = fr.size;
                fwrite(big + sizeof(fr), 1, (unsigned)(req.retlen - sizeof(fr)), f);
                off = req.retlen - sizeof(fr);
                printf("frame %lu: %ux%u, %lu bytes\n", fr.seq, fr.w, fr.h, fr.size);
            } else {
                unsigned long k2;
                for (k2 = 0; k2 < req.retlen; k2 += 16384)
                    fwrite(big + (unsigned)k2, 1, (unsigned)MIN(16384UL, req.retlen - k2), f);
                off += req.retlen;
            }
            if (off >= size) break;
        }
        fclose(f);
        call_api(WINF_CAM_STOP, cd[0].id, 0, 0, NULL, 0);
        if (!size) printf("no frame arrived\n");
    }
    return 0;
}

static void usage(void)
{
    printf("WININFO [/B] [/C [mode file]] [/D id] [/E id] [/L] [/LU] [/P] [/T] [/U]\n"
           "  (none)  status, disks, drive letters, USB tree, Thunderbolt / USB4\n"
           "  /B      Thunderbolt / USB4 host, routers, every adapter and tunnel\n"
           "  /T      USB tree with all ports\n"
           "  /D id   descriptors of USB device <id>\n"
           "  /E id   safe removal of USB storage device <id>\n"
           "  /L      WINDISK event log    /LU  WINUSB event log\n"
           "  /P      partitions and file systems of all disks\n"
           "  /C [mode file]  USB cameras; with a mode: one frame into the file\n"
           "  /U      drive letters only\n");
}

static void show_tree(int allports)
{
    int n, i;
    struct sbuf s;
    struct winusbnode __far *nd = (struct winusbnode __far *)big;
    if (call_api(WINF_USBTREE, 0, 0, 0, big, BIGSZ) != WINE_OK) {
        printf("USB stack not active in DOS (WINUSB.SYS loaded with /WIN)\n");
        return;
    }
    n = (int)(req.retlen / sizeof(struct winusbnode));
    printf("USB tree (device id in brackets)\n");
    for (i = 0; i < n; i++) {
        struct winusbnode node;
        _fmemcpy(&node, &nd[i], sizeof(node));
        if (node.type == UN_PORT && !allports && !(node.portstatus & 1)) continue;
        sb_init(&s, text, sizeof(text));
        win_node_line(&s, &node);
        if (node.type == UN_DEV) printf("[%3u] ", node.id); else printf("      ");
        out(text);
        printf("\n");
    }
}

static const char *lane_name(unsigned st)
{
    switch (st) {
    case 0: return "disabled";
    case 1: return "training";
    case 2: return "up";
    case 7: return "unplugged";
    default: return "?";
    }
}

static const char *slevel_name(unsigned long sl)
{
    switch (sl) {
    case 0: return "none";
    case 1: return "user authorization";
    case 2: return "secure connect";
    case 3: return "DisplayPort only";
    case 4: return "USB only";
    case 5: return "no PCIe";
    default: return "?";
    }
}

/* Thunderbolt / USB4 host, routers, adapters and tunnels (WINUSB) */
static void show_tbt(int verbose)
{
    struct wintbnode __far *nd = (struct wintbnode __far *)big;
    int n, i, r;
    r = call_api(WINF_TBTREE, 0, 0, 0, big, BIGSZ);
    if (r == WINE_NODEV) {
        if (verbose) printf("No Thunderbolt / USB4 host interface found (or /NOTB).\n");
        return;
    }
    if (r != WINE_OK) {
        if (verbose) printf("USB stack not active in DOS (WINUSB.SYS loaded with /WIN)\n");
        return;
    }
    n = (int)(req.retlen / sizeof(struct wintbnode));
    printf("Thunderbolt / USB4\n");
    for (i = 0; i < n; i++) {
        struct wintbnode d;
        _fmemcpy(&d, &nd[i], sizeof(d));
        switch (d.type) {
        case TN_HOST:
            printf("  %s, %04x:%04x, host interface version %lx%s\n", d.name, d.vendor, d.device, d.bw_up,
                   (d.flags & TNF_ACTIVE) ? "" : " (not active)");
            if (TNF_MODE(d.flags) == 2) printf("  firmware security level %lu (%s)\n", d.bw_down, slevel_name(d.bw_down));
            break;
        case TN_ROUTER:
            printf("  %*s%s route %08lx%08lx: %s, %u adapters%s\n", d.depth * 2, "", d.name, d.route_hi, d.route_lo,
                   (d.flags & TNF_USB4) ? "USB4" : "Thunderbolt", d.nhops, (d.flags & TNF_ACTIVE) ? "" : ", not configured");
            break;
        case TN_ADAPTER:
            if (!verbose && d.tunnel == 0 && d.src != 2) break;       /* idle lanes only with /B */
            printf("  %*s  adapter %2u: %-9s", d.depth * 2, "", d.port, d.name);
            if (d.tunnel == 0) {
                printf(" %s", lane_name(d.src));
                if (d.dst) printf(", router #%u", d.dst);
            } else if (d.flags & TNF_ACTIVE) {
                printf(" in use");
            }
            if (d.flags & TNF_USB4) printf(" (USB4)");
            printf("\n");
            break;
        case TN_TUNNEL:
            printf("  %s to route %08lx%08lx: adapter %u of the %s -> adapter %u%s", d.name, d.route_hi, d.route_lo,
                   d.src, d.port ? "parent router" : "host", d.dst, (d.flags & TNF_FIRMWARE) ? " (set up by the firmware)" : "");
            if (d.tunnel == 2 && d.bw_down) printf(", %lu/%lu Mb/s", d.bw_up, d.bw_down);
            printf("\n");
            break;
        case TN_ICMDEV:
            printf("  device %s: link %u depth %u route %08lx%08lx%s%s\n", d.name, d.port, d.depth, d.route_hi, d.route_lo,
                   (d.flags & TNF_APPROVED) ? ", approved" : ", NOT approved", (d.flags & TNF_FIRMWARE) ? ", boot" : "");
            break;
        }
    }
}

static void show_driver(int k)
{
    struct sbuf s;
    if (!use(k)) return;
    printf("\n== %s ==\n", names[k]);
    if (call_api(WINF_INFO, 0, 0, 0, big, BIGSZ) == WINE_OK) {
        struct wininfo in;
        _fmemcpy(&in, big, sizeof(in));
        sb_init(&s, text, sizeof(text));
        win_info(&s, &in);
        out(text);
    }
    if (call_api(WINF_DISKS, 0, 0, 0, big, BIGSZ) == WINE_OK && req.retlen) {
        static struct windiskinfo dk[24];
        int n = (int)(req.retlen / sizeof(struct windiskinfo));
        if (n > 24) n = 24;
        _fmemcpy(dk, big, n * sizeof(struct windiskinfo));
        sb_init(&s, text, sizeof(text));
        win_disks(&s, dk, n, cbp->firstdrive);
        out(text);
    }
    if (cbp->nunits) show_units();
}

int main(int argc, char **argv)
{
    struct sbuf s;
    int i;
    unsigned seg;
    printf("WININFO 1.0 - WINDRV status\n");
    if (_dos_allocmem(BIGSZ / 16, &seg)) {
        printf("not enough memory\n");
        return 1;
    }
    big = MK_FP(seg, 0);
    cbs[0] = detect(WIN_MPLEX_DISK);
    cbs[1] = detect(WIN_MPLEX_USB);
    if (!cbs[0] && !cbs[1]) {
        printf("Neither WINDISK.SYS nor WINUSB.SYS is loaded.\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        switch (a[1] | 0x20) {
        case 'd':
            if (i + 1 >= argc) { usage(); return 1; }
            if (!use(1)) { printf("WINUSB.SYS is not loaded.\n"); return 1; }
            if (call_api(WINF_USBDESC, atol(argv[i + 1]), 0, 0, big, BIGSZ) != WINE_OK) {
                printf("No such device.\n");
                return 1;
            }
            {
                static unsigned char blob[4096];
                unsigned n = req.retlen > sizeof(blob) ? sizeof(blob) : (unsigned)req.retlen;
                _fmemcpy(blob, big, n);
                sb_init(&s, text, sizeof(text));
                win_descriptors(&s, blob, n);
                out(text);
            }
            return 0;
        case 'e':
            if (i + 1 >= argc) { usage(); return 1; }
            if (!use(1)) { printf("WINUSB.SYS is not loaded.\n"); return 1; }
            flush_caches();
            switch (call_api(WINF_USBEJECT, atol(argv[i + 1]), 0, 0, NULL, 0)) {
            case WINE_OK: printf("Device %s can be removed safely.\n", argv[i + 1]); return 0;
            case WINE_NODEV: printf("No such device.\n"); return 1;
            case WINE_BADFN: printf("Device %s is not a storage device.\n", argv[i + 1]); return 1;
            default: printf("Flush failed; the device may still have unwritten data.\n"); return 1;
            }
        case 'l':
            if (!use((a[2] | 0x20) == 'u' ? 1 : 0) && !use(1) && !use(0)) return 1;
            if (call_api(WINF_LOG, 0, 0, 0, big, BIGSZ) == WINE_OK) {
                unsigned long k;
                for (k = 0; k < req.retlen; k++) if (big[k] != '\r') putchar(big[k]);
            }
            return 0;
        case 'u':
            if (use(0) && cbp->nunits) { printf("WINDISK.SYS: "); show_units(); }
            if (use(1)) { printf("WINUSB.SYS: "); show_units(); }
            return 0;
        case 'c':
            if (!use(1)) { printf("WINUSB.SYS is not loaded.\n"); return 1; }
            return cameras(i + 2 < argc ? atoi(argv[i + 1]) : -1, i + 2 < argc ? argv[i + 2] : "");
        case 'p':
            if (use(0)) { printf("WINDISK.SYS:"); show_partitions(); }
            if (use(1)) { printf("%sWINUSB.SYS:", cbs[0] ? "\n" : ""); show_partitions(); }
            return 0;
        case 't':
            if (!use(1)) { printf("WINUSB.SYS is not loaded.\n"); return 1; }
            show_tree(1);
            return 0;
        case 'b':
            if (!use(1)) { printf("WINUSB.SYS is not loaded.\n"); return 1; }
            show_tbt(1);
            return 0;
        default:
            usage();
            return 1;
        }
    }
    show_driver(0);
    show_driver(1);
    if (use(1)) {
        show_tree(0);
        show_tbt(0);
    }
    return 0;
}
