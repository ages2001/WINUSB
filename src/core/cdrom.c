/* cdrom.c - CD/DVD drives on IDE (ATAPI) and AHCI: SCSI MMC commands, medium tracking and
   the requests of the DOS CD-ROM device driver interface that MSCDEX uses (READ LONG,
   IOCTL input/output, ...). The drives are also block devices with 2048-byte sectors
   (USBTREE / WININFO see them), but never get drive letters of their own. */
#include "core.h"

#define MAXCD       8
#define RAWSECT     2352

/* DOS request header */
#define RH_UNIT     1
#define RH_CMD      2
#define RH_STATUS   3
#define ST_ERR      0x8000
#define ST_BUSY     0x0200
#define ST_DONE     0x0100
#define E_UNIT      0x01
#define E_NOTREADY  0x02
#define E_BADCMD    0x03
#define E_SEEK      0x06
#define E_MEDIA     0x07
#define E_NOTFOUND  0x08
#define E_READ      0x0B
#define E_GENERAL   0x0C
#define E_BADCHG    0x0F

/* device status (IOCTL input 6) */
#define DS_DOOROPEN 0x0001
#define DS_UNLOCKED 0x0002
#define DS_RAW      0x0004
#define DS_REDBOOK  0x0200
#define DS_NODISC   0x0800

struct cdrom {
    struct blkdev bd;
    cd_packet_fn packet;
    void *priv;
    u8 media;               /* medium present, capacity known */
    u8 changed;             /* medium changed since DOS last asked */
    u8 gen;                 /* counts medium changes (Windows 9x port driver) */
    u8 locked;
    u8 key, asc, ascq;      /* sense of the last check condition */
    u32 blocks;
    u32 head;               /* last position, for IOCTL "location of head" */
    u32 checked;            /* plat_ms() of the last TEST UNIT READY */
    u8 *small;              /* 256 bytes of pool memory */
};

/* slot i is DOS unit i of the CD-ROM device: a slot keeps its number while the other
   drives come and go (USB), a new drive takes the lowest free slot */
static struct cdrom *cds[MAXCD];
static int ncd;                 /* slots up to the highest one in use */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* one command; a check condition fetches the sense data. 0 ok, 1 check condition, -1 error */
static int cmd(struct cdrom *cd, const u8 *cdb, void *buf, u32 len, u32 ms)
{
    u8 rs[12];
    int r = cd->packet(cd->priv, cdb, buf, len, ms);
    if (r != 1) return r;
    memset(rs, 0, sizeof(rs));
    rs[0] = 0x03;               /* REQUEST SENSE */
    rs[4] = 18;
    memset(cd->small, 0, 18);
    if (cd->packet(cd->priv, rs, cd->small, 18, 2000) == 0) {
        cd->key = cd->small[2] & 0x0F;
        cd->asc = cd->small[12];
        cd->ascq = cd->small[13];
    } else {
        cd->key = 0x02;         /* unknown: not ready */
        cd->asc = cd->ascq = 0;
    }
    if (cd->key == 0x06) {      /* unit attention: medium changed or drive reset */
        cd->changed = 1;
        cd->gen++;
        cd->media = 0;
    }
    return 1;
}

static int read_capacity(struct cdrom *cd)
{
    u8 c[12];
    memset(c, 0, sizeof(c));
    c[0] = 0x25;
    if (cmd(cd, c, cd->small, 8, 5000)) return -1;
    cd->blocks = be32(cd->small) + 1;
    return 0;
}

/* medium state; 0 = ready */
static int cd_ready(struct cdrom *cd, int force)
{
    u8 c[12];
    int i, r = -1;
    u32 now = plat_ms();
    if (!force && cd->media && now - cd->checked < 1000) return 0;
    cd->checked = now;
    memset(c, 0, sizeof(c));
    for (i = 0; i < 4; i++) {
        r = cmd(cd, c, NULL, 0, 5000);        /* TEST UNIT READY */
        if (r != 1 || cd->key != 0x06) break;
    }
    if (r) {
        if (cd->media) {
            cd->changed = 1;
            cd->gen++;
        }
        cd->media = 0;
        cd->bd.present = 0;
        return -2;
    }
    if (!cd->media) {
        if (read_capacity(cd)) return -2;
        cd->media = 1;
        cd->bd.present = 1;
        cd->bd.sectors = cd->blocks;
        kprintf("cd: %s: medium, %lu sectors\n", cd->bd.model, cd->blocks);
    }
    return 0;
}

