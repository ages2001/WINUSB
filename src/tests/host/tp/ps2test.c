/* ps2test.c - host harness: ps2tp.c and ptp.c against models of a Synaptics (absolute mode,
   W, AGM, clickpad, pass-through TrackPoint) and an ELAN v4 PS/2 touchpad */
#include "tp.h"
#include "../../../core/ptp.c"
#include "../../../core/ps2tp.c"

/* ---------------------------------------------------------------- device model */
static int model;                   /* 1 Synaptics, 2 ELAN */
static u8 outq[8];
static int on, op;
static int argfor;                  /* the command whose argument comes next */
static int e8n, e6n, f8;
static u8 e8v, f8q;
static int mode, agm, streaming, n_cmds;
static u8 regs[256];

static void put(u8 b) { outq[on++] = b; }

static void info(void)
{
    u8 q = e8v;
    if (model == 1 && e8n == 4) {
        switch (q) {
        case 0x00: put(0x01); put(0x47); put(0x18); break;     /* identify: major 8 */
        case 0x02: put(0xD0); put(0x47); put(0x83); break;     /* caps: extended, 5 requests, pass-through, multifinger, palm */
        case 0x03: put(0x01); put(0x6B); put(0x80); break;     /* model: new absolute format */
        case 0x08: put(85); put(0x80); put(94); break;         /* resolution */
        case 0x0C: put(0x1A); put(0x40); put(0x00); break;     /* clickpad, AGM, max dimensions, InterTouch */
        case 0x0D: put(175); put(0x00); put(143); break;       /* 5600 x 4576 */
        default: put(0); put(0); put(0);
        }
    } else if (model == 2 && f8) {
        switch (f8q) {
        case 0x00: put(0x7B); put(0xB8); put(0xD0); break;     /* 3000 x 2000 */
        case 0x02: put(0x00); put(15); put(10); break;
        case 0x04: put(0x00); put(0x44); put(0x00); break;
        case 0x03: put(0x00); put(0x00); put(0x00); break;
        default: put(0); put(0); put(0);
        }
    } else if (model == 2 && e8n == 4 && e8v == 0x01) {
        put(0x46); put(0x1F); put(0x01);                        /* firmware: IC 6 -> v4, clickpad */
    } else if (model == 2 && e6n >= 3) {
        put(0x3C); put(0x03); put(0xC8);
    } else {
        put(0x00); put(0x02); put(0x64);
    }
    e8n = 0; e8v = 0; e6n = 0; f8 = 0;
}

static int dev_cmd(u8 c)
{
    n_cmds++;
    on = op = 0;
    if (argfor) {
        int a = argfor;
        argfor = 0;
        if (a == 0xE8) { e8v = (u8)((e8v << 2) | (c & 3)); e8n++; }
        if (a == 0xF3) {
            if (model == 1 && c == 0x14 && e8n == 4) mode = e8v;
            if (model == 1 && c == 0xC8 && e8n == 4 && e8v == 3) agm = 1;
            e8n = 0; e8v = 0;
        }
        if (a == 0xF8) { f8q = c; f8 = 1; regs[0] = c; }
        put(0xFA);
        return 0;
    }
    switch (c) {
    case 0xE8: case 0xF3: case 0xF8: argfor = c; break;
    case 0xE6: if (!f8) { e6n++; e8n = 0; e8v = 0; } else f8 = 0; break;
    case 0xE9: put(0xFA); info(); return 0;
    case 0xF4: streaming = 1; break;
    case 0xF5: streaming = 0; break;
    case 0xF6: mode = 0; agm = 0; e6n = e8n = 0; break;
    case 0xFF: mode = 0; agm = 0; put(0xFA); put(0xAA); put(0x00); return 0;
    }
    put(0xFA);
    return 0;
}

static int io_cmd(u8 c)
{
    dev_cmd(c);
    if (op < on && outq[op] == 0xFA) { op++; return 0; }
    return -1;
}

static int io_read(u32 ms)
{
    now_us += 200;
    (void)ms;
    return op < on ? outq[op++] : -1;
}

static const struct ps2io io_ = { io_cmd, io_read };

static void bytes(const u8 *b, int n)
{
    int i;
    for (i = 0; i < n; i++) { now_us += 100; ps2tp_byte(b[i]); }
}

static void run(u32 ms)
{
    u32 end = now_us + ms * 1000;
    while (now_us < end) { now_us += 1000; ptp_tick(); }
}

