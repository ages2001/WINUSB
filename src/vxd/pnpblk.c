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

/* _CONFIGMG_Register_Device_Driver (vxdent.asm hook), a[] = devnode, handler, refdata, flags:
   for the disk of a BIOS drive the I/O Supervisor's handler goes behind bios_disk_config and
   the devnode is not removable (the Configuration Manager removes removable devnodes at
   shutdown) */
void __cdecl pnp_regdrv_hook(u32 *a)
{
    struct pnode *p = pnp_node_by_dn(a[0]);
    int unit = bios_disk_node(p);
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
