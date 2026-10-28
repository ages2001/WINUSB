/* corent.c - core entry points shared by the DOS and VxD platforms */
#include "core.h"

#ifdef WIN_USB
#define CORENAME "WINUSB"
#include "audio.h"
#include "usb.h"
#elif defined(WIN_AUDIO)
#define CORENAME "WINAUDIO"
#include "audio.h"
#include "sbemu.h"
#else
#define CORENAME "WINDISK"
#endif

struct wincb *cb;
u32 core_flags;
static int pnp_mode;
static struct pcidev pcis[256];
static int npci;
static u32 start_ms;

void blk_adopt_units(void);
void blk_rebind_units(void);
void nvme_resume(void);
void ahci_resume(void);
int usb_api(struct winapireq *r, u8 *buf);
int uvc_api(struct winapireq *r, u8 *buf);
void tbt_start(struct pcidev *list, int n);     /* tbt.c: Thunderbolt / USB4 hosts */
void tbt_poll(void);
void tbt_shutdown(void);
int tbt_api_tree(void *buf, u32 len, u32 *retlen);
void pcihp_boot(void);
int nvme_probe_hot(struct pcidev *pd, int removable);
void nvme_remove(u32 bdf);
int usb_hotplug_hc(struct pcidev *p);
void usb_hotunplug_hc(u32 bdf);
void usb_pci_list(struct pcidev *l, int n);

#if defined(WIN_DISK) || defined(WIN_USB)
/* DOS: the BIOS disks that are disks of ours on controllers we took over from the BIOS (USB;
   NVMe with /NVME): their INT 13h calls are served by the driver from now on, the BIOS code
   of those controllers no longer works (a USB boot disk would be lost: "Bad or missing ..."
   for every file DOS reads from it afterwards). A disk matches by its first sector, once. */
static int api_biosmap(struct winbiosmap *m)
{
    int i, k, hits, hit;
    u8 *buf;
    memset(m, 0, sizeof(*m));
    if (plat_mode != PM_DOS || !cb->nbios) return WINE_OK;
    buf = kmalloc_align(4096, 4096);
    if (!buf) return WINE_IO;
    for (k = 0; k < cb->nbios && k < 8; k++) {
        hits = 0;
        hit = -1;
        if (!cb->biosmbr[k]) continue;
        for (i = 0; i < blk_count(); i++) {
            struct blkdev *d = blk_get(i);
            if (!d || !d->present || d->cdrom || d->ssize != 512) continue;
            if (d->kind != DK_USB && !(d->kind == DK_NVME && (cb->opts & OPT_NVME))) continue;
            if (blk_read_abs(d, 0, 1, buf) || crc32(0, buf, 512) != cb->biosmbr[k]) continue;
            hits++;
            hit = i;
        }
        if (hits == 1) {
            struct blkdev *d = blk_get(hit);
            m->disk[k] = (u8)(hit + 1);
            m->sectors[k] = (u32)d->sectors;
            m->sectorshi[k] = (u32)(d->sectors >> 32);
            if (!d->shared) d->shared = (u8)(k + 1);
            kprintf("BIOS disk %02xh is disk%d (%s): its INT 13h calls are served here\n", 0x80 + k, hit, d->model);
        }
    }
    kfree(buf);
    return WINE_OK;
}
#endif

#ifdef WIN_DISK
static void match_bios_disks(u8 *mbrs)
{
    int i, k;
    u8 *buf;
    (void)mbrs;
    buf = kmalloc_align(4096, 4096);
    for (i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        u32 c;
        if (d->removable) continue;
        if (blk_read_abs(d, 0, 1, buf)) continue;
        c = crc32(0, buf, 512);
        for (k = 0; k < cb->nbios && k < 8; k++) {
            if (cb->biosmbr[k] == c) {
                d->shared = (u8)(k + 1);
                kprintf("disk%d is BIOS disk %02xh\n", i, 0x80 + k);
                break;
            }
        }
    }
    kfree(buf);
}
#endif

#ifdef WIN_DISK
/* AHCI, and the AHCI-compatible RAID mode of Intel (RST) and AMD chipsets: a RAID-class
   function whose BAR5 holds AHCI registers is driven as plain AHCI (single disks only;
   RAID volumes need the vendor option ROM and are not supported) */
