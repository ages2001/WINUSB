/* winfmt.c - text formatting of WINDISK / WINUSB API data (16-bit, shared by USBTREE.EXE and WININFO.EXE) */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "winfmt.h"

void sb_init(struct sbuf *s, char *buf, unsigned size)
{
    s->p = buf;
    s->left = size;
    s->len = 0;
    if (size) buf[0] = 0;
}

void sb_printf(struct sbuf *s, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    unsigned n;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    n = (unsigned)strlen(tmp);
    if (s->left <= 1) return;
    if (n >= s->left) n = s->left - 1;
    memcpy(s->p, tmp, n);
    s->p += n;
    *s->p = 0;
    s->left -= n;
    s->len += n;
}

const char *win_speed(unsigned char s)
{
    static const char *n[] = { "Low (1.5 Mbit/s)", "Full (12 Mbit/s)", "High (480 Mbit/s)",
                               "Super (5 Gbit/s)", "Super+ (10 Gbit/s)" };
    return s <= 4 ? n[s] : "unknown";
}

const char *win_hctype(unsigned char t)
{
    switch (t) {
    case 1: return "UHCI";
    case 2: return "OHCI";
    case 3: return "EHCI";
    case 4: return "xHCI";
    }
    return "?";
}

const char *win_driver(unsigned char d)
{
    switch (d) {
    case UD_HUB: return "Hub";
    case UD_KBD: return "Keyboard";
    case UD_MOUSE: return "Mouse";
    case UD_BOT: return "Mass storage (BOT)";
    case UD_UAS: return "Mass storage (UAS)";
    case UD_HIDOTHER: return "HID";
    case UD_AUDIO: return "Audio";
    case UD_MIDI: return "MIDI";
    case UD_TABLET: return "Tablet / touch";
    case UD_VIDEO: return "Camera";
    case UD_WDM: return "Windows driver";
    }
    return "no driver";
}

const char *win_class(unsigned char cls, unsigned char sub, unsigned char proto)
{
    switch (cls) {
    case 0x00: return "(per interface)";
    case 0x01: return "Audio";
    case 0x02: return "Communications";
    case 0x03:
        if (sub == 1 && proto == 1) return "HID boot keyboard";
        if (sub == 1 && proto == 2) return "HID boot mouse";
        return "HID";
    case 0x05: return "Physical";
    case 0x06: return "Image";
    case 0x07: return "Printer";
    case 0x08:
        if (proto == 0x62) return "Mass storage, UAS";
        if (proto == 0x50) return "Mass storage, Bulk-Only";
        if (proto == 0x00 || proto == 0x01) return "Mass storage, CBI";
        return "Mass storage";
    case 0x09: return proto == 3 ? "Hub (SuperSpeed)" : proto == 2 ? "Hub (multi TT)" : proto == 1 ? "Hub (single TT)" : "Hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "Smart card";
    case 0x0D: return "Content security";
    case 0x0E: return "Video";
    case 0x0F: return "Personal healthcare";
    case 0x10: return "Audio/Video";
    case 0x11: return "Billboard";
    case 0xDC: return "Diagnostic";
    case 0xE0: return "Wireless controller";
    case 0xEF: return "Miscellaneous";
    case 0xFE: return "Application specific";
    case 0xFF: return "Vendor specific";
    }
    return "unknown class";
}

const char *win_portstate(unsigned long st)
{
    switch ((unsigned)((st >> 24) & 0x7F)) {
    case 0: return (st & 1) ? "connected" : "empty";
    case 1: return "connecting";
    case 2: return "active";
    case 3: return "enumeration failed";
    case 4: return "routed to companion controller";
    case 5: return "safe to remove";
    }
    return "?";
}

static const char *short_speed(unsigned char s)
{
    static const char *n[] = { "LS", "FS", "HS", "SS", "SS+" };
    return s <= 4 ? n[s] : "?";
}

