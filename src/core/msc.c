/* msc.c - USB mass storage: Bulk-Only Transport and USB Attached SCSI (with and without streams) */
#include "usb.h"

#define MAXLUN 8
#define MAXMSC 8

struct mscdev;

struct msclun {
    struct usb_dev *dev;
    struct mscdev *m;
    int lun;
    struct blkdev bd;
    int removable;
    int registered;
    u32 next_check;
    char vendor[9], prod[17];
    struct blkdev *cd;          /* CD/DVD drive: the block device of cdrom.c */
    int sense_held;             /* m->sense holds the sense of the last CD command */
};

struct mscdev {
    struct usb_dev *dev;
    int iface;
    int uas;
    int streams;
    struct usb_pipe in, out;            /* BOT */
    struct usb_pipe cmd, sts, din, dout; /* UAS */
    u32 tag;
    u8 *cbw, *csw, *iu, *siu, *sense, *small;
    int maxlun;
    int nluns;
    int broken;
    struct msclun *luns[MAXLUN];
};

static struct mscdev *mscs[MAXMSC];

#define SCSI_OK         0
#define SCSI_CHECK      1
#define SCSI_ERR        (-1)
#define SCSI_GONE       (-2)

/* ---- Bulk-Only Transport ---- */
static int bot_reset(struct mscdev *m)
{
    kprintf("msc: BOT reset recovery\n");
    usb_control(m->dev, 0x21, 0xFF, 0, (u16)m->iface, NULL, 0);
    mdelay(10);
    usb_clear_halt(&m->in);
    usb_clear_halt(&m->out);
    return 0;
}

static int bot_cmd(struct mscdev *m, int lun, const u8 *cdb, int cdblen, u8 *buf, u32 len, int in, u32 tmo)
{
    u8 *w = m->cbw;
    u32 act = 0, tag = ++m->tag;
    int r, tries;
    memset(w, 0, 31);
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    *(u32 *)(w + 4) = tag;
    *(u32 *)(w + 8) = len;
    w[12] = (u8)(in ? 0x80 : 0x00);
    w[13] = (u8)lun;
    w[14] = (u8)cdblen;
    memcpy(w + 15, cdb, (u32)cdblen);
    r = usb_xfer_sync(&m->out, w, 31, 0, 5000, &act);
    if (r == XS_GONE) return SCSI_GONE;
    if (r == XS_STALL) { usb_clear_halt(&m->out); return SCSI_ERR; }
    if (r) { bot_reset(m); return SCSI_ERR; }
    if (len) {
        r = usb_xfer_sync(in ? &m->in : &m->out, buf, len, in, tmo, &act);
        if (r == XS_GONE) return SCSI_GONE;
        if (r == XS_STALL) usb_clear_halt(in ? &m->in : &m->out);
        else if (r) { bot_reset(m); return SCSI_ERR; }
    }
    for (tries = 0; tries < 2; tries++) {
        r = usb_xfer_sync(&m->in, m->csw, 13, 1, tmo, &act);
        if (r == XS_STALL) { usb_clear_halt(&m->in); continue; }
        break;
    }
    if (r == XS_GONE) return SCSI_GONE;
    if (r || act < 13 || m->csw[0] != 'U' || m->csw[1] != 'S' || m->csw[2] != 'B' || m->csw[3] != 'S' ||
        *(u32 *)(m->csw + 4) != tag) {
        bot_reset(m);
        return SCSI_ERR;
    }
    if (m->csw[12] == 0) return SCSI_OK;
    if (m->csw[12] == 1) return SCSI_CHECK;
    bot_reset(m);
    return SCSI_ERR;
}

