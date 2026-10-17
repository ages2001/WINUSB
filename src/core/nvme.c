/* nvme.c - NVM Express disk driver (polled admin queue + one I/O queue pair)
   A controller the BIOS has running (its INT 13h disk) is borrowed: the BIOS's admin queue is
   shared and an I/O queue pair of our own is added next to the BIOS's, so that the BIOS can
   go on with its own queues once we let go of the controller (Windows exits to DOS). */
#include "core.h"

#define NV_CAP      0x00
#define NV_VS       0x08
#define NV_INTMS    0x0C
#define NV_INTMC    0x10
#define NV_CC       0x14
#define NV_CSTS     0x1C
#define NV_AQA      0x24
#define NV_ASQ      0x28
#define NV_ACQ      0x30

#define QDEPTH      32
#define MAXCTRL     4
#define MAXNS       8

struct nvme_q {
    u8 *sq;             /* sqdepth * 64 */
    u8 *cq;             /* cqdepth * 16 */
    u16 sqt, cqh, phase, cid;
    u16 sqdepth, cqdepth;
    u8 *sqdb, *cqdb;
};

struct nvme_ns {
    struct blkdev bd;
    struct nvme_ctrl *c;
    u32 nsid;
};

struct nvme_ctrl {
    struct pcidev pci;
    u8 *regs;
    u32 dstrd;
    u32 mdts_bytes;
    u32 timeout_ms;
    u8 gone;            /* removed from the bus (Thunderbolt unplug) */
    u8 removable;       /* hot plugged: namespaces get slot drive letters */
    u8 borrow;          /* the BIOS's controller: its admin queue, our I/O queue next to its own */
    u8 ioq_up;          /* our I/O queue pair exists */
    u16 ioqid;          /* its queue id */
    struct nvme_q aq, ioq;
    u8 *prplist;        /* one page of PRP entries */
    u8 *idbuf;
    int nns;
    struct nvme_ns *ns[MAXNS];
    char model[41];
    char serial[21];
};

static struct nvme_ctrl *ctrls[MAXCTRL];
static int nctrl;

static int wait_rdy(struct nvme_ctrl *c, int want)
{
    u32 t0 = plat_ms();
    while (((RD32(c->regs + NV_CSTS) & 1) != 0) != want) {
        if (RD32(c->regs + NV_CSTS) & 2) return -2;     /* fatal */
        if (plat_ms() - t0 > c->timeout_ms) return -1;
        cpu_pause();
    }
    return 0;
}

/* submit one command and poll for completion; returns status field (0 = success) */
static int nvme_cmd(struct nvme_ctrl *c, struct nvme_q *q, u32 *cmd, u32 *dw0)
{
    u32 *sqe = (u32 *)(q->sq + q->sqt * 64);
    u32 t0, *cqe;
    u16 cid = q->cid++;
    int i, st;
    for (i = 0; i < 16; i++) sqe[i] = cmd[i];
    sqe[0] = (cmd[0] & 0xFFFF) | ((u32)cid << 16);
    q->sqt = (u16)((q->sqt + 1) % q->sqdepth);
    WR32(q->sqdb, q->sqt);
    t0 = plat_ms();
    for (;;) {
        cqe = (u32 *)(q->cq + q->cqh * 16);
        if (((cqe[3] >> 16) & 1) == q->phase) break;
        if (c->gone || RD32(c->regs + NV_CSTS) == 0xFFFFFFFFUL) {
            c->gone = 1;                /* unplugged: fail at once instead of waiting */
            return -1;
        }
        if (plat_ms() - t0 > c->timeout_ms) {
            kprintf("nvme: command %02x timeout CSTS=%08lx\n", cmd[0] & 0xFF, RD32(c->regs + NV_CSTS));
            return -1;
        }
        cpu_pause();
    }
    st = (int)((cqe[3] >> 17) & 0x7FFF);
    if (dw0) *dw0 = cqe[0];
    q->cqh = (u16)(q->cqh + 1);
    if (q->cqh == q->cqdepth) { q->cqh = 0; q->phase ^= 1; }
    WR32(q->cqdb, q->cqh);
    if (st) kprintf("nvme: command %02x status %04x\n", cmd[0] & 0xFF, st);
    return st;
}