/* ---------------------------------------------------------------- Synaptics packets */
static void syn(int x, int y, int z, int w, int press)
{
    u8 b[6];
    b[0] = (u8)(0x80 | (((w >> 2) & 3) << 4) | (((w >> 1) & 1) << 2));
    b[1] = (u8)((((y >> 8) & 0x0F) << 4) | ((x >> 8) & 0x0F));
    b[2] = (u8)z;
    b[3] = (u8)(0xC0 | (((x >> 12) & 1) << 4) | (((y >> 12) & 1) << 5) | ((w & 1) << 2) | (press ? 1 : 0));
    b[4] = (u8)x;
    b[5] = (u8)y;
    bytes(b, 6);
    run(12);
}

static void syn_stick(int dx, int dy, int btn)
{
    u8 b[6];
    int w = 3;
    b[0] = (u8)(0x80 | (((w >> 2) & 3) << 4) | (((w >> 1) & 1) << 2));
    b[1] = (u8)(btn | 0x08 | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0));
    b[2] = 0;
    b[3] = (u8)(0xC0 | ((w & 1) << 2));
    b[4] = (u8)dx;
    b[5] = (u8)dy;
    bytes(b, 6);
    run(12);
}

static void test_synaptics(void)
{
    int i;
    u8 sa;
    model = 1;
    CHECK(ps2tp_probe(&io_, plat_mouse) == 1, "Synaptics not found");
    CHECK(mode == 0xC1, "mode %02x, want C1 (absolute, high rate, W)", mode);
    CHECK(agm, "advanced gesture mode not set");
    CHECK(ps2tp_kind(&sa) == 1 && sa == 0x2C, "kind / SMBus address %02x", sa);
    CHECK(xmax == 5600 && ymax == 4576, "range %d %d", (int)xmax, (int)ymax);
    printf("synaptics: %d commands, %s\n", n_cmds, ps2tp_name());
    /* one finger to the right: pointer right */
    ev_reset();
    for (i = 0; i < 30; i++) syn(2000 + i * 40, 3000, 60, 6, 0);
    syn(0, 0, 0, 0, 0);
    run(100);
    printf("  move right: dx %d dy %d wheel %d clicks %d\n", ev_dx, ev_dy, ev_dz, ev_clicks[0]);
    CHECK(ev_dx > 50 && abs(ev_dy) < 3 && !ev_dz && !ev_clicks[0], "one-finger move");
    /* one finger up (raw y grows): pointer up */
    ev_reset();
    for (i = 0; i < 30; i++) syn(3000, 2000 + i * 40, 60, 6, 0);
    syn(0, 0, 0, 0, 0);
    run(100);
    CHECK(ev_dy < -50 && abs(ev_dx) < 3, "one-finger move up: dy %d", ev_dy);
    /* two fingers (W 0) down: wheel towards the user (negative) */
    ev_reset();
    for (i = 0; i < 40; i++) syn(3000, 3500 - i * 40, 70, 0, 0);
    syn(0, 0, 0, 0, 0);
    run(100);
    printf("  two-finger scroll down: wheel %d pointer %d %d\n", ev_dz, ev_dx, ev_dy);
    CHECK(ev_dz < -3 && !ev_dx && !ev_dy, "two-finger scroll");
    /* tap: left click; two-finger tap: right click */
    ev_reset();
    syn(3000, 3000, 60, 6, 0); syn(3002, 3000, 60, 6, 0); syn(0, 0, 0, 0, 0);
    run(100);
    CHECK(ev_clicks[0] == 1 && !ev_lastbtn, "tap: clicks %d", ev_clicks[0]);
    ev_reset();
    syn(3000, 3000, 60, 0, 0); syn(3002, 3000, 60, 0, 0); syn(0, 0, 0, 0, 0);
    run(100);
    CHECK(ev_clicks[1] == 1 && !ev_clicks[0], "two-finger tap: right %d left %d", ev_clicks[1], ev_clicks[0]);
    /* clickpad press with one finger: left button held while pressed */
    ev_reset();
    syn(3000, 3000, 80, 6, 0);
    for (i = 0; i < 10; i++) syn(3000, 3000, 90, 6, 1);
    CHECK(ev_btn == 1, "clickpad press: buttons %d", ev_btn);
    syn(3000, 3000, 80, 6, 0); syn(0, 0, 0, 0, 0);
    run(100);
    CHECK(ev_clicks[0] == 1 && !ev_btn, "clickpad release: clicks %d btn %d", ev_clicks[0], ev_btn);
    /* TrackPoint through the pass-through port: motion, middle-button scrolling */
    ev_reset();
    for (i = 0; i < 5; i++) syn_stick(10, 0, 0);
    CHECK(ev_dx == 50, "stick motion %d", ev_dx);
    ev_reset();
    for (i = 0; i < 10; i++) syn_stick(0, -10, 4);
    syn_stick(0, 0, 0);
    CHECK(ev_dz < 0 && !ev_dy && !ev_clicks[2], "stick scroll: wheel %d dy %d middle %d", ev_dz, ev_dy, ev_clicks[2]);
    /* resynchronization after a lost byte */
    {
        u8 junk[2] = { 0x12, 0x34 };
        u32 bad0, bad1;
        ps2tp_stats(&bad0);
        bytes(junk, 2);
        ev_reset();
        for (i = 0; i < 10; i++) syn(2000 + i * 40, 3000, 60, 6, 0);
        ps2tp_stats(&bad1);
        CHECK(bad1 > bad0 && ev_dx > 10, "resync: bad %u dx %d", bad1 - bad0, ev_dx);
    }
    /* again after the BIOS reset it */
    dev_cmd(0xFF);
    mode = 0;
    CHECK(ps2tp_reprogram(&io_) == 1 && mode == 0xC1, "reprogram: mode %02x", mode);
}

