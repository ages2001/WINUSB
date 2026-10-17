/* i2ctest.c - host harness: i2c.c (HID over I2C) with an ACPI device (descriptor register from
   _DSM, active-low GpioInt) and a precision touchpad model behind i2c_xfer / gpio_get */
#include "tp.h"
struct acpi_hidi2c {
    char seg[5]; char hid[9]; u16 addr[4]; u32 naddr; u32 speed; char bus[32];
    u16 descreg; u16 gpin; char gctl[32]; u8 gflags;
};
struct i2cbus { int id; };
static struct i2cbus bus0;
static int use_gpio = 1, gpio_reads;
static int acpi_hid_i2c(struct acpi_hidi2c *o, int max)
{
    memset(o, 0, sizeof(*o));
    strcpy(o->seg, "TPD0"); strcpy(o->bus, "\\_SB.PCI0.I2C1");
    o->addr[0] = 0x15; o->addr[1] = 0x2C; o->naddr = 2;
    o->descreg = 0x20;
    if (use_gpio) { strcpy(o->gctl, "\\_SB.GPI0"); o->gpin = 53; o->gflags = 0x02; }
    (void)max;
    return 1;
}
static struct i2cbus *i2c_bus_get(const char *p) { (void)p; return &bus0; }
static struct i2cbus *i2c_bus_pci(struct pcidev *p) { (void)p; return NULL; }
static void i2c_bus_off(void) { }
static struct hiddev *hid_mouse_new(const u8 *rd, int n) { (void)rd; (void)n; return NULL; }
static void hid_mouse_input(struct hiddev *h, const u8 *r, u32 len) { (void)h; (void)r; (void)len; }
static void hid_mouse_free(struct hiddev *h) { (void)h; }

#include "../../../core/ptp.c"

/* ---------------------------------------------------------------- device model at 2Ch */
static u8 rd[1024]; static int rdlen;
static u8 q[64][40]; static int qlen[64], qh, qt;
static int irq, mode, resets, reads, empty_reads;
static FILE *rep; static u32 t0;

static void feed(void)
{
    u8 h[5];
    long pos;
    u32 t;
    if (!rep || mode != 3) return;
    for (;;) {
        pos = ftell(rep);
        if (fread(h, 1, 5, rep) != 5) return;
        memcpy(&t, h, 4);
        if (now_us / 1000 < t0 + t) { fseek(rep, pos, SEEK_SET); return; }
        fread(q[qh] + 2, 1, h[4], rep);
        q[qh][0] = (u8)(h[4] + 2); q[qh][1] = 0; qlen[qh] = h[4] + 2;
        qh = (qh + 1) & 63;
        irq = 1;
    }
}

static int i2c_xfer(struct i2cbus *b, int addr, const u8 *w, int wn, u8 *r, int rn)
{
    (void)b;
    now_us += 50 + 25 * (wn + rn);
    if (addr != 0x2C) return -1;
    if (wn == 2 && rn) {
        int reg = w[0] | (w[1] << 8);
        if (reg == 0x20) {
            u16 v[15] = { 30, 0x100, (u16)rdlen, 0x21, 0x23, 40, 0x25, 0, 0x22, 0x24, 0x06CB, 0x1234, 1, 0, 0 };
            memcpy(r, v, rn < 30 ? rn : 30);
            return 0;
        }
        if (reg == 0x21) { memcpy(r, rd, rn); return 0; }
        return -1;
    }
    if (wn >= 4 && w[0] == 0x22) {                          /* command register */
        int op = w[3] & 15;
        if (op == 1) { resets++; irq = 1; }
        if (op == 3 && wn >= 10 && w[2] == 0x33 && w[9] == 3) { mode = 3; t0 = now_us / 1000 + 50; rep = fopen("rep.bin", "rb"); }
        return 0;
    }
    if (!wn && rn) {                                        /* input register */
        reads++;
        feed();
        memset(r, 0, rn);
        if (qt != qh) {
            memcpy(r, q[qt], qlen[qt] < rn ? qlen[qt] : rn);
            qt = (qt + 1) & 63;
        } else {
            empty_reads++;
        }
        irq = qt != qh;
        return 0;
    }
    return -1;
}

static int gpio_get(const char *c, u16 pin)
{
    (void)c; (void)pin;
    gpio_reads++;
    feed();
    return irq ? 0 : 1;                                     /* active low */
}

static int gpio_set(const char *c, u16 pin, int v) { (void)c; (void)pin; (void)v; return 0; }

#include "../../../core/i2c.c"

static void run(u32 ms)
{
    u32 end = now_us + ms * 1000;
    while (now_us < end) { now_us += 1000; i2c_poll(); }
}

int main(int argc, char **argv)
{
    FILE *f = fopen("rd.bin", "rb");
    if (argc > 1) verbose = atoi(argv[1]);
    rdlen = (int)fread(rd, 1, sizeof rd, f);
    fclose(f);
    (void)gpio_set;
    i2c_start(NULL, 0);
    CHECK(ndev == 1 && devs[0].ptp && devs[0].descreg == 0x20, "devices %d", ndev);
    CHECK(devs[0].gctl[0], "interrupt line not used");
    CHECK(mode == 3, "touchpad mode %d", mode);
    ev_reset();
    run(6000);
    printf("i2c: pointer %d %d wheel %d, left %d right %d, %d input reads (%d empty), %d line reads\n", ev_dx, ev_dy,
           ev_dz, ev_clicks[0], ev_clicks[1], reads, empty_reads, gpio_reads);
    CHECK(ev_dx > 100 && ev_dz < -3 && ev_clicks[0] >= 1, "gestures");
    CHECK(empty_reads < 20, "input register read while the line was idle: %d", empty_reads);
    i2c_shutdown();
    printf("i2c: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