/* fill PRP1/PRP2 for a contiguous pool buffer */
static void set_prp(struct nvme_ctrl *c, u32 *cmd, void *buf, u32 bytes)
{
    u32 ph = vphys(buf), first, rest, i, n;
    u32 *list = (u32 *)c->prplist;
    cmd[6] = ph; cmd[7] = 0;
    cmd[8] = 0; cmd[9] = 0;
    first = 4096 - (ph & 4095);
    if (bytes <= first) return;
    rest = bytes - first;
    if (rest <= 4096) {
        cmd[8] = (ph + first);
        return;
    }
    n = (rest + 4095) / 4096;
    for (i = 0; i < n; i++) {
        list[i * 2] = ph + first + i * 4096;
        list[i * 2 + 1] = 0;
    }
    cmd[8] = vphys(list);
}

static int nvme_rw(struct blkdev *d, u64 lba, u32 cnt, void *buf, int wr)
{
    struct nvme_ns *ns = d->priv;
    struct nvme_ctrl *c = ns->c;
    u32 cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = wr ? 0x01 : 0x02;
    cmd[1] = ns->nsid;
    set_prp(c, cmd, buf, cnt * d->ssize);
    cmd[10] = (u32)lba;
    cmd[11] = (u32)(lba >> 32);
    cmd[12] = (cnt - 1) & 0xFFFF;
    return nvme_cmd(c, &c->ioq, cmd, NULL) ? -1 : 0;
}

static int nvme_flush(struct blkdev *d)
{
    struct nvme_ns *ns = d->priv;
    u32 cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x00;
    cmd[1] = ns->nsid;
    return nvme_cmd(ns->c, &ns->c->ioq, cmd, NULL) ? -1 : 0;
}

static int nvme_ready(struct blkdev *d)
{
    struct nvme_ns *ns = d->priv;
    return ns->c->gone ? -1 : 0;
}

static struct blkops nvme_ops = { nvme_rw, nvme_flush, nvme_ready };

static void q_setup(struct nvme_ctrl *c, struct nvme_q *q, int qid)
{
    if (!q->sq) {
        q->sq = kmalloc_align(QDEPTH * 64, 4096);
        q->cq = kmalloc_align(QDEPTH * 16, 4096);
    }
    memset(q->sq, 0, QDEPTH * 64);
    memset(q->cq, 0, QDEPTH * 16);
    q->sqt = 0; q->cqh = 0; q->phase = 1; q->cid = 0;
    q->sqdepth = q->cqdepth = QDEPTH;
    q->sqdb = c->regs + 0x1000 + (2 * qid) * (4 << c->dstrd);
    q->cqdb = c->regs + 0x1000 + (2 * qid + 1) * (4 << c->dstrd);
}

/* I/O completion queue, then submission queue c->ioqid (polled, no interrupts) */
static int ioq_create(struct nvme_ctrl *c)
{
    u32 cmd[16];
    q_setup(c, &c->ioq, c->ioqid);
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x05;
    cmd[6] = vphys(c->ioq.cq);
    cmd[10] = ((QDEPTH - 1UL) << 16) | c->ioqid;
    cmd[11] = 1;                        /* physically contiguous, IEN = 0 */
    if (nvme_cmd(c, &c->aq, cmd, NULL)) return -1;
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x01;
    cmd[6] = vphys(c->ioq.sq);
    cmd[10] = ((QDEPTH - 1UL) << 16) | c->ioqid;
    cmd[11] = ((u32)c->ioqid << 16) | 1;    /* its CQ, contiguous */
    if (nvme_cmd(c, &c->aq, cmd, NULL)) return -1;
    c->ioq_up = 1;
    return 0;
}

/* our I/O queue pair goes away, the BIOS's queues stay */
static void ioq_delete(struct nvme_ctrl *c)
{
    u32 cmd[16];
    if (!c->ioq_up) return;
    c->ioq_up = 0;
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x00;                      /* Delete I/O Submission Queue */
    cmd[10] = c->ioqid;
    nvme_cmd(c, &c->aq, cmd, NULL);
    cmd[0] = 0x04;                      /* Delete I/O Completion Queue */
    nvme_cmd(c, &c->aq, cmd, NULL);
}

/* The BIOS has the controller enabled: continue on its admin queue, where every command it
   gave has completed. The newest completion entries carry the phase of the last pass, the
   older ones the other phase: the first change of phase is the BIOS's completion head, and
   the SQ head field of the newest entry its submission tail. An I/O queue pair is added
   with an id the BIOS does not use (create fails with Invalid Queue Identifier for one in
   use). 0 ok; -1: not possible, the controller is reset instead. */