static int is_ahci(struct pcidev *d)
{
    if (d->sub == 6 && d->prog == 1) return 1;
    if (d->sub == 4 && (d->vendor == 0x8086 || d->vendor == 0x1002 || d->vendor == 0x1022) &&
        pci_bar_mem(d, 5, NULL))
        return 1;
    return 0;
}
#endif

#ifdef WIN_AUDIO
#include "dsp.h"
static int fw_wait;
static void audio_start(void);
static void audio_summary(void);

/* WINAUDIO.SYS loaded the firmware files the core asked for: the sound devices start */
int core_filedone(void)
{
    if (!fw_wait) return 0;
    fw_wait = 0;
    if (cb->opts & OPT_USB) audio_start();
    core_flags |= CF_STARTED;
    kprintf(CORENAME " ready: %d sound device(s)\n", adev_count());
    return 0;
}
#else
int core_filedone(void)
{
    return 0;
}
#endif

#ifdef WIN_AUDIO
/* sound controllers: HD Audio (04/03, and Intel 04/01 / 04/80 in DSP mode), AC'97 (04/01);
   the audio DSP of Bay Trail / Cherry Trail / Broadwell (an ACPI device, sometimes PCI) */
static void audio_start(void)
{
    int i;
    aeng_init();
    sst_probe(pcis, npci);
    for (i = 0; i < npci; i++) {
        struct pcidev *d = &pcis[i];
        if (d->cls != 4 || sst_pci(d)) continue;
        if (d->sub == 3 && !(cb->opts & OPT_NOHDA)) {
            hda_probe(d);
            continue;
        }
        if (d->sub == 1 || d->sub == 0x80) {
            /* Intel DSP (SST / SOF) functions keep HD Audio registers in BAR0 */
            if (d->vendor == 0x8086 && pci_bar_mem(d, 0, NULL) && !(cb->opts & OPT_NOHDA) && hda_probe(d) == 0)
                continue;
            if (d->sub == 1 && !(cb->opts & OPT_NOAC97)) ac97_probe(d);
        }
    }
    core_flags |= CF_AUDIO;
    aeng_bridge_probe();                /* a USB Audio device of WINUSB */
    if (adev_count()) cb->flags |= CBF_HAVEDISK;
    sb_start(plat_mode == PM_VXD);
    audio_summary();
}

/* DOS, at boot: what was found, on the screen */
static void audio_summary(void)
{
    static int done;
    int i;
    if (plat_mode != PM_DOS || done) return;
    done = 1;
    plat_screen(1);
    for (i = 0; i < 8; i++) {
        struct adev *a = adev_get(i);
        if (!a) continue;
        kprintf(CORENAME ": %s\n", a->name);
        if (a->pcivid)
            kprintf(CORENAME ":   %s %04x:%04x at PCI %02x:%02x.%x, IRQ %d, %lu Hz\n",
                    a->type == AT_HDA ? "HD Audio" : "AC'97", a->pcivid, a->pcidid, a->loc >> 8, (a->loc >> 3) & 31,
                    a->loc & 7, a->irq, a->out.rate);
    }
    {
        /* USB Audio through WINUSB: wanted (/USBSB) or the only way, but not there */
        const char *why = aeng_bridge_why();
        int own = 0;
        for (i = 0; i < 8; i++)
            if (adev_get(i) && !adev_get(i)->hidden) own++;
        if (why && (!own || (cb->opts2 & OPT2_USBSB))) kprintf(CORENAME ": USB Audio: %s\n", why);
    }
    plat_screen((cb->opts & OPT_SCREEN) != 0);
}
#endif

