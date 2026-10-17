/* acpi.c - just enough ACPI for I2C HID devices: the DSDT and SSDTs are searched (no AML
   interpreter) for I2cSerialBus resource descriptors, whether a "PNP0C50" (HID over I2C)
   identifier comes shortly before them, and for the FMCN / SSCN timing packages of the
   I2C controllers. */
#ifndef CORE_H
#include "core.h"
#endif
#include "acpi.h"

static int sum8(const u8 *p, u32 n)
{
    u8 s = 0;
    while (n--) s = (u8)(s + *p++);
    return s;
}

/* the RSDP in the EBDA or in E0000-FFFFF */
static u32 find_rsdp(void)
{
    u8 *bios = plat_mmio(0xE0000UL, 0x20000UL), *ebda;
    u16 *ebseg = plat_mmio(0x40EUL, 2);
    u32 i, eb = ebseg ? (u32)*ebseg << 4 : 0;
    if (eb >= 0x80000UL && eb < 0xA0000UL && (ebda = plat_mmio(eb, 1024)) != NULL)
        for (i = 0; i < 1024; i += 16)
            if (!memcmp(ebda + i, "RSD PTR ", 8) && !sum8(ebda + i, 20)) return eb + i;
    if (bios)
        for (i = 0; i < 0x20000UL; i += 16)
            if (!memcmp(bios + i, "RSD PTR ", 8) && !sum8(bios + i, 20)) return 0xE0000UL + i;
    return 0;
}

/* a whole table (header first to learn its length) */
static u8 *map_table(u32 phys)
{
    u8 *h = plat_mmio(phys, 36);
    u32 len;
    if (!h) return NULL;
    len = *(u32 *)(h + 4);
    if (len < 36 || len > 0x400000UL) return NULL;
    return plat_mmio(phys, len);
}

/* ---------------------------------------------------------------- AML scan */

static const u8 pnp0c50[5] = { 0x0C, 0x41, 0xD0, 0x0C, 0x50 };    /* DWordPrefix EISAID("PNP0C50") */

static int near_hid(const u8 *aml, u32 pos)
{
    u32 from = pos > 2048 ? pos - 2048 : 0, i;
    for (i = pos; i-- > from; ) {
        if (!memcmp(aml + i, pnp0c50, 5) || !memcmp(aml + i, "PNP0C50", 7) || !memcmp(aml + i, "ACPI0C50", 8))
            return 1;
        if (aml[i] == 0x5B && aml[i + 1] == 0x82) return 0;  /* DeviceOp: start of this device */
    }
    return 0;
}

int acpi_scan_aml(const u8 *aml, u32 len, struct acpi_i2c *out, int n, int max)
{
    u32 i;
    for (i = 0; i + 20 < len && n < max; i++) {
        u32 dl, speed, k;
        u16 addr, tlen;
        const u8 *d = aml + i;
        struct acpi_i2c *e;
        if (d[0] != 0x8E || d[5] != 1) continue;        /* serial bus descriptor, I2C */
        dl = d[1] | (d[2] << 8);
        tlen = (u16)(d[10] | (d[11] << 8));
        if (dl < 15 || dl > 200 || tlen < 6 || i + 3 + dl > len || 12 + tlen > 3 + dl) continue;
        speed = d[12] | ((u32)d[13] << 8) | ((u32)d[14] << 16) | ((u32)d[15] << 24);
        addr = (u16)(d[16] | (d[17] << 8));
        if (speed < 10000UL || speed > 3400000UL || (d[7] & 1) || addr < 0x08 || addr > 0x77) continue;
        for (k = 0; k < (u32)n && (out[k].addr != addr || strncmp(out[k].path, (const char *)d + 12 + tlen, 31)); k++) ;
        if (k < (u32)n) {
            if (near_hid(aml, i)) out[k].hid = 1;
            continue;
        }
        e = &out[n++];
        memset(e, 0, sizeof(*e));
        e->addr = addr;
        e->speed = speed;
        e->hid = (u8)near_hid(aml, i);
        strlcpy(e->path, (const char *)d + 12 + tlen, sizeof(e->path));
        i += 2 + dl;
    }
    return n;
}

/* Name (FMCN, Package () { HCNT, LCNT, SDA hold }) */
static int scan_name(const u8 *aml, u32 len, const char *name, u16 *v)
{
    u32 i;
    for (i = 0; i + 12 < len; i++) {
        const u8 *p = aml + i;
        int k, n;
        u32 pl;
        if (p[0] != 0x08 || memcmp(p + 1, name, 4) || p[5] != 0x12) continue;
        p += 6;
        pl = (p[0] >> 6) + 1;                           /* PkgLength bytes */
        p += pl;
        n = *p++;
        if (n < 2) continue;
        for (k = 0; k < 3 && k < n; k++) {
            if (*p == 0x0A) { v[k] = p[1]; p += 2; }
            else if (*p == 0x0B) { v[k] = (u16)(p[1] | (p[2] << 8)); p += 3; }
            else if (*p == 0x0C) { v[k] = (u16)(p[1] | (p[2] << 8)); p += 5; }
            else if (*p == 0x00) { v[k] = 0; p++; }
            else if (*p == 0x01) { v[k] = 1; p++; }
            else break;
        }
        if (k >= 2) return 0;
    }
    return -1;
}

/* the RSDT / XSDT, mapped once */
static u8 *root_tab;
static int root_xsdt;

static u8 *acpi_root(void)
{
    u32 rsdp, root;
    u8 *r;
    if (root_tab) return root_tab;
    rsdp = find_rsdp();
    if (!rsdp) return NULL;
    r = plat_mmio(rsdp, 36);
    if (!r) return NULL;
    root_xsdt = r[15] >= 2 && *(u32 *)(r + 24) && !*(u32 *)(r + 28);
    root = root_xsdt ? *(u32 *)(r + 24) : *(u32 *)(r + 16);
    root_tab = map_table(root);
    DBG("acpi: RSDP at %05lx, %s\n", rsdp, root_xsdt ? "XSDT" : "RSDT");
    return root_tab;
}

