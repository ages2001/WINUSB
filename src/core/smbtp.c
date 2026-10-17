/* smbtp.c - touchpads on the SMBus of Intel chipsets (i801 host, 00:1F.3 / 00:1F.4) that are
   PS/2 touchpads too: the PS/2 driver (ps2tp.c) finds them and asks this one to take over.
     Synaptics InterTouch (address 2Ch): RMI4 over SMBus (rmi_smbus.c of Linux): the version
     byte at FDh switches the pad to SMBus, RMI addresses go through a table of 8 entries
     (address, read count, write flag) written with block writes to 80h + n; the command code
     of a read or write is the entry's number. The page description table names the
     functions: F01 (device control, interrupt status), F11 or F12 (2D sensor: fingers), F30 /
     F3A (buttons). The interrupt status is polled.
     ELAN (address 15h, elan_i2c_smbus.c): hello packet A7h (55h x5), enable 20h, absolute mode,
     the range A0h, the resolution A4h; reports by block read A8h (report 5Dh: buttons and up
     to five fingers). Read when the pad's Host Notify arrives (the host's slave status is
     polled), every 10 ms while none has been seen yet.
   Fingers and buttons go to the gesture engine of ptp.c. When the pad stops answering it is
   given back to PS/2 by the caller (smbtp_active() drops to 0). */
#include "usb.h"

/* i801 registers */
#define HSTSTS      0x00
#define HSTCNT      0x02
#define HSTCMD      0x03
#define HSTADD      0x04
#define HSTDAT0     0x05
#define HSTDAT1     0x06
#define BLKDAT      0x07
#define AUXCTL      0x0D
#define SLVSTS      0x10
#define NTFADD      0x14

#define ST_BUSY     0x01
#define ST_INTR     0x02
#define ST_DEVERR   0x04
#define ST_BUSERR   0x08
#define ST_FAILED   0x10
#define ST_INUSE    0x40
#define ST_BYTEDONE 0x80
#define ST_CLEAR    (ST_BYTEDONE | ST_FAILED | ST_BUSERR | ST_DEVERR | ST_INTR)

#define CNT_BYTE    0x04
#define CNT_BDATA   0x08
#define CNT_BLOCK   0x14
#define CNT_START   0x40

#define T_SYN       1
#define T_ELAN      2

static u16 smb;                     /* I/O base, 0: no host */
static u8 kind, addr;
static struct ptp *pad;
static u32 errors, last_ms, nfy_ms;
static u8 nfy_seen;

/* RMI4 */
static struct { u16 addr; u8 cnt, wr; } map[8];
static int map_next;
static u16 f01_ctl, f01_data;
static int nirq;                    /* interrupt sources in all */
static u8 irq_bytes;
static u32 sensor_irq, button_irq, f01_irq;
static u8 is_f12;
static u16 sens_data;               /* address of the finger data */
static u16 sens_len;
static u16 f11_fstate;              /* F11: bytes of finger states before the positions */
static int nfingers;
static u16 btn_data;
static u8 btn_len, btn_mask[2];
static s32 rmax_x, rmax_y;
static u32 rum_x, rum_y;            /* sensor size in micrometres, 0 unknown */

/* ELAN */
static s32 emax_x, emax_y;

/* ---------------------------------------------------------------- i801 host */

static int host_find(void)
{
    static const u8 fns[2] = { 3, 4 };
    int i;
    for (i = 0; i < 2; i++) {
        struct pcidev p;
        u32 bar;
        u8 hc;
        if (pci_fill(&p, 0, 31, fns[i]) || p.vendor != 0x8086 || p.cls != 0x0C || p.sub != 0x05) continue;
        bar = pci_rd32(0, 31, fns[i], 0x20);
        if (!(bar & 1) || !(bar & 0xFFE0)) continue;
        pci_enable(&p, 0, 1, 0);
        hc = pci_rd8(0, 31, fns[i], 0x40);
        if (hc & 4) continue;                       /* I2C mode: not SMBus */
        if (!(hc & 1)) pci_wr8(0, 31, fns[i], 0x40, (u8)(hc | 1));
        smb = (u16)(bar & 0xFFE0);
        DBG("smbtp: SMBus host 00:1f.%u at %04x\n", fns[i], smb);
        return 0;
    }
    return -1;
}

