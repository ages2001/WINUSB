/* pnpblk.c - Windows 95/98/ME: the disk services of the VxD for its I/O Supervisor port driver
   (WINUSB.PDR / WINDISK.PDR call them through the VxD's service table, vxdent.asm) */
#include "pnpint.h"

int pkg_blk_owned(struct pnode *p, struct blkdev *d);

#define PDR_VERSION 0x0100
struct pdrinfo {
    u32 handle;
    u32 sectors_lo, sectors_hi;     /* 512-byte sectors */
    u32 ssize;
    u8 removable, wprot, cdrom, present;
    char model[41];
    char serial[21];
    u8 bios;                        /* the BIOS INT 13h drive (80h ..) of this disk, 0 none */
    u8 pad;
    u32 letters;                    /* its DOS drives (bit 0 A:), from IO.SYS at the start */
};

u32 __cdecl svc_get_version(void)
{
    /* bit 16: /D, PDR traces too; bit 17: the services of the USB WDM bridge (6-11) are there */
#ifdef WIN_USB
    return PDR_VERSION | ((cb && (cb->opts & OPT_DEBUG)) ? 0x10000UL : 0) | 0x20000UL;
#else
    return PDR_VERSION | ((cb && (cb->opts & OPT_DEBUG)) ? 0x10000UL : 0);
#endif
}

static struct blkdev *blk_valid(u32 h)
{
    int i;
    for (i = 0; i < blk_count(); i++)
        if ((u32)blk_get(i) == h) return blk_get(i);
    return NULL;
}

/* unit n of the devnode: 0 found, 1 no more units, -1 unknown devnode */
int __cdecl svc_blk_lookup(u32 dn, u32 unit, struct pdrinfo *out)
{
    struct pnode *p = pnp_node_by_dn(dn);
    int i, k = 0;
    if (!p || !vxd_core_up()) return -1;
    for (i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (!pkg_blk_owned(p, d)) continue;
        if ((u32)k++ != unit) continue;
        memset(out, 0, sizeof(*out));
        out->handle = (u32)d;
        out->sectors_lo = (u32)(d->sectors * (d->ssize / 512));
        out->sectors_hi = (u32)((d->sectors * (d->ssize / 512)) >> 32);
        out->ssize = d->cdrom ? 2048 : 512;
        out->removable = d->removable;
        out->wprot = d->wprot;
        out->cdrom = d->cdrom;
        out->present = d->present;
        strlcpy(out->model, d->model, sizeof(out->model));
        strlcpy(out->serial, d->serial, sizeof(out->serial));
        out->bios = (u8)vxd_bios_unit(d);
        if (out->bios) out->letters = vxd_bios_letters(out->bios);
        return 0;
    }
    return 1;
}

int __cdecl svc_blk_io(u32 h, u32 lba_lo, u32 lba_hi, u32 count, void *buf, u32 write)
{
    struct blkdev *d = blk_valid(h);
    u64 lba;
    int r;
    if (!d || !d->present) return -2;
    lba = lba_lo | ((u64)lba_hi << 32);
    if (lba + count > d->sectors * (d->ssize / 512)) return -1;
    vmm_begin_crit();
    r = write ? blk_write_abs(d, lba, count, buf) : blk_read_abs(d, lba, count, buf);
    vmm_end_crit();
    return r ? -1 : 0;
}

/* a disk devnode of ours that is a BIOS drive (the boot disk among them); of the USB disks
   only the boot disk (80h), the others stay removable (unplugging without a restart) */
static int bios_disk_node(struct pnode *p)
{
    int i, unit;
    if (!p || p->kind != PK_IOSDISK || !vxd_core_up()) return 0;
    for (i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (!d || !pkg_blk_owned(p, d)) continue;
        unit = vxd_bios_unit(d);
        if (unit && (d->kind != DK_USB || unit == 0x80)) return unit;
    }
    return 0;
}

typedef u32 (__cdecl *cmhandler)(u32 func, u32 sub, u32 dn, u32 ref, u32 flags);