void win_node_line(struct sbuf *s, const struct winusbnode *n)
{
    int i;
    for (i = 0; i < n->depth; i++) sb_printf(s, "   ");
    if (n->type == UN_HC) {
        sb_printf(s, "%s controller %04X:%04X at %02X:%02X.%u, IRQ %u, %u ports",
                  win_hctype(n->hctype), n->vid, n->pid, (unsigned)(n->pciloc >> 8),
                  (unsigned)((n->pciloc >> 3) & 0x1F), (unsigned)(n->pciloc & 7),
                  n->irq == 0xFF ? 0 : n->irq, n->nports);
        return;
    }
    if (n->type == UN_PORT) {
        sb_printf(s, "Port %u: %s", n->port, win_portstate(n->portstatus));
        return;
    }
    sb_printf(s, "Port %u: [%s] %04X:%04X %s %s (%s)", n->port, win_driver(n->driver), n->vid, n->pid,
              n->manuf[0] ? n->manuf : "", n->product[0] ? n->product : "USB device", short_speed(n->speed));
    if (n->letters[0]) {
        sb_printf(s, " ->");
        for (i = 0; i < 8 && n->letters[i]; i++) sb_printf(s, " %c:", n->letters[i]);
    }
    if (n->portstatus & 0x00800000UL) sb_printf(s, " [safe to remove]");
}

void win_node_detail(struct sbuf *s, const struct winusbnode *n)
{
    if (n->type == UN_HC) {
        sb_printf(s, "Host controller: %s\r\n", n->product);
        sb_printf(s, "  Type:           %s\r\n", win_hctype(n->hctype));
        sb_printf(s, "  PCI ID:         %04X:%04X\r\n", n->vid, n->pid);
        sb_printf(s, "  PCI location:   bus %u, device %u, function %u\r\n", (unsigned)(n->pciloc >> 8),
                  (unsigned)((n->pciloc >> 3) & 0x1F), (unsigned)(n->pciloc & 7));
        sb_printf(s, "  Registers:      %08lX\r\n", n->mmio & ~0xFUL);
        if (n->irq == 0xFF) sb_printf(s, "  Interrupt:      none (polled)\r\n");
        else sb_printf(s, "  Interrupt:      IRQ %u (%lu interrupts handled)\r\n", n->irq, n->xfers);
        sb_printf(s, "  Root ports:     %u\r\n", n->nports);
        sb_printf(s, "  Errors:         %lu\r\n", n->errors);
        return;
    }
    if (n->type == UN_PORT) {
        unsigned long st = n->portstatus;
        sb_printf(s, "Port %u (empty)\r\n", n->port);
        sb_printf(s, "  State:   %s\r\n", win_portstate(st));
        sb_printf(s, "  Status:  %s%s%s%s%s\r\n", (st & 1) ? "connected " : "", (st & 2) ? "enabled " : "",
                  (st & 8) ? "powered " : "", (st & 0x10) ? "OVERCURRENT " : "", (st & 0x80) ? "companion-owned " : "");
        return;
    }
    sb_printf(s, "%s %s\r\n", n->manuf, n->product);
    sb_printf(s, "  Device id (WIN):  %u\r\n", n->id);
    sb_printf(s, "  Vendor/Product:  %04X:%04X, revision %X.%02X\r\n", n->vid, n->pid, n->bcd >> 8, n->bcd & 0xFF);
    sb_printf(s, "  USB version:     %X.%02X\r\n", n->bcdusb >> 8, n->bcdusb & 0xFF);
    sb_printf(s, "  Speed:           %s\r\n", win_speed(n->speed));
    sb_printf(s, "  Device class:    %02X/%02X/%02X %s\r\n", n->cls, n->sub, n->proto, win_class(n->cls, n->sub, n->proto));
    sb_printf(s, "  Driver:          %s\r\n", win_driver(n->driver));
    sb_printf(s, "  Address:         %u", n->addr);
    if (n->slot) sb_printf(s, " (xHCI slot %u)", n->slot);
    sb_printf(s, "\r\n  Port:            %u, tier %u\r\n", n->port, n->depth);
    sb_printf(s, "  Configuration:   %u, %u interface(s), max power %u mA\r\n", n->config, n->iface_count, n->maxpower);
    if (n->serial[0]) sb_printf(s, "  Serial number:   %s\r\n", n->serial);
    if (n->nports) sb_printf(s, "  Hub ports:       %u\r\n", n->nports);
    if (n->letters[0]) {
        int i;
        sb_printf(s, "  Drive letters:  ");
        for (i = 0; i < 8 && n->letters[i]; i++) sb_printf(s, " %c:", n->letters[i]);
        sb_printf(s, "\r\n");
    }
    sb_printf(s, "  Transfers:       %lu, errors %lu\r\n", n->xfers, n->errors);
    if (n->portstatus & 0x00800000UL) sb_printf(s, "  State:           SAFE TO REMOVE\r\n");
}

