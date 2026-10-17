/* gpio.c - Intel GPIO pads (Sunrise Point to Panther Lake, Bay Trail, Cherry Trail) for the
   codec drivers: the speaker amplifier enables and similar lines the ACPI tables give as
   GpioIo resources. The ACPI GPIO number is translated to a pad with the pinctrl tables of
   Linux (gpiotab.h); the registers of a community come from the controller's _CRS or, when
   the BIOS fills those in at run time, from the sideband (P2SB) base and the port id of the
   community. Bay Trail (INT33FC, one device per community: SCORE, NCORE, SUS) and Cherry
   Trail (INT33FF: SW, N, E, SE) have their own pad layouts (pinctrl-baytrail.c,
   pinctrl-cherryview.c). AMD FCH GPIO (AMD0030, AMDI0030): one register per pin, pin status
   in bit 16 (pinctrl-amd.c). */
#include "i2cbus.h"

struct gpgrp { u16 base, size; s16 gbase; };
struct gpcom { u16 pin_base, npins; u8 barno, comid; u16 g0; u8 ng; };
struct gpsoc { const char *hid; u8 uid, c0, nc, fam; };

#define GF_SPT  1
#define GF_BXT  2
#define GF_GLK  3
#define GF_CNL  4
#define GF_MTL  5

#include "gpiotab.h"

#define REVID       0x000
#define PADBAR      0x00C
#define PADCFG0_TXSTATE  0x01
#define PADCFG0_RXSTATE  0x02
#define PADCFG0_TXDIS    0x100
#define PADCFG0_RXDIS    0x200
#define PADCFG0_PMODE    (7UL << 10)

#define MAXCTL 4
static struct gpctl {
    char path[32];
    char hid[9];
    u32 uid;
    const struct gpsoc *soc;
    u32 mem[8];
    int nmem;
    u8 *regs[8];
    u8 atom;                        /* 1 Bay Trail, 2 Cherry Trail, 3 AMD */
} ctls[MAXCTL];

/* Bay Trail: ACPI GPIO number -> pad of the SCORE (_UID 1), NCORE (2), SUS (3) community */
static const u8 byt_score[102] = {
    85, 89, 93, 96, 99, 102, 98, 101, 34, 37, 36, 38, 39, 35, 40, 84, 62, 61, 64, 59,
    54, 56, 60, 55, 63, 57, 51, 50, 53, 47, 52, 49, 48, 43, 46, 41, 45, 42, 58, 44,
    95, 105, 70, 68, 67, 66, 69, 71, 65, 72, 86, 90, 88, 92, 103, 77, 79, 83, 78, 81,
    80, 82, 13, 12, 15, 14, 17, 18, 19, 16, 2, 1, 0, 4, 6, 7, 9, 8, 33, 32,
    31, 30, 29, 27, 25, 28, 26, 23, 21, 20, 24, 22, 5, 3, 10, 11, 106, 87, 91, 104,
    97, 100
};
static const u8 byt_ncore[28] = {
    19, 18, 17, 20, 21, 22, 24, 25, 23, 16, 14, 15, 12, 26, 27, 1, 4, 8, 11, 0,
    3, 6, 10, 13, 2, 5, 9, 7
};
static const u8 byt_sus[44] = {
    29, 33, 30, 31, 32, 34, 36, 35, 38, 37, 18, 7, 11, 20, 17, 1, 8, 10, 19, 12,
    0, 2, 23, 39, 28, 27, 22, 21, 24, 25, 26, 51, 56, 54, 49, 55, 48, 57, 50, 58,
    52, 53, 59, 40
};
static int nctl;

/* the sideband register base (P2SB BAR), 0 when it cannot be found */
static u32 sbreg(void)
{
    static u32 base = 1;
    static const u8 devs[2][2] = { { 31, 1 }, { 13, 0 } };    /* PCH, Apollo / Gemini Lake */
    int i;
    if (base != 1) return base;
    base = 0;
    for (i = 0; i < 2 && !base; i++) {
        int dv = devs[i][0], fn = devs[i][1];
        u8 hide = 0;
        u32 lo, hi = 0;
        if ((pci_rd32(0, 31, 0, 0) & 0xFFFF) != 0x8086) break;
        if ((pci_rd32(0, dv, fn, 0) & 0xFFFF) == 0xFFFF) {
            hide = 1;
            pci_wr8(0, dv, fn, 0xE1, 0);
        }
        if ((pci_rd32(0, dv, fn, 0) & 0xFFFF) == 0x8086) {
            lo = pci_rd32(0, dv, fn, 0x10);
            if ((lo & 6) == 4) hi = pci_rd32(0, dv, fn, 0x14);
            if (!hi && (lo & ~0xFUL)) base = lo & ~0xFUL;
        }
        if (hide) pci_wr8(0, dv, fn, 0xE1, 1);
    }
    if (!base && !acpi_name_int("SBRG", &base) && (base & 0xFFFF)) base = 0;
    DBG("gpio: sideband base %08lx\n", base);
    return base;
}