int core_init(struct core_params *p)
{
#ifdef WIN_DISK
    int i, nide = 0, nahci = 0, nnvme = 0;
#endif
    cb = (struct wincb *)p->cb_lin;
    pool_init(p->pool_lin, p->pool_phys, p->pool_size);
    log_init();
    dbg_level = (cb->opts & OPT_DEBUG) ? 2 : 1;
    start_ms = plat_ms();
    kprintf(CORENAME " %s mode, pool %luK at %08lx\n", plat_mode == PM_VXD ? "VxD" : "DOS",
            p->pool_size >> 10, p->pool_phys);
    blk_init();
    if (plat_mode == PM_VXD) blk_adopt_units();
    if (plat_mode == PM_DOS && p->mbr_lin) {
        /* first sectors of the BIOS disks: the disks DOS already uses (and serves) */
        int k;
        for (k = 0; k < cb->nbios && k < 8; k++) {
            /* loaded high, the DOS driver read every sector twice: into its upper memory block
               (right for BIOS code running in V86 mode) and into the HMA (right for BIOS code
               writing to physical memory: 32-bit or SMM disk code, bus master USB); the copy
               with a boot signature counts */
            u8 *a = (u8 *)p->mbr_lin + k * 512;
            if (p->rsv[0] && !(a[510] == 0x55 && a[511] == 0xAA)) {
                u8 *b = (u8 *)p->rsv[0] + k * 512;
                if (b[510] == 0x55 && b[511] == 0xAA) a = b;
            }
            cb->biosmbr[k] = crc32(0, a, 512);
        }
        DBG("dos: %u BIOS disk(s), first sectors at %08lx / %08lx\n", cb->nbios, p->mbr_lin, p->rsv[0]);
    }
    /* DOS start, before Windows and its drivers see the machine: the PCI resources of the
       devices WINDRV drives that the firmware put above 4G are moved below 4G (the first
       WINDRV driver loaded does it; /ALL4G: of every device) */
    if (plat_mode == PM_DOS && !(cb->opts2 & OPT2_NORELOC)) pci_relocate_all((cb->opts2 & OPT2_ALL4G) != 0);
    npci = pci_scan(pcis, ARRAY_SIZE(pcis));
    kprintf("pci: %d functions\n", npci);
#ifdef WIN_DISK
    if (p->flags & CPF_PNP) {
        /* Windows 95/98/ME: the controllers come one by one from the Configuration Manager,
           the I/O Supervisor gives the drive letters */
        pnp_mode = 1;
        core_flags |= CF_DISK | CF_PNP;
        blk_dosunits = 0;
    }
    for (i = 0; i < npci && !pnp_mode; i++) {
        struct pcidev *d = &pcis[i];
        if (d->cls == 8) {
            if (d->sub == 5 && d->prog <= 2) sdhci_probe(d);     /* SD host controller */
            continue;
        }
        if (d->cls != 1) continue;
        if (d->sub == 1 && !(cb->opts & OPT_NOIDE)) {
            ide_probe(d);
            nide++;
        }
        if (is_ahci(d) && !(cb->opts & OPT_NOAHCI) && ahci_probe(d) == 0) nahci++;
        if (d->sub == 8 && d->prog == 2 && !(cb->opts & OPT_NONVME)) {
            nvme_probe(d);
            nnvme++;
        }
    }
    if (!(cb->opts & OPT_NOIDE) && !pnp_mode) ide_probe_legacy(!nide);
    if (!pnp_mode) sdhci_probe_acpi();  /* SD host controllers without a PCI function */
    /* SD slots get drive letters even without /R:n */
    if (sd_count() && !(cb->slotreq & 0x7F)) cb->slotreq = (u8)(sd_count() | (cb->slotreq & 0x80));
    if (plat_mode == PM_DOS) {
        if (p->mbr_lin) match_bios_disks((u8 *)p->mbr_lin);
        ide_claim();
    }
    core_flags |= CF_DISK;
    if (plat_mode == PM_DOS && blk_count()) cb->flags |= CBF_HAVEDISK;
#elif defined(WIN_AUDIO)
    if (p->flags & CPF_PNP) {
        aeng_init();
        core_flags |= CF_AUDIO | CF_PNP;
        pnp_mode = 1;
        sst_probe(pcis, npci);              /* an ACPI device: the Configuration Manager has none */
        if (adev_count()) cb->flags |= CBF_HAVEDISK;
        sb_start(1);
    } else {
        if (plat_mode == PM_DOS && !(cb->opts2 & OPT2_NODSP)) {
            /* firmware of the Intel audio DSPs: WINAUDIO.SYS loads the files into XMS (also
               for the VxD of Windows 3.x) and calls ent_filedone */
            dsp_fw_prefetch(pcis, npci);
            if (cb->flags & CBF_FWREQ) {
                fw_wait = 1;
                return 0;
            }
        }
        if (plat_mode == PM_VXD || (cb->opts & OPT_USB)) audio_start();
    }
#else
    if (p->flags & CPF_PNP) {
        /* Windows 95/98/ME: the controllers come one by one from the Configuration Manager */
        core_flags |= CF_USB | CF_AUDIO | CF_I2C | CF_PNP;
        blk_dosunits = 0;
        aeng_init();
        usb_init();
        pnp_mode = 1;
        i2c_start(pcis, 0);                 /* touchpads on ACPI-only I2C controllers (no PCI function) */
    } else if (plat_mode == PM_VXD || (cb->opts & OPT_USB)) {
        core_flags |= CF_USB | CF_AUDIO;
        aeng_init();                    /* USB Audio devices */
        usb_init();
        usb_start_all(pcis, npci);
        i2c_start(pcis, npci);              /* HID over I2C touchpads */
        core_flags |= CF_I2C;
        tbt_start(pcis, npci);              /* Thunderbolt / USB4: tunnels, NVMe and xHCI behind them */
        pcihp_boot();
        if (plat_mode == PM_DOS && cb->nunits == 0 && !(cb->opts2 & OPT2_NODISK)) usb_settle(3000);
    }
#endif
#ifndef WIN_AUDIO
    if (plat_mode == PM_DOS) blk_scan_fixed();
#endif
#ifdef WIN_DISK
    if (plat_mode == PM_DOS && !cb->nfixed) {
        plat_screen(1);
        blk_explain(nide, nahci, nnvme);
        plat_screen((cb->opts & OPT_SCREEN) != 0);
    }
#endif
    core_flags |= CF_STARTED;
#ifdef WIN_AUDIO
    kprintf(CORENAME " ready: %d sound device(s)\n", adev_count());
#else
    if (cb->nunits)
        kprintf(CORENAME " ready: %d disk(s), %u unit(s) from %c:\n", blk_count(), cb->nunits, 'A' + cb->firstdrive);
    else
        kprintf(CORENAME " ready: %d disk(s)\n", blk_count());
#endif
    return 0;
}