static int ctrl_borrow(struct nvme_ctrl *c)
{
    u32 aqa = RD32(c->regs + NV_AQA), asq = RD32(c->regs + NV_ASQ), acq = RD32(c->regs + NV_ACQ);
    u32 sqs = (aqa & 0xFFF) + 1, cqs = ((aqa >> 16) & 0xFFF) + 1, k, p0, last, dw0, maxq;
    u32 cmd[16];
    if (RD32(c->regs + NV_ASQ + 4) || RD32(c->regs + NV_ACQ + 4) || (asq & 0xFFF) || (acq & 0xFFF)) return -1;
    if (((RD32(c->regs + NV_CC) >> 7) & 0xF) != 0) return -1;      /* page size other than 4K */
    c->aq.sq = plat_mmio(asq, sqs * 64);
    c->aq.cq = plat_mmio(acq, cqs * 16);
    if (!c->aq.sq || !c->aq.cq) return -1;
    p0 = c->aq.cq[14] & 1;
    for (k = 1; k < cqs && (c->aq.cq[k * 16 + 14] & 1) == p0; k++) ;
    if (k == cqs) k = 0;
    c->aq.cqh = (u16)k;
    c->aq.phase = (u16)(k ? p0 : p0 ^ 1);
    last = (k ? k : cqs) - 1;
    c->aq.sqt = (u16)((*(u32 *)(c->aq.cq + last * 16 + 8) & 0xFFFF) % sqs);
    if (!p0 && !k) c->aq.sqt = 0;       /* nothing completed yet */
    c->aq.cid = 0x4000;
    c->aq.sqdepth = (u16)sqs;
    c->aq.cqdepth = (u16)cqs;
    c->aq.sqdb = c->regs + 0x1000;
    c->aq.cqdb = c->regs + 0x1000 + (4 << c->dstrd);
    /* Number of Queues: how many I/O queue pairs the controller gives */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x0A;
    cmd[10] = 7;
    if (nvme_cmd(c, &c->aq, cmd, &dw0)) return -1;
    maxq = MIN(dw0 & 0xFFFF, dw0 >> 16) + 1;
    for (c->ioqid = 1; c->ioqid <= maxq && c->ioqid < 64; c->ioqid++) {
        int st;
        q_setup(c, &c->ioq, c->ioqid);
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x05;
        cmd[6] = vphys(c->ioq.cq);
        cmd[10] = ((QDEPTH - 1UL) << 16) | c->ioqid;
        cmd[11] = 1;
        st = nvme_cmd(c, &c->aq, cmd, NULL);
        if ((st & 0x7FF) == 0x101) continue;   /* Invalid Queue Identifier: the BIOS's */
        if (st) return -1;
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x01;
        cmd[6] = vphys(c->ioq.sq);
        cmd[10] = ((QDEPTH - 1UL) << 16) | c->ioqid;
        cmd[11] = ((u32)c->ioqid << 16) | 1;
        st = nvme_cmd(c, &c->aq, cmd, NULL);
        if (st == 0) break;
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x04;                  /* delete the CQ again */
        cmd[10] = c->ioqid;
        nvme_cmd(c, &c->aq, cmd, NULL);
        if ((st & 0x7FF) != 0x101) return -1;
    }
    if (c->ioqid > maxq || c->ioqid >= 64) {
        kprintf("nvme: no free I/O queue next to the BIOS's (%lu)\n", maxq);
        return -1;
    }
    c->ioq_up = 1;
    c->borrow = 1;
    kprintf("nvme: %02x:%02x.%u shared with the BIOS, our I/O queue %u\n", c->pci.bus, c->pci.dev, c->pci.fn, c->ioqid);
    return 0;
}

static int ctrl_enable(struct nvme_ctrl *c)
{
    if (RD32(c->regs + NV_CC) & 1) {
        WR32(c->regs + NV_CC, RD32(c->regs + NV_CC) & ~1UL);
        if (wait_rdy(c, 0)) return -1;
    }
    q_setup(c, &c->aq, 0);
    WR32(c->regs + NV_INTMS, 0xFFFFFFFFUL);
    WR32(c->regs + NV_AQA, ((QDEPTH - 1UL) << 16) | (QDEPTH - 1));
    WR32(c->regs + NV_ASQ, vphys(c->aq.sq));
    WR32(c->regs + NV_ASQ + 4, 0);
    WR32(c->regs + NV_ACQ, vphys(c->aq.cq));
    WR32(c->regs + NV_ACQ + 4, 0);
    WR32(c->regs + NV_CC, (4UL << 20) | (6UL << 16) | 1);  /* IOCQES 16, IOSQES 64, MPS 4K, EN */
    if (wait_rdy(c, 1)) {
        kprintf("nvme: controller did not become ready\n");
        return -1;
    }
    c->ioqid = 1;
    return ioq_create(c);
}

