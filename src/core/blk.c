/* blk.c - disk registry, partition scanning, DOS units and DOS device requests */
#include "core.h"

#define MAXDISKS 32
static struct blkdev *disks[MAXDISKS];
static int ndisks;
static u8 why[MAXDISKS];       /* blk_scan_fixed: why a fixed disk got no drive letter */
u8 *bounce;
static u8 *secbuf;              /* 8K scratch for partition/boot sector reads */
static int scanned;             /* fixed units assigned */

/* request header offsets */
#define RH_UNIT     1
#define RH_CMD      2
#define RH_STATUS   3
#define RH_MEDIA    13

#define ST_DONE     0x0100
#define ST_BUSY     0x0200
#define ST_ERR      0x8000
#define E_WP        0x00
#define E_UNIT      0x01
#define E_NOTREADY  0x02
#define E_BADCMD    0x03
#define E_SEEK      0x06
#define E_NOTFOUND  0x08
#define E_WRITE     0x0A
#define E_READ      0x0B
#define E_GENERAL   0x0C
#define E_BADCHG    0x0F

void blk_init(void)
{
    if (!bounce) bounce = kmalloc_align(BOUNCE_SIZE + 8192, 4096);
    if (!secbuf) secbuf = kmalloc_align(8192, 4096);
}

int blk_count(void) { return ndisks; }
struct blkdev *blk_get(int i) { return (i >= 0 && i < ndisks) ? disks[i] : NULL; }

/* ---- 512-byte sector view over any device sector size ---- */
static int dev_rw(struct blkdev *d, u64 lba, u32 cnt, void *buf, int wr)
{
    int r = -1, tries;
    if (!d->present) return -2;
    for (tries = 0; tries < 3; tries++) {
        r = d->ops->rw(d, lba, cnt, buf, wr);
        if (r == 0) break;
        if (r == -2) break;     /* no media / gone */
    }
    if (r) d->errors++;
    else if (wr) d->writes += cnt;
    else d->reads += cnt;
    return r;
}

/* read cnt512 512-byte sectors into buf (any address) */
int blk_read_abs(struct blkdev *d, u64 lba512, u32 cnt512, void *buf)
{
    u32 spd = d->ssize / 512;   /* 512-sectors per device sector */
    u8 *out = buf;
    if (spd <= 1) {
        while (cnt512) {
            u32 n = MIN(cnt512, MIN(d->maxxfer, BOUNCE_SIZE / 512));
            if (dev_rw(d, lba512, n, bounce, 0)) return -1;
            memcpy(out, bounce, n * 512);
            out += n * 512; lba512 += n; cnt512 -= n;
        }
        return 0;
    }
    while (cnt512) {
        u64 dl = lba512 / spd;
        u32 off = (u32)(lba512 % spd);
        u32 dn = (off + cnt512 + spd - 1) / spd;
        u32 take;
        dn = MIN(dn, MIN(d->maxxfer, BOUNCE_SIZE / d->ssize));
        if (dev_rw(d, dl, dn, bounce, 0)) return -1;
        take = MIN(cnt512, dn * spd - off);
        memcpy(out, bounce + off * 512, take * 512);
        out += take * 512; lba512 += take; cnt512 -= take;
    }
    return 0;
}

int blk_write_abs(struct blkdev *d, u64 lba512, u32 cnt512, const void *buf)
{
    u32 spd = d->ssize / 512;
    const u8 *in = buf;
    if (d->wprot) return -3;
    if (spd <= 1) {
        while (cnt512) {
            u32 n = MIN(cnt512, MIN(d->maxxfer, BOUNCE_SIZE / 512));
            memcpy(bounce, in, n * 512);
            if (dev_rw(d, lba512, n, bounce, 1)) return -1;
            in += n * 512; lba512 += n; cnt512 -= n;
        }
        return 0;
    }
    while (cnt512) {
        u64 dl = lba512 / spd;
        u32 off = (u32)(lba512 % spd);
        u32 dn = (off + cnt512 + spd - 1) / spd;
        u32 take;
        dn = MIN(dn, MIN(d->maxxfer, BOUNCE_SIZE / d->ssize));
        take = MIN(cnt512, dn * spd - off);
        /* read-modify-write for partial device sectors */
        if (off || (take % spd)) {
            if (dev_rw(d, dl, dn, bounce, 0)) return -1;
        }
        memcpy(bounce + off * 512, in, take * 512);
        if (dev_rw(d, dl, dn, bounce, 1)) return -1;
        in += take * 512; lba512 += take; cnt512 -= take;
    }
    return 0;
}

static u64 dev_sectors512(struct blkdev *d)
{
    return d->sectors * (d->ssize / 512);
}

/* partition start of a unit, 512-byte sectors */
static u64 unit_lba(const struct winunit *u)
{
    return u->lba | ((u64)u->lba_hi << 32);
}

/* ---- BPB handling ---- */
static int fat32_dos(void);
static int fat32_bs(const u8 *bs);

static int bpb_valid(const u8 *bs)
{
    u16 bps = bs[11] | (bs[12] << 8);
    u8 spc = bs[13];
    if (fat32_dos() && fat32_bs(bs) && ((bs[510] == 0x55 && bs[511] == 0xAA) || bs[0] == 0xEB || bs[0] == 0xE9))
        return 1;
    if (bs[510] != 0x55 || bs[511] != 0xAA) {
        /* some formatters omit the signature, accept a sane BPB anyway */
        if (bs[0] != 0xEB && bs[0] != 0xE9) return 0;
    }
    if (bps != 512) return 0;
    if (!spc || (spc & (spc - 1))) return 0;
    if (!(bs[14] | bs[15])) return 0;
    if (bs[16] < 1 || bs[16] > 2) return 0;
    if (!(bs[22] | bs[23])) return 0;       /* FAT12/16 only: sectors per FAT16 */
    return 1;
}

