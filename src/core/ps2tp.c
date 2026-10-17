/* ps2tp.c - touchpads on the PS/2 auxiliary port (PNP0F13 / SYN.... / ELAN....): Synaptics and
   ELAN pads are switched from their mouse emulation to their absolute protocols, and their
   fingers go to the gesture engine of ptp.c (pointer, two-finger scrolling, taps, clickpad
   buttons). The port itself belongs to the platform (the VxD's ps2.c in Windows 3.x enhanced
   mode and 95/98/ME, platdos.c in DOS and the other Windows modes): it sends the commands
   through struct ps2io while the aux interrupt is off and hands every aux byte to
   ps2tp_byte() afterwards.
     Synaptics (synaptics.c of Linux): identify (E8 00 x4, E9: 47h), queries as "sliced"
     commands (E6, E8 x4, E9), absolute mode with W (mode byte as sliced command, F3 14h),
     advanced gesture mode for the second finger, clickpads, pass-through (TrackPoint) packets.
     ELAN (elantech.c): E6 x3 E9 = 3Ch 03h C8h/00h, firmware version, hardware versions 2, 3
     and 4 (register writes through F8 custom commands), multi-finger head / motion / status
     packets of version 4, the TrackPoint packets of versions 3 and 4.
   Other pads (ALPS, older Synaptics and ELAN) stay in their mouse emulation. A pad that also
   speaks SMBus (Synaptics InterTouch, ELAN with host notify) may be taken over by smbtp.c. */
#include "usb.h"

#define T_NONE      0
#define T_SYN       1
#define T_ELAN      2

static const struct ps2io *io;
static u8 kind;
static u8 smbus_cap;                /* the pad says it has an SMBus companion */
static struct ptp *pad;
static void (*sink)(int dx, int dy, int dz, int dw, int buttons);
static int padbtn, stickbtn;
static u8 pkt[6];
static u8 pos;
static u32 last_ms;
static u32 n_pkt, n_bad;
static s32 xmin, xmax, ymin, ymax;
static char name[48];

/* Synaptics */
static u32 syn_caps, syn_ext, syn_ext0c, syn_model;
static u8 syn_agm, syn_clickpad, syn_middle;
static s32 agm_x, agm_y;
static u8 agm_valid;
static u8 syn_touch;
static int stick_scroll, stick_moved, stick_mid;

/* ELAN */
static u8 el_hw, el_crc, el_buttonpad, el_tp, el_ic, el_samples1;
static u32 el_fw;
static u8 el_reports_pressure;
static struct { s32 x, y; u8 on; } mt[5];

/* ---------------------------------------------------------------- port helpers */

static int cmd(u8 c) { return io->cmd(c); }

static int getinfo(u8 *r)
{
    int i, b;
    if (cmd(0xE9)) return -1;
    for (i = 0; i < 3; i++) {
        if ((b = io->read(30)) < 0) return -1;
        r[i] = (u8)b;
    }
    return 0;
}

/* E6, then the byte two bits at a time as resolutions (E8 n) */
static int sliced(u8 c)
{
    int i;
    if (cmd(0xE6)) return -1;
    for (i = 6; i >= 0; i -= 2)
        if (cmd(0xE8) || cmd((u8)((c >> i) & 3))) return -1;
    return 0;
}

static int syn_query(u8 q, u8 *r)
{
    return sliced(q) || getinfo(r) ? -1 : 0;
}

/* ELAN version 3 and later: F8 c E9 */
static int el_query(u8 q, u8 *r)
{
    if (el_hw >= 3) return cmd(0xF8) || cmd(q) || getinfo(r) ? -1 : 0;
    return syn_query(q, r);
}

static int el_write(u8 reg, u8 val)
{
    switch (el_hw) {
    case 2:
        return cmd(0xF8) || cmd(0x11) || cmd(0xF8) || cmd(reg) || cmd(0xF8) || cmd(val) || cmd(0xE6) ? -1 : 0;
    case 3:
        return cmd(0xF8) || cmd(0x00) || cmd(0xF8) || cmd(reg) || cmd(0xF8) || cmd(val) || cmd(0xE6) ? -1 : 0;
    case 4:
        return cmd(0xF8) || cmd(0x00) || cmd(0xF8) || cmd(reg) || cmd(0xF8) || cmd(0x00) || cmd(0xF8) || cmd(val) ||
               cmd(0xE6) ? -1 : 0;
    }
    return -1;
}

