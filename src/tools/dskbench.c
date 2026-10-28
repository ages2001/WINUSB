/* dskbench.c - DSKBENCH.EXE: sequential read benchmark for DOS drives and raw WINDISK / WINUSB disks */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <i86.h>
#include "winapi.h"

static unsigned char __far *buf;
static struct winapireq req;
static struct { unsigned long sector; unsigned count; void __far *buffer; } pkt;
static unsigned char mplex = WIN_MPLEX_DISK;

static unsigned long ticks(void)
{
    return *(unsigned long __far *)MK_FP(0x40, 0x6C);
}

/* INT 25h, large partition form; returns 0 on success */
static int absread(int drive, unsigned long sector, unsigned count, void __far *dst)
{
    unsigned err = 0;
    void __far *p = &pkt;
    unsigned ps = FP_SEG(p), po = FP_OFF(p);
    pkt.sector = sector;
    pkt.count = count;
    pkt.buffer = dst;
    _asm {
        push ds
        push bp
        push si
        push di
        mov al, byte ptr drive
        mov cx, 0FFFFh
        mov bx, po
        mov ds, ps
        int 25h
        jc failed
        xor ax, ax
    failed:
        popf
        pop di
        pop si
        pop bp
        pop ds
        mov err, ax
    }
    return err;
}

static int raw_read(unsigned disk, unsigned long lba, unsigned count, int copy)
{
    union REGS r;
    struct SREGS sr;
    memset(&req, 0, sizeof(req));
    req.fn = WINF_RAWREAD;
    req.arg0 = disk;
    req.arg1 = lba;
    req.arg2 = count | (copy ? 0 : 0x80000000UL);
    req.buflen = 65535U;
    req.buf = ((unsigned long)FP_SEG(buf) << 16) | FP_OFF(buf);
    segread(&sr);
    r.h.ah = mplex;
    r.h.al = 0x01;
    sr.es = FP_SEG((void __far *)&req);
    r.x.bx = FP_OFF((void __far *)&req);
    int86x(0x2F, &r, &r, &sr);
    return req.status;
}

static void result(const char *what, unsigned long bytes, unsigned long t)
{
    unsigned long ms = t * 55;
    if (!ms) ms = 1;
    printf("%s: %lu KB in %lu.%02lu s = %lu KB/s (%lu.%02lu MB/s)\n", what, bytes >> 10, ms / 1000, (ms % 1000) / 10,
           (bytes / ms) * 1000 / 1024, (bytes / ms) / 1048, ((bytes / ms) % 1048) * 100 / 1048);
}

int main(int argc, char **argv)
{
    int drive = -1, disk = -1, i, copy = 1;
    unsigned long mb = 32, total, done = 0, t0, t1, start = 0;
    unsigned chunk = 64;
    unsigned seg;
    printf("DSKBENCH 1.1 - sequential read benchmark\n");
    if (_dos_allocmem(0x1000, &seg)) {
        printf("not enough memory\n");
        return 1;
    }
    buf = MK_FP(seg, 0);
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') {
            char c = (char)toupper(a[1]);
            if (c == 'D' && a[2] == ':') drive = toupper(a[3]) - 'A';
            else if (c == 'R' && a[2] == ':') disk = atoi(a + 3);
            else if (c == 'U' && a[2] == ':') { disk = atoi(a + 3); mplex = WIN_MPLEX_USB; }
            else if (c == 'M' && a[2] == ':') mb = atol(a + 3);
            else if (c == 'S' && a[2] == ':') start = atol(a + 3);
            else if (c == 'B' && a[2] == ':') chunk = atoi(a + 3);
            else if (c == 'N') copy = 0;
        } else if (isalpha(a[0]) && a[1] == ':') {
            drive = toupper(a[0]) - 'A';
        }
    }
    if (drive < 0 && disk < 0) {
        printf("usage: DSKBENCH d: [/M:mb] [/B:sectors] [/S:start]\n"
               "       DSKBENCH /R:disk [/M:mb] [/B:sectors] [/N]   (raw WINDISK disk, see WININFO)\n"
               "       DSKBENCH /U:disk [/M:mb] [/B:sectors] [/N]   (raw WINUSB disk)\n"
               "  /N  measure the device only, without copying into DOS memory\n");
        return 1;
    }
    if (chunk < 1) chunk = 1;
    if (chunk > 127) chunk = 127;
    total = mb * 2048UL;
    if (drive >= 0) {
        printf("Reading %lu MB from %c: in %u-sector blocks...\n", mb, 'A' + drive, chunk);
        t0 = ticks();
        while (t0 == ticks()) ;
        t0 = ticks();
        while (done < total) {
            unsigned n = (unsigned)((total - done) < chunk ? total - done : chunk);
            unsigned rc = absread(drive, start + done, n, buf);
            if (rc) {
                printf("read error %04X at sector %lu\n", rc, start + done);
                break;
            }
            done += n;
        }
        t1 = ticks();
        result("DOS drive", done * 512, t1 - t0);
    } else {
        printf("Reading %lu MB from %s disk %d in %u-sector blocks%s...\n", mb, mplex == WIN_MPLEX_USB ? "WINUSB" : "WINDISK", disk, chunk,
               copy ? "" : " (device only)");
        t0 = ticks();
        while (t0 == ticks()) ;
        t0 = ticks();
        {
            unsigned long bytes = 0, lba = start;
            while (bytes < total * 512UL) {
                int rc = raw_read((unsigned)disk, lba, chunk, copy);
                if (rc != WINE_OK || !req.retlen) {
                    printf("raw read failed (%d) at sector %lu\n", rc, lba);
                    break;
                }
                bytes += req.retlen;
                lba += chunk;
            }
            t1 = ticks();
            result("Raw disk", bytes, t1 - t0);
        }
    }
    return 0;
}
