/* core.h - WINDRV portable 32-bit driver core (DOS PM and VxD builds) */
#ifndef CORE_H
#define CORE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef signed short s16;
typedef signed long s32;
typedef signed long long s64;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#include "winapi.h"

/* ---- port I/O and CPU helpers ---- */
u8 inb(u16 port);
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
u16 inw(u16 port);
#pragma aux inw = "in ax,dx" parm [dx] value [ax] modify exact [ax];
u32 inl(u16 port);
#pragma aux inl = "in eax,dx" parm [dx] value [eax] modify exact [eax];
void outb(u16 port, u8 v);
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
void outw(u16 port, u16 v);
#pragma aux outw = "out dx,ax" parm [dx] [ax] modify exact [];
void outl(u16 port, u32 v);
#pragma aux outl = "out dx,eax" parm [dx] [eax] modify exact [];
u32 irq_save(void);
#pragma aux irq_save = "pushfd" "pop eax" "cli" value [eax] modify exact [eax];
void irq_restore(u32 f);
#pragma aux irq_restore = "push eax" "popfd" parm [eax] modify exact [];
void cpu_pause(void);
#pragma aux cpu_pause = "db 0F3h, 90h" modify exact [];
u32 cpu_features(void);         /* CPUID 1 EDX (0 without CPUID leaf 1) */
#pragma aux cpu_features = "xor eax,eax" "cpuid" "test eax,eax" "jz n" "mov eax,1" "cpuid" "mov eax,edx" "jmp x" \
    "n: xor eax,eax" "x:" value [eax] modify exact [eax ebx ecx edx];
u32 cpu_rdmsr(u32 msr);          /* low dword of a model specific register */
#pragma aux cpu_rdmsr = "db 0Fh, 32h" parm [ecx] value [eax] modify exact [eax edx];
void cpu_wbinvd(void);
#pragma aux cpu_wbinvd = "wbinvd" modify exact [];
void cpu_clflush(void *p);
#pragma aux cpu_clflush = "db 0Fh, 0AEh, 38h" parm [eax] modify exact [];

#define RD32(p) (*(volatile u32 *)(p))
#define WR32(p, v) (*(volatile u32 *)(p) = (u32)(v))
#define RD16(p) (*(volatile u16 *)(p))
#define WR16(p, v) (*(volatile u16 *)(p) = (u16)(v))
#define RD8(p) (*(volatile u8 *)(p))
#define WR8(p, v) (*(volatile u8 *)(p) = (u8)(v))

/* ---- platform services (platdos.c / platvxd.c) ---- */
void plat_putc(char c);
void plat_screen(int on);
void plat_mouse_abs(int x, int y, int dz, int dw, int buttons);   /* x, y 0..65535 */
void plat_irq_detach(int irq);  /* stop using an IRQ (masked again) */       /* DOS: log to the text screen too */
u32 plat_ms(void);
void plat_udelay(u32 us);
void *plat_mmio(u32 phys, u32 len);
void *plat_rm(u32 segoff);          /* real-mode seg:off -> linear in current context */
void *plat_sysrm(u32 segoff);       /* seg:off in the system VM (rings of WINSOUND.DRV) */
void plat_key(const u8 *sc, int n);  /* deliver set-1 scan code bytes */
void plat_mouse(int dx, int dy, int dz, int dw, int buttons);
int plat_irq_attach(int irq);        /* returns 0 if attached */
void plat_kick(void);                /* ask platform to call core_poll soon */
void plat_unit_changed(int unit);
int plat_aux_irq(void);              /* DOS: aux bytes WINUSB.SYS caught on IRQ 12 (PS/2 touchpad) */
void plat_aux_poll(void);            /* DOS: the PS/2 touchpad after BIOS calls, Windows, resets */
extern u32 plat_mode;                /* PM_DOS / PM_VXD */
#define PM_DOS 1
#define PM_VXD 2

/* ---- library (winlib.c) ---- */
void *memset(void *d, int c, u32 n);
void *memcpy(void *d, const void *s, u32 n);
int memcmp(const void *a, const void *b, u32 n);
u32 strlen(const char *s);
char *strcpy(char *d, const char *s);
void strlcpy(char *d, const char *s, u32 n);
void strlcat(char *d, const char *s, u32 n);
int strncmp(const char *a, const char *b, u32 n);
int ksnprintf(char *buf, u32 n, const char *fmt, ...);
void kprintf(const char *fmt, ...);
void log_init(void);
void mdelay(u32 ms);
void udelay(u32 us);
u32 crc32(u32 crc, const void *p, u32 n);
void trim_ata_string(char *dst, const u16 *src, int words);
void trim_string(char *s);