static unsigned w16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static const char *ep_type(unsigned char a)
{
    static const char *t[] = { "Control", "Isochronous", "Bulk", "Interrupt" };
    return t[a & 3];
}

static void bos_caps(struct sbuf *s, const unsigned char *p, unsigned len)
{
    unsigned off = p[0];
    sb_printf(s, "BOS descriptor: %u capabilities\r\n", p[4]);
    while (off + 3 <= len && p[off]) {
        const unsigned char *c = p + off;
        if (c[1] == 0x10) {
            switch (c[2]) {
            case 0x02: sb_printf(s, "  USB 2.0 extension: LPM %s\r\n", (c[3] & 2) ? "supported" : "not supported"); break;
            case 0x03:
                sb_printf(s, "  SuperSpeed: speeds %s%s%s%s, U1 exit %u us, U2 exit %u us\r\n",
                          (c[4] & 1) ? "LS " : "", (c[4] & 2) ? "FS " : "", (c[4] & 4) ? "HS " : "", (c[4] & 8) ? "SS" : "",
                          c[7], w16(c + 8));
                break;
            case 0x04: sb_printf(s, "  Container ID\r\n"); break;
            case 0x0A: sb_printf(s, "  SuperSpeedPlus capability\r\n"); break;
            default: sb_printf(s, "  Capability type %02X\r\n", c[2]);
            }
        }
        off += c[0];
    }
}