/* the semaphore (reading the status sets INUSE) and an idle host; -1 when someone else has it */
static int host_get(void)
{
    static int inuse_seen;
    u8 st = inb((u16)(smb + HSTSTS));
    u32 t0;
    /* a firmware that never gives the semaphore back is ignored after a while */
    if ((st & ST_INUSE) && ++inuse_seen < 50) return -1;
    inuse_seen = 0;
    t0 = plat_ms();
    while (inb((u16)(smb + HSTSTS)) & ST_BUSY)
        if (plat_ms() - t0 > 5) {
            outb((u16)(smb + HSTSTS), ST_INUSE);
            return -1;
        }
    outb((u16)(smb + HSTSTS), ST_CLEAR);
    return 0;
}

static void host_put(void)
{
    outb((u16)(smb + HSTSTS), (u8)(ST_CLEAR | ST_INUSE));
}

static int host_run(u8 cnt)
{
    u32 t0 = plat_ms();
    u8 st;
    outb((u16)(smb + HSTCNT), (u8)(cnt | CNT_START));
    for (;;) {
        st = inb((u16)(smb + HSTSTS));
        if (st & (ST_INTR | ST_DEVERR | ST_BUSERR | ST_FAILED)) break;
        if (plat_ms() - t0 > 30) {
            outb((u16)(smb + HSTCNT), 0x02);      /* kill */
            plat_udelay(100);
            outb((u16)(smb + HSTCNT), 0);
            return -1;
        }
    }
    return (st & (ST_DEVERR | ST_BUSERR | ST_FAILED)) ? -1 : 0;
}

/* SMBus block read / write with the 32-byte buffer; the count read, -1 */
static int smb_block(u8 a, u8 c, u8 *buf, int len, int rd)
{
    int i, n = len;
    if (host_get()) return -1;
    outb((u16)(smb + AUXCTL), (u8)((inb((u16)(smb + AUXCTL)) & ~1) | 2));      /* E32B, no PEC */
    outb((u16)(smb + HSTADD), (u8)((a << 1) | (rd ? 1 : 0)));
    outb((u16)(smb + HSTCMD), c);
    if (!rd) {
        outb((u16)(smb + HSTDAT0), (u8)len);
        (void)inb((u16)(smb + HSTCNT));            /* buffer index to 0 */
        for (i = 0; i < len; i++) outb((u16)(smb + BLKDAT), buf[i]);
    }
    if (host_run(CNT_BLOCK)) {
        n = -1;
    } else if (rd) {
        n = inb((u16)(smb + HSTDAT0));
        if (n < 1 || n > 32) {
            n = -1;
        } else {
            (void)inb((u16)(smb + HSTCNT));
            for (i = 0; i < n; i++) {
                u8 b = inb((u16)(smb + BLKDAT));
                if (i < len) buf[i] = b;
            }
        }
    }
    outb((u16)(smb + AUXCTL), (u8)(inb((u16)(smb + AUXCTL)) & ~2));
    host_put();
    return n;
}

static int smb_read_byte(u8 a, u8 c)
{
    int v = -1;
    if (host_get()) return -1;
    outb((u16)(smb + HSTADD), (u8)((a << 1) | 1));
    outb((u16)(smb + HSTCMD), c);
    if (!host_run(CNT_BDATA)) v = inb((u16)(smb + HSTDAT0));
    host_put();
    return v;
}

static int smb_send_byte(u8 a, u8 c)
{
    int r;
    if (host_get()) return -1;
    outb((u16)(smb + HSTADD), (u8)(a << 1));
    outb((u16)(smb + HSTCMD), c);
    r = host_run(CNT_BYTE);
    host_put();
    return r;
}

/* a Host Notify of our address since the last look */
static int notified(void)
{
    u8 st = inb((u16)(smb + SLVSTS)), na;
    if (!(st & 1)) return 0;
    na = inb((u16)(smb + NTFADD));
    outb((u16)(smb + SLVSTS), 1);
    return (na >> 1) == addr;
}

/* ---------------------------------------------------------------- RMI4 over SMBus */

static int rmi_cmd(u16 ra, int cnt, int wr)
{
    int i;
    u8 e[4];
    for (i = 0; i < 8; i++)
        if (map[i].addr == ra && (wr ? map[i].wr : (!map[i].wr && map[i].cnt == cnt))) return i;
    i = map_next;
    map_next = (map_next + 1) & 7;
    e[0] = (u8)ra;
    e[1] = (u8)(ra >> 8);
    e[2] = (u8)(wr ? 0 : cnt);
    e[3] = (u8)(wr ? 1 : 0);
    if (smb_block(addr, (u8)(0x80 + i), e, 4, 0) < 0) {
        map[i].addr = 0xFFFF;
        return -1;
    }
    map[i].addr = ra;
    map[i].cnt = (u8)cnt;
    map[i].wr = (u8)wr;
    return i;
}