/* ---- USB Attached SCSI ---- */
static int uas_cmd(struct mscdev *m, int lun, const u8 *cdb, int cdblen, u8 *buf, u32 len, int in, u32 tmo)
{
    u8 *iu = m->iu, *s = m->siu;
    u16 tag = 1;
    struct usb_xfer sx, dx;
    u32 act;
    int r;
    memset(iu, 0, 32);
    iu[0] = 0x01;                       /* COMMAND IU */
    iu[2] = (u8)(tag >> 8);
    iu[3] = (u8)tag;
    iu[9] = (u8)lun;
    memcpy(iu + 16, cdb, (u32)cdblen);
    memset(&sx, 0, sizeof(sx));
    sx.pipe = &m->sts;
    sx.buf = s;
    sx.len = 96;
    sx.in = 1;
    memset(&dx, 0, sizeof(dx));
    dx.pipe = in ? &m->din : &m->dout;
    dx.buf = buf;
    dx.len = len;
    dx.in = (u8)in;
    if (m->streams) {
        sx.stream = tag;
        dx.stream = tag;
        if (usb_submit(&sx)) return SCSI_ERR;
        if (len && usb_submit(&dx)) { usb_cancel(&sx); return SCSI_ERR; }
        r = usb_xfer_sync(&m->cmd, iu, 32, 0, 5000, &act);
        if (r) { usb_cancel(&sx); if (len) usb_cancel(&dx); return r == XS_GONE ? SCSI_GONE : SCSI_ERR; }
        if (len) {
            r = usb_wait(&dx, tmo);
            if (r && r != XS_STALL) { usb_cancel(&sx); return r == XS_GONE ? SCSI_GONE : SCSI_ERR; }
        }
        r = usb_wait(&sx, tmo);
        if (r) return r == XS_GONE ? SCSI_GONE : SCSI_ERR;
    } else {
        if (usb_submit(&sx)) return SCSI_ERR;
        r = usb_xfer_sync(&m->cmd, iu, 32, 0, 5000, &act);
        if (r) { usb_cancel(&sx); return r == XS_GONE ? SCSI_GONE : SCSI_ERR; }
        r = usb_wait(&sx, tmo);
        if (r) return r == XS_GONE ? SCSI_GONE : SCSI_ERR;
        if (s[0] == 0x06 || s[0] == 0x07) {     /* READ READY / WRITE READY */
            if (usb_submit(&sx)) return SCSI_ERR;
            if (len) {
                r = usb_xfer_sync(dx.pipe, buf, len, in, tmo, &act);
                if (r && r != XS_STALL) { usb_cancel(&sx); return r == XS_GONE ? SCSI_GONE : SCSI_ERR; }
            }
            r = usb_wait(&sx, tmo);
            if (r) return r == XS_GONE ? SCSI_GONE : SCSI_ERR;
        }
    }
    if (s[0] == 0x03) {                 /* SENSE IU */
        u16 sl = (u16)((s[14] << 8) | s[15]);
        if (s[6] == 0) return SCSI_OK;
        if (sl) memcpy(m->sense, s + 16, MIN(sl, 18));
        return SCSI_CHECK | 0x100;      /* sense already available */
    }
    if (s[0] == 0x04) {
        kprintf("msc: UAS response IU code %02x\n", s[7]);
        return SCSI_ERR;
    }
    return SCSI_ERR;
}

