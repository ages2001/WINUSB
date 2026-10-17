/* smbtest.c - host harness: smbtp.c against a model of the i801 SMBus host with a Synaptics
   RMI4 pad (F01, F12 with register descriptors, F3A buttons) and an ELAN pad behind it */
#include "tp.h"
#define OPT2_NOSMB 0x2000

/* ---------------------------------------------------------------- PCI and I/O model */
static u8 pcicfg[256];
static int pci_fill(struct pcidev *p, int b, int d, int f)
{
    if (b || d != 31 || f != 4) return -1;
    memset(p, 0, sizeof(*p));
    p->dev = 31; p->fn = 4; p->vendor = 0x8086; p->cls = 0x0C; p->sub = 0x05;
    return 0;
}
static u32 pci_rd32(int b, int d, int f, int o) { (void)b; (void)d; (void)f; return o == 0x20 ? 0xEFA1 : 0; }
static u8 pci_rd8(int b, int d, int f, int o) { (void)b; (void)d; (void)f; return pcicfg[o]; }
static void pci_wr8(int b, int d, int f, int o, u8 v) { (void)b; (void)d; (void)f; pcicfg[o] = v; }
static void pci_enable(struct pcidev *p, int m, int i, int b) { (void)p; (void)m; (void)i; (void)b; }

#define BASE 0xEFA0
static u8 hreg[32], blk[32];
static int blkidx, inuse, model;
static u8 slvsts, ntfadd;
static int transactions;

/* devices */
static u8 rmi_regs[65536][1];       /* default one-byte registers */
struct preg { u16 addr; u8 len; u8 v[48]; };
static struct preg pregs[16];
static int npregs;
static struct { u16 addr; u8 cnt, wr; } rmap[8];
static int mdl_buttons = 0xFF;
static u8 rmi_irq;
static u8 data1[40];

static struct preg *pr(u16 a) { int i; for (i = 0; i < npregs; i++) if (pregs[i].addr == a) return &pregs[i]; return NULL; }
static void preg(u16 a, int len, const u8 *v) { pregs[npregs].addr = a; pregs[npregs].len = (u8)len; memcpy(pregs[npregs].v, v, len); npregs++; }

/* RMI block read at a: packet registers stream their bytes, then the next address */
static int rmi_rd(u16 a, u8 *out, int n)
{
    int k = 0;
    while (k < n) {
        struct preg *p = pr(a);
        int i;
        if (a == 0x0001) {                          /* F01 interrupt status: read clears */
            out[k++] = rmi_irq;
            rmi_irq = 0;
        } else if (a == 0x0060) {                   /* F12 data0 + data1 */
            u8 d[42] = { 0, 0 };
            memcpy(d + 2, data1, 40);
            for (i = 0; i < 42 && k < n; i++) out[k++] = d[i];
            a++;
            continue;
        } else if (a == 0x007A) {
            out[k++] = (u8)mdl_buttons;
        } else if (p) {
            for (i = 0; i < p->len && k < n; i++) out[k++] = p->v[i];
        } else {
            out[k++] = rmi_regs[a][0];
        }
        a++;
    }
    return n;
}

static void rmi_wr(u16 a, const u8 *v, int n)
{
    int i;
    for (i = 0; i < n; i++) rmi_regs[a + i][0] = v[i];
}

static u32 elan_seq;
static u8 elan_rep[32];
static int elan_on, elan_abs;

/* one SMBus transaction; 0 or -1 (device error) */
static int device(u8 addr, int rd, u8 cmd, int proto)
{
    transactions++;
    if (model == 1 && addr == 0x2C) {
        if (proto == 0x08 && rd && cmd == 0xFD) { hreg[5] = 1; return 0; }   /* version 2 */
        if (proto != 0x14) return -1;
        if (!rd && cmd >= 0x80 && cmd < 0x88) {
            int i = cmd - 0x80;
            rmap[i].addr = (u16)(blk[0] | (blk[1] << 8));
            rmap[i].cnt = blk[2];
            rmap[i].wr = blk[3];
            return 0;
        }
        if (cmd < 8) {
            if (rd) {
                if (rmap[cmd].wr) return -1;
                rmi_rd(rmap[cmd].addr, blk, rmap[cmd].cnt);
                hreg[5] = rmap[cmd].cnt;
            } else {
                if (!rmap[cmd].wr) return -1;
                rmi_wr(rmap[cmd].addr, blk, hreg[5]);
            }
            return 0;
        }
        return -1;
    }
    if (model == 2 && addr == 0x15) {
        if (proto == 0x04 && !rd && cmd == 0x20) { elan_on = 1; return 0; }
        if (proto != 0x14) return -1;
        if (!rd && cmd == 0x00) { elan_abs = blk[3]; return 0; }
        if (!rd) return -1;
        switch (cmd) {
        case 0xA7: memset(blk, 0x55, 5); hreg[5] = 5; return 0;
        case 0xA0: blk[0] = 0x7B; blk[1] = 0xB8; blk[2] = 0xD0; hreg[5] = 3; return 0;
        case 0xA4: blk[0] = 0; blk[1] = 0x44; hreg[5] = 2; return 0;
        case 0xA8: memcpy(blk, elan_rep, 32); hreg[5] = 32; return 0;
        }
        return -1;
    }
    return -1;
}