static int rmi_read(u16 ra, u8 *buf, int len)
{
    while (len > 0) {
        int n = len > 32 ? 32 : len, c = rmi_cmd(ra, n, 0);
        if (c < 0 || smb_block(addr, (u8)c, buf, n, 1) != n) return -1;
        ra = (u16)(ra + n);
        buf += n;
        len -= n;
    }
    return 0;
}

static int rmi_write(u16 ra, const u8 *buf, int len)
{
    int c = rmi_cmd(ra, len, 1);
    u8 t[32];
    if (c < 0 || len > 32) return -1;
    memcpy(t, buf, (u32)len);
    return smb_block(addr, (u8)c, t, len, 0) < 0 ? -1 : 0;
}

/* RMI register descriptor (F12): the present registers' sizes and subpackets */
struct rdesc {
    int n;
    u8 reg[16];
    u16 size[16];
    u32 sub[16];
};

static int rmi_desc(u16 ra, struct rdesc *d)
{
    u8 ps, buf[35], st[96];
    int i, b, off = 0, po = 1, ss, map_off = 0, k;
    u8 present[256 / 8];
    memset(d, 0, sizeof(*d));
    memset(present, 0, sizeof(present));
    if (rmi_read(ra, &ps, 1) || ps < 1 || ps > 35) return -1;
    if (rmi_read((u16)(ra + 1), buf, ps)) return -1;
    if (buf[0] == 0) {
        po = 3;
        ss = buf[1] | (buf[2] << 8);
    } else {
        ss = buf[0];
    }
    if (ss > (int)sizeof(st)) ss = sizeof(st);
    for (i = po; i < ps; i++)
        for (b = 0; b < 8; b++, map_off++)
            if ((buf[i] & (1 << b)) && map_off < 256) present[map_off / 8] |= (u8)(1 << (map_off % 8));
    if (rmi_read((u16)(ra + 2), st, ss)) return -1;
    for (k = 0; k < 256 && d->n < 16; k++) {
        int rs;
        u32 sub = 0;
        if (!(present[k / 8] & (1 << (k % 8)))) continue;
        if (off >= ss) break;
        rs = st[off++];
        if (rs == 0 && off + 1 < ss) {
            rs = st[off] | (st[off + 1] << 8);
            off += 2;
            if (rs == 0 && off + 3 < ss) {
                rs = (int)(st[off] | (st[off + 1] << 8) | ((u32)st[off + 2] << 16));
                off += 4;
            }
        }
        map_off = 0;
        do {
            for (b = 0; b < 7; b++, map_off++)
                if ((st[off] & (1 << b)) && map_off < 32) sub |= 1UL << map_off;
        } while (off < ss && (st[off++] & 0x80));
        d->reg[d->n] = (u8)k;
        d->size[d->n] = (u16)rs;
        d->sub[d->n] = sub;
        d->n++;
    }
    return 0;
}

/* byte offset of register 'reg' in a block read of the registers (the sizes of the present
   registers before it), its index in *idx (registers are addressed by index); -1 absent */
static int desc_off(const struct rdesc *d, int reg, int *idx)
{
    int i, off = 0;
    for (i = 0; i < d->n; i++) {
        if (d->reg[i] == reg) {
            if (idx) *idx = i;
            return off;
        }
        off += d->size[i];
    }
    return -1;
}

static int bitcount(u32 v)
{
    int n = 0;
    while (v) {
        n += (int)(v & 1);
        v >>= 1;
    }
    return n;
}