/* DMA-capable pool (all core memory comes from it) */
void pool_init(u32 lin, u32 phys, u32 size);
void *kmalloc(u32 size);                /* zeroed, 16-byte aligned */
void *kmalloc_align(u32 size, u32 align); /* zeroed, aligned, no 4K crossing if size<=4K and align>=size */
void kfree(void *p);
u32 vphys(const void *p);
void *pvirt(u32 phys);
u32 pool_free_bytes(void);

/* ---- debug ---- */
extern u32 dbg_level;
#define DBG(...) do { if (dbg_level) kprintf(__VA_ARGS__); } while (0)
#define DBG2(...) do { if (dbg_level > 1) kprintf(__VA_ARGS__); } while (0)

/* ---- PCI (pci.c) ---- */
struct pcidev {
    u8 bus, dev, fn, irq;
    u16 vendor, device;
    u8 cls, sub, prog, rev;
    u32 bar[6];
};
u32 pci_rd32(int bus, int dev, int fn, int off);
void pci_wr32(int bus, int dev, int fn, int off, u32 v);
u16 pci_rd16(int bus, int dev, int fn, int off);
void pci_wr16(int bus, int dev, int fn, int off, u16 v);
u8 pci_rd8(int bus, int dev, int fn, int off);
void pci_wr8(int bus, int dev, int fn, int off, u8 v);
int pci_scan(struct pcidev *out, int max);
int pci_fill(struct pcidev *out, int bus, int dev, int fn);
u32 pci_bar_mem(struct pcidev *p, int bar, u32 *size);
u32 pci_bar_io(struct pcidev *p, int bar);
void pci_intx(struct pcidev *p, int on);
int pci_find_cap(struct pcidev *p, u8 id);
void pci_enable(struct pcidev *p, int mem, int io, int master);
void pci_relocate_all(int all);     /* start: 64-bit BARs above 4G moved below (all: also other devices, bridge windows) */
void *acpi_table(const char *sig, int idx);     /* acpi.c */
#define PCI_BDF(p) (((u32)(p)->bus << 8) | ((p)->dev << 3) | (p)->fn)

/* ---- block layer (blk.c) ---- */
struct blkdev;
struct blkops {
    int (*rw)(struct blkdev *d, u64 lba, u32 cnt, void *buf, int write); /* buf in pool, cnt <= maxxfer */
    int (*flush)(struct blkdev *d);
    int (*ready)(struct blkdev *d);   /* 0 = ready */
};
struct blkdev {
    struct blkops *ops;
    void *priv;
    u32 key;            /* identity (controller bdf <<16 | port/nsid), USB: hash */
    u32 key2;
    u64 sectors;        /* in device sectors */
    u32 ssize;          /* device sector size */
    u32 maxxfer;        /* max sectors per rw call */
    u8 kind;            /* DK_* */
    u8 removable;
    u8 present;
    u8 wprot;
    u8 shared;          /* also used by BIOS INT 13h */
    u8 index;
    u16 owner;          /* USB device id of a USB disk, 0 otherwise */
    u8 cdrom;           /* CD/DVD drive (cdrom.c): no drive letters, DOS CD-ROM device */
    char model[41];
    char serial[21];
    u32 errors;
    u32 reads, writes;   /* sector counters */
};
#define DK_AHCI 1
#define DK_NVME 2
#define DK_USB  3
#define DK_IDE  4
#define DK_SD   5           /* SD card (SD host controller) */
#define DK_MMC  6           /* MMC card or eMMC device (SD host controller) */

int blk_register(struct blkdev *d);       /* add a disk; hotplug-aware */
void blk_unregister(struct blkdev *d);
int blk_has_bpb(struct blkdev *d, const u8 *bpb);   /* a FAT volume with this DOS BPB */
int blk_read_abs(struct blkdev *d, u64 lba512, u32 cnt512, void *buf);
int blk_write_abs(struct blkdev *d, u64 lba512, u32 cnt512, const void *buf);
int blk_dos_request(u8 *pkt);
void blk_init(void);
void blk_explain(int nide, int nahci, int nnvme);
#define CORENAME_STR "WINDISK"
void blk_scan_fixed(void);
int blk_count(void);
struct blkdev *blk_get(int i);
void blk_flush_all(void);
int blk_find_unit_letter(struct blkdev *d, char *letters, int max);
extern u8 *bounce;          /* 64K+ bounce buffer */
extern void (*blk_poll_hook)(void);   /* removable media without hot-plug events (SD slots) */
#define BOUNCE_SIZE 0x10000UL

/* ---- CD/DVD drives (cdrom.c) ----
   packet: one ATAPI/SCSI command of 12 bytes; data (len bytes, pool memory) flows to the
   host. Returns 0, 1 = check condition (sense data wanted), -1 = transport error. */