static int el_read(u8 reg, u8 *val)
{
    u8 r[3];
    if (el_hw == 2) {
        if (cmd(0xF8) || cmd(0x10) || cmd(0xF8) || cmd(reg) || getinfo(r)) return -1;
    } else {
        if (cmd(0xF8) || cmd(0x00) || cmd(0xF8) || cmd(reg) || getinfo(r)) return -1;
    }
    *val = el_hw == 4 ? r[1] : r[0];
    return 0;
}

/* ---------------------------------------------------------------- output */

/* pad and stick buttons are kept apart: a report of one does not release the other's */
static void pad_out(int dx, int dy, int dz, int dw, int b)
{
    padbtn = b;
    sink(dx, dy, dz, dw, padbtn | stickbtn);
}

static void stick_out(int dx, int dy, int b)
{
    int dz = 0;
    if (b & 4) {                    /* middle button held: the stick scrolls */
        if (!stick_mid) {
            stick_mid = 1;
            stick_moved = 0;
            stick_scroll = 0;
        }
        if (dx || dy) stick_moved = 1;
        stick_scroll += dy;
        dz = stick_scroll / 8;
        stick_scroll -= dz * 8;
        dx = dy = 0;
    } else if (stick_mid) {
        stick_mid = 0;
        if (!stick_moved) sink(0, 0, 0, 0, padbtn | (b & 3) | 4);     /* still: a middle click */
    }
    stickbtn = b & 3;
    sink(dx, dy, -dz, 0, padbtn | stickbtn);
}

/* ---------------------------------------------------------------- Synaptics */

static int syn_probe(void)
{
    u8 r[3];
    u32 ident;
    int req, major;
    s32 xres = 0, yres = 0;
    if (cmd(0xE8) || cmd(0) || cmd(0xE8) || cmd(0) || cmd(0xE8) || cmd(0) || cmd(0xE8) || cmd(0) || getinfo(r))
        return -1;
    if (r[1] != 0x47) return -1;
    if (syn_query(0x00, r)) return -1;
    ident = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    major = (int)(ident & 0x0F);
    if (syn_query(0x03, r)) return -1;
    syn_model = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    if (syn_query(0x02, r)) return -1;
    syn_caps = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    if (r[1] != 0x47) syn_caps = 0;
    ksnprintf(name, sizeof(name), "Synaptics %u.%u", major, (unsigned)(ident >> 16));
    if (major < 4 || !(syn_model & 0x80)) {
        kprintf("ps2tp: %s touchpad (model %06lx): old absolute format, left in mouse mode\n", name, syn_model);
        return -1;
    }
    req = (int)((syn_caps >> 20) & 7);
    if (!(syn_caps & 0x800000UL)) req = 0;
    syn_ext = syn_ext0c = 0;
    if (req >= 1 && !syn_query(0x09, r)) syn_ext = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    if (req >= 4 && !syn_query(0x0C, r)) syn_ext0c = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    if (!syn_query(0x08, r) && r[0] && (r[1] & 0x80) && r[2]) {
        xres = r[0];
        yres = r[2];
    }
    xmin = 1472; xmax = 5472; ymin = 1408; ymax = 4448;
    if (req >= 5 && (syn_ext0c & 0x20000UL) && !syn_query(0x0D, r)) {
        xmax = ((s32)r[0] << 5) | ((r[1] & 0x0F) << 1);
        ymax = ((s32)r[2] << 5) | ((r[1] & 0xF0) >> 3);
    }
    if (req >= 7 && (syn_ext0c & 0x2000UL) && !syn_query(0x0F, r)) {
        xmin = ((s32)r[0] << 5) | ((r[1] & 0x0F) << 1);
        ymin = ((s32)r[2] << 5) | ((r[1] & 0xF0) >> 3);
    }
    if (xmax <= xmin || ymax <= ymin) {
        xmin = 1472; xmax = 5472; ymin = 1408; ymax = 4448;
    }
    syn_clickpad = (syn_ext0c & 0x100000UL) != 0;
    syn_middle = (syn_caps & 0x40000UL) != 0;
    syn_agm = (syn_ext0c & (0x80000UL | 0x800UL)) != 0;
    smbus_cap = (syn_ext0c & 0x4000UL) != 0;
    /* absolute, 80 packets/s, W mode */
    if (sliced((u8)(0x80 | 0x40 | ((syn_caps & 0x800000UL) ? 1 : 0))) || cmd(0xF3) || cmd(0x14)) return -1;
    if (syn_agm && (sliced(0x03) || cmd(0xF3) || cmd(0xC8))) syn_agm = 0;
    kprintf("ps2tp: %s touchpad, caps %06lx ext %06lx %06lx, %ld-%ld x %ld-%ld, %ld x %ld units/mm%s%s%s\n", name,
            syn_caps, syn_ext, syn_ext0c, xmin, xmax, ymin, ymax, xres, yres, syn_clickpad ? ", clickpad" : "",
            syn_agm ? ", second finger" : "", (syn_caps & 0x80) ? ", pass-through port" : "");
    pad = ptp_new_abs(xmin, xmax, ymin, ymax, xres ? (u32)((xmax - xmin) * 1000L / xres) : 0,
                      yres ? (u32)((ymax - ymin) * 1000L / yres) : 0, pad_out);
    return pad ? 0 : -1;
}