static u8 inb(u16 port)
{
    int r = port - BASE;
    if (r == 0) {                                   /* status: reading takes the semaphore */
        u8 v = (u8)(hreg[0] | (inuse ? 0x40 : 0));
        inuse = 1;
        return v;
    }
    if (r == 2) { blkidx = 0; return hreg[2]; }
    if (r == 7) return blk[blkidx++ & 31];
    if (r == 0x10) return slvsts;
    if (r == 0x14) return ntfadd;
    return hreg[r];
}

static void outb(u16 port, u8 v)
{
    int r = port - BASE;
    if (r == 0) {
        hreg[0] &= (u8)~(v & 0x9E);
        if (v & 0x40) inuse = 0;
        return;
    }
    if (r == 7) { blk[blkidx++ & 31] = v; return; }
    if (r == 0x10) { slvsts &= (u8)~v; return; }
    hreg[r] = v;
    if (r == 2 && (v & 0x40)) {
        int e = device((u8)(hreg[4] >> 1), hreg[4] & 1, hreg[3], v & 0x1C);
        hreg[0] |= e ? 0x04 : 0x02;
    }
}

#include "../../../core/ptp.c"
#include "../../../core/smbtp.c"

static void run(u32 ms)
{
    u32 end = now_us + ms * 1000;
    while (now_us < end) { now_us += 1000; smbtp_poll(); ptp_tick(); }
}

/* ---------------------------------------------------------------- RMI4 pad */
static void rmi_setup_model(void)
{
    /* page 0: F01 at E9, F12 at E3, F3A at DD, end at D7; page 1 empty */
    static const u8 pdt[3][6] = {
        { 0x30, 0x38, 0x20, 0x00, 0x01, 0x01 },     /* F01: control 20, data 00 */
        { 0x40, 0x00, 0x50, 0x60, 0x02, 0x12 },     /* F12: query 40, control 50, data 60 */
        { 0x70, 0x00, 0x78, 0x7A, 0x01, 0x3A },     /* F3A: query 70, data 7A */
    };
    int i, k;
    memset(rmi_regs, 0, sizeof(rmi_regs));
    npregs = 0;
    for (k = 0; k < 3; k++)
        for (i = 0; i < 6; i++) rmi_regs[0xE9 - 6 * k + i][0] = pdt[k][i];
    rmi_regs[0x70][0] = 3;                          /* three buttons */
    rmi_regs[0x40][0] = 0x01;                       /* F12 with register descriptors */
    {
        u8 qp[2] = { 1, 0x00 }, qst[1] = { 0 };
        u8 cp[3] = { 2, 0x00, 0x01 }, cst[2] = { 10, 0x0B };       /* control 8: subpackets 0, 1, 3 */
        u8 dp[2] = { 4, 0x03 }, dst[4] = { 2, 0x01, 40, 0x1F };     /* data 0 (2), data 1 (5 fingers) */
        rmi_regs[0x41][0] = 2; preg(0x42, 2, qp); preg(0x43, 1, qst);
        rmi_regs[0x44][0] = 3; preg(0x45, 3, cp); preg(0x46, 2, cst);
        rmi_regs[0x47][0] = 2; preg(0x48, 2, dp); preg(0x49, 4, dst);
    }
    {
        u8 c8[10] = { 0x70, 0x0F, 0x40, 0x0A, 0x00, 0x40, 0x00, 0x40, 26, 17 };  /* 3952 x 2624, pitch 4 mm, 26 x 17 */
        preg(0x50, 10, c8);
    }
    memset(rmap, 0xFF, sizeof(rmap));
}

static void rmi_finger(int slot, int type, int x, int y)
{
    u8 *o = data1 + slot * 8;
    o[0] = (u8)type; o[1] = (u8)x; o[2] = (u8)(x >> 8); o[3] = (u8)y; o[4] = (u8)(y >> 8); o[5] = 50;
    rmi_irq |= 0x02;
}