/* ---- Plug and Play (Windows 95/98/ME): the Configuration Manager hands the PCI functions
   over one by one. Started together so that companion controllers (UHCI/OHCI next to EHCI,
   xHCI port routing, shared IRQs) see each other. ---- */
static struct pcidev *pnp_find(u32 bdf)
{
    int i;
    for (i = 0; i < npci; i++)
        if (PCI_BDF(&pcis[i]) == bdf) return &pcis[i];
    /* a function that was not there at boot (docked, tunnelled) */
    if (npci < (int)ARRAY_SIZE(pcis) && pci_fill(&pcis[npci], (int)(bdf >> 8), (int)((bdf >> 3) & 0x1F), (int)(bdf & 7)) == 0)
        return &pcis[npci++];
    return NULL;
}

#if defined(WIN_DISK) || defined(WIN_USB)
/* controllers started at the boot of Windows 95/98/ME before the Configuration Manager hands
   them over (core_pnp_start_boot): the hand-over finds them running */
static u32 early_bdf[16];
static int nearly;

static int early_started(u32 bdf)
{
    int i;
    for (i = 0; i < nearly; i++)
        if (early_bdf[i] == bdf) return 1;
    return 0;
}
#endif

int core_pnp_start(const u32 *bdfs, int n)
{
    struct pcidev sub[32];
    int i, m = 0, started = 0;
    if (!pnp_mode) return -1;
    for (i = 0; i < n && m < (int)ARRAY_SIZE(sub); i++) {
        struct pcidev *d = pnp_find(bdfs[i]);
        if (!d) continue;
#if defined(WIN_DISK) || defined(WIN_USB)
        if (early_started(bdfs[i])) {
            started++;
            continue;
        }
#endif
        /* the Configuration Manager may have moved the BARs since the boot scan */
        pci_fill(d, d->bus, d->dev, d->fn);
        sub[m++] = *d;
    }
    if (!m) return 0;
#ifdef WIN_USB
    {
        int husb = 0, hi2c = 0, hnhi = 0;
        for (i = 0; i < m; i++) {
            struct pcidev *d = &sub[i];
            if (d->cls == 0x0C && d->sub == 0x03) husb++;
            else if (d->cls == 0x0C && d->sub == 0x80) hi2c++;
            else if (d->cls == 0x01 && d->sub == 0x08 && d->prog == 0x02) {
                /* the Configuration Manager gave us this controller: the BIOS keeps nothing */
                u32 save = cb->opts;
                cb->opts |= OPT_NVME;
                if (nvme_probe_hot(d, 0) == 0) started++;
                cb->opts = save;
            } else hnhi++;
        }
        if (husb) {
            usb_pci_list(pcis, npci);
            usb_start_all(sub, m);
            started += husb;
            /* BIOS drives on USB (Windows 95/98/ME): their disks set up before the I/O
               Supervisor checks the real-mode drives through INT 13h */
            if (cb->nbios) usb_settle(3000);
        }
        if (hi2c) {
            i2c_start(sub, m);
            started += hi2c;
        }
        if (hnhi) {
            /* the hot-plug code wants the PCI bridges too */
            for (i = 0; i < npci && m < (int)ARRAY_SIZE(sub); i++)
                if (pcis[i].cls == 6) sub[m++] = pcis[i];
            tbt_start(sub, m);
            started += hnhi;
        }
    }
#elif defined(WIN_DISK)
    for (i = 0; i < m; i++) {
        struct pcidev *d = &sub[i];
        if (d->cls == 8) {
            if (d->sub == 5 && d->prog <= 2 && sdhci_probe(d) == 0) started++;
            continue;
        }
        if (d->cls != 1) continue;
        if (d->sub == 1) {
            if (!(cb->opts & OPT_NOIDE) && ide_probe(d) == 0) started++;
        } else if (is_ahci(d)) {
            if (!(cb->opts & OPT_NOAHCI) && ahci_probe(d) == 0) started++;
        } else if (d->sub == 8 && d->prog == 2 && !(cb->opts & OPT_NONVME)) {
            /* the Configuration Manager gave us the controller: a BIOS drive on it gets its
               INT 13h transfers from the VxD once the controller is ours */
            u32 save = cb->opts;
            cb->opts |= OPT_NVME;
            if (nvme_probe(d) == 0) started++;
            cb->opts = save;
        }
    }
    if (blk_count()) cb->flags |= CBF_HAVEDISK;
#else
    for (i = 0; i < m; i++) {
        struct pcidev *d = &sub[i];
        if (d->cls != 4 || sst_pci(d)) continue;
        if (d->sub == 3 && !(cb->opts & OPT_NOHDA)) {
            if (hda_probe(d) == 0) started++;
            continue;
        }
        if (d->sub == 1 || d->sub == 0x80) {
            if (d->vendor == 0x8086 && pci_bar_mem(d, 0, NULL) && !(cb->opts & OPT_NOHDA) && hda_probe(d) == 0) {
                started++;
                continue;
            }
            if (d->sub == 1 && !(cb->opts & OPT_NOAC97) && ac97_probe(d) == 0) started++;
        }
    }
    if (adev_count()) cb->flags |= CBF_HAVEDISK;
#endif
    return started;
}