static void syn_packet(void)
{
    const u8 *b = pkt;
    int w = ((b[0] & 0x30) >> 2) | ((b[0] & 0x04) >> 1) | ((b[3] & 0x04) >> 2);
    s32 x, y, z;
    int btn, n = 0;
    struct ptp_contact c[3];
    if ((syn_caps & 0x800000UL) && w == 3 && (syn_caps & 0x80)) {   /* pass-through: the stick */
        int dx = (int)b[4] - ((b[1] & 0x10) ? 256 : 0), dy = (int)b[5] - ((b[1] & 0x20) ? 256 : 0);
        stick_out(dx, -dy, b[1] & 7);
        return;
    }
    if (syn_agm && w == 2) {
        switch ((b[5] & 0x30) >> 4) {
        case 1:
            agm_x = ((((s32)b[4] & 0x0F) << 8) | b[1]) << 1;
            agm_y = ((((s32)b[4] & 0xF0) << 4) | b[2]) << 1;
            agm_valid = 1;
            break;
        }
        return;
    }
    x = (((s32)b[3] & 0x10) << 8) | (((s32)b[1] & 0x0F) << 8) | b[4];
    y = (((s32)b[3] & 0x20) << 7) | (((s32)b[1] & 0xF0) << 4) | b[5];
    z = b[2];
    if (x > 8176) x -= 8192;
    if (y > 8176) y -= 8192;
    btn = b[0] & 3;
    if (syn_clickpad) btn = ((b[0] ^ b[3]) & 1) ? 1 : 0;
    else if (syn_middle && ((b[0] ^ b[3]) & 1)) btn |= 4;
    /* a finger from pressure 30 on, lifted below 25 */
    if (z >= (syn_touch ? 25 : 30) && x > 1) {
        syn_touch = 1;
        n = 1;
        if ((syn_caps & 0x800000UL) && (syn_caps & 2) && w <= 1) n = w + 2;
    } else {
        syn_touch = 0;
    }
    if (n) {
        c[0].id = 0;
        c[0].x = x;
        c[0].y = ymax - y;
        if (n >= 2) {
            c[1].id = 1;
            c[1].x = agm_valid ? agm_x : x;
            c[1].y = agm_valid ? ymax - agm_y : ymax - y;
        }
        if (n >= 3) {
            c[2] = c[1];
            c[2].id = 2;
        }
    } else {
        agm_valid = 0;
    }
    ptp_contacts(pad, c, n, btn);
}

/* ---------------------------------------------------------------- ELAN */

static int el_signature_ok(const u8 *p)
{
    static const u8 rates[] = { 200, 100, 80, 60, 40, 20, 10 };
    int i;
    if (p[0] == 0) return 0;
    if (p[1] == 0) return 1;
    if ((p[0] & 0x0F) >= 0x06 && (p[1] & 0xAF) == 0x0F && p[2] < 40) return 1;
    for (i = 0; i < (int)sizeof(rates); i++)
        if (p[2] == rates[i]) return 0;
    return 1;
}

static s32 el_res(u8 v)
{
    return ((s32)v * 10 + 790) * 10 / 254;
}