/* port id of a community on the sideband */
static int port_id(const struct gpctl *c, const struct gpcom *m)
{
    switch (c->soc->fam) {
    case GF_SPT: return 0xAF - m->comid;
    case GF_CNL: return 0x6E - m->comid;
    case GF_BXT: {
        static const u8 apl[4] = { 0xC0, 0xC4, 0xC7, 0xC5 };       /* N, NW, W, SW */
        return c->uid >= 1 && c->uid <= 4 ? apl[c->uid - 1] : -1;
    }
    case GF_GLK: {
        static const u8 glk[4] = { 0xC4, 0xC5, 0xC9, 0xC8 };       /* NW, N, AUDIO, SCC */
        return c->uid >= 1 && c->uid <= 4 ? glk[c->uid - 1] : -1;
    }
    case GF_MTL:
        if (strncmp(c->hid, "INTC1083", 9) && strncmp(c->hid, "INTC105E", 9)) return -1;
        return m->comid == 0 ? 0xD1 : m->comid == 1 ? 0xD2 : 0xD0 + m->comid;
    }
    return -1;
}

static struct gpctl *ctl_get(const char *path)
{
    struct gpctl *c;
    int i;
    for (i = 0; i < nctl; i++)
        if (!strncmp(ctls[i].path, path, sizeof(ctls[i].path))) return &ctls[i];
    if (nctl >= MAXCTL) return NULL;
    c = &ctls[nctl];
    memset(c, 0, sizeof(*c));
    strlcpy(c->path, path, sizeof(c->path));
    c->nmem = acpi_dev_info(path, c->hid, &c->uid, c->mem, 8);
    if (c->nmem < 0) {
        kprintf("gpio: controller %s not in the ACPI tables\n", path);
        return NULL;
    }
    if (!strncmp(c->hid, "AMD0030", 9) || !strncmp(c->hid, "AMDI0030", 9) || !strncmp(c->hid, "AMDI0031", 9)) {
        c->atom = 3;
        if (c->nmem < 1 || !c->mem[0]) {
            u32 len[8];
            c->nmem = acpi_dev_mem(path, c->mem, len, 8);
        }
        if (c->nmem < 1 || !c->mem[0] || !(c->regs[0] = plat_mmio(c->mem[0], 0x400))) {
            kprintf("gpio: %s: no register address\n", path);
            return NULL;
        }
        nctl++;
        return c;
    }
    if (!strncmp(c->hid, "INT33FC", 9) || !strncmp(c->hid, "INT33FF", 9)) {
        u32 len[8];
        c->atom = (u8)(c->hid[6] == 'C' ? 1 : 2);
        c->nmem = acpi_dev_mem(path, c->mem, len, 8);      /* bases the BIOS sets at run time */
        if (c->nmem < 1 || !c->mem[0] || !(c->regs[0] = plat_mmio(c->mem[0], c->atom == 1 ? 0x1000 : 0x8000))) {
            kprintf("gpio: %s: no register address\n", path);
            return NULL;
        }
        nctl++;
        return c;
    }
    for (i = 0; gpsocs[i].hid; i++)
        if (!strncmp(gpsocs[i].hid, c->hid, 9) && (!gpsocs[i].uid || gpsocs[i].uid == c->uid)) {
            c->soc = &gpsocs[i];
            break;
        }
    if (!c->soc && strncmp(c->hid, "INTC105F", 9)) {
        kprintf("gpio: controller %s (%s) not known\n", path, c->hid);
        return NULL;
    }
    nctl++;
    return c;
}

/* registers of community 'k' of the controller (barno 'bar') */
static u8 *comm_regs(struct gpctl *c, const struct gpcom *m, int bar)
{
    u32 phys;
    if (bar >= 8) return NULL;
    if (c->regs[bar]) return c->regs[bar];
    phys = bar < c->nmem ? c->mem[bar] : 0;
    if (!phys && m) {
        int pid = port_id(c, m);
        u32 sb = sbreg();
        if (pid >= 0 && sb) phys = sb + ((u32)pid << 16);
    }
    if (!phys) {
        kprintf("gpio: %s: no register address for community %d\n", c->path, bar);
        return NULL;
    }
    c->regs[bar] = plat_mmio(phys, 0x10000);
    DBG("gpio: %s community %d at %08lx\n", c->path, bar, phys);
    return c->regs[bar];
}