/* cooked (2048) or raw (2352) sectors into pool memory */
static int read_sectors(struct cdrom *cd, u32 lba, u32 n, u8 *buf, int raw)
{
    u8 c[12];
    memset(c, 0, sizeof(c));
    if (raw) {
        c[0] = 0xBE;            /* READ CD: any sector type, sync + headers + user data + EDC/ECC */
        put_be32(c + 2, lba);
        c[6] = (u8)(n >> 16);
        c[7] = (u8)(n >> 8);
        c[8] = (u8)n;
        c[9] = 0xF8;
    } else {
        c[0] = 0x28;            /* READ(10) */
        put_be32(c + 2, lba);
        c[7] = (u8)(n >> 8);
        c[8] = (u8)n;
    }
    return cmd(cd, c, buf, n * (raw ? RAWSECT : 2048), 20000);
}

static int cd_rw(struct blkdev *d, u64 lba, u32 cnt, void *buf, int wr)
{
    struct cdrom *cd = d->priv;
    int i, r = -1;
    if (wr) return -3;
    if (cd_ready(cd, 0)) return -2;
    for (i = 0; i < 3 && r; i++) {
        r = read_sectors(cd, (u32)lba, cnt, buf, 0);
        if (r == 1 && cd->key == 0x06 && !cd->media) return -2;
    }
    return r ? -1 : 0;
}

static int cd_flush(struct blkdev *d)
{
    (void)d;
    return 0;
}

static int cd_ready_op(struct blkdev *d)
{
    return cd_ready(d->priv, 1);
}

static struct blkops cd_ops = { cd_rw, cd_flush, cd_ready_op };

struct blkdev *cd_register(const char *model, const char *serial, u8 kind, u32 key, cd_packet_fn packet, void *priv)
{
    struct cdrom *cd;
    int slot;
    for (slot = 0; slot < MAXCD && cds[slot]; slot++)
        ;
    if (slot == MAXCD) return NULL;
    cd = kmalloc(sizeof(*cd));
    cd->small = kmalloc_align(256, 256);
    cd->packet = packet;
    cd->priv = priv;
    cd->bd.ops = &cd_ops;
    cd->bd.priv = cd;
    cd->bd.kind = kind;
    cd->bd.cdrom = 1;
    cd->bd.ssize = 2048;
    cd->bd.maxxfer = BOUNCE_SIZE / 2048;
    cd->bd.wprot = 1;
    cd->bd.key = key;
    strlcpy(cd->bd.model, model, sizeof(cd->bd.model));
    strlcpy(cd->bd.serial, serial, sizeof(cd->bd.serial));
    cd->bd.key2 = crc32(0x43440000UL, serial, strlen(serial));
    cds[slot] = cd;
    if (slot >= ncd) ncd = slot + 1;
    cd_ready(cd, 1);
    /* a drive plugged in later than DOS last asked: report a medium change for its unit */
    cd->changed = kind == DK_USB;
    kprintf("cd%d: %s%s\n", slot, model, cd->media ? "" : ", no medium");
    blk_register(&cd->bd);
    return &cd->bd;
}

/* the drive is gone (USB unplug) */
void cd_unregister(struct blkdev *d)
{
    int i;
    for (i = 0; i < ncd; i++) {
        struct cdrom *cd = cds[i];
        if (!cd || &cd->bd != d) continue;
        kprintf("cd%d: %s removed\n", i, cd->bd.model);
        cd->bd.present = 0;
        blk_unregister(&cd->bd);
        kfree(cd->small);
        kfree(cd);
        cds[i] = NULL;
        while (ncd && !cds[ncd - 1]) ncd--;
        return;
    }
}