/* the full BPB of a unit (the one DOS gets): FAT32 needs 53 bytes */
static u8 *ubpbx(struct winunit *u)
{
    return (u8 *)cb + WIN_BPBXOFF + (u32)(u - cb->units) * WIN_BPBXSIZE;
}

/* FAT32 drives: MS-DOS 7.10 and later (FreeDOS with FAT32 reports 7.10 too) */
static int fat32_dos(void)
{
    return cb->dosver >= 0x070A && plat_mode == PM_DOS;
}

static int fat32_bs(const u8 *bs)
{
    return !(bs[22] | bs[23]) && !(bs[17] | bs[18]) && (bs[36] | bs[37] | bs[38] | bs[39]) &&
           bs[11] == 0 && bs[12] == 2 && bs[13] && !(bs[13] & (bs[13] - 1)) && (bs[16] == 1 || bs[16] == 2);
}

static void unit_fill_bpb(struct winunit *u, const u8 *bs)
{
    memcpy(u->bpb, bs + 11, 25);
    memset(ubpbx(u), 0, WIN_BPBXSIZE);
    memcpy(ubpbx(u), bs + 11, fat32_bs(bs) ? 53 : 25);
    if (fat32_bs(bs)) {
        u->media = bs[21];
        if (bs[66] == 0x29) {
            u->serial = bs[67] | ((u32)bs[68] << 8) | ((u32)bs[69] << 16) | ((u32)bs[70] << 24);
            memcpy(u->label, bs + 71, 11);
        } else {
            u->serial = 0;
            memcpy(u->label, "NO NAME    ", 11);
        }
        return;
    }
    u->media = bs[21];
    if (bs[38] == 0x29) {
        u->serial = bs[39] | ((u32)bs[40] << 8) | ((u32)bs[41] << 16) | ((u32)bs[42] << 24);
        memcpy(u->label, bs + 43, 11);
    } else {
        u->serial = 0;
        memcpy(u->label, "NO NAME    ", 11);
    }
}

/* default BPB for empty slots: 1.44M-like geometry is useless for big sticks, use a 32 MB FAT16 shape */
static const u8 default_bpb[25] = {
    0x00, 0x02, 0x04, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0xF8, 0x40, 0x00,
    0x3F, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00
};

int blk_dosunits = 1;            /* 0: Windows 95/98/ME Plug and Play, the I/O Supervisor gives the letters */
static int fat32_too;            /* blk_has_bpb: every volume (BIOS drive matching) */

/* file systems of the volumes (pent.fs) */
#define FS_NONE     0
#define FS_FAT      1           /* FAT12/16, FAT32 from MS-DOS 7.10: DOS mounts it */
#define FS_FAT32    2           /* FAT32 on an older DOS */
#define FS_NTFS     3
#define FS_EXFAT    4
#define FS_HPFS     5
#define FS_EXT      6
#define FS_HFS      7
static const char *const fsname[] = { "", "", "FAT32", "NTFS", "EXFAT", "HPFS", "LINUX", "HFS+" };