static int el_probe(void)
{
    u8 r[3], caps[3], v;
    s32 xres = 0, yres = 0;
    u32 ver;
    if (cmd(0xE6) || cmd(0xE6) || cmd(0xE6) || getinfo(r)) return -1;
    if (r[0] != 0x3C || r[1] != 0x03 || (r[2] != 0xC8 && r[2] != 0x00)) return -1;
    el_hw = 0;
    if (syn_query(0x01, r) || !el_signature_ok(r)) return -1;
    el_fw = ((u32)r[0] << 16) | ((u32)r[1] << 8) | r[2];
    ver = (el_fw & 0x0F0000UL) >> 16;
    el_ic = (u8)ver;
    if (el_fw < 0x020030UL || el_fw == 0x020600UL) el_hw = 1;
    else if (ver == 2 || ver == 4) el_hw = 2;
    else if (ver == 5) el_hw = 3;
    else if (ver >= 6) el_hw = 4;
    ksnprintf(name, sizeof(name), "ELAN v%u (firmware %06lx)", el_hw, el_fw);
    if (el_hw < 2) {
        kprintf("ps2tp: %s touchpad: left in mouse mode\n", name);
        return -1;
    }
    el_crc = (el_fw & 0x4000) != 0;
    el_buttonpad = (el_fw & 0x1000) != 0;
    el_reports_pressure = el_fw >= 0x020800UL;
    if (el_query(0x02, caps)) return -1;
    el_tp = el_hw >= 3 && (caps[0] & 0x80) != 0;
    el_samples1 = 0;
    if (el_hw == 4 && !el_query(0x03, r)) el_samples1 = r[1];
    smbus_cap = 0;
    switch (el_hw) {
    case 2:
        if (el_fw == 0x020800UL || el_fw == 0x020B00UL || el_fw == 0x020030UL) {
            xmax = 1152;
            ymax = 768;
        } else {
            int i = (el_fw > 0x020800UL && el_fw < 0x020900UL) ? 1 : 2;
            if (el_query(0x00, r)) return -1;
            if ((el_fw >> 16) == 0x14 && (r[1] & 0x10)) {
                if (el_query(0x03, r)) return -1;
                xmax = (caps[1] - i) * r[1] / 2;
                ymax = (caps[2] - i) * r[2] / 2;
            } else if (el_fw == 0x040216UL) {
                xmax = 819;
                ymax = 405;
            } else if (el_fw == 0x040219UL || el_fw == 0x040215UL) {
                xmax = 900;
                ymax = 500;
            } else {
                xmax = (caps[1] - i) * 64;
                ymax = (caps[2] - i) * 64;
            }
        }
        if (el_write(0x10, 0x54) || el_write(0x11, 0x88) || el_write(0x21, 0x60)) return -1;
        break;
    case 3:
    case 4:
        if (el_query(0x00, r)) return -1;
        xmax = ((r[0] & 0x0F) << 8) | r[1];
        ymax = ((r[0] & 0xF0) << 4) | r[2];
        xres = yres = 31;
        if (el_hw == 4) {
            if (!el_query(0x04, r)) {
                xres = el_res((u8)(r[1] & 0x0F));
                yres = el_res((u8)(r[1] >> 4));
                if (r[2] == 2 || r[2] == 4) smbus_cap = 1;      /* SMBus with host notify */
            }
            if ((el_fw & 0x0F2000UL) == 0x0F2000UL && (el_fw & 0xFF)) smbus_cap = 1;
            if (el_write(0x07, 0x01)) return -1;
        } else if (el_write(0x10, 0x0B)) {
            return -1;
        }
        break;
    }
    xmin = ymin = 0;
    if (xmax <= 0 || ymax <= 0) return -1;
    if (el_hw <= 3) {
        int tries;
        for (tries = 0; tries < 6 && el_read(0x10, &v); tries++) mdelay(10);
    }
    memset(mt, 0, sizeof(mt));
    kprintf("ps2tp: %s touchpad, caps %02x %02x %02x, 0-%ld x 0-%ld, %ld x %ld units/mm%s%s%s\n", name, caps[0], caps[1],
            caps[2], xmax, ymax, xres, yres, el_buttonpad ? ", clickpad" : "", el_tp ? ", TrackPoint" : "",
            el_crc ? ", CRC" : "");
    pad = ptp_new_abs(0, xmax, 0, ymax, xres ? (u32)(xmax * 1000L / xres) : 0, yres ? (u32)(ymax * 1000L / yres) : 0,
                      pad_out);
    return pad ? 0 : -1;
}

static int el_buttons(void)
{
    if (el_buttonpad) return (pkt[0] & 3) ? 1 : 0;
    return (pkt[0] & 1) | (pkt[0] & 2) | (pkt[0] & 4);
}

static void el_report(void)
{
    struct ptp_contact c[5];
    int i, n = 0;
    for (i = 0; i < 5; i++)
        if (mt[i].on) {
            c[n].id = (u8)i;
            c[n].x = mt[i].x;
            c[n].y = mt[i].y;
            n++;
        }
    ptp_contacts(pad, c, n, el_buttons());
}