int cd_count(void)
{
    return ncd;
}

/* the drive of DOS unit u, NULL when none is plugged in */
struct blkdev *cd_unit(int u)
{
    return u < MAXCD && cds[u] ? &cds[u]->bd : NULL;
}

static struct cdrom *cd_of(struct blkdev *d)
{
    int i;
    for (i = 0; i < ncd; i++)
        if (cds[i] && &cds[i]->bd == d) return cds[i];
    return NULL;
}

/* Windows 9x port driver: the medium of CD block device d. Bit 0: a medium is there,
   bits 8-15: count of medium changes; -1: not a CD/DVD drive */
int cd_media(struct blkdev *d)
{
    struct cdrom *cd = cd_of(d);
    if (!cd) return -1;
    cd_ready(cd, 0);
    return (cd->media ? 1 : 0) | ((int)cd->gen << 8);
}

/* Windows 9x port driver: one command the client built (data in only, len bytes at most
   BOUNCE_SIZE, buf in any memory). 0 ok, 1 check condition (18 bytes of sense data in
   sense), -1 error */
int cd_command(struct blkdev *d, const u8 *cdb, void *buf, u32 len, u8 *sense)
{
    struct cdrom *cd = cd_of(d);
    int r;
    if (!cd || len > BOUNCE_SIZE) return -1;
    if (len) memset(bounce, 0, len);
    r = cmd(cd, cdb, len ? bounce : NULL, len, 20000);
    if (r == 0 && len) memcpy(buf, bounce, len);
    if (r == 1 && sense) {
        memset(sense, 0, 18);
        sense[0] = 0x70;
        sense[2] = cd->key;
        sense[7] = 10;
        sense[12] = cd->asc;
        sense[13] = cd->ascq;
    }
    if (cdb[0] == 0x1B && r == 0 && (cdb[4] & 3) == 2) {
        cd->media = 0;          /* ejected */
        cd->bd.present = 0;
        cd->gen++;
    }
    return r;
}

/* ---------------------------------------------------------------- DOS requests */

static void setst(u8 *pkt, u16 st)
{
    *(u16 *)(pkt + RH_STATUS) = st;
}

static u16 sense_error(struct cdrom *cd)
{
    switch (cd->key) {
    case 0x02: return cd->asc == 0x3A ? E_NOTREADY : E_NOTREADY;
    case 0x03: return cd->asc == 0x30 ? E_MEDIA : E_READ;
    case 0x05: return E_NOTFOUND;
    case 0x06: return E_BADCHG;
    }
    return E_GENERAL;
}

/* Red Book (frame | second << 8 | minute << 16) <-> HSG (LBA) */
static u32 rb2hsg(u32 a)
{
    return ((a >> 16) & 0xFF) * 4500UL + ((a >> 8) & 0xFF) * 75UL + (a & 0xFF) - 150;
}

static u32 hsg2rb(u32 l)
{
    l += 150;
    return ((l / 4500) << 16) | (((l / 75) % 60) << 8) | (l % 75);
}

static int start_stop(struct cdrom *cd, u8 how)
{
    u8 c[12];
    memset(c, 0, sizeof(c));
    c[0] = 0x1B;
    c[1] = 1;                   /* immediate */
    c[4] = how;                 /* 2 = eject, 3 = load */
    return cmd(cd, c, NULL, 0, 10000);
}

static int lock_door(struct cdrom *cd, int lock)
{
    u8 c[12];
    memset(c, 0, sizeof(c));
    c[0] = 0x1E;
    c[4] = (u8)(lock ? 1 : 0);
    if (cmd(cd, c, NULL, 0, 5000)) return -1;
    cd->locked = (u8)lock;
    return 0;
}

/* READ TOC (MSF) into cd->small; returns the number of bytes or -1 */
static int read_toc(struct cdrom *cd, u8 track)
{
    u8 c[12];
    memset(c, 0, sizeof(c));
    c[0] = 0x43;
    c[1] = 0x02;                /* MSF */
    c[6] = track;
    c[8] = 252;
    if (cmd(cd, c, cd->small, 252, 5000)) return -1;
    return (int)(((cd->small[0] << 8) | cd->small[1]) + 2);
}