static int f12_setup(u16 qb, u16 cb_, u16 db)
{
    struct rdesc q, c, d;
    u8 q0, buf[32];
    int i, o, off;
    if (rmi_read(qb, &q0, 1) || !(q0 & 1)) return -1;
    if (rmi_desc((u16)(qb + 1), &q) || rmi_desc((u16)(qb + 4), &c) || rmi_desc((u16)(qb + 7), &d)) return -1;
    rmax_x = rmax_y = 0;
    rum_x = rum_y = 0;
    /* control 8, sensor tuning: maxima, electrode pitch (1/4096 mm), receivers / transmitters */
    if (desc_off(&c, 8, &i) >= 0 && c.size[i] >= 4 && (c.sub[i] & 1) &&
        !rmi_read((u16)(cb_ + i), buf, c.size[i] > 32 ? 32 : c.size[i])) {
        u32 px = 0, py = 0;
        rmax_x = buf[0] | (buf[1] << 8);
        rmax_y = buf[2] | (buf[3] << 8);
        o = 4;
        if ((c.sub[i] & 2) && o + 4 <= c.size[i] && o + 4 <= 32) {
            px = buf[o] | (buf[o + 1] << 8);
            py = buf[o + 2] | (buf[o + 3] << 8);
            o += 4;
        }
        if ((c.sub[i] & 4)) o += 4;
        if ((c.sub[i] & 8) && o + 2 <= c.size[i] && o + 2 <= 32) {
            rum_x = px * buf[o] * 1000UL >> 12;
            rum_y = py * buf[o + 1] * 1000UL >> 12;
        }
    }
    off = desc_off(&d, 1, &i);
    if (off < 0) return -1;
    nfingers = bitcount(d.sub[i]);
    if (nfingers < 1) nfingers = 1;
    if (nfingers > 10) nfingers = 10;
    while (nfingers > 1 && off + nfingers * 8 > 96) nfingers--;
    sens_data = db;
    sens_len = (u16)(off + nfingers * 8);
    f11_fstate = (u16)off;                  /* F12: where data1 starts */
    is_f12 = 1;
    return rmax_x > 0 && rmax_y > 0 ? 0 : -1;
}

static int f11_setup(u16 qb, u16 cb_, u16 db)
{
    u8 q[6], c[4];
    if (rmi_read(qb, q, 6)) return -1;
    if (!(q[1] & 0x10)) return -1;          /* no absolute data */
    nfingers = (q[1] & 7) == 5 ? 10 : (q[1] & 7) + 1;
    if (rmi_read((u16)(cb_ + 6), c, 4)) return -1;
    rmax_x = c[0] | ((c[1] & 0x0F) << 8);
    rmax_y = c[2] | ((c[3] & 0x0F) << 8);
    f11_fstate = (u16)((nfingers + 3) / 4);
    sens_data = db;
    sens_len = (u16)(f11_fstate + 5 * nfingers);
    is_f12 = 0;
    return rmax_x > 0 && rmax_y > 0 ? 0 : -1;
}

static int rmi_setup(void)
{
    int page, sensor = 0, i;
    u8 e[6], ctl, en[4];
    int v = smb_read_byte(addr, 0xFD);
    if (v < 1 || v > 2) {
        DBG("smbtp: RMI SMBus version byte %d\n", v);
        return -1;
    }
    for (i = 0; i < 8; i++) map[i].addr = 0xFFFF;
    map_next = 0;
    nirq = 0;
    f01_ctl = 0;
    sensor_irq = button_irq = f01_irq = 0;
    btn_len = 0;
    for (page = 0; page < 4; page++) {
        u16 base = (u16)(page << 8);
        int found = 0, a;
        for (a = 0xE9; a >= 0x05; a -= 6) {
            u32 bits;
            int cnt;
            if (rmi_read((u16)(base + a), e, 6)) return -1;
            if (e[5] == 0 || e[5] == 0xFF) break;
            found = 1;
            cnt = e[4] & 7;
            bits = cnt ? ((1UL << cnt) - 1) << nirq : 0;
            switch (e[5]) {
            case 0x01:
                f01_ctl = (u16)(base + e[2]);
                f01_data = (u16)(base + e[3]);
                f01_irq = bits;
                break;
            case 0x11:
                if (!sensor && !f11_setup((u16)(base + e[0]), (u16)(base + e[2]), (u16)(base + e[3]))) {
                    sensor = 0x11;
                    sensor_irq = bits;
                }
                break;
            case 0x12:
                if (!sensor && !f12_setup((u16)(base + e[0]), (u16)(base + e[2]), (u16)(base + e[3]))) {
                    sensor = 0x12;
                    sensor_irq = bits;
                }
                break;
            case 0x30:
            case 0x3A:
                if (!btn_len) {
                    u8 q[2];
                    int nb;
                    if (rmi_read((u16)(base + e[0]), q, 2)) break;
                    nb = e[5] == 0x30 ? (q[1] & 0x1F) : (q[0] & 0x7F);
                    if (nb > 16) nb = 16;
                    if (nb < 1) break;
                    btn_data = (u16)(base + e[3]);
                    btn_len = (u8)((nb + 7) / 8);
                    button_irq = bits;
                }
                break;
            }
            nirq += cnt;
        }
        if (!found) break;
    }
    if (!f01_ctl || !sensor) {
        kprintf("smbtp: RMI4 pad without %s\n", f01_ctl ? "a 2D sensor" : "F01");
        return -1;
    }
    irq_bytes = (u8)((nirq + 7) / 8);
    if (irq_bytes < 1) irq_bytes = 1;
    if (irq_bytes > 4) irq_bytes = 4;
    /* configured, normal operation; the interrupts of our functions enabled */
    if (rmi_read(f01_ctl, &ctl, 1)) return -1;
    ctl = (u8)((ctl & ~3) | 0x80 | 0x04);
    if (rmi_write(f01_ctl, &ctl, 1)) return -1;
    {
        u32 m = sensor_irq | button_irq | f01_irq;
        for (i = 0; i < irq_bytes; i++) en[i] = (u8)(m >> (8 * i));
        if (rmi_write((u16)(f01_ctl + 1), en, irq_bytes)) return -1;
    }
    /* buttons that read pressed now are no buttons (unused GPIOs) */
    btn_mask[0] = btn_mask[1] = 0;
    if (btn_len) {
        u8 b[2] = { 0xFF, 0xFF };
        if (!rmi_read(btn_data, b, btn_len)) {
            btn_mask[0] = (u8)(b[0] & 7);
            btn_mask[1] = 0;
        }
    }
    (void)rmi_read((u16)(f01_data + 1), en, irq_bytes);         /* clears the pending ones */
    kprintf("smbtp: Synaptics RMI4 over SMBus v%d, F%02x, %d fingers, 0-%ld x 0-%ld, %d interrupt(s)%s\n", v + 1,
            sensor, nfingers, rmax_x, rmax_y, nirq, btn_len ? ", buttons" : "");
    return 0;
}