static void el_trackpoint(void)
{
    const u8 *b = pkt;
    u32 t = b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    int x, y;
    switch (t & ~7UL) {
    case 0x06000030UL: case 0x16008020UL: case 0x26800010UL: case 0x36808000UL:
        if (b[4] == 0x80 || b[5] == 0x80 || b[1] >> 7 == b[4] >> 7 || b[2] >> 7 == b[5] >> 7) return;
        x = (int)b[4] - (int)((b[1] ^ 0x80) << 1);
        y = (int)((b[2] ^ 0x80) << 1) - (int)b[5];
        stick_out(x, y, b[0] & 7);
        break;
    }
}

static void el_v2(void)
{
    const u8 *b = pkt;
    int f = (b[0] & 0xC0) >> 6, i;
    int ok = el_reports_pressure ? ((b[0] & 0x0C) == 0x04 && (b[3] & 0x0F) == 0x02) :
             (b[0] & 0xC0) == 0x80 ? ((b[0] & 0x0C) == 0x0C && (b[3] & 0x0E) == 0x08) :
             ((b[0] & 0x3C) == 0x3C && (b[1] & 0xF0) == 0 && (b[3] & 0x3E) == 0x38 && (b[4] & 0xF0) == 0);
    if (!ok) {
        n_bad++;
        return;
    }
    for (i = 0; i < 5; i++) mt[i].on = 0;
    if (f == 1 || f == 3) {
        mt[0].x = ((b[1] & 0x0F) << 8) | b[2];
        mt[0].y = ymax - (((b[4] & 0x0F) << 8) | b[5]);
        mt[0].on = 1;
        if (f == 3) {
            mt[1] = mt[0];
            mt[2] = mt[0];
        }
    } else if (f == 2) {
        mt[0].x = (((b[0] & 0x10) << 4) | b[1]) << 2;
        mt[0].y = ymax - ((((b[0] & 0x20) << 3) | b[2]) << 2);
        mt[1].x = (((b[3] & 0x10) << 4) | b[4]) << 2;
        mt[1].y = ymax - ((((b[3] & 0x20) << 3) | b[5]) << 2);
        mt[0].on = mt[1].on = 1;
    }
    el_report();
}

static void el_v3(void)
{
    static const u8 debounce[6] = { 0xC4, 0xFF, 0xFF, 0x02, 0xFF, 0xFF };
    const u8 *b = pkt;
    int head, tail, f = (b[0] & 0xC0) >> 6, i;
    if (!memcmp(b, debounce, 6)) return;
    if (el_crc) {
        head = (b[3] & 0x09) == 0x08;
        tail = (b[3] & 0x09) == 0x09;
    } else {
        head = (b[0] & 0x0C) == 0x04 && (b[3] & 0xCF) == 0x02;
        tail = (b[0] & 0x0C) == 0x0C && (b[3] & 0xCE) == 0x0C;
        if (!head && !tail && el_tp && (b[3] & 0x0F) == 0x06) {
            el_trackpoint();
            return;
        }
    }
    if (!head && !tail) {
        n_bad++;
        return;
    }
    if (f == 2 && head) {                           /* first of two fingers, the second follows */
        mt[0].x = ((b[1] & 0x0F) << 8) | b[2];
        mt[0].y = ymax - (((b[4] & 0x0F) << 8) | b[5]);
        return;
    }
    for (i = 1; i < 5; i++) mt[i].on = 0;
    if (f == 2) {
        mt[1].x = ((b[1] & 0x0F) << 8) | b[2];
        mt[1].y = ymax - (((b[4] & 0x0F) << 8) | b[5]);
        mt[0].on = mt[1].on = 1;
    } else if (f) {
        mt[0].x = ((b[1] & 0x0F) << 8) | b[2];
        mt[0].y = ymax - (((b[4] & 0x0F) << 8) | b[5]);
        mt[0].on = 1;
        if (f == 3) {
            mt[1] = mt[0];
            mt[2] = mt[0];
        }
    } else {
        mt[0].on = 0;
    }
    el_report();
}