static void test_rmi(void)
{
    int i;
    model = 1;
    rmi_setup_model();
    CHECK(smbtp_start(1, 0x2C, plat_mouse) == 0, "RMI4 pad not started");
    CHECK(rmax_x == 3952 && rmax_y == 2624 && nfingers == 5 && is_f12, "F12 %d x %d, %d fingers", (int)rmax_x,
          (int)rmax_y, nfingers);
    CHECK(rum_x == 104000 && rum_y == 68000, "sensor %u x %u um", rum_x, rum_y);
    CHECK(f01_irq == 1 && sensor_irq == 6 && button_irq == 8, "irq bits %x %x %x", f01_irq, sensor_irq, button_irq);
    CHECK((rmi_regs[0x20][0] & 0x80) && rmi_regs[0x21][0] == 0x0F, "F01 control %02x %02x", rmi_regs[0x20][0],
          rmi_regs[0x21][0]);
    ev_reset();
    for (i = 0; i < 30; i++) { rmi_finger(0, 1, 1000 + i * 20, 1000); run(10); }
    memset(data1, 0, sizeof(data1)); rmi_irq |= 2; run(100);
    printf("rmi: move right dx %d dy %d, %d transactions\n", ev_dx, ev_dy, transactions);
    CHECK(ev_dx > 30 && !ev_dy && !ev_clicks[0], "RMI move");
    ev_reset();
    for (i = 0; i < 40; i++) { rmi_finger(0, 1, 1000, 1000 + i * 20); rmi_finger(1, 1, 1500, 1000 + i * 20); run(10); }
    memset(data1, 0, sizeof(data1)); rmi_irq |= 2; run(100);
    printf("rmi: two-finger scroll down wheel %d\n", ev_dz);
    CHECK(ev_dz < -3 && !ev_dx && !ev_dy, "RMI scroll");
    ev_reset();
    rmi_finger(0, 1, 1000, 1000); run(10);
    mdl_buttons = 0xFE; rmi_irq |= 8; run(30);
    CHECK(ev_btn == 1, "RMI button %d", ev_btn);
    mdl_buttons = 0xFF; rmi_irq |= 8; run(10);
    memset(data1, 0, sizeof(data1)); rmi_irq |= 2; run(100);
    CHECK(ev_clicks[0] == 1 && !ev_btn, "RMI button up");
    /* palm (type 3) is no finger */
    ev_reset();
    for (i = 0; i < 10; i++) { rmi_finger(0, 3, 1000 + i * 30, 1000); run(10); }
    CHECK(!ev_dx, "RMI palm moved the pointer");
    memset(data1, 0, sizeof(data1)); rmi_irq |= 2; run(50);
    smbtp_stop();
}

/* ---------------------------------------------------------------- ELAN pad */
static void elan_report(int n, int x, int ydown, int x2, int btn)
{
    memset(elan_rep, 0, sizeof(elan_rep));
    elan_rep[0] = 0x5D;
    elan_rep[1] = (u8)btn;
    if (n >= 1) {
        int y = 2000 - ydown;
        elan_rep[1] |= 0x08;
        elan_rep[2] = (u8)(((x >> 8) << 4) | (y >> 8)); elan_rep[3] = (u8)x; elan_rep[4] = (u8)y;
    }
    if (n >= 2) {
        int y = 2000 - ydown;
        elan_rep[1] |= 0x10;
        elan_rep[7] = (u8)(((x2 >> 8) << 4) | (y >> 8)); elan_rep[8] = (u8)x2; elan_rep[9] = (u8)y;
    }
    slvsts |= 1;
    ntfadd = 0x15 << 1;
}

static void test_elan(void)
{
    int i;
    model = 2;
    CHECK(smbtp_start(2, 0x15, plat_mouse) == 0, "ELAN pad not started");
    CHECK(elan_on && elan_abs == 1 && emax_x == 3000 && emax_y == 2000, "ELAN setup %d %d %d %d", elan_on, elan_abs,
          (int)emax_x, (int)emax_y);
    ev_reset();
    for (i = 0; i < 30; i++) { elan_report(1, 1000 + i * 15, 1000, 0, 0); run(10); }
    elan_report(0, 0, 0, 0, 0); run(150);
    printf("elan smbus: move right dx %d, host notify %s\n", ev_dx, nfy_seen ? "seen" : "not seen");
    CHECK(ev_dx > 30 && !ev_dy && nfy_seen, "ELAN SMBus move");
    ev_reset();
    for (i = 0; i < 40; i++) { elan_report(2, 1000, 800 + i * 15, 1300, 0); run(10); }
    elan_report(0, 0, 0, 0, 0); run(150);
    CHECK(ev_dz < -3 && !ev_dx, "ELAN SMBus scroll %d", ev_dz);
    smbtp_stop();
}

int main(int argc, char **argv)
{
    if (argc > 1) verbose = atoi(argv[1]);
    test_rmi();
    test_elan();
    printf("smbus: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