/* the idx-th table with signature 'sig' below 4G (mapped), NULL if none */
void *acpi_table(const char *sig, int idx)
{
    u8 *t = acpi_root();
    u32 i, entries, esz;
    if (!t) return NULL;
    esz = root_xsdt ? 8 : 4;
    entries = (*(u32 *)(t + 4) - 36) / esz;
    for (i = 0; i < entries; i++) {
        u32 phys = *(u32 *)(t + 36 + i * esz);
        u8 *h;
        if (root_xsdt && *(u32 *)(t + 40 + i * esz)) continue;
        h = plat_mmio(phys, 36);
        if (!h || memcmp(h, sig, 4)) continue;
        if (idx-- > 0) continue;
        return map_table(phys);
    }
    return NULL;
}

/* every I2C device of the DSDT and the SSDTs; FMCN / SSCN when found (else 0) */
int acpi_i2c_devices(struct acpi_i2c *out, int max, u16 *fmcn, u16 *sscn)
{
    u32 rsdp = find_rsdp(), root, entries, i, esz;
    u8 *r, *t;
    int n = 0, xsdt;
    if (!rsdp) {
        kprintf("acpi: no RSDP\n");
        return 0;
    }
    r = plat_mmio(rsdp, 36);
    xsdt = r[15] >= 2 && *(u32 *)(r + 24) && !*(u32 *)(r + 28);
    root = xsdt ? *(u32 *)(r + 24) : *(u32 *)(r + 16);
    t = map_table(root);
    if (!t) return 0;
    esz = xsdt ? 8 : 4;
    entries = (*(u32 *)(t + 4) - 36) / esz;
    DBG("acpi: RSDP at %05lx, %s with %lu tables\n", rsdp, xsdt ? "XSDT" : "RSDT", entries);
    for (i = 0; i < entries; i++) {
        u32 phys = *(u32 *)(t + 36 + i * esz);
        u8 *h, *aml;
        if (xsdt && *(u32 *)(t + 40 + i * esz)) continue;  /* above 4 GB */
        h = plat_mmio(phys, 36);
        if (!h) continue;
        if (!memcmp(h, "FACP", 4)) {
            u8 *f = map_table(phys);
            u32 dsdt = f ? *(u32 *)(f + 40) : 0;
            if (f && *(u32 *)(f + 4) >= 148 && *(u32 *)(f + 140) && !*(u32 *)(f + 144)) dsdt = *(u32 *)(f + 140);
            if (!dsdt || !(aml = map_table(dsdt))) continue;
        } else if (!memcmp(h, "SSDT", 4)) {
            if (!(aml = map_table(phys))) continue;
        } else {
            continue;
        }
        {
            char sig[5];
            memcpy(sig, aml, 4);
            sig[4] = 0;
            DBG("acpi: %s, %lu bytes\n", sig, *(u32 *)(aml + 4));
        }
        n = acpi_scan_aml(aml + 36, *(u32 *)(aml + 4) - 36, out, n, max);
        if (fmcn && !fmcn[0]) scan_name(aml + 36, *(u32 *)(aml + 4) - 36, "FMCN", fmcn);
        if (sscn && !sscn[0]) scan_name(aml + 36, *(u32 *)(aml + 4) - 36, "SSCN", sscn);
    }
    for (i = 0; i < (u32)n; i++)
        DBG("acpi: I2C device %02x on %s, %lu Hz%s\n", out[i].addr, out[i].path, out[i].speed,
            out[i].hid ? ", HID" : "");
    return n;
}

/* ---------------------------------------------------------------- devices by _HID
   No AML interpreter: every Device () of the DSDT and the SSDTs is found by its opcode,
   its _HID / _CID string or EISA id read when it is a Name, and the resource descriptors
   (I2cSerialBus, GpioIo / GpioInt) collected from its body up to the first child device.
   _STA is not evaluated: the drivers check that the chip answers. */

/* PkgLength: total length (from its first byte), *n = encoding bytes */
static u32 pkglen(const u8 *p, int *n)
{
    int k = p[0] >> 6, i;
    u32 len;
    if (!k) {
        *n = 1;
        return p[0] & 0x3F;
    }
    len = p[0] & 0x0F;
    for (i = 0; i < k; i++) len |= (u32)p[1 + i] << (4 + 8 * i);
    *n = k + 1;
    return len;
}

/* the last name segment of a NameString; its length in *n */
static const u8 *namestr(const u8 *p, int *n, char *seg)
{
    const u8 *s = p;
    while (*p == '\\' || *p == '^') p++;
    if (*p == 0x2E) {
        memcpy(seg, p + 5, 4);
        p += 9;
    } else if (*p == 0x2F) {
        int c = p[1];
        memcpy(seg, p + 2 + (c - 1) * 4, 4);
        p += 2 + c * 4;
    } else if (*p == 0) {
        memset(seg, 0, 4);
        p++;
    } else {
        memcpy(seg, p, 4);
        p += 4;
    }
    seg[4] = 0;
    *n = (int)(p - s);
    return p;
}

static void eisaid(u32 v, char *out)
{
    /* compressed EISA id (big endian bytes in the AML dword) */
    u8 b0 = (u8)v, b1 = (u8)(v >> 8), b2 = (u8)(v >> 16), b3 = (u8)(v >> 24);
    static const char hx[] = "0123456789ABCDEF";
    out[0] = (char)('@' + ((b0 >> 2) & 0x1F));
    out[1] = (char)('@' + (((b0 & 3) << 3) | (b1 >> 5)));
    out[2] = (char)('@' + (b1 & 0x1F));
    out[3] = hx[b2 >> 4];
    out[4] = hx[b2 & 15];
    out[5] = hx[b3 >> 4];
    out[6] = hx[b3 & 15];
    out[7] = 0;
}

/* Name (xxxx, value): the string or EISA id */
static int name_id(const u8 *p, const u8 *end, char *out)
{
    if (p < end && *p == 0x0D) {
        int i;
        for (i = 0; i < 8 && p + 1 + i < end && p[1 + i]; i++) out[i] = (char)p[1 + i];
        out[i] = 0;
        return 1;
    }
    if (p + 5 <= end && *p == 0x0C) {
        eisaid(*(const u32 *)(p + 1), out);
        return 1;
    }
    return 0;
}

static int name_int(const u8 *p, const u8 *end, u32 *v)
{
    if (p >= end) return 0;
    switch (*p) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0x0A: *v = p[1]; return 1;
    case 0x0B: *v = *(const u16 *)(p + 1); return 1;
    case 0x0C: *v = *(const u32 *)(p + 1); return 1;
    case 0x0E: *v = *(const u32 *)(p + 1); return 1;
    }
    return 0;
}