/* ---------------------------------------------------------------- ELAN v4 packets */
static void el_head(int id, int x, int ydown, int btn)
{
    int y = 2000 - ydown;
    u8 b[6];
    b[0] = (u8)(0x50 | btn);
    b[1] = (u8)(0x30 | ((x >> 8) & 0x0F));
    b[2] = (u8)x;
    b[3] = (u8)(((id + 1) << 5) | 0x10 | 0x01);
    b[4] = (u8)(0x20 | ((y >> 8) & 0x0F));
    b[5] = (u8)y;
    bytes(b, 6);
    run(10);
}

static void el_motion(int id, int dx, int dydown, int sid, int dx2, int dy2down, int btn)
{
    u8 b[6];
    b[0] = (u8)(((id + 1) << 5) | btn);
    b[1] = (u8)dx;
    b[2] = (u8)-dydown;
    b[3] = (u8)(((sid + 1) << 5) | 0x10 | 0x02);
    b[4] = (u8)dx2;
    b[5] = (u8)-dy2down;
    bytes(b, 6);
    run(10);
}

static void el_status(int mask, int btn)
{
    u8 b[6] = { 0, 0, 0, 0x10, 0, 0 };
    b[0] = (u8)btn;
    b[1] = (u8)mask;
    bytes(b, 6);
    run(10);
}

static void test_elan(void)
{
    int i;
    u8 sa;
    model = 2;
    n_cmds = 0;
    CHECK(ps2tp_probe(&io_, plat_mouse) == 1, "ELAN not found");
    CHECK(ps2tp_kind(&sa) == 2 && el_hw == 4 && el_buttonpad, "ELAN kind %d hw %d", ps2tp_kind(NULL), el_hw);
    CHECK(xmax == 3000 && ymax == 2000, "ELAN range %d %d", (int)xmax, (int)ymax);
    printf("elan: %d commands, %s\n", n_cmds, ps2tp_name());
    ev_reset();
    el_status(1, 0);
    el_head(0, 1000, 1000, 0);
    for (i = 0; i < 30; i++) el_motion(0, 20, 0, -1, 0, 0, 0);
    el_status(0, 0);
    run(100);
    printf("  move right: dx %d dy %d\n", ev_dx, ev_dy);
    CHECK(ev_dx > 50 && !ev_dy && !ev_clicks[0], "ELAN move");
    ev_reset();
    el_status(3, 0);
    el_head(0, 1000, 800, 0);
    el_head(1, 1400, 800, 0);
    for (i = 0; i < 40; i++) el_motion(0, 0, 20, 1, 0, 20, 0);
    el_status(0, 0);
    run(100);
    printf("  two-finger scroll down: wheel %d\n", ev_dz);
    CHECK(ev_dz < -3 && !ev_dx && !ev_dy, "ELAN scroll");
    ev_reset();
    el_status(1, 0);
    el_head(0, 1000, 1000, 0);
    el_status(0, 0);
    run(100);
    CHECK(ev_clicks[0] == 1, "ELAN tap");
    ev_reset();
    el_status(1, 0);
    el_head(0, 1000, 1000, 0);
    el_motion(0, 0, 0, -1, 0, 0, 2);       /* the pad pressed (either bit: left on a clickpad) */
    CHECK(ev_btn == 1, "ELAN clickpad press %d", ev_btn);
    el_motion(0, 0, 0, -1, 0, 0, 0);
    el_status(0, 0);
}

int main(int argc, char **argv)
{
    if (argc > 1) verbose = atoi(argv[1]);
    test_synaptics();
    test_elan();
    printf("ps2: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