static int identify(struct nvme_ctrl *c, u32 cns, u32 nsid)
{
    u32 cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x06;
    cmd[1] = nsid;
    cmd[6] = vphys(c->idbuf);
    cmd[10] = cns;
    return nvme_cmd(c, &c->aq, cmd, NULL);
}

static void add_ns(struct nvme_ctrl *c, u32 nsid)
{
    struct nvme_ns *ns;
    u8 *id = c->idbuf;
    u32 nsze, flbas, lbaf, lbads;
    if (c->nns >= MAXNS) return;
    if (identify(c, 0, nsid)) return;
    nsze = *(u32 *)(id + 0);
    if (!nsze && !*(u32 *)(id + 4)) return;         /* exactly 2^32 sectors keeps the high half */
    flbas = id[26] & 0x0F;
    lbaf = *(u32 *)(id + 128 + flbas * 4);
    lbads = (lbaf >> 16) & 0xFF;
    if (lbads < 9 || lbads > 12) {
        kprintf("nvme: ns %lu sector size 2^%lu unsupported\n", nsid, lbads);
        return;
    }
    if ((lbaf & 0xFFFF) != 0) {
        kprintf("nvme: ns %lu uses metadata format, skipped\n", nsid);
        return;
    }
    ns = kmalloc(sizeof(*ns));
    ns->c = c;
    ns->nsid = nsid;
    ns->bd.ops = &nvme_ops;
    ns->bd.priv = ns;
    ns->bd.kind = DK_NVME;
    ns->bd.present = 1;
    ns->bd.removable = c->removable;
    ns->bd.key = (PCI_BDF(&c->pci) << 16) | 0x8000 | nsid;
    ns->bd.ssize = 1UL << lbads;
    ns->bd.sectors = (u64)nsze | ((u64)*(u32 *)(id + 4) << 32);
    ns->bd.maxxfer = MIN(BOUNCE_SIZE, c->mdts_bytes) / ns->bd.ssize;
    strlcpy(ns->bd.model, c->model, sizeof(ns->bd.model));
    strlcpy(ns->bd.serial, c->serial, sizeof(ns->bd.serial));
    ns->bd.key2 = crc32(nsid, c->serial, strlen(c->serial));
    kprintf("nvme%02x:%02x ns %lu: %s, %lu MB, %lu-byte sectors\n", c->pci.bus, c->pci.dev, nsid,
            c->model, (u32)(ns->bd.sectors / (1048576UL / ns->bd.ssize)), ns->bd.ssize);
    c->ns[c->nns++] = ns;
    blk_register(&ns->bd);
}

int nvme_probe_hot(struct pcidev *pd, int removable);

int nvme_probe(struct pcidev *pd)
{
    return nvme_probe_hot(pd, 0);
}