void core_pnp_stop(u32 bdf)
{
    struct pcidev *d = pnp_find(bdf);
    if (!pnp_mode || !d) return;
#ifdef WIN_USB
    if (d->cls == 0x0C && d->sub == 0x03) usb_hotunplug_hc(bdf);
    else if (d->cls == 0x0C && d->sub == 0x80) i2c_shutdown();
    else if (d->cls == 0x01 && d->sub == 0x08) nvme_remove(bdf);
    else tbt_shutdown();
#elif defined(WIN_DISK)
    if (d->cls == 1 && d->sub == 1) ide_remove(bdf);
    else if (d->cls == 1 && d->sub == 8) nvme_remove(bdf);
    else if (d->cls == 1) ahci_remove(bdf);
    else if (d->cls == 8) sdhci_remove(bdf);
#else
    hda_remove(bdf);
    ac97_remove(bdf);
#endif
}

#if defined(WIN_DISK) || defined(WIN_USB)
/* Windows 95/98/ME, the VxD loaded at the start (SYSTEM.INI device=): the controllers that
   can carry BIOS drives are started before the I/O Supervisor reads the BIOS drives, whose
   INT 13h transfers the VxD then does. WINDISK: AHCI, RAID as AHCI, NVMe, SD hosts of PCI and
   ACPI (IDE stays with the BIOS and Windows' or our IDE driver); WINUSB: the USB host
   controllers, their mass storage devices set up before this returns. */