/* the I/O Supervisor's configuration handler of a BIOS drive's disk devnode: when Windows
   shuts down, the devnode is neither stopped nor removed (the I/O Supervisor would take the
   volumes away with the swap file and other files still open) */
static u32 __cdecl bios_disk_config(u32 func, u32 sub, u32 dn, u32 ref, u32 flags)
{
    struct pnode *p = pnp_node_by_dn(dn);
    (void)ref;
    if (!p || !p->cfgh) return CR_DEFAULT;
    if (cb && (cb->opts & OPT_DEBUG)) kprintf("pnp: config %lu/%lu of the BIOS drive's disk %08lx\n", func, sub, dn);
    if (pnp_shutting_down() && (func == CONFIG_STOP || func == CONFIG_REMOVE || func == CONFIG_PREREMOVE ||
                                func == CONFIG_PREREMOVE2)) {
        kprintf("pnp: devnode %08lx: Windows shuts down, the BIOS drive's disk stays (config %lu)\n", dn, func);
        return CR_SUCCESS;
    }
    return ((cmhandler)p->cfgh)(func, sub, dn, p->cfgref, flags);
}

#ifdef WIN_DISK
/* ---- other port drivers of the I/O Supervisor on hardware WINDISK drives ----
   The detection of Windows (or an earlier driver) can leave devnodes that describe the same
   hardware again: "Primary / Secondary IDE controller" (ESDI_506.PDR) at the compatibility
   ports of a PCI IDE controller WINDISK now drives, an old SCSI miniport (AHCI, NVMe) entry.
   Two drivers on one IDE channel take each other's interrupts and status: requests that
   never end (Explorer hangs when it asks every drive at once), every drive twice in Device
   Manager. Such a devnode is not started: its CONFIG_START fails (Device Manager shows it
   with a problem), its drives stay with WINDISK. */
#define MAXFOREIGN 24
static struct { u32 dn, h, ref; } foreign[MAXFOREIGN];
static int nforeign;

static int overlaps(u32 a, u32 alen, u32 b, u32 blen)
{
    if (!alen) alen = 1;
    if (!blen) blen = 1;
    return a < b + blen && b < a + alen;
}

/* the ports [base, base + len) belong to a controller WINDISK drives */
static int io_ours(u32 base, u32 len)
{
    int i, k;
    for (k = 0; k < 8; k++)
        if (cb && cb->ideown[k] && overlaps(base, len, cb->ideown[k], 8)) return 1;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        int bus, dev, fn, b;
        u32 cls;
        if (!p->dn || p->kind != PK_PCI) continue;
        bus = (int)(p->bdf >> 8);
        dev = (int)((p->bdf >> 3) & 0x1F);
        fn = (int)(p->bdf & 7);
        cls = pci_rd32(bus, dev, fn, 8) >> 8;
        if ((cls >> 16) != 0x01) continue;
        if ((cls >> 8) == 0x0101) {
            if (!(cls & 1) && overlaps(base, len, 0x1F0, 8)) return 1;
            if (!(cls & 4) && overlaps(base, len, 0x170, 8)) return 1;
        }
        for (b = 0; b < 6; b++) {
            u32 bar = pci_rd32(bus, dev, fn, 0x10 + 4 * b);
            if ((bar & 1) && (bar & 0xFFFC) && overlaps(base, len, bar & 0xFFFC, 1)) return 1;
        }
    }
    return 0;
}

/* the memory [base, base + len) is a register window of a controller WINDISK drives */
static int mem_ours(u32 base, u32 len)
{
    int i, b;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        int bus, dev, fn;
        if (!p->dn || p->kind != PK_PCI) continue;
        bus = (int)(p->bdf >> 8);
        dev = (int)((p->bdf >> 3) & 0x1F);
        fn = (int)(p->bdf & 7);
        if ((pci_rd32(bus, dev, fn, 8) >> 24) != 0x01) continue;
        for (b = 0; b < 6; b++) {
            u32 bar = pci_rd32(bus, dev, fn, 0x10 + 4 * b);
            if (!(bar & 1) && (bar & 0xFFFFFFF0UL) && overlaps(base, len, bar & 0xFFFFFFF0UL, 1)) return 1;
            if ((bar & 7) == 4) b++;                    /* 64-bit BAR */
        }
    }
    return 0;
}