static void dev_resources(const u8 *b, const u8 *end, struct acpi_dev *d)
{
    const u8 *p;
    for (p = b; p + 20 < end; p++) {
        if (p[0] == 0x5B && p[1] == 0x82) break;                    /* child device */
        if (p[0] == 0x8E && p[5] == 1 && d->ni2c < 2) {             /* I2cSerialBus */
            u32 dl = p[1] | (p[2] << 8), tl = p[10] | (p[11] << 8), sp = *(const u32 *)(p + 12);
            u16 a = *(const u16 *)(p + 16);
            if (dl < 15 || dl > 200 || tl < 6 || p + 3 + dl > end || 12 + tl > 3 + dl) continue;
            if (sp < 10000UL || sp > 3400000UL || a < 0x08 || a > 0x77) continue;
            if (!d->ni2c) {
                strlcpy(d->i2cbus, (const char *)p + 12 + tl, sizeof(d->i2cbus));
                d->i2cspeed = sp;
            }
            d->i2caddr[d->ni2c++] = a;
            p += 2 + dl;
        } else if (p[0] == 0x8C && p[3] == 1 && p[4] <= 1 && d->ngpio < 4) {   /* GpioInt / GpioIo */
            u32 dl = p[1] | (p[2] << 8), pt = p[14] | (p[15] << 8), rs = p[17] | (p[18] << 8);
            if (dl < 22 || dl > 200 || p + 3 + dl > end || pt >= 3 + dl || rs >= 3 + dl) continue;
            d->gpio[d->ngpio].io = p[4];
            d->gpio[d->ngpio].pull = p[9];
            d->gpio[d->ngpio].pin = *(const u16 *)(p + pt);
            strlcpy(d->gpio[d->ngpio].ctl, (const char *)p + rs, sizeof(d->gpio[0].ctl));
            d->ngpio++;
            p += 2 + dl;
        }
    }
}

static int scan_devices(const u8 *aml, u32 len, const char *const *hids, struct acpi_dev *out, int n, int max)
{
    u32 i;
    for (i = 0; i + 8 < len && n < max; i++) {
        const u8 *p = aml + i, *body, *end, *q;
        u32 pl;
        int k, nn, j, found = 0;
        char seg[5], id[9];
        if (p[0] != 0x5B || p[1] != 0x82) continue;
        pl = pkglen(p + 2, &k);
        if (pl < 6 || i + 2 + pl > len) continue;
        end = p + 2 + pl;
        body = namestr(p + 2 + k, &nn, seg);
        if (body > end) continue;
        /* _HID / _CID as Name objects, before any child device */
        for (q = body; q + 6 < end && !found; q++) {
            if (q[0] == 0x5B && q[1] == 0x82) break;
            if (q[0] != 0x08 || (memcmp(q + 1, "_HID", 4) && memcmp(q + 1, "_CID", 4))) continue;
            if (!name_id(q + 5, end, id)) continue;
            for (j = 0; hids[j]; j++)
                if (!strncmp(id, hids[j], 9)) {
                    found = 1;
                    break;
                }
        }
        if (!found) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].hid, id, sizeof(out[n].hid));
        memcpy(out[n].path, seg, 5);
        for (q = body; q + 6 < end; q++) {
            if (q[0] == 0x5B && q[1] == 0x82) break;
            if (q[0] == 0x08 && !memcmp(q + 1, "_ADR", 4)) {
                name_int(q + 5, end, &out[n].adr);
                break;
            }
        }
        dev_resources(body, end, &out[n]);
        n++;
    }
    return n;
}

/* the AML of the DSDT and every SSDT (mapped) */
static const u8 *aml_tab[24];
static u32 aml_len[24];
static int naml = -1;

static void aml_load(void)
{
    u8 *f, *t;
    int i;
    if (naml >= 0) return;
    naml = 0;
    f = acpi_table("FACP", 0);
    if (f) {
        u32 dsdt = *(u32 *)(f + 40);
        if (*(u32 *)(f + 4) >= 148 && *(u32 *)(f + 140) && !*(u32 *)(f + 144)) dsdt = *(u32 *)(f + 140);
        if (dsdt && (t = map_table(dsdt)) != NULL) {
            aml_tab[naml] = t + 36;
            aml_len[naml++] = *(u32 *)(t + 4) - 36;
        }
    }
    for (i = 0; naml < (int)ARRAY_SIZE(aml_tab) && (t = acpi_table("SSDT", i)) != NULL; i++) {
        aml_tab[naml] = t + 36;
        aml_len[naml++] = *(u32 *)(t + 4) - 36;
    }
}

int acpi_find_devices(const char *const *hids, struct acpi_dev *out, int max)
{
    int i, n = 0;
    aml_load();
    for (i = 0; i < naml; i++) n = scan_devices(aml_tab[i], aml_len[i], hids, out, n, max);
    return n;
}

/* the Device () named by the last segment of 'path': its body up to the end */
static const u8 *find_dev(const char *path, const u8 **endp)
{
    const char *seg = path + strlen(path);
    int i;
    u32 k;
    while (seg > path && seg[-1] != '.' && seg[-1] != '\\') seg--;
    if (strlen(seg) != 4) return NULL;
    aml_load();
    for (i = 0; i < naml; i++) {
        const u8 *aml = aml_tab[i];
        for (k = 0; k + 12 < aml_len[i]; k++) {
            const u8 *p = aml + k, *body;
            u32 pl;
            int n, nn;
            char s[5];
            if (p[0] != 0x5B || p[1] != 0x82) continue;
            pl = pkglen(p + 2, &n);
            if (pl < 6 || k + 2 + pl > aml_len[i]) continue;
            body = namestr(p + 2 + n, &nn, s);
            if (memcmp(s, seg, 4)) continue;
            *endp = p + 2 + pl;
            return body;
        }
    }
    return NULL;
}

/* _ADR of the device named by the last segment of 'path' (e.g. I2C1): PCI dev << 16 | fn */
int acpi_path_adr(const char *path, u32 *adr)
{
    const u8 *end, *q, *body = find_dev(path, &end);
    if (!body) return -1;
    for (q = body; q + 6 < end; q++) {
        if (q[0] == 0x5B && q[1] == 0x82) break;
        if (q[0] == 0x08 && !memcmp(q + 1, "_ADR", 4) && name_int(q + 5, end, adr)) return 0;
    }
    return -1;
}

/* _HID, _UID and the Memory32Fixed ranges (base 0 when the BIOS fills them in at run time) of
   the device named by 'path'; returns the number of ranges, -1 when there is no such device */