static int rmi_buttons(void)
{
    u8 b[2];
    int r = 0;
    if (!btn_len || rmi_read(btn_data, b, btn_len)) return 0;
    /* active low */
    if ((btn_mask[0] & 1) && !(b[0] & 1)) r |= 1;
    if ((btn_mask[0] & 2) && !(b[0] & 2)) r |= 2;
    if ((btn_mask[0] & 4) && !(b[0] & 4)) r |= 4;
    return r;
}

static int rmi_poll(void)
{
    u8 st[4], d[96];
    struct ptp_contact c[10];
    int n = 0, i;
    static int btn;
    u32 s;
    if (rmi_read((u16)(f01_data + 1), st, irq_bytes)) return -1;
    if (!pad) return -1;
    s = st[0] | ((u32)(irq_bytes > 1 ? st[1] : 0) << 8) | ((u32)(irq_bytes > 2 ? st[2] : 0) << 16);
    if (s & f01_irq) {
        u8 ds;
        if (!rmi_read(f01_data, &ds, 1) && (ds & 0x80)) return -1;     /* reset: unconfigured */
    }
    if (s & button_irq) btn = rmi_buttons();
    if (!(s & (sensor_irq | button_irq))) return 0;
    if (sens_len > sizeof(d) || rmi_read(sens_data, d, sens_len)) return -1;
    for (i = 0; i < nfingers; i++) {
        s32 x, y;
        if (is_f12) {
            const u8 *o = d + f11_fstate + i * 8;
            if (o[0] != 1 && o[0] != 4 && o[0] != 6) continue;       /* finger, unclassified, glove */
            x = o[1] | (o[2] << 8);
            y = o[3] | (o[4] << 8);
        } else {
            const u8 *o = d + f11_fstate + i * 5;
            int stt = (d[i / 4] >> (2 * (i % 4))) & 3;
            if (stt != 1 && stt != 2) continue;
            x = ((s32)o[0] << 4) | (o[2] & 0x0F);
            y = ((s32)o[1] << 4) | (o[2] >> 4);
        }
        c[n].id = (u8)i;
        c[n].x = x;
        c[n].y = y;
        n++;
    }
    ptp_contacts(pad, c, n, btn);
    return 0;
}

/* ---------------------------------------------------------------- ELAN over SMBus */

static s32 elan_res(u8 v)
{
    return ((s32)(s8)v * 10 + 790) * 10 / 254;
}