/* removable: a controller that arrived through a Thunderbolt / USB4 tunnel after boot */
int nvme_probe_hot(struct pcidev *pd, int removable)
{
    struct nvme_ctrl *c;
    u32 phys = pci_bar_mem(pd, 0, NULL), cap_lo, cap_hi, nn, i, mps_min;
    u8 *id;
    if (!phys || nctrl >= MAXCTRL) return -1;
    pci_enable(pd, 1, 0, 1);
    c = kmalloc(sizeof(*c));
    c->pci = *pd;
    c->removable = (u8)removable;
    c->regs = plat_mmio(phys, 0x2000);
    if (!c->regs) return -1;
    cap_lo = RD32(c->regs + NV_CAP);
    cap_hi = RD32(c->regs + NV_CAP + 4);
    c->dstrd = cap_hi & 0x0F;
    c->timeout_ms = ((cap_lo >> 24) & 0xFF) * 500 + 1000;
    mps_min = 1UL << (12 + ((cap_hi >> 16) & 0x0F));
    if (mps_min > 4096) {
        kprintf("nvme: minimum page size %lu unsupported\n", mps_min);
        return -1;
    }
    if (RD32(c->regs + NV_CC) & 1) {
        int k, owned = 0;
        for (k = 0; k < 8; k++) if (cb->nvmeown[k] == PCI_BDF(pd) + 1) owned = 1;
        if (!owned && !(cb->opts & OPT_NVME)) {
            kprintf("nvme: %02x:%02x.%u already enabled by BIOS, left alone (use /NVME)\n", pd->bus, pd->dev, pd->fn);
            return -1;
        }
        if (!owned && (RD32(c->regs + NV_CSTS) & 1)) c->borrow = 2;     /* try borrowing below */
    }
    if (0x1000 + 4 * (4 << c->dstrd) > 0x2000) c->regs = plat_mmio(phys, 0x1000 + 4 * (4 << c->dstrd));
    c->prplist = kmalloc_align(4096, 4096);
    c->idbuf = kmalloc_align(4096, 4096);
    kprintf("nvme: %02x:%02x.%u %04x:%04x ver %08lx\n", pd->bus, pd->dev, pd->fn, pd->vendor, pd->device,
            RD32(c->regs + NV_VS));
    if (c->borrow == 2) {
        c->borrow = 0;
        if (ctrl_borrow(c)) {
            kprintf("nvme: %02x:%02x.%u cannot be shared with the BIOS, reset\n", pd->bus, pd->dev, pd->fn);
            c->aq.sq = c->aq.cq = NULL;
        }
    }
    if (!c->borrow && ctrl_enable(c)) return -1;
    if (plat_mode == PM_DOS && !c->borrow) {
        /* enabled by us: the VxD takes it over from the DOS core later */
        int k;
        for (k = 0; k < 8; k++) if (!cb->nvmeown[k]) { cb->nvmeown[k] = (u16)(PCI_BDF(pd) + 1); break; }
    }
    if (identify(c, 1, 0)) return -1;
    id = c->idbuf;
    memcpy(c->serial, id + 4, 20); c->serial[20] = 0; trim_string(c->serial);
    memcpy(c->model, id + 24, 40); c->model[40] = 0; trim_string(c->model);
    c->mdts_bytes = id[77] ? mps_min << id[77] : 0x100000UL;
    if (c->mdts_bytes > 0x100000UL) c->mdts_bytes = 0x100000UL;
    nn = *(u32 *)(id + 516);
    ctrls[nctrl++] = c;
    if (RD32(c->regs + NV_VS) >= 0x00010100UL && !identify(c, 2, 0)) {
        static u32 list[64];
        memcpy(list, c->idbuf, sizeof(list));
        for (i = 0; i < 64 && list[i]; i++) add_ns(c, list[i]);
    } else {
        for (i = 1; i <= nn && i <= 16; i++) add_ns(c, i);
    }
    return 0;
}

/* stop the controller so another owner (VxD / DOS core / BIOS) can take it over */
void nvme_shutdown(void)
{
    int i, j;
    for (i = 0; i < nctrl; i++) {
        struct nvme_ctrl *c = ctrls[i];
        if (c->gone) continue;
        for (j = 0; j < c->nns; j++) nvme_flush(&c->ns[j]->bd);
        if (c->borrow) {
            ioq_delete(c);
            continue;
        }
        WR32(c->regs + NV_CC, RD32(c->regs + NV_CC) & ~1UL);
        wait_rdy(c, 0);
    }
}

void nvme_resume(void)
{
    int i;
    for (i = 0; i < nctrl; i++) {
        struct nvme_ctrl *c = ctrls[i];
        if (c->gone) continue;
        if (c->borrow) ioq_create(c);
        else ctrl_enable(c);
    }
}

/* the controller at bdf vanished (Thunderbolt unplug): drop its disks */
void nvme_remove(u32 bdf)
{
    int i, j;
    for (i = 0; i < nctrl; i++) {
        struct nvme_ctrl *c = ctrls[i];
        if (PCI_BDF(&c->pci) != bdf) continue;
        c->gone = 1;
        for (j = 0; j < c->nns; j++) {
            blk_unregister(&c->ns[j]->bd);
            kfree(c->ns[j]);
        }
        kprintf("nvme: %02x:%02x.%u removed, %d namespace(s) gone\n", c->pci.bus, c->pci.dev, c->pci.fn, c->nns);
        for (j = 0; j < 8; j++) if (cb->nvmeown[j] == bdf + 1) cb->nvmeown[j] = 0;
        if (!c->borrow) {
            kfree(c->aq.sq);
            kfree(c->aq.cq);
        }
        kfree(c->ioq.sq); kfree(c->ioq.cq);
        kfree(c->prplist); kfree(c->idbuf);
        kfree(c);
        ctrls[i] = ctrls[nctrl - 1];
        nctrl--;
        return;
    }
}
