/* nhi.c - Thunderbolt / USB4 Native Host Interface: ring 0 control channel, config space
   access to routers and adapters, the in/out mailbox and the firmware mode of the host.
   Everything is polled; the NHI never raises an interrupt. */
#include "tbt.h"

struct ring_desc {              /* 16 bytes */
    u32 phys_lo, phys_hi;
    u32 dw2;                    /* length:12 eof:4 sof:4 flags:12 */
    u32 time;
};
#define DESC_LEN(d)     ((d)->dw2 & 0xFFF)
#define DESC_EOF(d)     (((d)->dw2 >> 12) & 0xF)
#define DESC_SOF(d)     (((d)->dw2 >> 16) & 0xF)
#define DESC_FLAGS(d)   (((d)->dw2 >> 20) & 0xFFF)
#define DESC_MAKE(len, eof, sof, flags) \
    ((u32)((len) & 0xFFF) | ((u32)((eof) & 0xF) << 12) | ((u32)((sof) & 0xF) << 16) | ((u32)((flags) & 0xFFF) << 20))

struct nhi {
    struct pcidev pci;
    u8 *regs;
    u32 nrings;
    u32 version;
    u32 fw_sts;
    int present, started;
    struct ring_desc *txd, *rxd;
    u8 *txbuf, *rxbuf;          /* TB_RING_SIZE frames of TB_FRAME_SIZE each */
    u32 tx_head, rx_head, rx_tail;
};

static struct nhi nhi;
static u32 crc32c_tab[256];
u32 tb_last_error;
u32 tb_cfg_timeout = 2000;

/* events that arrived while a request was waiting for its reply; delivered from tbt_poll */
#define EVQ_LEN 8
static struct { u32 pdf, dwords; u32 data[64]; } evq[EVQ_LEN];
static u32 evq_head, evq_tail;
static u32 evq_dropped;

static u32 bswap(u32 v)
{
    return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}

static void crc32c_init(void)
{
    u32 i, j, c;
    if (crc32c_tab[1]) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78UL : c >> 1;
        crc32c_tab[i] = c;
    }
}