/* returns SCSI_OK, or sense key (>0) / error (<0) */
static int scsi(struct msclun *l, const u8 *cdb, int cdblen, u8 *buf, u32 len, int in, u32 tmo)
{
    struct mscdev *m = l->m;
    int r;
    if (m->broken || m->dev->gone) return SCSI_GONE;
    r = m->uas ? uas_cmd(m, l->lun, cdb, cdblen, buf, len, in, tmo) : bot_cmd(m, l->lun, cdb, cdblen, buf, len, in, tmo);
    if (r == SCSI_OK) return SCSI_OK;
    if (r < 0) return r;
    if (!(r & 0x100)) {
        u8 rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        rs[1] = (u8)(l->lun << 5);
        memset(m->sense, 0, 18);
        if ((m->uas ? uas_cmd(m, l->lun, rs, 6, m->sense, 18, 1, 3000)
                    : bot_cmd(m, l->lun, rs, 6, m->sense, 18, 1, 3000)) & ~0x100) return 0x10;
    }
    return (m->sense[2] & 0x0F) ? (m->sense[2] & 0x0F) : 0x10;
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* CD/DVD drive (cdrom.c): one command, data in; a check condition keeps its sense for the
   REQUEST SENSE that follows */
static int cd_packet(void *priv, const u8 *cdb, void *buf, u32 len, u32 ms)
{
    struct msclun *l = priv;
    struct mscdev *m = l->m;
    int r, n = cdb[0] < 0x20 ? 6 : cdb[0] < 0x60 ? 10 : cdb[0] >= 0x80 && cdb[0] < 0xA0 ? 16 : 12;
    if (cdb[0] == 0x03 && l->sense_held) {
        l->sense_held = 0;
        memcpy(buf, m->sense, MIN(len, 18));
        return 0;
    }
    l->sense_held = 0;
    r = scsi(l, cdb, n, buf, len, 1, ms);
    if (r == SCSI_OK) return 0;
    if (r < 0 || r == 0x10) return -1;
    l->sense_held = 1;
    return 1;
}

static int lun_capacity(struct msclun *l)
{
    struct mscdev *m = l->m;
    u8 cdb[16];
    u8 *b = m->small;
    int r, i;
    u32 last, bs;
    for (i = 0; i < 4; i++) {
        memset(cdb, 0, sizeof(cdb));
        r = scsi(l, cdb, 6, NULL, 0, 0, 5000);          /* TEST UNIT READY */
        if (r == SCSI_OK) break;
        if (r < 0) return r;
        if (r == 2) return 2;                           /* NOT READY: no medium */
        mdelay(100);
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    r = scsi(l, cdb, 10, b, 8, 1, 5000);
    if (r) {
        r = scsi(l, cdb, 10, b, 8, 1, 5000);
        if (r) return r;
    }
    last = be32(b);
    bs = be32(b + 4);
    if (last == 0xFFFFFFFFUL) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x9E; cdb[1] = 0x10; cdb[13] = 32;
        r = scsi(l, cdb, 16, b, 32, 1, 5000);
        if (r) return r;
        l->bd.sectors = ((u64)be32(b) << 32) | be32(b + 4);
        l->bd.sectors++;
        bs = be32(b + 8);
    } else {
        l->bd.sectors = (u64)last + 1;
    }
    if (bs < 512 || bs > 4096 || (bs & (bs - 1))) {
        kprintf("msc: unsupported block size %lu\n", bs);
        return SCSI_ERR;
    }
    l->bd.ssize = bs;
    l->bd.maxxfer = BOUNCE_SIZE / bs;
    /* write protect: MODE SENSE(6), all pages */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1A; cdb[2] = 0x3F; cdb[4] = 192;
    l->bd.wprot = 0;
    if (scsi(l, cdb, 6, b, 192, 1, 3000) == SCSI_OK && (b[2] & 0x80)) l->bd.wprot = 1;
    return SCSI_OK;
}

static int msc_rw(struct blkdev *bd, u64 lba, u32 cnt, void *buf, int wr)
{
    struct msclun *l = bd->priv;
    u8 cdb[16];
    int r, tries, len, i;
    for (tries = 0; tries < 3; tries++) {
        memset(cdb, 0, sizeof(cdb));
        if (lba + cnt > 0xFFFFFFFFUL) {
            /* READ(16) / WRITE(16): disks above 2 TB (512-byte blocks) */
            cdb[0] = (u8)(wr ? 0x8A : 0x88);
            for (i = 0; i < 8; i++) cdb[2 + i] = (u8)(lba >> (56 - 8 * i));
            cdb[10] = (u8)(cnt >> 24); cdb[11] = (u8)(cnt >> 16); cdb[12] = (u8)(cnt >> 8); cdb[13] = (u8)cnt;
            len = 16;
        } else {
            cdb[0] = (u8)(wr ? 0x2A : 0x28);
            cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16); cdb[4] = (u8)(lba >> 8); cdb[5] = (u8)lba;
            cdb[7] = (u8)(cnt >> 8); cdb[8] = (u8)cnt;
            len = 10;
        }
        r = scsi(l, cdb, len, buf, cnt * bd->ssize, !wr, 20000);
        if (r == SCSI_OK) return 0;
        if (r == SCSI_GONE) return -2;
        if (r == 2) return -2;          /* no medium */
        if (r == 7) return -3;          /* data protect */
        if (r != 6) mdelay(20);         /* unit attention: retry at once */
    }
    return -1;
}