int core_pnp_start_boot(void)
{
    u32 bdfs[16];
    int i, n = 0, r;
    if (!pnp_mode) return -1;
    for (i = 0; i < npci && n < (int)ARRAY_SIZE(bdfs); i++) {
        struct pcidev *d = &pcis[i];
#ifdef WIN_USB
        if (d->cls == 0x0C && d->sub == 0x03 && d->prog != 0xFE)
#else
        if ((d->cls == 1 && d->sub != 1 && (is_ahci(d) || (d->sub == 8 && d->prog == 2))) ||
            (d->cls == 8 && d->sub == 5 && d->prog <= 2))
#endif
        {
            /* only the controllers Windows has this driver installed for: the others belong
               to Windows' own drivers (UHCD / OPENHCI / USBEHCI, ESDI_506 ...), which start
               them too */
            if (!plat_pnp_ours(PCI_BDF(d))) {
                kprintf("pnp: %02x:%02x.%x has another driver, not started at boot\n", d->bus, d->dev, d->fn);
                continue;
            }
            bdfs[n++] = PCI_BDF(d);
        }
    }
    r = core_pnp_start(bdfs, n);
    for (i = 0; i < n && nearly < (int)ARRAY_SIZE(early_bdf); i++) early_bdf[nearly++] = bdfs[i];
#ifdef WIN_DISK
    if (sdhci_probe_acpi() > 0) r++;
    if (blk_count()) cb->flags |= CBF_HAVEDISK;
#endif
    return r;
}
#endif

#ifdef WIN_DISK
/* a controller of another enumerator (ACPI SD host): key stands in for its PCI location */
int core_pnp_start_mmio(u32 key, u32 phys, u32 len, int irq)
{
    if (!pnp_mode) return -1;
    return sdhci_probe_mmio(phys, len, irq, key) == 0 ? 1 : 0;
}

void core_pnp_stop_mmio(u32 key)
{
    if (pnp_mode) sdhci_remove(key);
}
#endif

int core_dos(u8 *pkt)
{
    return blk_dos_request(pkt);
}

void core_poll(void)
{
    if (!(core_flags & CF_STARTED)) return;
#ifdef WIN_USB
    if (core_flags & CF_USB) {
        usb_poll();
        tbt_poll();
    }
    if (core_flags & CF_I2C) i2c_poll();
    smbtp_poll();
    ptp_tick();
    if (plat_mode == PM_DOS) plat_aux_poll();
#endif
#ifdef WIN_AUDIO
    if (core_flags & CF_AUDIO) aeng_poll();
#endif
}

int core_irq(int irq)
{
#ifdef WIN_USB
    /* DOS: IRQ 12 of the PS/2 touchpad (2: served, no poll needed) */
    int aux = irq == 12 && plat_mode == PM_DOS && plat_aux_irq() ? 2 : 0;
    if (!(core_flags & CF_USB)) return aux;
    return usb_irq(irq) ? 1 : aux;
#else
    (void)irq;
    return 0;
#endif
}

/* hand hardware over to another owner (DOS core -> VxD, VxD -> DOS core) */
void core_shutdown(int how)
{
    (void)how;
    if (!(core_flags & CF_STARTED)) return;
    kprintf(CORENAME " shutdown\n");
#ifdef WIN_USB
    if (core_flags & CF_AUDIO) aeng_shutdown();
    if (core_flags & CF_I2C) i2c_shutdown();
    if (core_flags & CF_USB) {
        tbt_shutdown();
        usb_shutdown();
    }
    blk_flush_all();
    nvme_shutdown();
#elif defined(WIN_AUDIO)
    if (core_flags & CF_AUDIO) aeng_shutdown();
    dsp_shutdown_all();
    core_flags &= ~CF_AUDIO;
#else
    blk_flush_all();
    ide_shutdown();
    ahci_shutdown();
    nvme_shutdown();
    sdhci_shutdown();
#endif
}