static u32 __cdecl foreign_config(u32 func, u32 sub, u32 dn, u32 ref, u32 flags)
{
    int i;
    (void)ref;
    for (i = 0; i < nforeign && foreign[i].dn != dn; i++) ;
    if (i == nforeign) return CR_DEFAULT;
    if (func == CONFIG_START) {
        struct cmconfig c;
        int k, hit = 0;
        memset(&c, 0, sizeof(c));
        if (cm_get_alloc_log_conf(&c, dn, 0) == CR_SUCCESS) {
            for (k = 0; k < c.nio && k < MAX_IO_PORTS && !hit; k++) hit = io_ours(c.iobase[k], c.iolen[k]);
            for (k = 0; k < c.nmem && k < MAX_MEM_REGISTERS && !hit; k++) hit = mem_ours(c.membase[k], c.memlen[k]);
        }
        if (hit) {
            kprintf("pnp: devnode %08lx: another port driver for a controller WINDISK drives, not started\n", dn);
            return CR_FAILURE;
        }
    }
    return ((cmhandler)foreign[i].h)(func, sub, dn, foreign[i].ref, flags);
}

/* a devnode of the I/O Supervisor that is not ours: its handler goes behind foreign_config */
static void foreign_hook(u32 *a)
{
    char buf[16];
    u32 len = sizeof(buf) - 1;
    int i;
    if (a[1] == (u32)foreign_config) return;
    memset(buf, 0, sizeof(buf));
    if (cm_read_registry_value(a[0], NULL, "DevLoader", REG_SZ, buf, &len, CM_REGISTRY_SOFTWARE) != CR_SUCCESS ||
        (buf[0] != '*' || (buf[1] | 0x20) != 'i' || (buf[2] | 0x20) != 'o' || (buf[3] | 0x20) != 's' || buf[4]))
        return;
    for (i = 0; i < nforeign && foreign[i].dn != a[0]; i++) ;
    if (i == MAXFOREIGN) return;
    if (i == nforeign) nforeign++;
    foreign[i].dn = a[0];
    foreign[i].h = a[1];
    foreign[i].ref = a[2];
    a[1] = (u32)foreign_config;
    a[2] = a[0];
}
#endif

/* _CONFIGMG_Register_Device_Driver (vxdent.asm hook), a[] = devnode, handler, refdata, flags:
   for the disk of a BIOS drive the I/O Supervisor's handler goes behind bios_disk_config and
   the devnode is not removable (the Configuration Manager removes removable devnodes at
   shutdown) */
void __cdecl pnp_regdrv_hook(u32 *a)
{
    struct pnode *p = pnp_node_by_dn(a[0]);
    int unit = bios_disk_node(p);
#ifdef WIN_DISK
    if (!p && a[1]) {
        foreign_hook(a);
        return;
    }
#endif
    if (!unit || a[1] == (u32)bios_disk_config) return;
    p->cfgh = a[1];
    p->cfgref = a[2];
    a[1] = (u32)bios_disk_config;
    a[2] = a[0];
    a[3] &= ~(u32)CM_REGISTER_DEVICE_DRIVER_REMOVABLE;
    kprintf("pnp: devnode %08lx: BIOS drive %02x, not removable\n", a[0], unit);
}

u32 __cdecl svc_blk_status(u32 h)
{
    struct blkdev *d = blk_valid(h);
    return d && d->present ? 1 : 0;
}

int __cdecl svc_cd_media(u32 h)
{
    struct blkdev *d = blk_valid(h);
    int r;
    if (!d) return -1;
    vmm_begin_crit();
    r = cd_media(d);
    vmm_end_crit();
    return r;
}

int __cdecl svc_cd_command(u32 h, const u8 *cdb, void *buf, u32 len, u8 *sense)
{
    struct blkdev *d = blk_valid(h);
    int r;
    if (!d) return -1;
    vmm_begin_crit();
    r = cd_command(d, cdb, buf, len, sense);
    vmm_end_crit();
    return r;
}