/* PADCFG0 of the pad behind an ACPI GPIO number */
static volatile u32 *pad_reg(const char *path, u16 pin)
{
    struct gpctl *c = ctl_get(path);
    const struct gpcom *m = NULL;
    u8 *regs;
    u32 padno = 0, stride;
    int i, k, bar = 0;
    if (!c) return NULL;
    if (!c->soc) {
        padno = pin;                    /* INTC105F: one community, pads numbered as GPIOs */
    } else {
        for (i = 0; i < c->soc->nc && !m; i++) {
            const struct gpcom *cm = &gpcoms[c->soc->c0 + i];
            for (k = 0; k < cm->ng; k++) {
                const struct gpgrp *g = &gpgrps[cm->g0 + k];
                if (g->gbase < 0 || pin < (u16)g->gbase || pin >= (u16)g->gbase + g->size) continue;
                m = cm;
                bar = cm->barno;
                padno = g->base + (pin - (u16)g->gbase) - cm->pin_base;
                break;
            }
        }
        if (!m) {
            kprintf("gpio: %s: pin %u not mapped\n", path, pin);
            return NULL;
        }
    }
    regs = comm_regs(c, m, bar);
    if (!regs) return NULL;
    stride = ((RD32(regs + REVID) >> 16) & 0xFFFF) >= 0x94 ? 16 : 8;
    return (volatile u32 *)(regs + (RD32(regs + PADBAR) & 0xFFFF) + padno * stride);
}

/* Bay Trail / Cherry Trail: the pad registers (BYT: CONF0, CONF1, VAL; CHT: PADCTRL0 / 1) */
static volatile u32 *atom_pad(struct gpctl *c, u16 pin)
{
    const u8 *map = c->uid == 1 ? byt_score : c->uid == 2 ? byt_ncore : byt_sus;
    u32 n = c->uid == 1 ? 102 : c->uid == 2 ? 28 : 44;
    if (c->atom == 2) return (volatile u32 *)(c->regs[0] + 0x4400 + 0x400 * (pin / 15) + 8 * (pin % 15));
    if (pin >= n) return NULL;
    return (volatile u32 *)(c->regs[0] + map[pin] * 16);
}

static int atom_set(struct gpctl *c, const char *path, u16 pin, int val)
{
    volatile u32 *r;
    u32 v, mux;
    if (c->atom == 3) {
        if (pin >= 256) return -1;
        r = (volatile u32 *)(c->regs[0] + pin * 4UL);
        v = *r | 0x00800000UL;                  /* output enable */
        *r = val ? v | 0x00400000UL : v & ~0x00400000UL;
        return 0;
    }
    r = atom_pad(c, pin);
    if (!r) {
        kprintf("gpio: %s: pin %u not mapped\n", path, pin);
        return -1;
    }
    if (c->atom == 1) {
        /* pin function: GPIO is mux 0, mux 1 for SCORE 92-93 and SUS 11-21 */
        mux = (c->uid == 1 && pin >= 92 && pin <= 93) || (c->uid == 3 && pin >= 11 && pin <= 21) ? 1 : 0;
        if ((r[0] & 7) != mux) {
            DBG("gpio: %s pin %u switched to GPIO (CONF0 %08lx)\n", path, pin, r[0]);
            r[0] = (r[0] & ~7UL) | mux;
        }
        v = r[2] & ~6UL;                        /* output and input buffers enabled */
        r[2] = val ? v | 1 : v & ~1UL;
        DBG("gpio: %s pin %u = %d (VAL %08lx)\n", path, pin, val, r[2]);
        return 0;
    }
    if ((r[1] & 0x80000000UL) && !(r[0] & 0x8000)) {
        kprintf("gpio: %s pin %u is locked in its native function\n", path, pin);
        return -1;
    }
    v = (r[0] & ~(0x700UL | 2)) | 0x8000 | 0x100;   /* GPIO enable, output only */
    r[0] = val ? v | 2 : v;
    DBG("gpio: %s pin %u = %d (PADCTRL0 %08lx)\n", path, pin, val, r[0]);
    return 0;
}

int gpio_set(const char *path, u16 pin, int val)
{
    volatile u32 *r;
    struct gpctl *c = ctl_get(path);
    u32 v;
    if (c && c->atom) return atom_set(c, path, pin, val);
    r = pad_reg(path, pin);
    if (!r) return -1;
    v = *r;
    if (v & PADCFG0_PMODE) {
        kprintf("gpio: %s pin %u is not in GPIO mode (%08lx)\n", path, pin, v);
        return -1;
    }
    v = (v & ~(PADCFG0_TXDIS | PADCFG0_TXSTATE)) | (val ? PADCFG0_TXSTATE : 0);
    *r = v;
    DBG("gpio: %s pin %u = %d (%08lx)\n", path, pin, val, *r);
    return 0;
}

int gpio_get(const char *path, u16 pin)
{
    volatile u32 *r;
    struct gpctl *c = ctl_get(path);
    u32 v;
    if (c && c->atom == 3) return pin < 256 ? (int)((RD32(c->regs[0] + pin * 4UL) >> 16) & 1) : -1;
    if (c && c->atom) {
        if ((r = atom_pad(c, pin)) == NULL) return -1;
        if (c->atom == 1) return (int)(r[2] & 1);
        return (r[0] & 0x700) == 0x100 ? (int)((r[0] >> 1) & 1) : (int)(r[0] & 1);
    }
    r = pad_reg(path, pin);
    if (!r) return -1;
    v = *r;
    if (v & PADCFG0_RXDIS) return (v & PADCFG0_TXSTATE) ? 1 : 0;
    return (v & PADCFG0_RXSTATE) ? 1 : 0;
}