static void el_v4(void)
{
    const u8 *b = pkt;
    int ok, id, sid, i, wt;
    if (el_tp && (b[3] & 0x0F) == 0x06) {
        el_trackpoint();
        return;
    }
    if (el_crc) ok = (b[3] & 0x08) == 0;
    else if (el_ic == 7 && el_samples1 == 0x2A) ok = (b[3] & 0x1C) == 0x10;
    else ok = (b[0] & 0x08) == 0 && (b[3] & 0x1C) == 0x10;
    if (!ok) {
        n_bad++;
        return;
    }
    switch (b[3] & 3) {
    case 0:                                         /* status: the fingers on the pad */
        for (i = 0; i < 5; i++)
            if (!(b[1] & (1 << i))) mt[i].on = 0;
        break;
    case 1:                                         /* head: a finger's position */
        id = ((b[3] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5) return;
        mt[id].x = ((b[1] & 0x0F) << 8) | b[2];
        mt[id].y = ymax - (((b[4] & 0x0F) << 8) | b[5]);
        mt[id].on = 1;
        break;
    case 2:                                         /* motion of one or two fingers */
        id = ((b[0] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5) return;
        sid = ((b[3] & 0xE0) >> 5) - 1;
        wt = (b[0] & 0x10) ? 5 : 1;
        mt[id].x += (s32)(s8)b[1] * wt;
        mt[id].y -= (s32)(s8)b[2] * wt;
        if (sid >= 0 && sid < 5) {
            mt[sid].x += (s32)(s8)b[4] * wt;
            mt[sid].y -= (s32)(s8)b[5] * wt;
        }
        break;
    default:
        n_bad++;
        return;
    }
    el_report();
}

/* ---------------------------------------------------------------- interface */

/* the device is reset to its defaults (F6) with streaming off (F5): a Synaptics or ELAN
   touchpad is switched to its absolute protocol (6-byte packets); 1 when one was, else 0.
   'out' receives the mouse events. Streaming is switched on by the caller (F4). */
int ps2tp_probe(const struct ps2io *p, void (*out)(int dx, int dy, int dz, int dw, int buttons))
{
    if (pad) ptp_free(pad);
    pad = NULL;
    kind = T_NONE;
    smbus_cap = 0;
    io = p;
    sink = out ? out : plat_mouse;
    padbtn = stickbtn = 0;
    pos = 0;
    if (syn_probe() == 0) {
        kind = T_SYN;
    } else {
        cmd(0xF6);
        if (el_probe() == 0) kind = T_ELAN;
    }
    if (kind == T_NONE) {
        if (pad) ptp_free(pad);
        pad = NULL;
        cmd(0xF6);                                  /* back to the plain mouse defaults */
        return 0;
    }
    return 1;
}

/* the same touchpad again after someone else (the BIOS) reset it: 1 when it is back */
int ps2tp_reprogram(const struct ps2io *p)
{
    void (*out)(int, int, int, int, int) = sink;
    if (kind == T_NONE) return 0;
    if (cmd(0xF5) && cmd(0xF5)) return 0;
    cmd(0xF6);
    return ps2tp_probe(p, out);
}

int ps2tp_active(void)
{
    return kind != T_NONE;
}

/* 1: Synaptics, 2: ELAN; the SMBus address of its companion in *smbaddr (0: none) */
int ps2tp_kind(u8 *smbaddr)
{
    if (smbaddr) *smbaddr = !smbus_cap ? 0 : kind == T_SYN ? 0x2C : 0x15;
    return kind;
}

const char *ps2tp_name(void)
{
    return kind != T_NONE ? name : "";
}

/* the pad is driven elsewhere now (SMBus) or not at all */
void ps2tp_off(void)
{
    if (pad) ptp_free(pad);
    pad = NULL;
    kind = T_NONE;
}

/* an aux byte (interrupt time): packets of 6 bytes, resynchronized on their constant bits */
void ps2tp_byte(u8 b)
{
    u32 now = plat_ms();
    if (kind == T_NONE) return;
    if (pos && now - last_ms > 40) pos = 0;
    last_ms = now;
    if (kind == T_SYN && ((pos == 0 && (b & 0xC8) != 0x80) || (pos == 3 && (b & 0xC8) != 0xC0))) {
        n_bad++;
        pos = 0;
        return;
    }
    pkt[pos++] = b;
    if (pos < 6) return;
    pos = 0;
    n_pkt++;
    if (kind == T_SYN) syn_packet();
    else if (el_hw == 2) el_v2();
    else if (el_hw == 3) el_v3();
    else el_v4();
}

u32 ps2tp_stats(u32 *bad)
{
    if (bad) *bad = n_bad;
    return n_pkt;
}