typedef int (*cd_packet_fn)(void *priv, const u8 *cdb, void *buf, u32 len, u32 ms);
struct blkdev *cd_register(const char *model, const char *serial, u8 kind, u32 key, cd_packet_fn packet, void *priv);
void cd_unregister(struct blkdev *d);
int cd_count(void);
struct blkdev *cd_unit(int u);
int cd_dos_request(u8 *pkt);
int cd_media(struct blkdev *d);     /* Windows 9x port driver: bit 0 medium, bits 8-15 changes */
int cd_command(struct blkdev *d, const u8 *cdb, void *buf, u32 len, u8 *sense);

/* ---- disk controllers ---- */
int ahci_probe(struct pcidev *p);
int sdhci_probe(struct pcidev *p);
int sdhci_probe_acpi(void);         /* SD host controllers enumerated by ACPI only */
int sdhci_probe_mmio(u32 phys, u32 len, int irq, u32 key);   /* ACPI host given by Windows 9x */
int sd_count(void);                 /* removable slots (drive letters for cards) */
void sdhci_shutdown(void);
void sdhci_resume(void);
void sdhci_remove(u32 key);         /* PCI function (bdf) or sdhci_probe_mmio key goes away */
void sdhci_poll(void);              /* card changes in removable slots */
struct sdinfo {                     /* sd_slot_info: one slot of an SD host controller */
    u32 bdf;                        /* PCI function (bus << 8 | dev << 3 | fn), 0xFFFFFFFF: ACPI device */
    u32 mmio;                       /* register base */
    u32 irq;                        /* ACPI: first Interrupt () of _CRS; PCI: interrupt line; 0 unknown */
    u32 uid;                        /* ACPI _UID */
    char hid[9];                    /* ACPI _HID */
    char path[5];                   /* ACPI device name segment */
    u8 slot;                        /* slot number of the controller */
    u8 fixed;                       /* non-removable: the card is a fixed disk */
    u8 borrow;                      /* card initialized and used by the BIOS too */
    u8 mmc;                         /* MMC / eMMC */
    struct blkdev *disk;            /* the card, NULL without one */
};
int sd_slots(void);
int sd_slot_info(int i, struct sdinfo *o);
int ide_probe(struct pcidev *p);
void ide_probe_legacy(int all);
void ide_claim(void);
void ide_shutdown(void);
void ide_resume(void);
int nvme_probe(struct pcidev *p);
void ahci_shutdown(void);
void nvme_shutdown(void);
int ide_dev_info(u32 key, u32 *bdf, int *chan, int *native);    /* IDE device -> controller */
void ide_remove(u32 bdf);           /* Plug and Play stop: drop the controller's disks */
void ahci_remove(u32 bdf);
void nvme_remove(u32 bdf);

/* ---- USB (usb.c etc.) ---- */
void usb_init(void);
void usb_settle(u32 max_ms);         /* DOS start: until the boot-time devices are set up */
int usb_pending(void);
void usb_poll(void);
int usb_irq(int irq);
void usb_shutdown(void);
void usb_start_all(struct pcidev *list, int n);
void i2c_start(struct pcidev *pcis, int n);  /* i2c.c: HID over I2C */
void i2c_poll(void);
void i2c_shutdown(void);

/* ---- input (input.c) ---- */
void kbd_usage_event(u8 mods, const u8 *keys, int nkeys, void *state);
void input_poll(void);
extern u8 kbd_leds;     /* bit0 scroll, bit1 num, bit2 caps (USB order: num=1, caps=2, scroll=4) */

/* ---- core entry (corent.c) ---- */
extern struct wincb *cb;
extern u32 core_flags;
#define CF_USB      0x0001
#define CF_DISK     0x0002
#define CF_STARTED  0x0004
#define CF_NOAHCI   0x0010
#define CF_NONVME   0x0020
#define CF_ALLDISKS 0x0040
#define CF_NOHID    0x0080
#define CF_NOMSC    0x0100
#define CF_AUDIO    0x0200
#define CF_I2C      0x0400
#define CF_PNP      0x0800          /* Windows 95/98/ME: controllers come from the Configuration Manager */

int core_init(struct core_params *p);
#define CPF_PNP 1                   /* core_params.flags: controllers come from core_pnp_start */
int core_pnp_start(const u32 *bdfs, int n);     /* PCI functions (bus << 8 | dev << 3 | fn) */
int core_pnp_start_boot(void);                  /* static VxD of Windows 9x: controllers of the BIOS drives now */
void core_pnp_stop(u32 bdf);
int core_pnp_start_mmio(u32 key, u32 phys, u32 len, int irq);  /* ACPI devnode: MMIO window */
void core_pnp_stop_mmio(u32 key);
int core_dos(u8 *pkt);
void core_poll(void);
int core_irq(int irq);
int core_api(struct winapireq *r);
void core_shutdown(int how);
int core_filedone(void);            /* WINAUDIO: firmware files loaded by the DOS driver */

/* ---- event log ring for USBTREE ---- */
void log_copy(char *dst, u32 max, u32 *outlen);

#endif