/* DOS core: take the hardware back after Windows exits */
void core_resume(void)
{
    kprintf(CORENAME " resume\n");
    blk_rebind_units();
#ifdef WIN_DISK
    ide_resume();
    ahci_resume();
    nvme_resume();
    sdhci_resume();
#elif defined(WIN_AUDIO)
    if (cb->opts & OPT_USB) audio_start();
#else
    if (core_flags & CF_USB) {
        aeng_init();
        usb_init();
        usb_start_all(pcis, npci);
        i2c_start(pcis, npci);              /* HID over I2C touchpads */
        core_flags |= CF_I2C;
        nvme_resume();
        tbt_start(pcis, npci);
    }
#endif
}

static int api_disks(u8 *buf, u32 len, u32 *ret)
{
    int i, n = blk_count();
    struct windiskinfo *di = (struct windiskinfo *)buf;
    *ret = 0;
    for (i = 0; i < n; i++) {
        struct blkdev *d = blk_get(i);
        if ((i + 1) * sizeof(*di) > len) return WINE_SMALL;
        memset(di, 0, sizeof(*di));
        di->index = (u8)i;
        di->kind = d->kind;
        di->present = d->present;
        di->shared = d->shared;
        di->sectors_lo = (u32)d->sectors;
        di->sectors_hi = (u32)(d->sectors >> 32);
        di->ssize = d->ssize;
        di->key = d->key;
        di->reads = d->reads;
        di->writes = d->writes;
        di->errors = d->errors;
        di->usbdev = d->owner;
        strlcpy(di->model, d->model, sizeof(di->model));
        strlcpy(di->serial, d->serial, sizeof(di->serial));
        blk_find_unit_letter(d, di->letters, sizeof(di->letters));
        di++;
        *ret += sizeof(*di);
    }
    return WINE_OK;
}