/* ~crc32c(~0, data): the Thunderbolt control packet checksum */
static u32 tb_crc(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFUL;
    while (n--) c = crc32c_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static u32 nhi_rd(u32 off) { return RD32(nhi.regs + off); }
static void nhi_wr(u32 off, u32 v) { WR32(nhi.regs + off, v); }

u32 nhi_fw_sts(void) { return nhi.present ? nhi_rd(NHI_FW_STS) : 0; }
u32 nhi_fw_mode(void) { return nhi.present ? NHI_OUTMAIL_OPMODE(nhi_rd(NHI_OUTMAIL_CMD)) : 0; }
u32 nhi_hif_version(void) { return nhi.version; }
int nhi_present(void) { return nhi.present; }
u16 nhi_device_id(void) { return nhi.pci.device; }
struct pcidev *nhi_pcidev(void) { return &nhi.pci; }

/* the firmware connection manager (ICM) runs on this host */
int nhi_icm_running(void)
{
    if (!nhi.present) return 0;
    return (nhi_rd(NHI_FW_STS) & NHI_FW_STS_ICM_EN) != 0;
}

/* in-mailbox command to the firmware; 0 = done */
int nhi_mailbox(u32 cmd, u32 data)
{
    u32 t0, v;
    if (!nhi.present) return -1;
    nhi_wr(NHI_INMAIL_DATA, data);
    v = nhi_rd(NHI_INMAIL_CMD) & ~(NHI_INMAIL_CMD_MASK | NHI_INMAIL_ERROR);
    v |= (cmd & NHI_INMAIL_CMD_MASK) | NHI_INMAIL_OP_REQ;
    nhi_wr(NHI_INMAIL_CMD, v);
    t0 = plat_ms();
    for (;;) {
        v = nhi_rd(NHI_INMAIL_CMD);
        if (v & NHI_INMAIL_ERROR) return -1;
        if (!(v & NHI_INMAIL_OP_REQ)) return 0;
        if (plat_ms() - t0 > 500) return -1;
        mdelay(1);
    }
}

/* ---- ring 0 ---- */
static void ring_wr(int tx, u32 off, u32 v)
{
    nhi_wr((tx ? NHI_TX_RING_BASE : NHI_RX_RING_BASE) + off, v);
}

static void ring_wr16(int tx, u32 off, u16 v)
{
    WR16(nhi.regs + (tx ? NHI_TX_RING_BASE : NHI_RX_RING_BASE) + off, v);
}

static void ring_opt(int tx, u32 off, u32 v)
{
    nhi_wr((tx ? NHI_TX_OPT_BASE : NHI_RX_OPT_BASE) + off, v);
}

static void rx_post(u32 i)
{
    struct ring_desc *d = &nhi.rxd[i];
    d->phys_lo = vphys(nhi.rxbuf + i * TB_FRAME_SIZE);
    d->phys_hi = 0;
    d->time = 0;
    d->dw2 = DESC_MAKE(0, 0, 0, RDESC_POSTED);
}

static void ring_start(void)
{
    u32 i;
    if (!nhi.txd) {
        nhi.txd = kmalloc_align(TB_RING_SIZE * sizeof(struct ring_desc), 4096);
        nhi.rxd = kmalloc_align(TB_RING_SIZE * sizeof(struct ring_desc), 4096);
        nhi.txbuf = kmalloc_align(TB_RING_SIZE * TB_FRAME_SIZE, 4096);
        nhi.rxbuf = kmalloc_align(TB_RING_SIZE * TB_FRAME_SIZE, 4096);
    }
    memset(nhi.txd, 0, TB_RING_SIZE * sizeof(struct ring_desc));
    memset(nhi.rxd, 0, TB_RING_SIZE * sizeof(struct ring_desc));
    nhi.tx_head = 0;
    nhi.rx_head = 0;
    nhi.rx_tail = 0;
    /* interrupts off, no throttling needed for a polled ring */
    nhi_wr(NHI_RING_INT, 0);
    /* tx ring 0 */
    ring_opt(1, 0, 0);
    ring_wr(1, 0, vphys(nhi.txd));
    ring_wr(1, 4, 0);
    ring_wr(1, 12, TB_RING_SIZE);
    ring_opt(1, 4, 0);
    ring_opt(1, 0, RING_FLAG_ENABLE | RING_FLAG_RAW);
    /* rx ring 0: all but one descriptor posted */
    ring_opt(0, 0, 0);
    ring_wr(0, 0, vphys(nhi.rxd));
    ring_wr(0, 4, 0);
    ring_wr(0, 12, ((u32)TB_FRAME_SIZE << 16) | TB_RING_SIZE);
    ring_opt(0, 4, 0xFFFFFFFFUL);       /* sof / eof masks: every PDF */
    ring_opt(0, 0, RING_FLAG_ENABLE | RING_FLAG_RAW);
    for (i = 0; i < TB_RING_SIZE - 1; i++) rx_post(i);
    nhi.rx_head = TB_RING_SIZE - 1;
    ring_wr16(0, 8, (u16)nhi.rx_head);
    nhi.started = 1;
}

static void ring_stop(void)
{
    if (!nhi.present || !nhi.started) return;
    ring_opt(1, 0, 0);
    ring_opt(0, 0, 0);
    ring_wr(1, 12, 0);
    ring_wr(0, 12, 0);
    ring_wr(1, 0, 0); ring_wr(1, 4, 0);
    ring_wr(0, 0, 0); ring_wr(0, 4, 0);
    nhi.started = 0;
}

/* send one control packet (data in host order, converted to the big-endian wire format) */
int tb_ctl_tx(const u32 *data, u32 dwords, u32 pdf)
{
    u32 i, len, t0, *w;
    struct ring_desc *d;
    if (!nhi.started) return -1;
    if (dwords > (TB_FRAME_SIZE - 4) / 4) return -1;
    w = (u32 *)(nhi.txbuf + nhi.tx_head * TB_FRAME_SIZE);
    for (i = 0; i < dwords; i++) w[i] = bswap(data[i]);
    len = dwords * 4;
    w[dwords] = tb_crc((u8 *)w, len);
    len += 4;
    d = &nhi.txd[nhi.tx_head];
    d->phys_lo = vphys(w);
    d->phys_hi = 0;
    d->time = 0;
    d->dw2 = DESC_MAKE(len, pdf, 0, RDESC_POSTED);
    nhi.tx_head = (nhi.tx_head + 1) % TB_RING_SIZE;
    ring_wr16(1, 10, (u16)nhi.tx_head);
    t0 = plat_ms();
    while (!(DESC_FLAGS(d) & RDESC_COMPLETED)) {
        if (plat_ms() - t0 > 500) {
            kprintf("tbt: control packet %lu not sent\n", pdf);
            return -1;
        }
        plat_udelay(20);
    }
    return 0;
}

/* next received packet, converted to host order; 1 = one copied into buf (<= 64 dwords) */
int tb_ctl_poll(u32 *buf, u32 *dwords, u32 *pdf)
{
    struct ring_desc *d;
    u32 len, i, n, *w, crc, f;
    if (!nhi.started) return 0;
    d = &nhi.rxd[nhi.rx_tail];
    if (!(DESC_FLAGS(d) & RDESC_COMPLETED)) return 0;
    f = DESC_FLAGS(d);
    len = DESC_LEN(d);
    *pdf = DESC_EOF(d);
    w = (u32 *)(nhi.rxbuf + nhi.rx_tail * TB_FRAME_SIZE);
    n = 0;
    if ((f & (RDESC_CRC_ERROR | RDESC_OVERRUN)) || len < 8 || len > TB_FRAME_SIZE || (len & 3)) {
        DBG("tbt: rx frame flags %lx len %lu dropped\n", f, len);
    } else {
        crc = tb_crc((u8 *)w, len - 4);
        if (crc != w[len / 4 - 1]) {
            DBG("tbt: rx crc error pdf %lu\n", *pdf);
        } else {
            n = len / 4 - 1;
            for (i = 0; i < n && i < 64; i++) buf[i] = bswap(w[i]);
            if (n > 64) n = 64;
        }
    }
    *dwords = n;
    /* recycle the slot: post the descriptor the consumer just finished */
    nhi.rx_tail = (nhi.rx_tail + 1) % TB_RING_SIZE;
    rx_post(nhi.rx_head);
    nhi.rx_head = (nhi.rx_head + 1) % TB_RING_SIZE;
    ring_wr16(0, 8, (u16)nhi.rx_head);
    return n ? 1 : 0;
}

static void evq_put(u32 pdf, const u32 *p, u32 n)
{
    u32 next = (evq_head + 1) % EVQ_LEN;
    if (next == evq_tail) {
        evq_dropped++;
        return;
    }
    evq[evq_head].pdf = pdf;
    evq[evq_head].dwords = n;
    memcpy(evq[evq_head].data, p, n * 4);
    evq_head = next;
}

/* called from tbt_poll: hand queued and fresh packets to the connection manager */
void nhi_dispatch(void (*handler)(u32 pdf, const u32 *pkt, u32 dwords))
{
    u32 buf[64], n, pdf;
    int guard = 32;
    while (evq_tail != evq_head) {
        u32 i = evq_tail;
        evq_tail = (evq_tail + 1) % EVQ_LEN;
        handler(evq[i].pdf, evq[i].data, evq[i].dwords);
    }
    while (guard-- && tb_ctl_poll(buf, &n, &pdf)) handler(pdf, buf, n);
}

/* ---- requests with replies ---- */


/* send a request and wait for the packet 'match' accepts; other packets are queued as events.
   Returns the reply length in dwords, 0 on timeout, -1 on send failure */
int tb_ctl_request(const u32 *req, u32 reqdw, u32 pdf, u32 *resp, tb_match_fn match, void *ctx, u32 timeout)
{
    u32 t0, n, rpdf;
    int r;
    if (tb_ctl_tx(req, reqdw, pdf)) return -1;
    t0 = plat_ms();
    for (;;) {
        if (tb_ctl_poll(resp, &n, &rpdf)) {
            r = match(resp, n, rpdf, ctx);
            if (r > 0) return (int)n;
            if (r < 0) return 0;        /* error reply, tb_last_error set */
            evq_put(rpdf, resp, n);
        } else {
            if (plat_ms() - t0 > timeout) return 0;
            plat_udelay(50);
        }
    }
}

struct cfg_ctx { u32 route_lo, route_hi, addr, pdf; };

static int cfg_match(const u32 *r, u32 n, u32 pdf, void *vctx)
{
    struct cfg_ctx *c = vctx;
    if (n < 2) return 0;
    if ((r[0] & 0x3FFFFF) != c->route_hi || r[1] != c->route_lo) return 0;
    if (pdf == TB_PKG_ERROR) {
        if (n < 3) return 0;
        tb_last_error = r[2] & 0xF;
        /* plug event acknowledgements and notifications are not replies */
        if (tb_last_error == TB_ERR_ACK_PLUG_EVENT) return 0;
        if (tb_last_error >= TB_ERR_DP_BW) return 0;
        return -1;
    }
    if (pdf != c->pdf || n < 3) return 0;
    if ((r[2] & 0x07FFFFFFUL) != (c->addr & 0x07FFFFFFUL)) return 0;
    return 1;
}

static u32 cfg_addr(u32 offset, u32 len, int port, int space)
{
    return (offset & 0x1FFF) | ((len & 0x3F) << 13) | ((u32)(port & 0x3F) << 19) | ((u32)(space & 3) << 25);
}

/* read 'len' dwords (<= 60) of a config space; 0 = ok */
int tb_cfg_read(u32 route_lo, u32 route_hi, int port, int space, u32 offset, u32 *buf, u32 len)
{
    u32 req[3], resp[64];
    struct cfg_ctx c;
    int n;
    if (len == 0 || len > 60) return -1;
    req[0] = route_hi & 0x3FFFFF;
    req[1] = route_lo;
    req[2] = cfg_addr(offset, len, port, space);
    c.route_lo = route_lo; c.route_hi = route_hi & 0x3FFFFF; c.addr = req[2]; c.pdf = TB_PKG_READ;
    tb_last_error = 0xFF;
    n = tb_ctl_request(req, 3, TB_PKG_READ, resp, cfg_match, &c, tb_cfg_timeout);
    if (n < 3) return -1;
    if ((u32)n < 3 + len) {
        DBG("tbt: short read reply %d/%lu\n", n, len);
        return -1;
    }
    memcpy(buf, resp + 3, len * 4);
    return 0;
}

int tb_cfg_write(u32 route_lo, u32 route_hi, int port, int space, u32 offset, const u32 *buf, u32 len)
{
    u32 req[64], resp[64];
    struct cfg_ctx c;
    int n;
    if (len == 0 || len > 60) return -1;
    req[0] = route_hi & 0x3FFFFF;
    req[1] = route_lo;
    req[2] = cfg_addr(offset, len, port, space);
    memcpy(req + 3, buf, len * 4);
    c.route_lo = route_lo; c.route_hi = route_hi & 0x3FFFFF; c.addr = req[2]; c.pdf = TB_PKG_WRITE;
    tb_last_error = 0xFF;
    n = tb_ctl_request(req, 3 + len, TB_PKG_WRITE, resp, cfg_match, &c, tb_cfg_timeout);
    return n >= 3 ? 0 : -1;
}

/* reset a router (TB_PKG_RESET, replied with an empty packet of the same type) */
static int reset_match(const u32 *r, u32 n, u32 pdf, void *vctx)
{
    struct cfg_ctx *c = vctx;
    if (n < 2 || pdf != TB_PKG_RESET) return 0;
    return (r[0] & 0x3FFFFF) == c->route_hi && r[1] == c->route_lo;
}

int tb_cfg_reset(u32 route_lo, u32 route_hi)
{
    u32 req[2], resp[64];
    struct cfg_ctx c;
    req[0] = route_hi & 0x3FFFFF;
    req[1] = route_lo;
    c.route_lo = route_lo; c.route_hi = route_hi & 0x3FFFFF; c.addr = 0; c.pdf = TB_PKG_RESET;
    return tb_ctl_request(req, 2, TB_PKG_RESET, resp, reset_match, &c, tb_cfg_timeout) > 0 ? 0 : -1;
}

/* acknowledge a hot plug / unplug event on a port */
void tb_ack_plug(u32 route_lo, u32 route_hi, int port, int unplug)
{
    u32 pkt[3];
    pkt[0] = route_hi & 0x3FFFFF;
    pkt[1] = route_lo;
    pkt[2] = TB_ERR_ACK_PLUG_EVENT | ((u32)(port & 0x3F) << 8) |
             ((u32)(unplug ? TB_ERR_PG_HOT_UNPLUG : TB_ERR_PG_HOT_PLUG) << 30);
    tb_ctl_tx(pkt, 3, TB_PKG_ERROR);
}

/* acknowledge a notification (ROP/POP complete, DP changes, link recovery...) */
void tb_ack_notify(u32 route_lo, u32 route_hi)
{
    u32 pkt[2];
    pkt[0] = route_hi & 0x3FFFFF;
    pkt[1] = route_lo;
    tb_ctl_tx(pkt, 2, TB_PKG_NOTIFY_ACK);
}

/* ---- host interface ---- */
static const u16 intel_nhi_ids[] = {
    0x1513, 0x151A, 0x1547, 0x1548, 0x1567, 0x1569, 0x156B, 0x1575, 0x1577, 0x15BF,
    0x15D2, 0x15D9, 0x15DC, 0x15DD, 0x15DE,             /* Light..Alpine Ridge (FR messages) */
    0x157D,                                             /* Win Ridge */
    0x15E8, 0x15EB, 0x1134, 0x1137,                     /* Titan Ridge, Maple Ridge */
    0x8A17, 0x8A0D, 0x9A1B, 0x9A1D, 0x9A1F, 0x9A21,     /* Ice Lake, Tiger Lake */
    0x463E, 0x466D, 0xA73E, 0xA76D,                     /* Alder Lake, Raptor Lake */
    0x7EB2, 0x7EC2, 0x7EC3, 0x6443,                     /* Meteor Lake, Arrow Lake */
    0xA833, 0xA834,                                     /* Lunar Lake */
    0xE323, 0xE333, 0xE334, 0xE423, 0xE433, 0xE434,     /* Panther Lake */
    0x4D33,                                             /* Wildcat Lake */
    0x5781, 0x5784,                                     /* Barlow Ridge (Thunderbolt 5, 80 / 40 Gbit/s) */
    0
};

/* ICM message generation of an Intel host, 0 for hosts without a firmware CM */
int nhi_icm_generation(void)
{
    u16 id = nhi.pci.device;
    if (nhi.pci.vendor != 0x8086) return 0;
    switch (id) {
    case 0x1513: case 0x151A: case 0x1547: case 0x1548: case 0x1567: case 0x1569: case 0x156B:
    case 0x1575: case 0x1577: case 0x15BF: case 0x15D2: case 0x15D9: case 0x15DC: case 0x15DD:
    case 0x15DE:
        return ICM_GEN_FR;
    case 0x157D:
        return ICM_GEN_FR;
    case 0x15E8: case 0x15EB: case 0x1134: case 0x1137:
        return ICM_GEN_TR;
    default:
        return ICM_GEN_ICL;         /* integrated hosts since Ice Lake use the Titan Ridge format */
    }
}

static int is_nhi(struct pcidev *p)
{
    int i;
    if (p->cls == 0x0C && p->sub == 0x03 && p->prog == 0x40) return 1;      /* USB4 host interface */
    if (p->vendor != 0x8086 || p->cls != 0x08) return 0;
    for (i = 0; intel_nhi_ids[i]; i++)
        if (p->device == intel_nhi_ids[i]) return 1;
    return 0;
}

int nhi_probe(struct pcidev *p)
{
    u32 phys, v, t0;
    if (!is_nhi(p)) return -1;
    if (nhi.present) {
        kprintf("tbt: second host interface %02x:%02x.%u ignored\n", p->bus, p->dev, p->fn);
        return -1;
    }
    phys = pci_bar_mem(p, 0, NULL);
    if (!phys) {
        kprintf("tbt: %02x:%02x.%u has no usable BAR0\n", p->bus, p->dev, p->fn);
        return -1;
    }
    crc32c_init();
    pci_enable(p, 1, 0, 1);
    nhi.pci = *p;
    nhi.regs = plat_mmio(phys, 0x40000);
    if (!nhi.regs) return -1;
    v = nhi_rd(NHI_HOP_COUNT);
    if (v == 0xFFFFFFFFUL) {
        kprintf("tbt: host interface does not respond\n");
        return -1;
    }
    nhi.nrings = v & 0x3FF;
    nhi.version = NHI_CAPS_VERSION(v);
    nhi.fw_sts = nhi_rd(NHI_FW_STS);
    nhi.present = 1;
    kprintf("tbt: host interface %02x:%02x.%u %04x:%04x, %lu rings, version %lx, firmware %s (mode %lu)\n",
            p->bus, p->dev, p->fn, p->vendor, p->device, nhi.nrings, nhi.version,
            nhi_icm_running() ? "CM running" : "idle", nhi_fw_mode());
    /* the ICM needs its NVM authenticated before it answers; wait a little for a cold host */
    if (nhi_icm_running() && !(nhi.fw_sts & NHI_FW_STS_NVM_AUTH)) {
        t0 = plat_ms();
        while (!(nhi_rd(NHI_FW_STS) & NHI_FW_STS_NVM_AUTH) && plat_ms() - t0 < 3000) mdelay(10);
    }
    nhi_wr(NHI_RING_INT, 0);
    ring_start();
    return 0;
}

/* stop the control ring so another owner (VxD, DOS core) can start it; tunnels stay up */
void nhi_stop(void)
{
    ring_stop();
    evq_head = evq_tail = 0;
}

/* a new owner after a handover: start the ring again on the same hardware */
void nhi_restart(void)
{
    if (nhi.present) ring_start();
}