static int elan_setup(s32 *xres, s32 *yres)
{
    u8 b[32], m[4];
    int i;
    if (smb_block(addr, 0xA7, b, 32, 1) != 5) return -1;
    for (i = 0; i < 5; i++)
        if (b[i] != 0x55) return -1;
    if (smb_send_byte(addr, 0x20)) return -1;                   /* enable */
    m[0] = 0x00; m[1] = 0x07; m[2] = 0x00; m[3] = 0x01;         /* absolute mode */
    if (smb_block(addr, 0x00, m, 4, 0) < 0) return -1;
    if (smb_block(addr, 0xA0, b, 32, 1) < 3) return -1;
    emax_x = ((b[0] & 0x0F) << 8) | b[1];
    emax_y = ((b[0] & 0xF0) << 4) | b[2];
    *xres = *yres = 0;
    if (smb_block(addr, 0xA4, b, 32, 1) >= 2) {
        *xres = elan_res((u8)(b[1] & 0x0F));
        *yres = elan_res((u8)(b[1] >> 4));
    }
    kprintf("smbtp: ELAN over SMBus, 0-%ld x 0-%ld, %ld x %ld units/mm\n", emax_x, emax_y, *xres, *yres);
    return emax_x > 0 && emax_y > 0 ? 0 : -1;
}

static int elan_poll(void)
{
    u8 r[32];
    struct ptp_contact c[5];
    const u8 *f;
    int i, n = 0, len = smb_block(addr, 0xA8, r, 32, 1);
    if (len < 0) return -1;
    if (len < 3 || r[0] != 0x5D) return 0;
    f = r + 2;
    for (i = 0; i < 5 && f + 3 <= r + len; i++) {
        if (!(r[1] & (1 << (3 + i)))) continue;
        c[n].id = (u8)i;
        c[n].x = ((f[0] & 0xF0) << 4) | f[1];
        c[n].y = emax_y - (((f[0] & 0x0F) << 8) | f[2]);
        n++;
        f += 5;
    }
    ptp_contacts(pad, c, n, (r[1] & 1) | (r[1] & 2) | (r[1] & 4));
    return 0;
}

/* ---------------------------------------------------------------- interface */

int smbtp_start(int k, u8 a, void (*out)(int dx, int dy, int dz, int dw, int buttons))
{
    s32 xres = 0, yres = 0;
    int r;
    smbtp_stop();
    if (!smb && host_find()) {
        DBG("smbtp: no SMBus host\n");
        return -1;
    }
    kind = (u8)k;
    addr = a;
    errors = 0;
    nfy_seen = 0;
    (void)notified();
    if (k == T_SYN) {
        r = rmi_setup();
        if (!r) pad = ptp_new_abs(0, rmax_x, 0, rmax_y, rum_x, rum_y, out);
    } else {
        r = elan_setup(&xres, &yres);
        if (!r) pad = ptp_new_abs(0, emax_x, 0, emax_y, xres ? (u32)(emax_x * 1000L / xres) : 0,
                                  yres ? (u32)(emax_y * 1000L / yres) : 0, out);
    }
    if (r || !pad) {
        kprintf("smbtp: %s pad at %02x does not answer on SMBus, stays on PS/2\n", k == T_SYN ? "Synaptics" : "ELAN",
                a);
        kind = 0;
        return -1;
    }
    last_ms = nfy_ms = plat_ms();
    return 0;
}

void smbtp_stop(void)
{
    if (pad) ptp_free(pad);
    pad = NULL;
    kind = 0;
}

int smbtp_active(void)
{
    return kind != 0;
}

/* every 10 ms or so (core poll); ELAN at its Host Notify once one was seen */
void smbtp_poll(void)
{
    u32 now;
    int r;
    if (!kind) return;
    now = plat_ms();
    if (notified()) {
        nfy_seen = 1;
        nfy_ms = now;
    } else if (kind == T_ELAN && nfy_seen && now - nfy_ms < 100) {
        return;
    } else if (now - last_ms < 8) {
        return;
    }
    last_ms = now;
    r = kind == T_SYN ? rmi_poll() : elan_poll();
    if (r == 0) {
        errors = 0;
        return;
    }
    if (++errors < 20) return;
    /* the pad went back to PS/2 (a reset): one try to bring it back, then give up */
    if (kind == T_SYN && !rmi_setup()) {
        errors = 0;
        return;
    }
    kprintf("smbtp: the pad stopped answering on SMBus\n");
    if (pad) ptp_free(pad);
    pad = NULL;
    kind = 0;
}