/* partition table entries that are not volumes */
static int is_ext_type(u8 t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static int is_fat_type(u8 t)
{
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0E || t == 0x0B || t == 0x0C;
}

/* the file system of the volume at 512-byte sector start; reads secbuf */
static int classify(struct blkdev *d, u64 start)
{
    if (blk_read_abs(d, start, 1, secbuf)) return FS_NONE;
    if (!memcmp(secbuf + 3, "NTFS    ", 8)) return FS_NTFS;
    if (!memcmp(secbuf + 3, "EXFAT   ", 8)) return FS_EXFAT;
    if (fat32_bs(secbuf) && ((secbuf[510] == 0x55 && secbuf[511] == 0xAA) || secbuf[0] == 0xEB || secbuf[0] == 0xE9))
        return fat32_dos() ? FS_FAT : FS_FAT32;
    if (bpb_valid(secbuf)) return FS_FAT;
    /* ext2/3/4 and HFS+ superblock at byte 1024, HPFS super block at sector 16 */
    if (!blk_read_abs(d, start + 2, 1, secbuf)) {
        if (secbuf[56] == 0x53 && secbuf[57] == 0xEF) return FS_EXT;
        if (secbuf[0] == 'H' && (secbuf[1] == '+' || secbuf[1] == 'X')) return FS_HFS;
    }
    if (!blk_read_abs(d, start + 16, 1, secbuf) && *(u32 *)secbuf == 0xF995E849UL) return FS_HPFS;
    return FS_NONE;
}

static int disk_index(struct blkdev *d)
{
    int i;
    for (i = 0; i < ndisks; i++) if (disks[i] == d) return i;
    return -1;
}

/* is this partition already served by DOS through INT 13h? */
static int bios_mounted(struct blkdev *d, u64 start)
{
    int i;
    if (!d->shared) return 0;
    if (cb->nmounted == 0xFF) return 1;     /* FreeDOS: its own drives, list unknown */
    for (i = 0; i < cb->nmounted && i < 16; i++) {
        if (cb->mounted[i].biosdrv - 0x80 == (int)d->shared - 1 && cb->mounted[i].start == start) return 1;
    }
    return 0;
}

struct pent { u8 type; u8 fs; u64 start; u32 size; };   /* 512-byte sectors (after scan_parts) */

/* collect the volumes of a disk: primaries first, then logical drives */
static int scan_parts_raw(struct blkdev *d, struct pent *pe, int max);

/* partition tables are written in device sectors: convert to 512-byte units, then find the
   file system of every partition. foreign: file systems DOS cannot mount stay in the list
   (they get a drive letter of their own on removable disks) */
static int scan_parts(struct blkdev *d, struct pent *pe, int max, int foreign)
{
    int n = scan_parts_raw(d, pe, max), i, k;
    u32 f = d->ssize / 512;
    if (n > 0 && f > 1) {
        for (i = 0; i < n; i++) {
            if (pe[i].start == 0 && pe[i].size == (u32)MIN(dev_sectors512(d), 0xFFFFFFFFUL)) continue;
            pe[i].start *= f;
            /* a FAT volume ends below 2^32 512-byte sectors */
            pe[i].size = (u64)pe[i].size * f > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : pe[i].size * f;
        }
    }
    for (i = k = 0; i < n; i++) {
        pe[i].fs = (u8)classify(d, pe[i].start);
        if (pe[i].fs == FS_NONE || (pe[i].fs != FS_FAT && !foreign && !fat32_too)) continue;
        pe[k++] = pe[i];
    }
    return n < 0 ? n : k;
}

static int scan_parts_raw(struct blkdev *d, struct pent *pe, int max)
{
    int n = 0, i, guard;
    u32 ext = 0, total = (u32)MIN(dev_sectors512(d), 0xFFFFFFFFUL);
    static u8 mbr[512];
    if (blk_read_abs(d, 0, 1, secbuf)) return -1;
    memcpy(mbr, secbuf, 512);
    /* superfloppy: a volume boot sector at LBA 0 (FAT, NTFS, exFAT) */
    if (!memcmp(mbr + 3, "NTFS    ", 8) || !memcmp(mbr + 3, "EXFAT   ", 8) ||
        ((bpb_valid(mbr) || fat32_bs(mbr)) && (mbr[0] == 0xEB || mbr[0] == 0xE9))) {
        u8 t = mbr[0x1C2];
        u32 s = mbr[0x1C6] | ((u32)mbr[0x1C7] << 8) | ((u32)mbr[0x1C8] << 16) | ((u32)mbr[0x1C9] << 24);
        if (!(mbr[510] == 0x55 && mbr[511] == 0xAA && is_fat_type(t) && s && s < total)) {
            pe[0].type = 0x06; pe[0].start = 0; pe[0].size = total;
            return 1;
        }
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    if (mbr[0x1C2] == 0xEE) {
        /* GPT protective MBR: every partition entry, anywhere on the disk (beyond 2 TB too);
           LBAs are in device sectors */
        u64 entlba, sec;
        u32 nent, esz, k;
        if (blk_read_abs(d, d->ssize / 512, 1, secbuf)) return n;
        if (memcmp(secbuf, "EFI PART", 8)) return n;
        entlba = *(u32 *)(secbuf + 72) | ((u64)*(u32 *)(secbuf + 76) << 32);
        nent = *(u32 *)(secbuf + 80);
        esz = *(u32 *)(secbuf + 84);
        if (esz < 128 || esz > 512) return n;
        for (k = 0; k < nent && k < 128 && n < max; k++) {
            u32 off = (k * esz) % 512;
            u8 *e;
            u64 first, last;
            sec = entlba * (d->ssize / 512) + (k * esz) / 512;
            if (off == 0 && blk_read_abs(d, sec, 1, secbuf)) break;
            e = secbuf + off;
            if (!(*(u32 *)e | *(u32 *)(e + 4) | *(u32 *)(e + 8) | *(u32 *)(e + 12))) continue;   /* unused */
            first = *(u32 *)(e + 32) | ((u64)*(u32 *)(e + 36) << 32);
            last = *(u32 *)(e + 40) | ((u64)*(u32 *)(e + 44) << 32);
            if (last < first || last - first >= 0xFFFFFFFFUL) continue;     /* too big for a letter */
            if ((first >> 32) >= 256 / (d->ssize / 512)) continue;          /* beyond 512 TB */
            pe[n].type = 0x0E; pe[n].start = first; pe[n].size = (u32)(last - first + 1);
            n++;
        }
        return n;
    }
    for (i = 0; i < 4 && n < max; i++) {
        u8 *e = mbr + 446 + i * 16;
        u32 s = *(u32 *)(e + 8), sz = *(u32 *)(e + 12);
        if (!e[4] || !sz) continue;
        if (is_ext_type(e[4])) {
            if (!ext) ext = s;
            continue;
        }
        pe[n].type = e[4]; pe[n].start = s; pe[n].size = sz; n++;
    }
    if (ext) {
        u32 cur = ext;
        for (guard = 0; guard < 64 && n < max; guard++) {
            u8 *e1, *e2;
            if (blk_read_abs(d, (u64)cur * (d->ssize / 512), 1, secbuf)) break;
            if (secbuf[510] != 0x55 || secbuf[511] != 0xAA) break;
            e1 = secbuf + 446;
            e2 = secbuf + 462;
            if (e1[4] && !is_ext_type(e1[4]) && *(u32 *)(e1 + 12)) {
                pe[n].type = e1[4];
                pe[n].start = cur + *(u32 *)(e1 + 8);
                pe[n].size = *(u32 *)(e1 + 12);
                n++;
            }
            if (is_ext_type(e2[4])) {
                u32 nx = ext + *(u32 *)(e2 + 8);
                if (nx == cur || !*(u32 *)(e2 + 8)) break;
                cur = nx;
            } else {
                break;
            }
        }
    }
    return n;
}

/* 1 if a FAT volume of d (FAT32 included) has the boot sector BPB of a DOS drive: bpb is the
   25-byte BPB of the DOS block device (bytes per sector .. total sectors), compared without
   the geometry and hidden sectors fields, which DOS may have adjusted */
/* a DOS drive (its BPB from IO.SYS) is on disk d: a partition with the same boot sector
   BPB, or a partition without a FAT boot sector (a drive DOS calls unformatted: UDF, a new
   partition) at the place or of the size the BPB names */
int blk_has_bpb(struct blkdev *d, const u8 *bpb)
{
    struct pent pe[24];
    int n, i;
    u32 hidden = bpb[17] | ((u32)bpb[18] << 8) | ((u32)bpb[19] << 16) | ((u32)bpb[20] << 24);
    u32 total = bpb[8] | ((u32)bpb[9] << 8);
    if (!total) total = bpb[21] | ((u32)bpb[22] << 8) | ((u32)bpb[23] << 16) | ((u32)bpb[24] << 24);
    fat32_too = 1;
    n = scan_parts(d, pe, 24, 1);
    fat32_too = 0;
    for (i = 0; i < n; i++) {
        u32 btot;
        if (blk_read_abs(d, pe[i].start, 1, secbuf)) continue;
        btot = secbuf[0x13] | ((u32)secbuf[0x14] << 8);
        if (!btot) btot = *(u32 *)(secbuf + 0x20);
        /* the same BPB (DOS may keep the total sectors in the other field) */
        if (secbuf[510] == 0x55 && secbuf[511] == 0xAA && !memcmp(secbuf + 0x0B, bpb, 8) &&
            !memcmp(secbuf + 0x15, bpb + 10, 3) && btot == total)
            return 1;
        if (!bpb_valid(secbuf) && !(secbuf[0x26] == 0x29 && secbuf[0x52] == 'F') &&
            (hidden == (u32)pe[i].start || (total && total == pe[i].size)))
            return 1;
    }
    return 0;
}

/* a volume DOS cannot mount (NTFS, exFAT, FAT32 before MS-DOS 7.10, ...) still gets its
   drive letter: DOS sees a tiny write-protected FAT12 volume labelled with the name of the
   file system, with nothing on it */
#define SYN_SECTORS 8
static void syn_mount(struct winunit *u, int fs)
{
    u8 *b = u->bpb;
    const char *nm = fsname[fs];
    int i;
    memset(b, 0, 25);
    b[1] = 2;                           /* 512 bytes per sector */
    b[2] = 1;                           /* 1 sector per cluster */
    b[3] = 1;                           /* 1 reserved sector */
    b[5] = 1;                           /* 1 FAT */
    b[6] = 16;                          /* 16 root directory entries */
    b[8] = SYN_SECTORS;
    b[10] = 0xF8;
    b[11] = 1;                          /* sectors per FAT */
    b[13] = 63;
    b[15] = 255;
    *(u32 *)(b + 17) = u->lba;
    memset(ubpbx(u), 0, WIN_BPBXSIZE);
    memcpy(ubpbx(u), b, 25);
    u->media = 0xF8;
    u->serial = u->lba ^ u->key;
    for (i = 0; i < 11; i++) u->label[i] = *nm ? *nm++ : ' ';
    u->state |= US_FOREIGN;
}

/* sector n of the volume of syn_mount */
static void syn_sector(struct winunit *u, u32 n, u8 *p)
{
    memset(p, 0, 512);
    if (n == 0) {
        p[0] = 0xEB; p[1] = 0x3C; p[2] = 0x90;
        memcpy(p + 3, "WINDRV  ", 8);
        memcpy(p + 11, u->bpb, 25);
        p[38] = 0x29;
        *(u32 *)(p + 39) = u->serial;
        memcpy(p + 43, u->label, 11);
        memcpy(p + 54, "FAT12   ", 8);
        p[510] = 0x55; p[511] = 0xAA;
    } else if (n == 1) {
        p[0] = 0xF8; p[1] = 0xFF; p[2] = 0xFF;
    } else if (n == 2) {
        memcpy(p, u->label, 11);        /* the volume label entry */
        p[11] = 0x08;
    }
}

/* read the boot sector of a unit and update its BPB/state */
static int unit_mount(struct winunit *u, struct blkdev *d)
{
    int fs = classify(d, unit_lba(u));
    u->state &= ~US_FOREIGN;
    if (fs == FS_FAT && !blk_read_abs(d, unit_lba(u), 1, secbuf)) {
        unit_fill_bpb(u, secbuf);
        return 0;
    }
    if (fs != FS_NONE && fs != FS_FAT && u->type == UT_SLOT) {
        syn_mount(u, fs);
        return 0;
    }
    memcpy(u->bpb, default_bpb, 25);
    memset(ubpbx(u), 0, WIN_BPBXSIZE);
    memcpy(ubpbx(u), default_bpb, 25);
    u->media = 0xF8;
    memcpy(u->label, "NO NAME    ", 11);
    return 1;
}

static u32 disk_key2(struct blkdev *d)
{
    return d->key2;
}

static int add_unit(int type, struct blkdev *d, struct pent *p, int partno)
{
    struct winunit *u;
    if (cb->nunits >= WIN_MAXUNITS) return -1;
    if (cb->firstdrive + cb->nunits >= 26) return -1;
    u = &cb->units[cb->nunits];
    memset(u, 0, sizeof(*u));
    u->type = (u8)type;
    u->disk = 0xFF;
    if (d) {
        u->disk = (u8)disk_index(d);
        u->key = d->key;
        u->key2 = disk_key2(d);
        u->part = (u8)partno;
        u->lba = (u32)p->start;
        u->lba_hi = (u8)(p->start >> 32);
        u->sectors = p->size;
        u->ptype = p->type;
        u->state = US_PRESENT;
        if (d->wprot) u->state |= US_WPROT;
        unit_mount(u, d);
    } else {
        memcpy(u->bpb, default_bpb, 25);
        memset(ubpbx(u), 0, WIN_BPBXSIZE);
        memcpy(ubpbx(u), default_bpb, 25);
        u->media = 0xF8;
    }
    return cb->nunits++;
}

/* DOS core init: create fixed units for every disk known now, then empty slots */
void blk_scan_fixed(void)
{
    struct pent pe[16];
    int i, j, n, nslots;
    if (scanned) return;
    scanned = 1;
    cb->nunits = 0;
    if (cb->opts2 & OPT2_NODISK) {
        kprintf("no DOS drive letters (MS-DOS 7: Windows owns the disks, /DOSDISK for DOS)\n");
        cb->nfixed = cb->nslots = 0;
        return;
    }
    for (i = 0; i < ndisks; i++) {
        struct blkdev *d = disks[i];
        if (d->removable || d->cdrom) continue;
        if (d->kind == DK_IDE && d->shared) {
            /* the IDE channel belongs to the BIOS (and to WDCTRL in Windows) */
            kprintf("disk%d: %s on a BIOS IDE channel, skipped\n", i, d->model);
            why[i] = 1;
            continue;
        }
        n = scan_parts(d, pe, 16, 0);
        why[i] = n <= 0 ? 2 : 3;
        kprintf("disk%d: %s, %d FAT partition(s)%s\n", i, d->model, n < 0 ? 0 : n,
                d->shared ? " (BIOS disk)" : "");
        for (j = 0; j < n; j++) {
            if (bios_mounted(d, pe[j].start) && !(cb->opts & OPT_ALL)) {
                kprintf("  part %d @%lu already mounted by DOS, skipped\n", j, (u32)pe[j].start);
                continue;
            }
            if (add_unit(UT_FIXED, d, &pe[j], j) < 0) break;
            why[i] = 0;
        }
    }
    cb->nfixed = cb->nunits;
    nslots = cb->slotreq & 0x7F;
    if (cb->slotreq & 0x80) {
        /* no /R:n: at least the default, more when the removable disks found at the start
           have more partitions (each gets a letter), and one to spare */
        int need = 0;
        for (i = 0; i < ndisks; i++) {
            if (!disks[i]->removable || disks[i]->cdrom || !disks[i]->present) continue;
            n = scan_parts(disks[i], pe, 16, 1);
            if (n > 0) need += n;
        }
        if (need && need + 1 > nslots) nslots = need + 1;
    }
    if (nslots > WIN_MAXUNITS - cb->nunits) nslots = WIN_MAXUNITS - cb->nunits;
    if (nslots > 26 - cb->firstdrive - cb->nunits) nslots = 26 - cb->firstdrive - cb->nunits;
    if (nslots < 0) nslots = 0;
    for (i = 0; i < nslots; i++)
        if (add_unit(UT_SLOT, NULL, NULL, 0) < 0) break;
    cb->nslots = (u8)(cb->nunits - cb->nfixed);
    /* removable disks known at init (USB in DOS mode) go into slots */
    for (i = 0; i < ndisks; i++)
        if (disks[i]->removable) blk_register(disks[i]);
}

/* WINDISK found nothing to serve: say what it saw, on the screen */
void blk_explain(int nide, int nahci, int nnvme)
{
    static const char *const kind[] = { "?", "AHCI", "NVMe", "USB", "IDE", "SD", "MMC" };
    static const char *const text[] = {
        "", "on an IDE channel the BIOS uses (DOS drives, boot disk)",
        "no FAT partition this DOS can use (FAT32: MS-DOS 7.10 and later)",
        "its FAT partitions are already DOS drives through the BIOS"
    };
    int i;
    kprintf(CORENAME_STR ": controllers: %d PCI IDE, %d AHCI, %d NVMe; %d disk(s)\n", nide, nahci, nnvme, ndisks);
    for (i = 0; i < ndisks; i++) {
        struct blkdev *d = disks[i];
        if (d->removable || d->cdrom || !why[i]) continue;
        kprintf("  %s %s, %lu MB%s:\n    %s\n", d->kind < 7 ? kind[d->kind] : "?", d->model,
                (u32)(d->sectors / (1048576UL / d->ssize)), d->shared && d->shared < 0xFE ? " (BIOS disk)" : "",
                text[why[i]]);
    }
    for (i = 0; i < ndisks && disks[i]->cdrom; i++) ;
    if (i == ndisks) kprintf("  no hard disk found on these controllers\n");
    for (i = 0; i < ndisks && disks[i]->kind != DK_NVME; i++) ;
    if (nnvme && i == ndisks && !(cb->opts & OPT_NVME))
        kprintf("  NVMe controllers the BIOS already runs are left to it: /NVME takes them over\n");
}

/* slot assignment for hot-plugged (removable) disks */
/* a removable disk the BIOS also offers through INT 13h (USB or SD boot support):
   its partitions that DOS already has as drives stay with DOS */
static void match_bios(struct blkdev *d)
{
    int k;
    u32 c;
    if (d->shared || !cb->nbios || blk_read_abs(d, 0, 1, bounce)) return;
    c = crc32(0, bounce, 512);
    DBG("disk%d: first sector %08lx, BIOS disks %08lx %08lx\n", disk_index(d), c, cb->biosmbr[0], cb->biosmbr[1]);
    for (k = 0; k < cb->nbios && k < 8; k++)
        if (cb->biosmbr[k] == c) {
            d->shared = (u8)(k + 1);
            kprintf("disk%d is BIOS disk %02xh\n", disk_index(d), 0x80 + k);
            return;
        }
}

static int slot_attach(struct blkdev *d)
{
    struct pent pe[16];
    int n, j, i, used = 0;
    if (!cb->nunits || !blk_dosunits) return 0;     /* no DOS drive letters (Plug and Play: IOS owns the disks) */
    match_bios(d);
    n = scan_parts(d, pe, 16, 1);
    if (n <= 0) {
        kprintf("disk%d: no volume DOS can use\n", disk_index(d));
        return 0;
    }
    for (j = 0; j < n; j++) {
        int pick = -1;
        if (bios_mounted(d, pe[j].start)) {
            kprintf("disk%d part %d: already a DOS drive through the BIOS, skipped\n", disk_index(d), j);
            continue;
        }
        /* sticky: same device and partition gets its old slot back, else the first free slot */
        for (i = 0; i < cb->nunits; i++) {
            struct winunit *u = &cb->units[i];
            if (u->type == UT_SLOT && !(u->state & (US_PRESENT | US_TAKEN)) && u->key == d->key &&
                u->key2 == d->key2 && u->part == j && u->key) { pick = i; break; }
        }
        if (pick < 0) {
            for (i = 0; i < cb->nunits; i++) {
                struct winunit *u = &cb->units[i];
                if (u->type == UT_SLOT && !(u->state & (US_PRESENT | US_TAKEN))) { pick = i; break; }
            }
        }
        if (pick < 0) {
            kprintf("disk%d: no free drive letter for partition %d (use /R:n)\n", disk_index(d), j);
            break;
        }
        {
            struct winunit *u = &cb->units[pick];
            u->disk = (u8)disk_index(d);
            u->key = d->key;
            u->key2 = d->key2;
            u->part = (u8)j;
            u->lba = (u32)pe[j].start;
            u->lba_hi = (u8)(pe[j].start >> 32);
            u->sectors = pe[j].size;
            u->ptype = pe[j].type;
            u->state = US_PRESENT | US_CHANGED;
            if (d->wprot) u->state |= US_WPROT;
            u->gen++;
            unit_mount(u, d);
            kprintf("disk%d part %d -> %c:%s%s\n", disk_index(d), j, 'A' + cb->firstdrive + pick,
                    pe[j].fs == FS_FAT ? "" : " ", fsname[pe[j].fs]);
            plat_unit_changed(pick);
            used++;
        }
    }
    return used;
}

int blk_register(struct blkdev *d)
{
    int i = disk_index(d);
    if (i < 0) {
        if (ndisks >= MAXDISKS) return -1;
        /* reuse a hole left by an unplugged disk */
        i = ndisks++;
        disks[i] = d;
    }
    d->index = (u8)i;
    if (!d->maxxfer) d->maxxfer = 128;
    if (d->removable && scanned) slot_attach(d);
    else if (!d->removable && scanned) {
        /* VxD side: bind fixed units created by the DOS core */
        int k;
        for (k = 0; k < cb->nunits; k++) {
            struct winunit *u = &cb->units[k];
            if (u->type == UT_FIXED && u->key == d->key && u->key2 == d->key2) u->disk = (u8)i;
        }
    }
    return i;
}

void blk_unregister(struct blkdev *d)
{
    int i = disk_index(d), k;
    if (i < 0) return;
    for (k = 0; k < cb->nunits; k++) {
        struct winunit *u = &cb->units[k];
        if (u->disk == i) {
            u->disk = 0xFF;
            if (u->state & US_PRESENT) {
                kprintf("%c: removed\n", 'A' + cb->firstdrive + k);
                u->state = US_CHANGED;
                u->gen++;
                plat_unit_changed(k);
            }
        }
    }
    /* keep the array dense: move the last disk into the hole and fix unit links */
    ndisks--;
    if (i != ndisks) {
        disks[i] = disks[ndisks];
        disks[i]->index = (u8)i;
        for (k = 0; k < cb->nunits; k++)
            if (cb->units[k].disk == ndisks) cb->units[k].disk = (u8)i;
    }
    disks[ndisks] = NULL;
}

/* VxD start: units already exist, mark them bound later through blk_register */
void blk_adopt_units(void)
{
    int k;
    scanned = 1;
    for (k = 0; k < cb->nunits; k++) {
        struct winunit *u = &cb->units[k];
        u->disk = 0xFF;
        if (u->type == UT_SLOT && (u->state & US_PRESENT)) {
            /* removable media will be re-detected */
            u->state = US_CHANGED;
            u->gen++;
        }
    }
}

/* DOS core after Windows: the VxD numbered the disks in its own order. Fixed units are
   bound again by disk identity, removable media is detected again. */
void blk_rebind_units(void)
{
    int k, i;
    for (k = 0; k < cb->nunits; k++) {
        struct winunit *u = &cb->units[k];
        u->disk = 0xFF;
        if (u->type == UT_FIXED) {
            for (i = 0; i < ndisks; i++)
                if (disks[i] && u->key == disks[i]->key && u->key2 == disks[i]->key2) u->disk = (u8)i;
            if (u->disk == 0xFF && (u->state & US_PRESENT)) {
                u->state = US_CHANGED;
                u->gen++;
            }
        } else if (u->state & US_PRESENT) {
            u->state = US_CHANGED;
            u->gen++;
        }
    }
}

void blk_flush_all(void)
{
    int i;
    for (i = 0; i < ndisks; i++)
        if (disks[i]->present && disks[i]->ops->flush) disks[i]->ops->flush(disks[i]);
}

int blk_find_unit_letter(struct blkdev *d, char *letters, int max)
{
    int k, n = 0, i = disk_index(d);
    for (k = 0; k < cb->nunits && n < max - 1; k++)
        if (cb->units[k].disk == i && i >= 0 && (cb->units[k].state & US_PRESENT))
            letters[n++] = (char)('A' + cb->firstdrive + k);
    letters[n] = 0;
    return n;
}

/* ---- DOS request processing ---- */
static void setst(u8 *pkt, u16 st)
{
    *(u16 *)(pkt + RH_STATUS) = st;
}

static struct blkdev *unit_disk(struct winunit *u)
{
    if (u->disk == 0xFF || u->disk >= ndisks) return NULL;
    return disks[u->disk];
}

static u16 io_error(int r, int wr)
{
    if (r == -2) return ST_ERR | ST_DONE | E_NOTREADY;
    if (r == -3) return ST_ERR | ST_DONE | E_WP;
    return ST_ERR | ST_DONE | (wr ? E_WRITE : E_READ);
}

/* offset of a CB field as seen from the driver segment */
static u32 cb_farptr(void *field)
{
    u32 off = (u32)field - (u32)cb + (cb->cb_lin - (u32)cb->drvseg * 16UL);
    return ((u32)cb->drvseg << 16) | (off & 0xFFFF);
}

static int dos_rw(u8 *pkt, struct winunit *u, int wr)
{
    struct blkdev *d = unit_disk(u);
    u16 cnt = *(u16 *)(pkt + 0x12);
    u32 start = *(u16 *)(pkt + 0x14);
    u8 *buf = plat_rm(*(u32 *)(pkt + 0x0E));
    int r;
    if (start == 0xFFFF && pkt[0] >= 0x1E) start = *(u32 *)(pkt + 0x1A);
    if (!d || !(u->state & US_PRESENT)) {
        *(u16 *)(pkt + 0x12) = 0;
        setst(pkt, ST_ERR | ST_DONE | E_NOTREADY);
        return 0;
    }
    /* reads of new media are harmless (absolute reads may precede the media check);
       writes must not hit new media with buffers that belong to the previous one */
    if (wr && (u->state & US_CHANGED) && u->type == UT_SLOT) {
        *(u16 *)(pkt + 0x12) = 0;
        *(u32 *)(pkt + 0x16) = cb_farptr(u->label);
        setst(pkt, ST_ERR | ST_DONE | E_BADCHG);
        return 0;
    }
    if (u->state & US_FOREIGN) {
        u32 k;
        if (wr || start + cnt > SYN_SECTORS) {
            *(u16 *)(pkt + 0x12) = 0;
            setst(pkt, ST_ERR | ST_DONE | (wr ? E_WP : E_NOTFOUND));
            return 0;
        }
        for (k = 0; k < cnt; k++) syn_sector(u, start + k, buf + k * 512);
        setst(pkt, ST_DONE);
        return 0;
    }
    if (start + cnt > u->sectors) {
        *(u16 *)(pkt + 0x12) = 0;
        setst(pkt, ST_ERR | ST_DONE | E_NOTFOUND);
        return 0;
    }
    if (wr && (u->state & US_WPROT)) {
        *(u16 *)(pkt + 0x12) = 0;
        setst(pkt, ST_ERR | ST_DONE | E_WP);
        return 0;
    }
    r = wr ? blk_write_abs(d, unit_lba(u) + start, cnt, buf) : blk_read_abs(d, unit_lba(u) + start, cnt, buf);
    if (r) {
        *(u16 *)(pkt + 0x12) = 0;
        setst(pkt, io_error(r, wr));
        return 0;
    }
    setst(pkt, ST_DONE);
    return 0;
}

static void geom(struct winunit *u, u16 *heads, u16 *spt, u16 *cyl)
{
    u32 c;
    *heads = u->bpb[15] | (u->bpb[16] << 8);
    *spt = u->bpb[13] | (u->bpb[14] << 8);
    if (!*heads) *heads = 255;
    if (!*spt) *spt = 63;
    c = u->sectors / ((u32)*heads * *spt);
    if (c > 65535) c = 65535;
    *cyl = (u16)(c ? c : 1);
}

static int dos_ioctl(u8 *pkt, struct winunit *u)
{
    u8 cat = pkt[0x0D], fn = pkt[0x0E];
    u8 *p = plat_rm(*(u32 *)(pkt + 0x13));
    u16 heads, spt, cyl;
    if (cat != 0x08 && cat != 0x48) {
        setst(pkt, ST_ERR | ST_DONE | E_BADCMD);
        return 0;
    }
    geom(u, &heads, &spt, &cyl);
    switch (fn) {
    case 0x60:  /* get device parameters */
        p[1] = 5;                                   /* hard disk */
        *(u16 *)(p + 2) = (u->type == UT_FIXED) ? 1 : 2;   /* nonremovable / changeline */
        *(u16 *)(p + 4) = cyl;
        p[6] = 0;
        if (cat == 0x48) {
            memcpy(p + 7, ubpbx(u), 53);    /* FAT32 BPB (MS-DOS 7.10) */
        } else {
            memcpy(p + 7, u->bpb, 25);
            memset(p + 7 + 25, 0, 6);
        }
        setst(pkt, ST_DONE);
        return 0;
    case 0x40:  /* set device parameters */
    case 0x42:  /* format track */
    case 0x62:  /* verify track */
    case 0x46:  /* set media id */
    case 0x47:  /* set access flag */
        if (fn == 0x42 || fn == 0x62) p[0] = 0;
        setst(pkt, ST_DONE);
        return 0;
    case 0x67:  /* get access flag */
        p[1] = 1;
        setst(pkt, ST_DONE);
        return 0;
    case 0x66:  /* get media id */
        if (!(u->state & US_PRESENT)) {
            setst(pkt, ST_ERR | ST_DONE | E_NOTREADY);
            return 0;
        }
        *(u16 *)p = 0;
        *(u32 *)(p + 2) = u->serial;
        memcpy(p + 6, u->label, 11);
        {
            u32 tot = u->bpb[8] | (u->bpb[9] << 8);
            u32 spc = u->bpb[2] ? u->bpb[2] : 1;
            if (!tot) tot = *(u32 *)(u->bpb + 21);
            memcpy(p + 17, !(u->bpb[11] | u->bpb[12]) ? "FAT32   " : (tot / spc) < 4085 ? "FAT12   " : "FAT16   ", 8);
        }
        setst(pkt, ST_DONE);
        return 0;
    case 0x41:  /* write track */
    case 0x61:  /* read track */
    {
        struct blkdev *d = unit_disk(u);
        u16 head = *(u16 *)(p + 1), cy = *(u16 *)(p + 3), sec = *(u16 *)(p + 5), n = *(u16 *)(p + 7);
        u8 *buf = plat_rm(*(u32 *)(p + 9));
        u32 hidden = *(u32 *)(u->bpb + 17);
        u32 abs = ((u32)cy * heads + head) * spt + sec;
        int r;
        if (!d || !(u->state & US_PRESENT)) {
            setst(pkt, ST_ERR | ST_DONE | E_NOTREADY);
            return 0;
        }
        if (u->state & US_FOREIGN) {
            setst(pkt, ST_ERR | ST_DONE | (fn == 0x41 ? E_WP : E_NOTFOUND));
            return 0;
        }
        abs = abs >= hidden ? abs - hidden : abs;
        if (abs + n > u->sectors) {
            setst(pkt, ST_ERR | ST_DONE | E_NOTFOUND);
            return 0;
        }
        r = fn == 0x41 ? blk_write_abs(d, unit_lba(u) + abs, n, buf) : blk_read_abs(d, unit_lba(u) + abs, n, buf);
        setst(pkt, r ? io_error(r, fn == 0x41) : ST_DONE);
        return 0;
    }
    }
    setst(pkt, ST_ERR | ST_DONE | E_BADCMD);
    return 0;
}

void (*blk_poll_hook)(void);

int blk_dos_request(u8 *pkt)
{
    u8 unit = pkt[RH_UNIT], cmd = pkt[RH_CMD];
    struct winunit *u;
    if ((cmd == 0x01 || cmd == 0x02) && blk_poll_hook) blk_poll_hook();
    if (unit >= cb->nunits) {
        setst(pkt, ST_ERR | ST_DONE | E_UNIT);
        return 0;
    }
    u = &cb->units[unit];
    switch (cmd) {
    case 0x01:  /* media check */
        if (u->type == UT_FIXED) {
            pkt[0x0E] = 1;
        } else if (u->state & US_CHANGED) {
            pkt[0x0E] = 0xFF;
            *(u32 *)(pkt + 0x0F) = cb_farptr(u->label);
            u->state &= ~US_CHANGED;
        } else if (!(u->state & US_PRESENT)) {
            pkt[0x0E] = 0xFF;
            *(u32 *)(pkt + 0x0F) = cb_farptr(u->label);
        } else {
            pkt[0x0E] = 1;
        }
        setst(pkt, ST_DONE);
        return 0;
    case 0x02:  /* build BPB */
        if (!(u->state & US_PRESENT)) {
            setst(pkt, ST_ERR | ST_DONE | E_NOTREADY);
            return 0;
        }
        if (u->type == UT_SLOT) {
            struct blkdev *d = unit_disk(u);
            if (d) unit_mount(u, d);
            u->state &= ~US_CHANGED;
        }
        *(u32 *)(pkt + 0x12) = cb_farptr(ubpbx(u));
        setst(pkt, ST_DONE);
        return 0;
    case 0x04:
        return dos_rw(pkt, u, 0);
    case 0x08:
    case 0x09:
        return dos_rw(pkt, u, 1);
    case 0x0D:  /* open */
    case 0x0E:  /* close */
        setst(pkt, ST_DONE);
        return 0;
    case 0x0F:  /* removable media */
        setst(pkt, u->type == UT_FIXED ? (ST_DONE | ST_BUSY) : ST_DONE);
        return 0;
    case 0x13:
        return dos_ioctl(pkt, u);
    case 0x17:  /* get logical device */
        pkt[RH_UNIT] = 0;
        setst(pkt, ST_DONE);
        return 0;
    case 0x18:  /* set logical device */
        setst(pkt, ST_DONE);
        return 0;
    case 0x19:  /* ioctl query */
    {
        u8 fn = pkt[0x0E];
        if (pkt[0x0D] == 0x08 && (fn == 0x60 || fn == 0x40 || fn == 0x41 || fn == 0x42 || fn == 0x46 ||
                                  fn == 0x47 || fn == 0x61 || fn == 0x62 || fn == 0x66 || fn == 0x67))
            setst(pkt, ST_DONE);
        else
            setst(pkt, ST_ERR | ST_DONE | E_BADCMD);
        return 0;
    }
    }
    setst(pkt, ST_ERR | ST_DONE | E_BADCMD);
    return 0;
}