void win_descriptors(struct sbuf *s, const unsigned char *blob, unsigned len)
{
    unsigned dl, cl, bl, off;
    const unsigned char *d, *c, *b;
    if (len < 4) return;
    dl = w16(blob);
    d = blob + 2;
    if (dl >= 18) {
        sb_printf(s, "\r\nDevice descriptor\r\n");
        sb_printf(s, "  bcdUSB %X.%02X  class %02X/%02X/%02X  bMaxPacketSize0 %u\r\n", d[3], d[2], d[4], d[5], d[6], d[7]);
        sb_printf(s, "  idVendor %04X  idProduct %04X  bcdDevice %X.%02X  configurations %u\r\n",
                  w16(d + 8), w16(d + 10), d[13], d[12], d[17]);
    }
    if (2 + dl + 2 > len) return;
    cl = w16(blob + 2 + dl);
    c = blob + 4 + dl;
    if (cl >= 9) {
        sb_printf(s, "\r\nConfiguration %u: %u interface(s), %s%s, %u mA\r\n", c[5], c[4],
                  (c[7] & 0x40) ? "self-powered" : "bus-powered", (c[7] & 0x20) ? ", remote wakeup" : "", c[8] * 2);
        off = c[0];
        while (off + 2 <= cl && c[off]) {
            const unsigned char *x = c + off;
            switch (x[1]) {
            case 0x04:
                sb_printf(s, "  Interface %u alt %u: class %02X/%02X/%02X %s, %u endpoint(s)\r\n", x[2], x[3], x[5], x[6],
                          x[7], win_class(x[5], x[6], x[7]), x[4]);
                break;
            case 0x05:
            {
                unsigned mps = w16(x + 4);
                sb_printf(s, "    Endpoint %02X %s %s, max packet %u", x[2], (x[2] & 0x80) ? "IN" : "OUT", ep_type(x[3]),
                          mps & 0x7FF);
                if (mps >> 11) sb_printf(s, " x%u", (mps >> 11) + 1);
                if ((x[3] & 3) == 1 || (x[3] & 3) == 3) sb_printf(s, ", interval %u", x[6]);
                sb_printf(s, "\r\n");
                break;
            }
            case 0x21:
                sb_printf(s, "    HID %X.%02X, country %u, report descriptor %u bytes\r\n", x[3], x[2], x[4], w16(x + 7));
                break;
            case 0x30:
                sb_printf(s, "      SuperSpeed companion: burst %u", x[2] + 1);
                if (x[3] & 0x1F) sb_printf(s, ", %u streams", 1 << (x[3] & 0x1F));
                sb_printf(s, "\r\n");
                break;
            case 0x24:
                if (x[0] == 4) {
                    static const char *pn[] = { "?", "command", "status", "data-in", "data-out" };
                    sb_printf(s, "      UAS pipe: %s\r\n", x[2] <= 4 ? pn[x[2]] : "?");
                }
                break;
            case 0x0B:
                sb_printf(s, "  Interface association: interfaces %u-%u, class %02X\r\n", x[2], x[2] + x[3] - 1, x[4]);
                break;
            }
            off += x[0];
        }
    }
    if (4 + dl + cl + 2 > len) return;
    bl = w16(blob + 4 + dl + cl);
    b = blob + 6 + dl + cl;
    if (bl >= 5) {
        sb_printf(s, "\r\n");
        bos_caps(s, b, bl);
    }
}

void win_disks(struct sbuf *s, const struct windiskinfo *d, int n, int firstdrive)
{
    int i;
    static const char *kind[] = { "?", "AHCI", "NVMe", "USB", "IDE", "SD", "MMC" };
    (void)firstdrive;
    sb_printf(s, "Disks managed by the driver\r\n");
    for (i = 0; i < n; i++, d++) {
        unsigned long mb;
        if (d->ssize >= 512) mb = (d->sectors_lo >> 11) * (d->ssize / 512) + (d->sectors_hi << 21) * (d->ssize / 512);
        else mb = 0;
        sb_printf(s, "  disk%u  %-4s  %-28s %7lu MB  %4lu-byte sectors%s%s\r\n", d->index,
                  kind[d->kind <= 6 ? d->kind : 0], d->model, mb, d->ssize,
                  d->shared ? "  (also BIOS disk)" : "", d->present ? "" : "  (no medium)");
        if (d->serial[0]) sb_printf(s, "         serial %s\r\n", d->serial);
        sb_printf(s, "         read %lu / written %lu sectors, %lu errors", d->reads, d->writes, d->errors);
        if (d->letters[0]) {
            int k;
            sb_printf(s, ", drives");
            for (k = 0; k < 10 && d->letters[k]; k++) sb_printf(s, " %c:", d->letters[k]);
        }
        sb_printf(s, "\r\n");
    }
}

void win_info(struct sbuf *s, const struct wininfo *in)
{
    sb_printf(s, "%s, running in %s\r\n", in->build, in->mode == 2 ? "Windows 386 enhanced mode (VxD)" :
                                                    "DOS / Windows standard mode (DOS core)");
    sb_printf(s, "Disks %u, drive letters %u from %c:, free DMA pool %lu KB, up %lu s\r\n", in->ndisks, in->nunits,
              'A' + in->firstdrive, in->poolfree >> 10, in->uptime_ms / 1000);
}