static int msc_flush(struct blkdev *bd)
{
    struct msclun *l = bd->priv;
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;
    return scsi(l, cdb, 10, NULL, 0, 0, 20000) == SCSI_OK ? 0 : -1;
}

static int msc_ready(struct blkdev *bd)
{
    (void)bd;
    return 0;
}

static struct blkops msc_ops = { msc_rw, msc_flush, msc_ready };

static void lun_attach(struct msclun *l)
{
    int r = lun_capacity(l);
    if (r == SCSI_OK) {
        l->bd.present = 1;
        kprintf("msc: LUN %d \"%s %s\" %lu MB, %lu-byte blocks%s\n", l->lun, l->vendor, l->prod,
                (u32)(l->bd.sectors / (1048576UL / l->bd.ssize)), l->bd.ssize, l->bd.wprot ? ", write protected" : "");
        blk_register(&l->bd);
        l->registered = 1;
    } else if (r == 2) {
        kprintf("msc: LUN %d \"%s %s\": no medium\n", l->lun, l->vendor, l->prod);
    }
}

static void lun_detach(struct msclun *l)
{
    if (l->registered) {
        blk_unregister(&l->bd);
        l->registered = 0;
    }
    l->bd.present = 0;
}

/* pick pipes of the UAS alternate setting by their pipe usage descriptors */
static int uas_pipes(struct mscdev *m, u8 *alt, u8 *end)
{
    u8 *p = alt + alt[0], *ep = NULL;
    int found = 0;
    while (p + 2 <= end && p[0] && p[1] != 4) {
        if (p[1] == 5) ep = p;
        if (p[1] == 0x24 && ep) {
            struct usb_pipe *pp = NULL;
            switch (p[2]) {
            case 1: pp = &m->cmd; break;
            case 2: pp = &m->sts; break;
            case 3: pp = &m->din; break;
            case 4: pp = &m->dout; break;
            }
            if (pp && usb_pipe_open(m->dev, pp, ep) == 0) found |= 1 << p[2];
            ep = NULL;
        }
        p += p[0];
    }
    return found == 0x1E ? 0 : -1;
}

static void close_all(struct mscdev *m)
{
    usb_pipe_close(&m->in);
    usb_pipe_close(&m->out);
    usb_pipe_close(&m->cmd);
    usb_pipe_close(&m->sts);
    usb_pipe_close(&m->din);
    usb_pipe_close(&m->dout);
}