int acpi_dev_info(const char *path, char *hid, u32 *uid, u32 *mem, int maxmem)
{
    const u8 *end, *q, *body = find_dev(path, &end);
    int n = 0;
    if (!body) return -1;
    hid[0] = 0;
    *uid = 0;
    for (q = body; q + 6 < end; q++) {
        if (q[0] == 0x5B && q[1] == 0x82) break;
        if (q[0] == 0x08 && !memcmp(q + 1, "_HID", 4)) name_id(q + 5, end, hid);
        if (q[0] == 0x08 && !memcmp(q + 1, "_UID", 4)) {
            if (q[5] == 0x0D) {
                const u8 *c = q + 6;
                u32 v = 0;
                while (c < end && *c >= '0' && *c <= '9') v = v * 10 + (u32)(*c++ - '0');
                *uid = v;
            } else {
                name_int(q + 5, end, uid);
            }
        }
        /* Memory32Fixed: 86 09 00 rw base length */
        if (q[0] == 0x86 && q[1] == 0x09 && q[2] == 0x00 && q[3] <= 1 && q + 12 <= end && n < maxmem) {
            mem[n++] = *(const u32 *)(q + 4);
            q += 11;
        }
    }
    return n;
}

/* Name (xxxx, integer) anywhere in the tables (e.g. SBRG, the sideband register base) */
int acpi_name_int(const char *name, u32 *v)
{
    int i;
    u32 k;
    aml_load();
    for (i = 0; i < naml; i++)
        for (k = 0; k + 10 < aml_len[i]; k++) {
            const u8 *p = aml_tab[i] + k;
            if (p[0] == 0x08 && !memcmp(p + 1, name, 4) && (p[5] == 0x0C || p[5] == 0x0E) &&
                name_int(p + 5, aml_tab[i] + aml_len[i], v))
                return 0;
        }
    return -1;
}

/* ---------------------------------------------------------------- device properties (_DSD)
   A property is a package (string, value). Without an interpreter the key string is looked
   for in the tables and the object after it read; 'scope' (a NameSeg) limits the search to
   the package of Name (scope, Package ()), e.g. the subproperties of a SoundWire link. */

/* body of Name (seg, Package ()) up to *end, NULL if there is none */
static const u8 *name_pkg(const char *seg, const u8 **end)
{
    int i, n;
    u32 k, pl;
    aml_load();
    for (i = 0; i < naml; i++)
        for (k = 0; k + 8 < aml_len[i]; k++) {
            const u8 *p = aml_tab[i] + k;
            if (p[0] != 0x08 || memcmp(p + 1, seg, 4) || p[5] != 0x12) continue;
            pl = pkglen(p + 6, &n);
            if (pl < 2 || k + 6 + pl > aml_len[i]) continue;
            *end = p + 6 + pl;
            return p + 6 + n;
        }
    return NULL;
}

/* the object after the string "key" in [b, end) */
static const u8 *prop_val(const u8 *b, const u8 *end, const char *key)
{
    u32 kl = strlen(key);
    const u8 *p;
    for (p = b; p + kl + 3 <= end; p++)
        if (p[0] == 0x0D && p[1] == (u8)key[0] && !memcmp(p + 1, key, kl) && p[1 + kl] == 0) return p + 2 + kl;
    return NULL;
}

static const u8 *prop_find(const char *scope, const char *key, const u8 **end)
{
    const u8 *b, *e, *v;
    int i;
    if (scope && scope[0]) {
        b = name_pkg(scope, &e);
        if (!b || (v = prop_val(b, e, key)) == NULL) return NULL;
        *end = e;
        return v;
    }
    aml_load();
    for (i = 0; i < naml; i++) {
        e = aml_tab[i] + aml_len[i];
        if ((v = prop_val(aml_tab[i], e, key)) != NULL) {
            *end = e;
            return v;
        }
    }
    return NULL;
}

int acpi_prop_int(const char *scope, const char *key, u32 *v)
{
    const u8 *end, *p = prop_find(scope, key, &end);
    u32 sz;
    if (!p) return -1;
    sz = *p == 0x0A ? 2 : *p == 0x0B ? 3 : *p == 0x0C ? 5 : *p == 0x0E ? 9 : 1;
    if (p + sz > end) return -1;
    if (*p == 0xFF) {                                   /* OnesOp */
        *v = 0xFFFFFFFFUL;
        return 0;
    }
    return name_int(p, end, v) ? 0 : -1;
}

int acpi_prop_str(const char *scope, const char *key, char *out, int n)
{
    const u8 *end, *p = prop_find(scope, key, &end);
    int i;
    if (!p || *p != 0x0D || n < 1) return -1;
    for (i = 0; i + 1 < n && p + 1 + i < end && p[1 + i]; i++) out[i] = (char)p[1 + i];
    out[i] = 0;
    return 0;
}

/* ---------------------------------------------------------------- devices by _ADR */

/* Name (_ADR, x) of a device body before its first child */
static int body_adr(const u8 *body, const u8 *end, u32 *adr)
{
    const u8 *q;
    for (q = body; q + 6 < end; q++) {
        if (q[0] == 0x5B && q[1] == 0x82) break;
        if (q[0] == 0x08 && !memcmp(q + 1, "_ADR", 4) && name_int(q + 5, end, adr)) return 1;
    }
    return 0;
}

static int nameseg_ok(const u8 *s)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_' || (i && s[i] >= '0' && s[i] <= '9'))) return 0;
    return 1;
}

/* the innermost Device () or Scope () of table t enclosing offset pos and opening before
   'below': its offset, -1 if none */
static s32 enclosing(const u8 *t, u32 len, u32 pos, u32 below)
{
    u32 k, pl;
    s32 best = -1;
    int n;
    for (k = 0; k < below && k + 12 < len; k++) {
        const u8 *p = t + k, *nm;
        if (p[0] == 0x5B && p[1] == 0x82) {
            pl = 2 + pkglen(p + 2, &n);
            nm = p + 2 + n;
        } else if (p[0] == 0x10) {
            pl = 1 + pkglen(p + 1, &n);
            nm = p + 1 + n;
        } else {
            continue;
        }
        if (k + pl <= pos || k + pl > len) continue;
        while (nm < t + len - 6 && (*nm == '\\' || *nm == '^')) nm++;
        if (*nm == 0x2E) nm++;
        else if (*nm == 0x2F) nm += 2;
        if (nm + 4 > t + len || !nameseg_ok(nm)) continue;
        best = (s32)k;
    }
    return best;
}

