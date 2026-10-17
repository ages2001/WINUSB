/* winfs.c - partition tables (MBR with extended partitions, GPT) and file system recognition:
   FAT12/16/32, exFAT, NTFS (with the version from $Volume), HPFS, ReFS, ext2/3/4, Btrfs, XFS,
   HFS+, APFS, ISO 9660, Linux swap, BitLocker and LUKS. 16-bit code, all reads go through
   the caller's sector read function. */
#include <stdio.h>
#include <string.h>
#include "winfs.h"

#define BUFSZ 8192U

static unsigned char sec[BUFSZ];
static unsigned char blk[4096];
static unsigned char gsec[4096];   /* GPT entry sector, kept while partitions are examined */

static unsigned w16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long d32(const unsigned char *p)
{
    return p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* reads len bytes at partition position (sect512 * 512 + off) into blk */
static int rdpos(const struct fsdisk *d, unsigned long start, unsigned long sect512, unsigned off, unsigned len)
{
    unsigned long per = d->ssize / 512, lba;
    unsigned long o;
    unsigned cnt;
    if (!per || len > sizeof(blk)) return -1;
    lba = start + sect512 / per;
    o = (sect512 % per) * 512UL + off;
    lba += o / d->ssize;
    o %= d->ssize;
    cnt = (unsigned)((o + len + d->ssize - 1) / d->ssize);
    if ((unsigned long)cnt * d->ssize > BUFSZ) return -1;
    if (d->sectors != 0xFFFFFFFFUL && lba + cnt > d->sectors) return -1;
    if (d->read(d->ctx, lba, cnt, sec, BUFSZ)) return -1;
    memcpy(blk, sec + (unsigned)o, len);
    return 0;
}

void win_size(char *out, unsigned long n, unsigned long hi, unsigned long ssize)
{
    /* size in units of 1/100 MB, without 64-bit arithmetic */
    unsigned long mb, kb;
    unsigned long per = ssize >= 512 ? ssize / 512 : 1;
    mb = (n >> 11) * per + (hi << 21) * per;
    kb = ((n & 2047) * per) / 2;
    mb += kb >> 10;
    kb &= 1023;
    if (mb >= 1024UL * 1024)
        sprintf(out, "%lu.%02lu TB", mb >> 20, ((mb & 0xFFFFF) * 100) >> 20);
    else if (mb >= 1024)
        sprintf(out, "%lu.%02lu GB", mb >> 10, ((mb & 1023) * 100) >> 10);
    else if (mb)
        sprintf(out, "%lu MB", mb);
    else
        sprintf(out, "%lu KB", kb);
}

static void copylabel(char *out, int max, const unsigned char *p, int n)
{
    int i, k = 0;
    for (i = 0; i < n && p[i] && k < max - 1; i++)
        out[k++] = (p[i] >= 32 && p[i] < 127) ? p[i] : '?';
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    if (!strcmp(out, "NO NAME")) out[0] = 0;
}

static void copyutf16(char *out, int max, const unsigned char *p, int nchars)
{
    int i, k = 0;
    for (i = 0; i < nchars && k < max - 1; i++) {
        unsigned c = w16(p + 2 * i);
        if (!c) break;
        out[k++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[k] = 0;
}

static int zeros(const unsigned char *p, int n)
{
    while (n--)
        if (*p++) return 0;
    return 1;
}

static int pow2(unsigned long v)
{
    return v && !(v & (v - 1));
}

/* FAT boot sector with a sane BPB; returns 12, 16, 32 or 0 */
static int fat_type(const unsigned char *b, unsigned long *clusters)
{
    unsigned bps = w16(b + 11), spc = b[13], res = w16(b + 14), nf = b[16], re = w16(b + 17);
    unsigned long tot = w16(b + 19), fsz = w16(b + 22), rootsec, meta, cl;
    if (b[0] != 0xEB && b[0] != 0xE9 && b[0] != 0x69) return 0;
    if (bps < 512 || bps > 4096 || !pow2(bps) || !pow2(spc) || !res || nf < 1 || nf > 2) return 0;
    if (b[21] < 0xF0 && b[21] != 0) return 0;
    if (!tot) tot = d32(b + 32);
    if (!fsz) fsz = d32(b + 36);
    if (!tot || !fsz) return 0;
    rootsec = ((unsigned long)re * 32 + bps - 1) / bps;
    meta = res + nf * fsz + rootsec;
    if (meta >= tot) return 0;
    cl = (tot - meta) / spc;
    if (clusters) *clusters = cl;
    if (!w16(b + 22) && !re) return 32;
    if (cl < 4085) return 12;
    if (cl < 65525) return 16;
    return 32;
}

/* NTFS version and volume name from the $Volume record (MFT record 3) */
static void ntfs_volume(const struct fsdisk *d, unsigned long start, const unsigned char *b, char *out, int outmax,
                        char *label, int lblmax)
{
    unsigned bps = w16(b + 11), spc = b[13], recsize, i, ao, usa, nusa;
    signed char cpr = (signed char)b[0x40];
    unsigned long mftlcn = d32(b + 0x30), spc512, pos512;
    unsigned char *r = blk;
    int maj = -1, min = 0;
    char name[40];
    name[0] = 0;
    if (spc > 0x80) spc = 1U << (256 - spc);
    if (bps < 512 || !pow2(bps) || !spc || d32(b + 0x34)) goto done;
    spc512 = (unsigned long)spc * (bps / 512);
    recsize = cpr > 0 ? (unsigned)(cpr * spc512 * 512UL) : (1U << (unsigned)(-cpr));
    if (recsize < 512 || recsize > sizeof(blk)) goto done;
    pos512 = mftlcn * spc512 + (3UL * recsize) / 512;
    if (rdpos(d, start, pos512, 0, recsize) || memcmp(r, "FILE", 4)) goto done;
    usa = w16(r + 4);
    nusa = w16(r + 6);
    for (i = 1; i < nusa && i * 512U <= recsize && usa + 2 * i + 1 < recsize; i++) {
        r[i * 512 - 2] = r[usa + 2 * i];
        r[i * 512 - 1] = r[usa + 2 * i + 1];
    }
    ao = w16(r + 0x14);
    while (ao + 16 < recsize) {
        unsigned long type = d32(r + ao), len = d32(r + ao + 4);
        if (type == 0xFFFFFFFFUL || len < 16 || len > recsize - ao) break;
        if (!r[ao + 8]) {
            unsigned vlen = (unsigned)d32(r + ao + 16), voff = w16(r + ao + 20);
            if (type == 0x70 && vlen >= 10 && ao + voff + 10 <= recsize) {
                maj = r[ao + voff + 8];
                min = r[ao + voff + 9];
            } else if (type == 0x60 && ao + voff + vlen <= recsize) {
                copyutf16(name, sizeof(name), r + ao + voff, vlen / 2);
            }
        }
        ao += (unsigned)len;
    }
done:
    if (maj >= 0) {
        sprintf(out, "NTFS %d.%d", maj, min);
    } else {
        sprintf(out, "NTFS");
    }
    (void)outmax;
    if (name[0]) copylabel(label, lblmax, (const unsigned char *)name, (int)strlen(name));
}

static void exfat_label(const struct fsdisk *d, unsigned long start, const unsigned char *b, char *label, int lblmax)
{
    unsigned bshift = b[0x6C], cshift = b[0x6D], i;
    unsigned long heap = d32(b + 0x58), root = d32(b + 0x60), pos512;
    if (bshift < 9 || bshift > 12 || bshift + cshift > 25 || root < 2) return;
    pos512 = (heap + ((root - 2) << cshift)) << (bshift - 9);
    if (rdpos(d, start, pos512, 0, 1024)) return;
    for (i = 0; i < 1024; i += 32) {
        if (blk[i] == 0x83) {
            unsigned n = blk[i + 1];
            if (n > 11) n = 11;
            copyutf16(label, lblmax, blk + i + 2, n);
            return;
        }
        if (!blk[i]) return;
    }
}

int win_fsdetect(const struct fsdisk *d, unsigned long start, unsigned long nsect, char *out, int outmax,
                 char *label, int lblmax)
{
    unsigned char b[512];
    unsigned long cl;
    int t;
    unsigned long n512 = nsect * (d->ssize / 512);
    out[0] = 0;
    label[0] = 0;
    if (rdpos(d, start, 0, 0, 512)) {
        sprintf(out, "unreadable");
        return 0;
    }
    memcpy(b, blk, 512);
    if (!memcmp(b + 3, "NTFS    ", 8)) {
        ntfs_volume(d, start, b, out, outmax, label, lblmax);
        return 1;
    }
    if (!memcmp(b + 3, "EXFAT   ", 8) && zeros(b + 11, 53)) {
        sprintf(out, "exFAT %u.%02u", b[0x69], b[0x68]);
        exfat_label(d, start, b, label, lblmax);
        return 1;
    }
    if (!memcmp(b + 3, "ReFS\0\0\0\0", 8) && !memcmp(b + 0x10, "FSRS", 4)) {
        sprintf(out, "ReFS %u.%u", b[0x28], b[0x29]);
        return 1;
    }
    if (!memcmp(b + 3, "-FVE-FS-", 8)) {
        sprintf(out, "BitLocker encrypted volume");
        return 1;
    }
    if (!memcmp(b, "LUKS\xBA\xBE", 6)) {
        sprintf(out, "LUKS%u encrypted volume", w16(b + 6) >> 8);
        return 1;
    }
    if (!memcmp(b, "XFSB", 4)) {
        sprintf(out, "XFS");
        copylabel(label, lblmax, b + 0x6C, 12);
        return 1;
    }
    if (!memcmp(b + 0x36, "HPFS    ", 8) ||
        (n512 > 17 && !rdpos(d, start, 16, 0, 16) && d32(blk) == 0xF995E849UL)) {
        if (n512 > 17 && !rdpos(d, start, 16, 0, 16) && d32(blk) == 0xF995E849UL)
            sprintf(out, "HPFS (version %u, %s)", blk[8], blk[9] >= 3 ? "volumes over 4 GB" : "up to 4 GB");
        else
            sprintf(out, "HPFS");
        copylabel(label, lblmax, b + 0x2B, 11);
        return 1;
    }
    if ((t = fat_type(b, &cl)) != 0) {
        if (t == 32) {
            sprintf(out, "FAT32");
            if (w16(b + 0x2A)) sprintf(out + 5, " (version %u.%u)", b[0x2B], b[0x2A]);
            if (b[0x42] == 0x29) copylabel(label, lblmax, b + 0x47, 11);
        } else {
            sprintf(out, "FAT%d", t);
            if (b[0x26] == 0x29) copylabel(label, lblmax, b + 0x2B, 11);
        }
        {
            unsigned long cs = (unsigned long)w16(b + 11) * b[13];
            if (cs >= 1024) sprintf(out + strlen(out), ", %lu clusters of %lu KB", cl, cs >> 10);
            else sprintf(out + strlen(out), ", %lu clusters of %lu bytes", cl, cs);
        }
        return 1;
    }
    /* superblocks after the boot area */
    if (n512 > 4 && !rdpos(d, start, 2, 0, 1024)) {
        const unsigned char *sb = blk;
        if (w16(sb + 0x38) == 0xEF53) {
            unsigned long compat = d32(sb + 0x5C), incompat = d32(sb + 0x60), ro = d32(sb + 0x64);
            if (incompat & 0x0008)
                sprintf(out, "ext3/ext4 external journal");
            else if ((incompat & ~0x0016UL) || (ro & ~0x0007UL))
                sprintf(out, "ext4");
            else if (compat & 0x0004)
                sprintf(out, "ext3");
            else
                sprintf(out, "ext2");
            if (d32(sb + 0x4C) == 0) strcat(out, " (revision 0)");
            copylabel(label, lblmax, sb + 0x78, 16);
            return 1;
        }
        if (sb[0] == 'H' && (sb[1] == '+' || sb[1] == 'X')) {
            sprintf(out, sb[1] == '+' ? "HFS+" : "HFSX");
            return 1;
        }
    }
    if (!memcmp(b + 32, "NXSB", 4)) {
        sprintf(out, "APFS container");
        return 1;
    }
    if (n512 > 130 && !rdpos(d, start, 128, 0, 1024) && !memcmp(blk + 0x40, "_BHRfS_M", 8)) {
        sprintf(out, "Btrfs");
        copylabel(label, lblmax, blk + 0x12B, 32);
        return 1;
    }
    if (n512 > 66 && !rdpos(d, start, 64, 0, 512) && !memcmp(blk + 1, "CD001", 5)) {
        sprintf(out, "ISO 9660 (CD/DVD image)");
        copylabel(label, lblmax, blk + 40, 32);
        return 1;
    }
    if (n512 > 8 && !rdpos(d, start, 7, 502, 10) &&
        (!memcmp(blk, "SWAPSPACE2", 10) || !memcmp(blk, "SWAP-SPACE", 10))) {
        sprintf(out, "Linux swap");
        return 1;
    }
    sprintf(out, "unknown or not formatted");
    return 0;
}

static const char *mbr_name(unsigned t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 (< 32 MB)";
    case 0x05: return "extended";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS / exFAT / HPFS";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16 (LBA)";
    case 0x0F: return "extended (LBA)";
    case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E: return "hidden FAT";
    case 0x17: return "hidden NTFS / HPFS";
    case 0x27: return "Windows recovery";
    case 0x42: return "Windows dynamic disk";
    case 0x82: return "Linux swap / Solaris";
    case 0x83: return "Linux";
    case 0x85: return "Linux extended";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xA6: return "OpenBSD";
    case 0xA8: return "macOS";
    case 0xA9: return "NetBSD";
    case 0xAF: return "HFS / HFS+";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    case 0xFB: return "VMware VMFS";
    case 0xFD: return "Linux RAID";
    }
    return "";
}

static int is_ext(unsigned t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static int letter_of(const struct fsdisk *d, unsigned long lba)
{
    int k;
    unsigned long l512 = lba * (d->ssize / 512);
    for (k = 0; k < d->nunits; k++)
        if (d->units[k].disk == d->index && (d->units[k].state & US_PRESENT) && d->units[k].lba == l512)
            return 'A' + d->firstdrive + k;
    return 0;
}

static void part_line(struct sbuf *s, const struct fsdisk *d, int num, unsigned long lba, unsigned long n,
                      unsigned long lbahi, const char *ptype)
{
    char fs[64], lbl[40], sz[24];
    int dl = lbahi ? 0 : letter_of(d, lba);
    win_size(sz, n, 0, d->ssize);
    if (lbahi) {
        sb_printf(s, " %2d  beyond 2 TB, %s\r\n", num, ptype);
        return;
    }
    win_fsdetect(d, lba, n, fs, sizeof(fs), lbl, sizeof(lbl));
    sb_printf(s, " %2d  %c%s %s\r\n", num, dl ? dl : ' ', dl ? ":" : " ", fs);
    if (lbl[0]) sb_printf(s, "       label \"%s\"\r\n", lbl);
    sb_printf(s, "       %s, start %lu\r\n", sz, lba);
    if (*ptype) sb_printf(s, "       type %s\r\n", ptype);
}

/* GPT type GUID in the usual text form from its on-disk bytes */
static void guid_text(char *out, const unsigned char *g)
{
    sprintf(out, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", d32(g), w16(g + 4), w16(g + 6), g[8], g[9],
            g[10], g[11], g[12], g[13], g[14], g[15]);
}

static const char *const gpt_types[] = {
    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system",
    "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "basic data",
    "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved",
    "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery",
    "5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", "LDM metadata",
    "AF9B60A0-1431-4F62-BC68-3311714A69AD", "LDM data",
    "E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D", "Storage Spaces",
    "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem",
    "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap",
    "E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM",
    "A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID",
    "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)",
    "BC13C2FF-59E6-4262-A352-B275FD6F7172", "Linux /boot",
    "21686148-6449-6E6F-744E-656564454649", "BIOS boot",
    "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+",
    "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS",
    "516E7CB4-6ECF-11D6-8FF8-00022D09712B", "FreeBSD",
    0, 0
};

static void gpt(struct sbuf *s, const struct fsdisk *d)
{
    unsigned long elba, nent, esz, i, per, lastlba = 0xFFFFFFFFUL;
    int num = 0;
    char g[40], name[40];
    if (d->read(d->ctx, 1, 1, sec, BUFSZ) || memcmp(sec, "EFI PART", 8)) {
        sb_printf(s, "Partition table: GPT (header unreadable)\r\n");
        return;
    }
    elba = d32(sec + 0x48);
    nent = d32(sec + 0x50);
    esz = d32(sec + 0x54);
    guid_text(g, sec + 0x38);
    sb_printf(s, "Partition table: GPT\r\n  disk GUID %s\r\n", g);
    if (esz < 128 || esz > d->ssize || d->ssize % esz || d->ssize > sizeof(gsec) || nent > 1024) return;
    per = d->ssize / esz;
    for (i = 0; i < nent; i++) {
        unsigned char e[128];
        int k;
        unsigned long first, last;
        const char *tn = "";
        if (i % per == 0) {
            if (d->read(d->ctx, elba + i / per, 1, sec, BUFSZ)) break;
            memcpy(gsec, sec, (unsigned)d->ssize);
        }
        memcpy(e, gsec + (unsigned)((i % per) * esz), 128);
        for (k = 0; k < 16 && !e[k]; k++)
            ;
        if (k == 16) continue;
        guid_text(g, e);
        for (k = 0; gpt_types[k]; k += 2)
            if (!strcmp(g, gpt_types[k])) {
                tn = gpt_types[k + 1];
                break;
            }
        copyutf16(name, sizeof(name), e + 56, 36);
        first = d32(e + 32);
        last = d32(e + 40);
        if (d32(e + 36) || d32(e + 44)) {
            sb_printf(s, " %2d  beyond 2 TB, %s\r\n", ++num, *tn ? tn : g);
            continue;
        }
        part_line(s, d, ++num, first, last - first + 1, 0, *tn ? tn : g);
        if (name[0]) sb_printf(s, "       name \"%s\"\r\n", name);
    }
    (void)lastlba;
    if (!num) sb_printf(s, "  no partitions\r\n");
}

void win_partitions(struct sbuf *s, const struct fsdisk *d)
{
    unsigned char m[512];
    char fs[64], lbl[40], tn[48];
    int i, num = 0, valid = 1, any = 0;
    if (d->read(d->ctx, 0, 1, sec, BUFSZ)) {
        sb_printf(s, "Medium not readable\r\n");
        return;
    }
    memcpy(m, sec, 512);
    if (m[510] == 0x55 && m[511] == 0xAA) {
        for (i = 0; i < 4; i++) {
            const unsigned char *e = m + 446 + 16 * i;
            if (e[0] != 0 && e[0] != 0x80) valid = 0;
            if (e[4]) {
                any = 1;
                if (!d32(e + 8) || !d32(e + 12)) valid = 0;
            }
            if (e[4] == 0xEE) {
                gpt(s, d);
                return;
            }
        }
        /* a FAT/NTFS/exFAT boot sector on the whole medium (no partition table) */
        if (!valid || !any || fat_type(m, NULL) || !memcmp(m + 3, "NTFS    ", 8) || (!memcmp(m + 3, "EXFAT   ", 8) && zeros(m + 11, 53))) {
            if (win_fsdetect(d, 0, d->sectors, fs, sizeof(fs), lbl, sizeof(lbl)) || !any) {
                char dl = (char)letter_of(d, 0);
                sb_printf(s, "No partition table (whole medium)\r\n");
                sb_printf(s, "  %c%s %s\r\n", dl ? dl : ' ', dl ? ":" : " ", fs);
                if (lbl[0]) sb_printf(s, "     label \"%s\"\r\n", lbl);
                return;
            }
        }
        sb_printf(s, "Partition table: MBR, disk signature %08lX\r\n", d32(m + 440));
        for (i = 0; i < 4; i++) {
            const unsigned char *e = m + 446 + 16 * i;
            unsigned long st = d32(e + 8), n = d32(e + 12);
            if (!e[4]) continue;
            sprintf(tn, "%02X %s%s", e[4], mbr_name(e[4]), e[0] == 0x80 ? ", active" : "");
            if (is_ext(e[4])) {
                unsigned long ebr = st, next;
                int guard = 0;
                sb_printf(s, "  extended partition at %lu\r\n", st);
                do {
                    unsigned char x[64];
                    if (d->read(d->ctx, ebr, 1, sec, BUFSZ) || sec[510] != 0x55 || sec[511] != 0xAA) break;
                    memcpy(x, sec + 446, 32);
                    if (x[4] && d32(x + 12)) {
                        sprintf(tn, "%02X %s (logical)", x[4], mbr_name(x[4]));
                        part_line(s, d, 5 + num++, ebr + d32(x + 8), d32(x + 12), 0, tn);
                    }
                    next = is_ext(x[16 + 4]) ? d32(x + 16 + 8) : 0;
                    ebr = st + next;
                } while (next && ++guard < 64);
                continue;
            }
            part_line(s, d, i + 1, st, n, 0, tn);
        }
        return;
    }
    if (win_fsdetect(d, 0, d->sectors, fs, sizeof(fs), lbl, sizeof(lbl))) {
        sb_printf(s, "No partition table (whole medium)\r\n  %s\r\n", fs);
        if (lbl[0]) sb_printf(s, "  label \"%s\"\r\n", lbl);
        return;
    }
    sb_printf(s, "No partition table, not formatted\r\n");
}