static int msc_probe(struct usb_dev *d, u8 *ifc, u8 *end)
{
    struct mscdev *m;
    u8 *p, *uasalt = NULL, *ep, *bot = NULL;
    int slot, i, ok = 0;
    u8 cdb[6];
    if (ifc[5] != 8) return -1;
    if (cb->opts & OPT_NOMSC) return -1;
    /* look for the BOT alternate (0x50) and a UAS alternate (0x62) of this interface */
    for (p = ifc; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] != 4 || p[2] != ifc[2]) continue;
        if (p[6] == 0x06 && p[7] == 0x62) uasalt = p;
        if (p[6] == 0x06 && p[7] == 0x50 && !bot) bot = p;
        if ((p[6] == 0x05 || p[6] == 0x02 || p[6] == 0x04) && p[7] == 0x50 && !bot) bot = p;
    }
    if (!bot && !uasalt) {
        kprintf("msc: subclass %02x protocol %02x not supported\n", ifc[6], ifc[7]);
        return -1;
    }
    for (slot = 0; slot < MAXMSC && mscs[slot]; slot++) ;
    if (slot == MAXMSC) return -1;
    m = kmalloc(sizeof(*m));
    m->dev = d;
    m->iface = ifc[2];
    m->cbw = kmalloc(64);
    m->csw = kmalloc(64);
    m->iu = kmalloc(64);
    m->siu = kmalloc(128);
    m->sense = kmalloc(32);
    m->small = kmalloc(256);
    /* a SuperSpeed UAS device needs streams; without them its BOT alternate is used */
    if (uasalt && bot && d->hc->nostreams && d->speed >= USB_SS) {
        kprintf("msc: controller without working streams, using Bulk-Only\n");
        uasalt = NULL;
    }
    if (uasalt) {
        if (usb_control(d, 0x01, 0x0B, uasalt[3], (u16)m->iface, NULL, 0) >= 0 && uas_pipes(m, uasalt, end) == 0) {
            m->uas = 1;
            m->streams = d->hc->ops->type == HC_XHCI && d->speed >= USB_SS && m->sts.maxstreams;
            ok = 1;
        } else {
            close_all(m);
            if (bot) usb_control(d, 0x01, 0x0B, bot[3], (u16)m->iface, NULL, 0);
        }
    }
    if (!ok && bot) {
        int gotin = 0, gotout = 0;
        for (ep = bot + bot[0]; ep + 2 <= end && ep[0] && ep[1] != 4; ep += ep[0]) {
            if (ep[1] != 5 || (ep[3] & 3) != EP_BULK) continue;
            if ((ep[2] & 0x80) && !gotin) gotin = usb_pipe_open(d, &m->in, ep) == 0;
            if (!(ep[2] & 0x80) && !gotout) gotout = usb_pipe_open(d, &m->out, ep) == 0;
        }
        if (gotin && gotout) ok = 1;
    }
    if (!ok) {
        kprintf("msc: no usable bulk endpoints (bot %d uas %d)\n", bot != NULL, uasalt != NULL);
        close_all(m);
        kfree(m->cbw); kfree(m->csw); kfree(m->iu); kfree(m->siu); kfree(m->sense); kfree(m->small);
        kfree(m);
        return -1;
    }
    d->driver = m->uas ? UD_UAS : UD_BOT;
    d->uas = (u8)m->uas;
    mscs[slot] = m;
    m->maxlun = 0;
    if (!m->uas) {
        u8 ml = 0;
        if (usb_control(d, 0xA1, 0xFE, 0, (u16)m->iface, &ml, 1) == 1 && ml < MAXLUN) m->maxlun = ml;
    }
    kprintf("msc: %s, %d LUN(s)%s\n", m->uas ? "UAS" : "Bulk-Only", m->maxlun + 1, m->streams ? ", streams" : "");
    for (i = 0; i <= m->maxlun; i++) {
        struct msclun *l = kmalloc(sizeof(*l));
        u8 *inq = m->small;
        l->dev = d;
        l->m = m;
        l->lun = i;
        l->bd.ops = &msc_ops;
        l->bd.priv = l;
        l->bd.kind = DK_USB;
        l->bd.owner = d->id;
        l->bd.removable = 1;
        m->luns[i] = l;
        m->nluns = i + 1;
        memset(cdb, 0, 6);
        cdb[0] = 0x12; cdb[4] = 36;
        if (scsi(l, cdb, 6, inq, 36, 1, 5000) != SCSI_OK) {
            if (scsi(l, cdb, 6, inq, 36, 1, 5000) != SCSI_OK) continue;
        }
        if ((inq[0] & 0x1F) == 5) {
            /* CD/DVD drive: cdrom.c */
            char model[41];
            memcpy(l->vendor, inq + 8, 8); l->vendor[8] = 0; trim_string(l->vendor);
            memcpy(l->prod, inq + 16, 16); l->prod[16] = 0; trim_string(l->prod);
            ksnprintf(model, sizeof(model), "%s %s", l->vendor, l->prod);
            l->cd = cd_register(model, d->serial, DK_USB, crc32(crc32(i, d->ddesc + 8, 4), d->serial, strlen(d->serial)),
                                cd_packet, l);
            if (l->cd) l->cd->owner = d->id;
            continue;
        }
        if ((inq[0] & 0x1F) != 0 && (inq[0] & 0x1F) != 7 && (inq[0] & 0x1F) != 0x0E) {
            kprintf("msc: LUN %d peripheral type %02x skipped\n", i, inq[0] & 0x1F);
            continue;
        }
        l->removable = (inq[1] & 0x80) != 0;
        memcpy(l->vendor, inq + 8, 8); l->vendor[8] = 0; trim_string(l->vendor);
        memcpy(l->prod, inq + 16, 16); l->prod[16] = 0; trim_string(l->prod);
        ksnprintf(l->bd.model, sizeof(l->bd.model), "%s %s", l->vendor, l->prod);
        strlcpy(l->bd.serial, d->serial, sizeof(l->bd.serial));
        l->bd.key = crc32(crc32(i, d->ddesc + 8, 4), d->serial, strlen(d->serial));
        l->bd.key2 = crc32(0, l->prod, strlen(l->prod));
        lun_attach(l);
        l->next_check = plat_ms() + 1000;
    }
    return 0;
}