/* the _ADR of the Device () or Scope () at offset k of table t */
static int scope_adr(const u8 *t, u32 len, u32 k, u32 *adr)
{
    const u8 *p = t + k, *body, *end;
    char seg[5];
    int n, nn;
    u32 pl;
    if (p[0] == 0x5B) {
        pl = pkglen(p + 2, &n);
        end = p + 2 + pl;
        body = namestr(p + 2 + n, &nn, seg);
        return end <= t + len && body < end && body_adr(body, end, adr);
    }
    pl = pkglen(p + 1, &n);
    namestr(p + 1 + n, &nn, seg);
    if (!nameseg_ok((const u8 *)seg)) return 0;
    return !acpi_path_adr(seg, adr);
}

/* a Device () with Name (_ADR, adr): 0 when found; up[0..n-1] receive the _ADR of its parent
   and the parent's parent (0xFFFFFFFF when unknown) */
int acpi_find_adr(u32 adr, u32 *up, int n)
{
    int i, j, k2;
    u32 k, pl, a;
    aml_load();
    for (j = 0; j < n; j++) up[j] = 0xFFFFFFFFUL;
    for (i = 0; i < naml; i++) {
        const u8 *t = aml_tab[i];
        u32 len = aml_len[i];
        for (k = 0; k + 12 < len; k++) {
            const u8 *p = t + k, *body, *end;
            char seg[5];
            int nn;
            s32 at;
            if (p[0] != 0x5B || p[1] != 0x82) continue;
            pl = pkglen(p + 2, &k2);
            if (pl < 6 || k + 2 + pl > len) continue;
            end = p + 2 + pl;
            body = namestr(p + 2 + k2, &nn, seg);
            if (body >= end || !body_adr(body, end, &a) || a != adr) continue;
            /* parents: the enclosing Device () / Scope () objects */
            at = (s32)k;
            for (j = 0; j < n; j++) {
                at = enclosing(t, len, (u32)at, (u32)at);
                if (at < 0) break;
                scope_adr(t, len, (u32)at, &up[j]);
            }
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- values set at run time
   The Atom and Broadwell devices in ACPI mode (audio DSP, LPSS I2C, GPIO) often carry a
   resource template that their _CRS method completes with fields of the BIOS's global NVS
   area, and power up through a field of their PCI configuration space mirror:
       Name (RBUF, ResourceTemplate () { Memory32Fixed (ReadWrite, 0, 0x1000, BAR0) ... })
       Method (_CRS) { CreateDWordField (RBUF, 0x04, B0BA)  Store (I21A, B0BA)  Return (RBUF) }
       OperationRegion (GNVS, SystemMemory, 0x7B9FA000, 0x0300)  Field (GNVS, ...) { ..., I21A, 32 }
       OperationRegion (KEYS, SystemMemory, I21B, 0x100)  Field (KEYS, ...) { Offset (0x84), PSAT, 32 }
   Without an interpreter exactly these patterns are followed: CreateDWordField and Store
   into the template, named fields of SystemMemory regions whose base is a constant, a name
   or another such field. */

struct span { const u8 *b, *e; };

/* an integer constant object at p: its encoded length, 0 when p is none */
static int aml_const(const u8 *p, const u8 *end, u32 *v)
{
    if (p >= end) return 0;
    switch (*p) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0xFF: *v = 0xFFFFFFFFUL; return 1;
    case 0x0A: if (p + 2 > end) return 0; *v = p[1]; return 2;
    case 0x0B: if (p + 3 > end) return 0; *v = p[1] | ((u32)p[2] << 8); return 3;
    case 0x0C: if (p + 5 > end) return 0; *v = *(const u32 *)(p + 1); return 5;
    case 0x0E: if (p + 9 > end) return 0; *v = *(const u32 *)(p + 1); return 9;
    }
    return 0;
}

static int segch(u8 c, int first)
{
    return (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/* a NameString at p (prefixes, dual / multi name paths): its length and last segment, 0 if
   p is none */
static int aml_name(const u8 *p, const u8 *end, char *seg)
{
    const u8 *s = p;
    int n, i;
    while (p < end && (*p == '\\' || *p == '^')) p++;
    if (p >= end) return 0;
    if (*p == 0x2E) {
        n = 2;
        p++;
    } else if (*p == 0x2F) {
        if (p + 2 > end) return 0;
        n = p[1];
        p += 2;
    } else {
        n = 1;
    }
    if (n < 1 || p + 4 * n > end) return 0;
    for (i = 0; i < 4 * n; i++)
        if (!segch(p[i], !(i & 3))) return 0;
    memcpy(seg, p + 4 * (n - 1), 4);
    seg[4] = 0;
    return (int)(p + 4 * n - s);
}

/* a Device () at p: its end, NULL if p is no (well formed) device */
static const u8 *dev_at(const u8 *p, const u8 *end)
{
    u32 pl;
    int k;
    char s[5];
    if (p + 8 > end || p[0] != 0x5B || p[1] != 0x82) return NULL;
    pl = pkglen(p + 2, &k);
    if (pl < 6 || p + 2 + pl > end || !aml_name(p + 2 + k, p + 2 + pl, s)) return NULL;
    return p + 2 + pl;
}

/* the Device () and Scope () blocks whose name ends in 'seg' */
static int dev_spans(const char *seg, struct span *sp, int max)
{
    int i, n = 0, k, nn;
    u32 j, pl;
    char s[5];
    aml_load();
    for (i = 0; i < naml; i++) {
        const u8 *t = aml_tab[i], *te = t + aml_len[i];
        for (j = 0; j + 8 < aml_len[i] && n < max; j++) {
            const u8 *p = t + j, *nm, *e;
            if (p[0] == 0x5B && p[1] == 0x82) {
                pl = pkglen(p + 2, &k);
                nm = p + 2 + k;
                e = p + 2 + pl;
            } else if (p[0] == 0x10) {
                pl = pkglen(p + 1, &k);
                nm = p + 1 + k;
                e = p + 1 + pl;
            } else {
                continue;
            }
            if (pl < 6 || e > te || (nn = aml_name(nm, e, s)) == 0 || memcmp(s, seg, 4)) continue;
            sp[n].b = nm + nn;
            sp[n].e = e;
            n++;
        }
    }
    return n;
}

/* the last segment of a path */
static void path_seg(const char *path, char *seg)
{
    const char *s = path + strlen(path);
    while (s > path && s[-1] != '.' && s[-1] != '\\') s--;
    memset(seg, 0, 5);
    strlcpy(seg, s, 5);
}

/* the bit offset and width of field unit 'name' in the Field () at f (5B 81); its region */
static int field_bits(const u8 *f, const u8 *end, const char *name, char *region, u32 *bit, u32 *width)
{
    u32 pl, off = 0, w;
    int k, n;
    const u8 *p, *fe;
    pl = pkglen(f + 2, &k);
    fe = f + 2 + pl;
    if (pl < 6 || fe > end) return -1;
    p = f + 2 + k;
    n = aml_name(p, fe, region);
    if (!n) return -1;
    p += n + 1;                                         /* field flags */
    while (p < fe) {
        if (*p == 0x00) {                               /* reserved bits (Offset ()) */
            w = pkglen(p + 1, &k);
            p += 1 + k;
            off += w;
        } else if (*p == 0x01) {                        /* AccessAs */
            p += 3;
        } else if (*p == 0x03) {                        /* extended AccessAs */
            p += 4;
        } else if (p + 5 <= fe && segch(p[0], 1) && segch(p[1], 0) && segch(p[2], 0) && segch(p[3], 0)) {
            w = pkglen(p + 4, &k);
            if (!memcmp(p, name, 4)) {
                *bit = off;
                *width = w;
                return 0;
            }
            p += 4 + k;
            off += w;
        } else {
            return -1;                                  /* Connection () and the like */
        }
    }
    return -1;
}

static int field_value(const char *name, const struct span *sp, int nsp, u32 *v, int depth);

/* the base of SystemMemory region 'seg' (OperationRegion 5B 80) in [b, e) */
static int region_in(const u8 *b, const u8 *e, const char *seg, u32 *base, int depth)
{
    const u8 *p;
    char s[5];
    int n, m;
    for (p = b; p + 8 < e; p++) {
        if (p[0] != 0x5B || p[1] != 0x80 || (n = aml_name(p + 2, e, s)) == 0 || memcmp(s, seg, 4)) continue;
        if (p[2 + n] != 0) return -1;                   /* not SystemMemory */
        if ((m = aml_const(p + 3 + n, e, base)) != 0) return 0;
        if (depth < 2 && aml_name(p + 3 + n, e, s) &&
            (!field_value(s, NULL, 0, base, depth + 1) || !acpi_name_int(s, base)))
            return 0;
        return -1;
    }
    return -1;
}

/* the physical address and width of field unit 'name': first in the spans, then anywhere */
static int field_addr(const char *name, const struct span *sp, int nsp, u32 *phys, u32 *width, int depth)
{
    int i, pass;
    aml_load();
    for (pass = 0; pass < 2; pass++) {
        int cnt = pass ? naml : nsp;
        for (i = 0; i < cnt; i++) {
            const u8 *b = pass ? aml_tab[i] : sp[i].b, *e = pass ? aml_tab[i] + aml_len[i] : sp[i].e, *p;
            for (p = b; p + 8 < e; p++) {
                char region[5];
                u32 bit, base;
                int j;
                if (p[0] != 0x5B || p[1] != 0x81 || field_bits(p, e, name, region, &bit, width)) continue;
                /* the region: in the same spans first */
                for (j = 0; j < nsp; j++)
                    if (!region_in(sp[j].b, sp[j].e, region, &base, depth)) break;
                if (j == nsp) {
                    for (j = 0; j < naml; j++)
                        if (!region_in(aml_tab[j], aml_tab[j] + aml_len[j], region, &base, depth)) break;
                    if (j == naml) return -1;
                }
                if (bit & 7) return -1;
                *phys = base + bit / 8;
                return 0;
            }
        }
    }
    return -1;
}

/* the current value of field unit 'name' (at most 32 bits) */
static int field_value(const char *name, const struct span *sp, int nsp, u32 *v, int depth)
{
    u32 phys, width;
    u8 *m;
    if (field_addr(name, sp, nsp, &phys, &width, depth) || !width || !phys) return -1;
    m = plat_mmio(phys, 4);
    if (!m) return -1;
    *v = width >= 32 ? *(volatile u32 *)m : *(volatile u32 *)m & ((1UL << width) - 1);
    return 0;
}

/* the value _CRS stores into the dword at byte 'idx' of buffer 'buf' (CreateDWordField /
   Store), 0 when found */
static int crs_store(const struct span *sp, int nsp, const char *buf, u32 idx, u32 *v)
{
    int i, j, n, m;
    const u8 *p, *q;
    char s[5], fld[5], src[5];
    u32 x;
    for (i = 0; i < nsp; i++)
        for (p = sp[i].b; p + 8 < sp[i].e; p++) {
            if (p[0] != 0x8A || (n = aml_name(p + 1, sp[i].e, s)) == 0 || memcmp(s, buf, 4)) continue;
            if ((m = aml_const(p + 1 + n, sp[i].e, &x)) == 0 || x != idx) continue;
            if (!aml_name(p + 1 + n + m, sp[i].e, fld)) continue;
            /* Store (source, field) */
            for (j = 0; j < nsp; j++)
                for (q = sp[j].b; q + 6 < sp[j].e; q++) {
                    int a;
                    if (q[0] != 0x70) continue;
                    if ((a = aml_const(q + 1, sp[j].e, &x)) != 0) {
                        src[0] = 0;
                    } else if ((a = aml_name(q + 1, sp[j].e, src)) == 0) {
                        continue;
                    }
                    if (!aml_name(q + 1 + a, sp[j].e, s) || memcmp(s, fld, 4)) continue;
                    if (!src[0]) {
                        *v = x;
                        return 0;
                    }
                    if (!field_value(src, sp, nsp, v, 0) || !acpi_name_int(src, v)) return 0;
                }
        }
    return -1;
}

/* the template buffer around descriptor q: Name (xxxx, Buffer ()) - its name and data */
static const u8 *tmpl_of(const u8 *b, const u8 *q, char *name)
{
    const u8 *r;
    for (r = q - 1; r > b + 5 && r + 512 > q; r--) {
        u32 pl, sz;
        int k, m;
        const u8 *data;
        if (r[0] != 0x11 || r[-5] != 0x08) continue;
        pl = pkglen(r + 1, &k);
        m = aml_const(r + 1 + k, q, &sz);
        if (!m) continue;
        data = r + 1 + k + m;
        if (data > q || q + 12 > r + 1 + pl || !aml_name(r - 4, r, name)) continue;
        return data;
    }
    return NULL;
}

int acpi_dev_mem(const char *path, u32 *base, u32 *len, int max)
{
    struct span sp[4];
    int nsp, i, n = 0;
    char seg[5];
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    if (!nsp) return -1;
    for (i = 0; i < nsp; i++) {
        const u8 *p = sp[i].b, *e = sp[i].e, *nd;
        while (p + 12 <= e && n < max) {
            const u8 *data;
            char buf[5];
            u32 v;
            if ((nd = dev_at(p, e)) != NULL) {          /* child devices have their own */
                p = nd;
                continue;
            }
            if (p[0] != 0x86 || p[1] != 0x09 || p[2] != 0x00 || p[3] > 1) {
                p++;
                continue;
            }
            base[n] = *(const u32 *)(p + 4);
            len[n] = *(const u32 *)(p + 8);
            data = tmpl_of(sp[i].b, p, buf);
            if (data) {
                if (!crs_store(sp, nsp, buf, (u32)(p - data) + 4, &v) && v && v != 0xFFFFFFFFUL) base[n] = v;
                if (!crs_store(sp, nsp, buf, (u32)(p - data) + 8, &v) && v && v != 0xFFFFFFFFUL) len[n] = v;
            }
            n++;
            p += 12;
        }
    }
    return n;
}

int acpi_dev_irqs(const char *path)
{
    struct span sp[4];
    int nsp, i, n = 0;
    char seg[5];
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    if (!nsp) return -1;
    for (i = 0; i < nsp; i++) {
        const u8 *p = sp[i].b, *e = sp[i].e, *nd;
        while (p + 9 <= e) {
            if ((nd = dev_at(p, e)) != NULL) {
                p = nd;
                continue;
            }
            /* Extended Interrupt: 89 len16 flags count irq[count] */
            if (p[0] == 0x89 && p[2] == 0 && p[4] >= 1 && p[1] >= 2 + 4 * p[4] && p[1] <= 2 + 4 * p[4] + 40) {
                n += p[4];
                p += 3 + p[1];
                continue;
            }
            p++;
        }
    }
    return n;
}

int acpi_dev_field(const char *path, const char *name, u32 *phys)
{
    struct span sp[4];
    int nsp, i;
    u32 width;
    char seg[5];
    const u8 *p;
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    /* the field must be the device's own */
    for (i = 0; i < nsp; i++)
        for (p = sp[i].b; p + 8 < sp[i].e; p++) {
            char region[5];
            u32 bit;
            if (p[0] == 0x5B && p[1] == 0x81 && !field_bits(p, sp[i].e, name, region, &bit, &width))
                return field_addr(name, sp, nsp, phys, &width, 0);
        }
    return -1;
}

int acpi_dev_const(const char *path, const char *name, u32 *v)
{
    struct span sp[4];
    int nsp, i, k, m;
    char seg[5];
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    for (i = 0; i < nsp; i++) {
        const u8 *p = sp[i].b, *e = sp[i].e, *nd;
        while (p + 6 < e) {
            if ((nd = dev_at(p, e)) != NULL) {          /* child devices have their own */
                p = nd;
                continue;
            }
            if (p[0] == 0x08 && !memcmp(p + 1, name, 4) && aml_const(p + 5, e, v)) return 0;
            if (p[0] == 0x14) {                         /* Method: PkgLength, name, flags, body */
                u32 pl = pkglen(p + 1, &k);
                const u8 *me = p + 1 + pl, *b = p + 1 + k + 5;
                if (pl > 6 && me <= e && !memcmp(p + 1 + k, name, 4) && segch(p[1 + k], 1)) {
                    /* the body is a single Return (constant) */
                    if (b + 1 < me && b[0] == 0xA4 && (m = aml_const(b + 1, me, v)) != 0 && b + 1 + m == me)
                        return 0;
                    return -1;
                }
            }
            p++;
        }
    }
    return -1;
}

int acpi_dev_irq(const char *path, u32 *irq)
{
    struct span sp[4];
    int nsp, i;
    char seg[5];
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    for (i = 0; i < nsp; i++) {
        const u8 *p = sp[i].b, *e = sp[i].e, *nd;
        while (p + 9 <= e) {
            if ((nd = dev_at(p, e)) != NULL) {
                p = nd;
                continue;
            }
            if (p[0] == 0x89 && p[2] == 0 && p[4] >= 1 && p[1] >= 2 + 4 * p[4] && p[1] <= 2 + 4 * p[4] + 40) {
                *irq = *(const u32 *)(p + 5);
                return 0;
            }
            p++;
        }
    }
    return -1;
}

int acpi_dev_prop(const char *path, const char *key, u32 *v)
{
    struct span sp[4];
    int nsp, i;
    char seg[5];
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    for (i = 0; i < nsp; i++) {
        const u8 *p = prop_val(sp[i].b, sp[i].e, key);
        if (p && aml_const(p, sp[i].e, v)) return 0;
    }
    return -1;
}

int acpi_dev_pkg(const char *path, const char *name, u32 *v, int n)
{
    struct span sp[4];
    int nsp, i, k, c, got;
    char seg[5];
    const u8 *p, *q, *e;
    path_seg(path, seg);
    nsp = dev_spans(seg, sp, 4);
    for (i = 0; i < nsp; i++)
        for (p = sp[i].b; p + 8 < sp[i].e; p++) {
            if (p[0] != 0x08 || memcmp(p + 1, name, 4) || p[5] != 0x12) continue;
            e = p + 6 + pkglen(p + 6, &k);
            if (e > sp[i].e) continue;
            q = p + 6 + k;
            c = *q++;
            for (got = 0; got < c && got < n; got++) {
                int m = aml_const(q, e, &v[got]);
                if (!m) break;
                q += m;
            }
            return got;
        }
    return -1;
}

/* ---------------------------------------------------------------- HID over I2C devices
   Every Device () whose _HID or _CID is PNP0C50 / ACPI0C50 (string or EISA id, as Name or as
   the value a Method returns): its I2cSerialBus addresses (alternatives of several resource
   templates included), its GpioInt line and the address of its HID descriptor, which _DSM
   function 1 of the HID over I2C UUID returns. */

/* ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de") */
static const u8 hid_dsm_uuid[16] = {
    0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE
};

static int has_hidi2c(const u8 *b, const u8 *e)
{
    const u8 *p;
    for (p = b; p + 8 < e; p++) {
        if (p[0] == 0x5B && p[1] == 0x82) break;
        if (!memcmp(p, pnp0c50, 5) || !memcmp(p, "PNP0C50", 8) || !memcmp(p, "ACPI0C50", 9)) return 1;
    }
    return 0;
}

/* the value of an operand: a constant, or Name (seg, constant) in the device, else anywhere;
   a field unit of the device's SystemMemory region last */
static int operand_val(const char *dev, const u8 *p, const u8 *pe, const u8 *db, const u8 *de, u32 *v)
{
    char seg[5];
    int i;
    u32 phys;
    if (aml_const(p, pe, v)) return 0;
    if (!aml_name(p, pe, seg)) return -1;
    for (i = -1; i < naml; i++) {
        const u8 *b = i < 0 ? db : aml_tab[i], *e = i < 0 ? de : aml_tab[i] + aml_len[i], *q;
        for (q = b; q + 6 < e; q++)
            if (q[0] == 0x08 && !memcmp(q + 1, seg, 4) && aml_const(q + 5, e, v)) return 0;
    }
    if (!acpi_dev_field(dev, seg, &phys) && phys) {
        u8 *m = plat_mmio(phys, 2);
        if (m) {
            *v = m[0];
            return 0;
        }
    }
    return -1;
}

/* the HID descriptor register: Return (x) after "Arg2 == One" (also through ToInteger and
   the _T_n temporaries of Switch) behind the UUID, or the last argument of a helper method
   called with (Arg0, Arg1, Arg2, Arg3, x); 0xFFFF when not found */
static u16 dsm_descreg(const char *dev, const u8 *b, const u8 *e)
{
    const u8 *p, *q, *u = NULL;
    u32 v;
    for (p = b; p + 16 < e && !u; p++) {
        if (p[0] == 0x5B && p[1] == 0x82) break;
        if (!memcmp(p, hid_dsm_uuid, 16)) u = p + 16;
    }
    if (!u) return 0xFFFF;
    for (p = u; p + 3 < e && p < u + 600; p++) {
        int n = 0;
        if (p + 4 < e && p[0] == 0x68 && p[1] == 0x69 && p[2] == 0x6A && p[3] == 0x6B)
            if (!operand_val(dev, p + 4, e, b, e, &v) && v < 0x10000UL) return (u16)v;
        if (p[0] != 0x93) continue;
        q = p + 1;
        if (q[0] == 0x99 && q[1] == 0x6A && q[2] == 0x00) n = 3;
        else if (q[0] == 0x6A) n = 1;
        else if (q[0] == '_' && q[1] == 'T' && q[2] == '_') n = 4;
        if (!n || q[n] != 0x01) continue;
        for (q += n + 1; q + 1 < e && q < p + 24; q++)
            if (q[0] == 0xA4) {
                if (!operand_val(dev, q + 1, e, b, e, &v) && v < 0x10000UL) return (u16)v;
                break;
            }
    }
    return 0xFFFF;
}

int acpi_hid_i2c(struct acpi_hidi2c *out, int max)
{
    int i, n = 0, k, nn;
    u32 j, pl;
    aml_load();
    for (i = 0; i < naml; i++) {
        const u8 *t = aml_tab[i];
        for (j = 0; j + 8 < aml_len[i] && n < max; j++) {
            const u8 *p = t + j, *body, *end, *q;
            struct acpi_hidi2c *d = &out[n];
            char seg[5];
            if (p[0] != 0x5B || p[1] != 0x82) continue;
            pl = pkglen(p + 2, &k);
            if (pl < 6 || j + 2 + pl > aml_len[i]) continue;
            end = p + 2 + pl;
            body = namestr(p + 2 + k, &nn, seg);
            if (body > end || !has_hidi2c(body, end)) continue;
            memset(d, 0, sizeof(*d));
            memcpy(d->seg, seg, 5);
            for (q = body; q + 6 < end; q++) {
                if (q[0] == 0x5B && q[1] == 0x82) break;
                if (q[0] == 0x08 && !memcmp(q + 1, "_HID", 4)) {
                    name_id(q + 5, end, d->hid);
                    break;
                }
            }
            for (q = body; q + 20 < end; q++) {
                if (q[0] == 0x5B && q[1] == 0x82) break;
                if (q[0] == 0x8E && q[5] == 1) {                        /* I2cSerialBus */
                    u32 dl = q[1] | (q[2] << 8), tl = q[10] | (q[11] << 8), sp = *(const u32 *)(q + 12), a;
                    u16 ad = *(const u16 *)(q + 16);
                    if (dl < 15 || dl > 200 || tl < 6 || q + 3 + dl > end || 12 + tl > 3 + dl) continue;
                    if (sp < 10000UL || sp > 3400000UL || ad < 0x08 || ad > 0x77 || (q[7] & 1)) continue;
                    for (a = 0; a < d->naddr && d->addr[a] != ad; a++) ;
                    if (a == d->naddr && d->naddr < ARRAY_SIZE(d->addr)) d->addr[d->naddr++] = ad;
                    if (!d->bus[0]) {
                        strlcpy(d->bus, (const char *)q + 12 + tl, sizeof(d->bus));
                        d->speed = sp;
                    }
                    q += 2 + dl;
                } else if (q[0] == 0x8C && q[3] == 1 && q[4] == 0 && !d->gctl[0]) {    /* GpioInt */
                    u32 dl = q[1] | (q[2] << 8), pt = q[14] | (q[15] << 8), rs = q[17] | (q[18] << 8);
                    if (dl < 22 || dl > 200 || q + 3 + dl > end || pt + 2 > 3 + dl || rs >= 3 + dl) continue;
                    d->gpin = *(const u16 *)(q + pt);
                    d->gflags = q[7];
                    strlcpy(d->gctl, (const char *)q + rs, sizeof(d->gctl));
                    q += 2 + dl;
                }
            }
            if (!d->naddr) continue;
            d->descreg = dsm_descreg(seg, body, end);
            DBG("acpi: HID over I2C %s (%s) at %02x on %s, descriptor %04x, GpioInt %s %u (%02x)\n", d->seg,
                d->hid, d->addr[0], d->bus, d->descreg, d->gctl[0] ? d->gctl : "-", d->gpin, d->gflags);
            n++;
            j += 1 + pl;
        }
    }
    return n;
}