int core_api(struct winapireq *r)
{
    u8 *buf = r->buf ? plat_rm(r->buf) : NULL;
    r->retlen = 0;
    switch (r->fn) {
    case WINF_INFO:
    {
        struct wininfo *in = (struct wininfo *)buf;
        if (r->buflen < sizeof(*in)) return WINE_SMALL;
        memset(in, 0, sizeof(*in));
        in->version = WIN_VERSION;
        in->mode = (u16)plat_mode;
        in->flags = core_flags;
        in->ndisks = (u8)blk_count();
        in->nunits = cb->nunits;
        in->firstdrive = cb->firstdrive;
#if defined(WIN_DISK) || defined(WIN_USB)
        in->ncd = (u8)cd_count();
#endif
        in->poolfree = pool_free_bytes();
        in->uptime_ms = plat_ms() - start_ms;
        strlcpy(in->build, "WINDRV 1.1 " __DATE__, sizeof(in->build));
        r->retlen = sizeof(*in);
        return WINE_OK;
    }
    case WINF_DISKS:
        return api_disks(buf, r->buflen, &r->retlen);
#if defined(WIN_DISK) || defined(WIN_USB)
    case WINF_BIOSMAP:
        if (!buf || r->buflen < sizeof(struct winbiosmap)) return WINE_SMALL;
        r->retlen = sizeof(struct winbiosmap);
        return api_biosmap((struct winbiosmap *)buf);
#endif
#if defined(WIN_DISK) || defined(WIN_USB)
    case WINF_CDREQ:
        /* request header of WINCD.SYS / WINUCD.SYS, the DOS CD-ROM devices for MSCDEX */
        if (!buf) return WINE_SMALL;
#ifdef WIN_USB
        /* DOS: a drive plugged in while DOS waited (critical error prompt, no polls) */
        if (plat_mode == PM_DOS && !cd_unit(buf[1])) {
            usb_poll();
            if (usb_pending()) usb_settle(3000);
        }
#endif
        cd_dos_request(buf);
        return WINE_OK;
#endif
    case WINF_UNITS:
    {
        u32 n = (u32)cb->nunits * sizeof(struct winunit);
        if (!buf || r->buflen < n) return WINE_SMALL;
        memcpy(buf, cb->units, n);
        r->retlen = n;
        return WINE_OK;
    }
    case WINF_RAWREAD:
    {
        struct blkdev *d = blk_get((int)r->arg0);
        u32 cnt = r->arg2 & 0xFFFF;
        int rc;
        if (!d) return WINE_NODEV;
        if (!d->present) return WINE_NODEV;
        if (cnt > d->maxxfer) cnt = d->maxxfer;
        if (cnt * d->ssize > BOUNCE_SIZE) cnt = BOUNCE_SIZE / d->ssize;
        rc = d->ops->rw(d, r->arg1, cnt, bounce, 0);
        if (rc) return WINE_IO;
        d->reads += cnt;
        if (buf && !(r->arg2 & 0x80000000UL)) {
            u32 n = MIN(cnt * d->ssize, r->buflen);
            memcpy(buf, bounce, n);
            r->retlen = n;
        } else {
            r->retlen = cnt * d->ssize;
        }
        return WINE_OK;
    }
    case WINF_RAWIO:
    {
        /* arg0 disk | 80000000h write, arg1:arg2 LBA, buflen bytes at buf (whole sectors) */
        struct blkdev *d = blk_get((int)(r->arg0 & 0xFFFF));
        int wr = (r->arg0 & 0x80000000UL) != 0, rc;
        u64 lba = ((u64)r->arg2 << 32) | r->arg1;
        u32 cnt;
        if (!d || !d->present || d->cdrom) return WINE_NODEV;
        if (!buf || !r->buflen || r->buflen % d->ssize) return WINE_SMALL;
        cnt = r->buflen / d->ssize;
        if (cnt > d->maxxfer) cnt = d->maxxfer;
        if (cnt * d->ssize > BOUNCE_SIZE) cnt = BOUNCE_SIZE / d->ssize;
        if (lba >= d->sectors || cnt > d->sectors - lba) return WINE_IO;
        if (wr) {
            if (d->wprot) return WINE_IO;
            memcpy(bounce, buf, cnt * d->ssize);
        }
        rc = d->ops->rw(d, lba, cnt, bounce, wr);
        if (rc) {
            d->errors++;
            return WINE_IO;
        }
        if (wr) {
            d->writes += cnt;
        } else {
            d->reads += cnt;
            memcpy(buf, bounce, cnt * d->ssize);
        }
        r->retlen = cnt * d->ssize;
        return WINE_OK;
    }
    case WINF_LOG:
        if (!buf) return WINE_SMALL;
        log_copy((char *)buf, r->buflen, &r->retlen);
        return WINE_OK;
    case WINF_FLUSH:
        blk_flush_all();
        return WINE_OK;
#ifdef WIN_USB
    case WINF_USBTREE:
    case WINF_USBDESC:
    case WINF_USBEJECT:
    case WINF_SETOPT:
        if (!(core_flags & CF_USB)) return WINE_NOCORE;
        return usb_api(r, buf);
    case WINF_TBTREE:
        if (!(core_flags & CF_USB)) return WINE_NOCORE;
        if (!buf) return WINE_SMALL;
        return tbt_api_tree(buf, r->buflen, &r->retlen);
    case WINF_CAM_DEVS:
    case WINF_CAM_MODES:
    case WINF_CAM_START:
    case WINF_CAM_STOP:
    case WINF_CAM_FRAME:
        if (!(core_flags & CF_USB)) return WINE_NOCORE;
        return uvc_api(r, buf);
#endif
#if defined(WIN_AUDIO) || defined(WIN_USB)
    case WINF_AUD_DEVS:
    case WINF_AUD_OPEN:
    case WINF_AUD_CTL:
    case WINF_AUD_MIX:
    case WINF_AUD_MIDI:
#ifdef WIN_AUDIO
    case WINF_AUD_SB:
        if (r->fn == WINF_AUD_SB) {
            u32 n = MIN(r->buflen, (u32)sizeof(struct winsb));
            if (!buf) return WINE_SMALL;
            memcpy(buf, (u8 *)cb + WIN_SBOFF, n);
            r->retlen = n;
            return WINE_OK;
        }
#endif
        if (!(core_flags & CF_AUDIO)) return WINE_NOCORE;
        if (r->fn == WINF_AUD_DEVS && !buf) return WINE_SMALL;
        return aeng_api(r, buf);
#endif
    }
    return WINE_BADFN;
}