static void msc_detach(struct usb_dev *d)
{
    int s, i;
    for (s = 0; s < MAXMSC; s++) {
        struct mscdev *m = mscs[s];
        if (!m || m->dev != d) continue;
        m->broken = 1;
        for (i = 0; i < m->nluns; i++) {
            if (!m->luns[i]) continue;
            if (m->luns[i]->cd) cd_unregister(m->luns[i]->cd);
            lun_detach(m->luns[i]);
            kfree(m->luns[i]);
        }
        close_all(m);
        kfree(m->cbw); kfree(m->csw); kfree(m->iu); kfree(m->siu); kfree(m->sense); kfree(m->small);
        kfree(m);
        mscs[s] = NULL;
    }
}

/* media change polling for removable LUNs (card readers, ZIP, etc.) */
void msc_tick(void)
{
    int s, i;
    u32 now = plat_ms();
    u8 cdb[6];
    for (s = 0; s < MAXMSC; s++) {
        struct mscdev *m = mscs[s];
        if (!m || m->broken || m->dev->gone || m->dev->ejected) continue;
        for (i = 0; i < m->nluns; i++) {
            struct msclun *l = m->luns[i];
            int r;
            if (!l || l->cd || !l->removable || (s32)(now - l->next_check) < 0) continue;
            l->next_check = now + 1500;
            memset(cdb, 0, 6);
            r = scsi(l, cdb, 6, NULL, 0, 0, 3000);
            if (r == SCSI_OK && !l->registered) {
                lun_attach(l);
            } else if (r == 6 && l->registered) {
                /* unit attention: medium may have changed */
                lun_detach(l);
                lun_attach(l);
            } else if (r == 2 && l->registered) {
                kprintf("msc: LUN %d medium removed\n", l->lun);
                lun_detach(l);
            } else if (r == 6 && !l->registered) {
                lun_attach(l);
            }
        }
    }
}

/* safe removal: flush, allow removal, eject, release drive letters */
int msc_eject(struct usb_dev *d)
{
    int s, i, rc = 0;
    u8 cdb[6];
    for (s = 0; s < MAXMSC; s++) {
        struct mscdev *m = mscs[s];
        if (!m || m->dev != d) continue;
        for (i = 0; i < m->nluns; i++) {
            struct msclun *l = m->luns[i];
            if (!l) continue;
            if (l->registered) {
                if (msc_flush(&l->bd)) rc = -1;
                lun_detach(l);
            }
            memset(cdb, 0, 6);
            cdb[0] = 0x1E;                      /* allow medium removal */
            scsi(l, cdb, 6, NULL, 0, 0, 3000);
            if (l->removable) {
                memset(cdb, 0, 6);
                cdb[0] = 0x1B; cdb[4] = 0x02;   /* stop + eject */
                scsi(l, cdb, 6, NULL, 0, 0, 10000);
            }
        }
        d->ejected = 1;
        if (d->hub) d->hub->port[d->port].state = PST_EJECTED;
        kprintf("usb: device %u is safe to remove\n", d->id);
    }
    return rc;
}

struct usb_class msc_class = { "msc", msc_probe, msc_detach, NULL };