static u32 msf(const u8 *p)
{
    return ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int ioctl_in(struct cdrom *cd, u8 *pkt)
{
    u8 *c = plat_rm(*(u32 *)(pkt + 14));
    int n;
    switch (c[0]) {
    case 1:                     /* location of head */
        *(u32 *)(c + 2) = c[1] ? hsg2rb(cd->head) : cd->head;
        return ST_DONE;
    case 4:                     /* audio channel info */
        c[1] = 0; c[2] = 0xFF; c[3] = 1; c[4] = 0xFF; c[5] = 2; c[6] = 0; c[7] = 3; c[8] = 0;
        return ST_DONE;
    case 6:                     /* device status */
    {
        u32 st = DS_RAW | DS_REDBOOK;
        cd_ready(cd, 0);
        if (!cd->locked) st |= DS_UNLOCKED;
        if (!cd->media) {
            st |= DS_NODISC;
            if (cd->key == 0x02 && cd->asc == 0x3A && cd->ascq == 0x02) st |= DS_DOOROPEN;
        }
        *(u32 *)(c + 1) = st;
        return ST_DONE;
    }
    case 7:                     /* sector size */
        *(u16 *)(c + 2) = c[1] ? RAWSECT : 2048;
        return ST_DONE;
    case 8:                     /* volume size */
        if (cd_ready(cd, 0)) return ST_ERR | ST_DONE | E_NOTREADY;
        *(u32 *)(c + 1) = cd->blocks;
        return ST_DONE;
    case 9:                     /* media changed */
        cd_ready(cd, 1);
        c[1] = cd->changed ? 0xFF : 1;
        cd->changed = 0;
        return ST_DONE;
    case 10:                    /* audio disc info: first and last track, lead-out */
        if (cd_ready(cd, 0) || (n = read_toc(cd, 0)) < 12) return ST_ERR | ST_DONE | E_NOTREADY;
        c[1] = cd->small[2];
        c[2] = cd->small[3];
        {
            int i;
            u32 lo = 0;
            for (i = 4; i + 8 <= n; i += 8)
                if (cd->small[i + 2] == 0xAA) lo = msf(cd->small + i + 4);
            *(u32 *)(c + 3) = lo;
        }
        return ST_DONE;
    case 11:                    /* audio track info: start and control of track c[1] */
    {
        int i;
        if (cd_ready(cd, 0) || (n = read_toc(cd, 0)) < 12) return ST_ERR | ST_DONE | E_NOTREADY;
        for (i = 4; i + 8 <= n; i += 8)
            if (cd->small[i + 2] == c[1]) {
                *(u32 *)(c + 2) = msf(cd->small + i + 4);
                c[6] = (u8)((cd->small[i + 1] & 0x0F) << 4 | (cd->small[i + 1] >> 4));
                return ST_DONE;
            }
        return ST_ERR | ST_DONE | E_NOTFOUND;
    }
    case 15:                    /* audio status: not paused, no play range */
        memset(c + 1, 0, 10);
        return ST_DONE;
    case 5:                     /* drive bytes */
        c[1] = 0;
        return ST_DONE;
    }
    return ST_ERR | ST_DONE | E_BADCMD;
}

static int ioctl_out(struct cdrom *cd, u8 *pkt)
{
    u8 *c = plat_rm(*(u32 *)(pkt + 14));
    switch (c[0]) {
    case 0:                     /* eject */
        if (cd->locked) return ST_ERR | ST_DONE | E_NOTREADY;
        start_stop(cd, 2);
        cd->media = 0;
        cd->bd.present = 0;
        cd->changed = 1;
        return ST_DONE;
    case 1:                     /* lock / unlock door */
        return lock_door(cd, c[1] != 0) ? (ST_ERR | ST_DONE | E_GENERAL) : ST_DONE;
    case 2:                     /* reset drive */
        cd->media = 0;
        cd_ready(cd, 1);
        return ST_DONE;
    case 3:                     /* audio channel control */
    case 4:                     /* device control string */
        return ST_DONE;
    case 5:                     /* close tray */
        start_stop(cd, 3);
        return ST_DONE;
    }
    return ST_ERR | ST_DONE | E_BADCMD;
}

static int read_long(struct cdrom *cd, u8 *pkt)
{
    u16 n = *(u16 *)(pkt + 18);
    u32 start = *(u32 *)(pkt + 20), done = 0;
    int raw = pkt[24] != 0, r;
    u32 ss = raw ? RAWSECT : 2048, per = BOUNCE_SIZE / ss;
    u8 *dst = plat_rm(*(u32 *)(pkt + 14));
    if (pkt[13] == 1) start = rb2hsg(start);
    if (pkt[13] > 1 || pkt[24] > 1) return ST_ERR | ST_DONE | E_BADCMD;
    if (cd_ready(cd, 0)) return ST_ERR | ST_DONE | (cd->changed ? E_BADCHG : E_NOTREADY);
    if (start + n > cd->blocks) return ST_ERR | ST_DONE | E_NOTFOUND;
    while (done < n) {
        u32 k = MIN(per, (u32)n - done);
        r = read_sectors(cd, start + done, k, bounce, raw);
        if (r == 1 && cd->key == 0x06) r = read_sectors(cd, start + done, k, bounce, raw);
        if (r) {
            *(u16 *)(pkt + 18) = (u16)done;
            return ST_ERR | ST_DONE | (r == 1 ? sense_error(cd) : E_READ);
        }
        memcpy(dst + done * ss, bounce, k * ss);
        done += k;
        cd->bd.reads += k;
    }
    cd->head = start + n;
    return ST_DONE;
}

int cd_dos_request(u8 *pkt)
{
    u8 unit = pkt[RH_UNIT];
    struct cdrom *cd;
    if (unit >= MAXCD) {
        setst(pkt, ST_ERR | ST_DONE | E_UNIT);
        return 0;
    }
    cd = cds[unit];
    if (!cd) {
        /* a USB drive that is not plugged in (now) */
        switch (pkt[RH_CMD]) {
        case 3:
        {
            u8 *cb = plat_rm(*(u32 *)(pkt + 14));
            if (cb[0] == 6) {               /* device status: no disc, door open */
                *(u32 *)(cb + 1) = DS_DOOROPEN | DS_UNLOCKED | DS_RAW | DS_NODISC;
                setst(pkt, ST_DONE);
                return 0;
            }
            if (cb[0] == 9) {               /* media changed: yes */
                cb[1] = 0xFF;
                setst(pkt, ST_DONE);
                return 0;
            }
            break;
        }
        case 7: case 11: case 13: case 14:
            setst(pkt, ST_DONE);
            return 0;
        }
        setst(pkt, ST_ERR | ST_DONE | E_NOTREADY);
        return 0;
    }
    switch (pkt[RH_CMD]) {
    case 3:                     /* IOCTL input */
        setst(pkt, (u16)ioctl_in(cd, pkt));
        return 0;
    case 12:                    /* IOCTL output */
        setst(pkt, (u16)ioctl_out(cd, pkt));
        return 0;
    case 7:                     /* input flush */
    case 11:                    /* output flush */
    case 13:                    /* device open */
    case 14:                    /* device close */
    case 130:                   /* read long prefetch */
        setst(pkt, ST_DONE);
        return 0;
    case 131:                   /* seek */
        cd->head = pkt[13] == 1 ? rb2hsg(*(u32 *)(pkt + 20)) : *(u32 *)(pkt + 20);
        setst(pkt, ST_DONE);
        return 0;
    case 128:                   /* read long */
        setst(pkt, (u16)read_long(cd, pkt));
        return 0;
    }
    /* audio play / stop / resume and the write commands are not supported */
    setst(pkt, ST_ERR | ST_DONE | E_BADCMD);
    return 0;
}
