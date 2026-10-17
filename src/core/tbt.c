/* tbt.c - Thunderbolt / USB4 software connection manager: enumerates the host router and
   the routers plugged into it (USB4 and Thunderbolt 1-3 devices), reacts to hot plug events
   and builds the PCIe and USB3 tunnels through which the devices' NVMe disks, xHCI
   controllers and USB hubs reach the host. Hosts whose firmware runs the connection
   manager are handed to icm.c. */
#include "tbt.h"

struct tb_switch tb_switches[TB_MAX_SWITCHES];
struct tb_tunnel tb_tunnels[TB_MAX_TUNNELS];
u32 tb_mode;


static int tbt_on;
static u32 next_check_ms;
static u32 opts2;

#define HOST (&tb_switches[0])

/* ---- config space helpers ---- */
static int sw_read(struct tb_switch *sw, u32 off, u32 *buf, u32 n)
{
    return tb_cfg_read(sw->route_lo, sw->route_hi, 0, TB_CFG_SWITCH, off, buf, n);
}

static int sw_write(struct tb_switch *sw, u32 off, const u32 *buf, u32 n)
{
    return tb_cfg_write(sw->route_lo, sw->route_hi, 0, TB_CFG_SWITCH, off, buf, n);
}

static int port_read(struct tb_switch *sw, int port, u32 off, u32 *buf, u32 n)
{
    return tb_cfg_read(sw->route_lo, sw->route_hi, port, TB_CFG_PORT, off, buf, n);
}

static int port_write(struct tb_switch *sw, int port, u32 off, const u32 *buf, u32 n)
{
    return tb_cfg_write(sw->route_lo, sw->route_hi, port, TB_CFG_PORT, off, buf, n);
}

static int hop_read(struct tb_switch *sw, int port, int hop, u32 *buf)
{
    return tb_cfg_read(sw->route_lo, sw->route_hi, port, TB_CFG_HOPS, 2 * (u32)hop, buf, 2);
}

static int hop_write(struct tb_switch *sw, int port, int hop, const u32 *buf)
{
    return tb_cfg_write(sw->route_lo, sw->route_hi, port, TB_CFG_HOPS, 2 * (u32)hop, buf, 2);
}

static int sw_index(struct tb_switch *sw)
{
    return (int)(sw - tb_switches);
}

static struct tb_port *P(struct tb_switch *sw, int port)
{
    return &sw->ports[port];
}

/* capability walk in a port or the switch config space; returns the offset or 0 */
static int port_find_cap(struct tb_switch *sw, int port, u32 cap)
{
    u32 off, hdr;
    int guard = 32;
    if (port_read(sw, port, ADP_CS_1, &hdr, 1)) return 0;
    off = hdr & 0xFF;
    while (off && guard--) {
        if (port_read(sw, port, off, &hdr, 1)) return 0;
        if (((hdr >> 8) & 0xFF) == cap) return (int)off;
        off = hdr & 0xFF;
    }
    return 0;
}

static int sw_find_vse_cap(struct tb_switch *sw, u32 vsec)
{
    u32 off, hdr[2];
    int guard = 32;
    if (sw_read(sw, ROUTER_CS_1, hdr, 1)) return 0;
    off = hdr[0] & 0xFF;
    while (off && guard--) {
        u32 cap;
        if (sw_read(sw, off, hdr, 2)) return 0;
        cap = (hdr[0] >> 8) & 0xFF;
        if (cap == TB_SW_CAP_VSE) {
            if (((hdr[0] >> 16) & 0xFF) == vsec) return (int)off;
            if (((hdr[0] >> 24) & 0xFF) == 0) off = hdr[1] & 0xFFFF;   /* long form */
            else off = hdr[0] & 0xFF;
        } else if (cap == TB_SW_CAP_TMU) {
            off = hdr[0] & 0xFF;
        } else {
            return 0;
        }
    }
    return 0;
}

static int lane_state(struct tb_switch *sw, int port)
{
    u32 v[2];
    struct tb_port *p = P(sw, port);
    if (!p->cap_phy) return -1;
    if (port_read(sw, port, p->cap_phy, v, 2)) return -1;
    return (int)LANE_ADP_CS_1_STATE(v[1]);
}

static int is_lane(struct tb_port *p) { return p->valid && p->type == TB_TYPE_PORT; }
static int is_upstream(struct tb_switch *sw, int port)
{
    return sw->depth && (port == sw->upstream || (P(sw, port)->dual && P(sw, port)->dual == sw->upstream &&
                                                  P(sw, port)->link_nr));
}

static void child_route(struct tb_switch *parent, int port, u32 *lo, u32 *hi)
{
    u32 shift = (u32)parent->depth * 8;
    *lo = parent->route_lo;
    *hi = parent->route_hi;
    if (shift < 32) *lo |= (u32)port << shift;
    else *hi |= (u32)port << (shift - 32);
}

/* ---- router enumeration ---- */
static int switch_enum(struct tb_switch *sw)
{
    u32 hdr[5], ph[ADP_HEADER_DWORDS];
    int i, prev_lane = 0;
    if (sw_read(sw, 0, hdr, 5)) {
        kprintf("tbt: router %08lx%08lx does not answer (error %lu)\n", sw->route_hi, sw->route_lo, tb_last_error);
        return -1;
    }
    sw->vendor = (u16)hdr[0];
    sw->device = (u16)(hdr[0] >> 16);
    sw->upstream = (u8)((hdr[1] >> 8) & 0x3F);
    sw->nports = (u8)((hdr[1] >> 14) & 0x3F);
    sw->cs4 = hdr[4];
    sw->usb4 = (u8)(((hdr[4] >> 24) & 0xFF) == USB4_VERSION_1_0);
    sw->generation = (u8)((hdr[4] >> 24) & 0xFF);
    if (sw->nports >= TB_MAX_PORTS) sw->nports = TB_MAX_PORTS - 1;
    memset(sw->ports, 0, sizeof(sw->ports));
    for (i = 1; i <= sw->nports; i++) {
        struct tb_port *p = P(sw, i);
        p->num = (u8)i;
        if (port_read(sw, i, 0, ph, ADP_HEADER_DWORDS)) {
            if (tb_last_error != TB_ERR_INVALID_SPACE && tb_last_error != TB_ERR_NO_SUCH_PORT &&
                tb_last_error != TB_ERR_PORT_NOT_CONNECTED)
                DBG("tbt: port %d of %s: error %lu\n", i, sw->name, tb_last_error);
            continue;
        }
        p->valid = 1;
        p->type = ph[2] & 0xFFFFFF;
        p->total_credits = (u16)ADP_CS_4_TOTAL(ph[4]);
        p->max_in_hop = (u16)(ph[5] & 0x7FF);
        p->max_out_hop = (u16)((ph[5] >> 11) & 0x7FF);
        p->in_hop_next = p->out_hop_next = TB_PATH_MIN_HOPID;
        if (p->type == TB_TYPE_PORT) {
            p->cap_phy = (u8)port_find_cap(sw, i, TB_ADP_CAP_PHY);
            p->cap_usb4 = (u8)port_find_cap(sw, i, TB_ADP_CAP_USB4);
            if (p->cap_phy) port_read(sw, i, p->cap_phy, &p->cs0, 1);
            /* lane adapters come in pairs: the first is lane 0 of the link */
            if (prev_lane && prev_lane == i - 1) {
                P(sw, prev_lane)->dual = (u8)i;
                p->dual = (u8)prev_lane;
                p->link_nr = 1;
                prev_lane = 0;
            } else {
                prev_lane = i;
            }
        } else if (p->type == TB_TYPE_PCIE_DOWN || p->type == TB_TYPE_PCIE_UP ||
                   p->type == TB_TYPE_USB3_DOWN || p->type == TB_TYPE_USB3_UP ||
                   p->type == TB_TYPE_DP_IN || p->type == TB_TYPE_DP_OUT) {
            p->cap_adap = (u8)port_find_cap(sw, i, TB_ADP_CAP_ADAP);
        }
    }
    if (!sw->usb4) {
        sw->cap_plug = (u16)sw_find_vse_cap(sw, TB_VSE_PLUG_EVENTS);
        sw->cap_lc = (u16)sw_find_vse_cap(sw, TB_VSE_LC);
    }
    sw->max_pcie_credits = TB_MAX_PCIE_CREDITS;
    sw->max_usb3_credits = TB_MAX_USB3_CREDITS;
    ksnprintf(sw->name, sizeof(sw->name), "%s %04x:%04x", sw->depth ? "router" : "host", sw->vendor, sw->device);
    kprintf("tbt: %s at %08lx%08lx: %s %d ports, upstream %d\n", sw->name, sw->route_hi, sw->route_lo,
            sw->usb4 ? "USB4" : "Thunderbolt", sw->nports, sw->upstream);
    return 0;
}

static const char *port_type_name(u32 t)
{
    switch (t) {
    case TB_TYPE_PORT: return "lane";
    case TB_TYPE_NHI: return "NHI";
    case TB_TYPE_DP_IN: return "DP in";
    case TB_TYPE_DP_OUT: return "DP out";
    case TB_TYPE_PCIE_DOWN: return "PCIe down";
    case TB_TYPE_PCIE_UP: return "PCIe up";
    case TB_TYPE_USB3_DOWN: return "USB3 down";
    case TB_TYPE_USB3_UP: return "USB3 up";
    default: return "other";
    }
}

static void switch_dump(struct tb_switch *sw)
{
    int i;
    if (dbg_level < 2) return;
    for (i = 1; i <= sw->nports; i++) {
        struct tb_port *p = P(sw, i);
        if (!p->valid || p->type == TB_TYPE_INACTIVE) continue;
        kprintf("tbt:   port %2d %-9s credits %u hops %u/%u%s%s\n", i, port_type_name(p->type), p->total_credits,
                p->max_in_hop, p->max_out_hop, p->dual ? (p->link_nr ? " lane1" : " lane0") : "",
                p->cap_usb4 ? " usb4" : "");
    }
}

/* ---- router configuration ---- */
static int plug_events_active(struct tb_switch *sw, int on)
{
    u32 v;
    if (sw->usb4 || !sw->cap_plug) return 0;
    v = (sw->cs4 & ~0xFFUL) | 0xFF;         /* plug events delay */
    sw_write(sw, ROUTER_CS_4, &v, 1);
    if (sw_read(sw, sw->cap_plug + 1, &v, 1)) return -1;
    if (on) {
        v &= 0xFFFFFF83UL;
        /* Light, Eagle and Port Ridge: bit 2 must stay clear; Alpine Ridge needs vendor settings */
        if (sw->vendor == 0x8086 && sw->device != 0x1513 && sw->device != 0x151A && sw->device != 0x1549 &&
            sw->device != 0x1575 && sw->device != 0x1577 && sw->device != 0x15BF && sw->device != 0x15D2 &&
            sw->device != 0x15D9)
            v |= 4;
    } else {
        v |= 0x7C;
    }
    return sw_write(sw, sw->cap_plug + 1, &v, 1);
}

static int usb4_switch_setup(struct tb_switch *sw, struct tb_switch *parent)
{
    u32 v, cs6, t0;
    int xhci, tbt3, usb3 = 0, pcie = 0;
    if (!sw->depth) return 0;
    if (sw_read(sw, ROUTER_CS_6, &cs6, 1)) return -1;
    xhci = (cs6 & ROUTER_CS_6_HCI) != 0;
    tbt3 = !(cs6 & ROUTER_CS_6_TNS);
    if (sw_read(sw, ROUTER_CS_5, &v, 1)) return -1;
    v &= ~(ROUTER_CS_5_PTO | ROUTER_CS_5_UTO | ROUTER_CS_5_HCO | ROUTER_CS_5_C3S);
    if (!(opts2 & OPT2_NOTBUSB) && parent->usb4) {
        int i;
        for (i = 1; i <= parent->nports; i++)
            if (P(parent, i)->valid && P(parent, i)->type == TB_TYPE_USB3_DOWN) usb3 = 1;
        for (i = 1; usb3 && i <= sw->nports; i++)
            if (P(sw, i)->valid && P(sw, i)->type == TB_TYPE_USB3_UP) usb3 = 2;
        if (usb3 == 2) {
            v |= ROUTER_CS_5_UTO;
            xhci = 0;
        }
    }
    if (!(opts2 & OPT2_NOTBPCIE)) {
        int i;
        for (i = 1; i <= parent->nports; i++)
            if (P(parent, i)->valid && P(parent, i)->type == TB_TYPE_PCIE_DOWN) pcie = 1;
        if (pcie) {
            v |= ROUTER_CS_5_PTO;
            if (tbt3) v |= ROUTER_CS_5_C3S;
        }
    }
    if (xhci && pcie) v |= ROUTER_CS_5_HCO;    /* internal xHCI reached through the PCIe tunnel */
    v |= ROUTER_CS_5_CV;
    if (sw_write(sw, ROUTER_CS_5, &v, 1)) return -1;
    t0 = plat_ms();
    for (;;) {
        if (sw_read(sw, ROUTER_CS_6, &cs6, 1)) return -1;
        if (cs6 & ROUTER_CS_6_CR) break;
        if (plat_ms() - t0 > 500) {
            kprintf("tbt: %s: configuration not accepted\n", sw->name);
            return -1;
        }
        mdelay(2);
    }
    DBG("tbt: %s: USB4 setup %08lx (pcie %d usb3 %d xhci %d)\n", sw->name, v, pcie, usb3 == 2, xhci);
    return 0;
}

/* link controller: mark the lane of an Intel Thunderbolt router as configured */
static int lc_configure_port(struct tb_switch *sw, int port, int on)
{
    u32 desc, ctrl, lane;
    int cap, start, size, phys;
    if (sw->usb4 || !sw->cap_lc) return 0;
    if (sw_read(sw, sw->cap_lc + TB_LC_DESC, &desc, 1)) return -1;
    start = (int)((desc >> 8) & 0xFF);
    size = (int)((desc >> 16) & 0xFFF);
    phys = (port - 1) / 2;
    cap = sw->cap_lc + start + phys * size;
    if (sw_read(sw, (u32)cap + TB_LC_SX_CTRL, &ctrl, 1)) return -1;
    lane = (port % 2) ? TB_LC_SX_CTRL_L1C : TB_LC_SX_CTRL_L2C;
    if (on) {
        ctrl |= lane;
        if (is_upstream(sw, port)) ctrl |= TB_LC_SX_CTRL_UPSTREAM;
    } else {
        ctrl &= ~lane;
        if (is_upstream(sw, port)) ctrl &= ~TB_LC_SX_CTRL_UPSTREAM;
    }
    return sw_write(sw, (u32)cap + TB_LC_SX_CTRL, &ctrl, 1);
}

static int usb4_port_configure(struct tb_switch *sw, int port, int on)
{
    u32 v;
    struct tb_port *p = P(sw, port);
    if (!p->cap_usb4) return 0;
    if (port_read(sw, port, p->cap_usb4 + PORT_CS_19, &v, 1)) return -1;
    if (on) v |= PORT_CS_19_PC; else v &= ~PORT_CS_19_PC;
    return port_write(sw, port, p->cap_usb4 + PORT_CS_19, &v, 1);
}

static int configure_link(struct tb_switch *sw, struct tb_switch *parent, int on)
{
    int r1, r2;
    if (parent->usb4) r1 = usb4_port_configure(parent, sw->parent_port, on);
    else r1 = lc_configure_port(parent, sw->parent_port, on);
    if (sw->usb4) r2 = usb4_port_configure(sw, sw->upstream, on);
    else r2 = lc_configure_port(sw, sw->upstream, on);
    return r1 | r2;
}

/* USB4 lane adapters: unlock them for the connection manager and enable their hot plug events */
static void usb4_ports_setup(struct tb_switch *sw)
{
    int i;
    u32 v;
    for (i = 1; i <= sw->nports; i++) {
        struct tb_port *p = P(sw, i);
        if (!is_lane(p) || !p->cap_usb4) continue;
        if (port_read(sw, i, ADP_CS_4, &v, 1) == 0 && (v & ADP_CS_4_LCK)) {
            v &= ~ADP_CS_4_LCK;
            port_write(sw, i, ADP_CS_4, &v, 1);
        }
        if (port_read(sw, i, ADP_CS_5, &v, 1) == 0 && (v & ADP_CS_5_DHP)) {
            v &= ~ADP_CS_5_DHP;
            port_write(sw, i, ADP_CS_5, &v, 1);
        }
    }
}

static int switch_configure(struct tb_switch *sw, struct tb_switch *parent)
{
    u32 hdr[3];
    if (sw_read(sw, ROUTER_CS_1, hdr, 1)) return -1;
    hdr[0] = (hdr[0] & ~(7UL << 20)) | ((u32)sw->depth << 20);
    if (sw->usb4) {
        if (sw_write(sw, ROUTER_CS_1, hdr, 1)) return -1;
        usb4_ports_setup(sw);
        if (usb4_switch_setup(sw, parent)) return -1;
    } else {
        hdr[1] = sw->route_lo;
        hdr[2] = sw->route_hi | 0x80000000UL;    /* enabled */
        if (sw_write(sw, ROUTER_CS_1, hdr, 3)) return -1;
        plug_events_active(sw, 1);
    }
    if (sw->depth) configure_link(sw, parent, 1);
    sw->configured = 1;
    return 0;
}

/* ---- hop ids and paths ---- */
static int alloc_link_hop(struct tb_port *out, struct tb_port *in)
{
    int h = MAX(out->out_hop_next, in->in_hop_next);
    int max = MIN(out->max_out_hop ? out->max_out_hop : 0x7FF, in->max_in_hop ? in->max_in_hop : 0x7FF);
    if (h > max) return -1;
    out->out_hop_next = (u8)(h + 1);
    in->in_hop_next = (u8)(h + 1);
    return h;
}

static u8 hop_credits(struct tb_switch *sw, struct tb_port *in, int pcie)
{
    u32 c, total = in->total_credits, limit = pcie ? sw->max_pcie_credits : sw->max_usb3_credits;
    if (in->type == TB_TYPE_PORT) {
        c = 16;
        if (total && total < c + 2) c = total > 8 ? total - 2 : 6;
        if (c > limit) c = limit;
    } else {
        c = 7;
    }
    return (u8)c;
}

/* deactivate one hop entry and wait for the pending bit to clear */
static int hop_deactivate(struct tb_switch *sw, int port, int hop)
{
    u32 v[2], t0;
    if (hop_read(sw, port, hop, v)) return -1;
    if (!(v[0] & HOP_DW0_ENABLE)) return 0;
    v[0] &= ~HOP_DW0_ENABLE;
    if (hop_write(sw, port, hop, v)) return -1;
    t0 = plat_ms();
    for (;;) {
        if (hop_read(sw, port, hop, v)) return -1;
        if (!(v[1] & 0x10000000UL)) {
            v[1] &= ~0x0F000000UL;          /* clear flow control bits */
            return hop_write(sw, port, hop, v);
        }
        if (plat_ms() - t0 > 500) return -1;
        plat_udelay(20);
    }
}

static int path_activate(struct tb_hop *hops, int n, int pcie)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        struct tb_hop *h = &hops[i];
        struct tb_switch *sw = &tb_switches[h->sw];
        u32 v[2];
        int last = (i == n - 1);
        hop_deactivate(sw, h->in_port, h->in_hop);
        v[0] = HOP_DW0(h->next_hop, h->out_port, h->credits, 1);
        v[1] = HOP_DW1(pcie ? TB_PCI_WEIGHT : TB_USB3_WEIGHT, pcie ? TB_PCI_PRIORITY : TB_USB3_PRIORITY,
                       1, !last);
        if (hop_write(sw, h->in_port, h->in_hop, v)) {
            kprintf("tbt: %s: hop %d on port %d failed\n", sw->name, h->in_hop, h->in_port);
            return -1;
        }
    }
    return 0;
}

static void path_deactivate(struct tb_hop *hops, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        struct tb_switch *sw = &tb_switches[hops[i].sw];
        if (!sw->used) continue;
        hop_deactivate(sw, hops[i].in_port, hops[i].in_hop);
    }
}

/* the two paths of a tunnel over one link: adapter of the parent -> lane -> adapter of the
   device, and back. Daisy chained devices tunnel to their parent, whose PCIe switch / USB3
   hub carries the traffic on towards the host. */
static int tunnel_build(struct tb_tunnel *t)
{
    struct tb_switch *dev = &tb_switches[t->sw], *par = &tb_switches[t->src_sw];
    int pcie = (t->type == TB_TUNNEL_PCIE), hop, fixed = pcie ? TB_PCI_HOPID : TB_USB3_HOPID;
    struct tb_port *plane = P(par, dev->parent_port), *dlane = P(dev, dev->upstream);
    t->nhops = 2;
    /* downstream */
    hop = alloc_link_hop(plane, dlane);
    if (hop < 0) return -1;
    t->down[0].sw = t->src_sw;
    t->down[0].in_port = t->src_port;
    t->down[0].in_hop = (u8)fixed;
    t->down[0].out_port = dev->parent_port;
    t->down[0].next_hop = (u8)hop;
    t->down[0].credits = hop_credits(par, P(par, t->src_port), pcie);
    t->down[1].sw = t->sw;
    t->down[1].in_port = dev->upstream;
    t->down[1].in_hop = (u8)hop;
    t->down[1].out_port = t->dst_port;
    t->down[1].next_hop = (u8)fixed;
    t->down[1].credits = hop_credits(dev, dlane, pcie);
    /* upstream */
    hop = alloc_link_hop(dlane, plane);
    if (hop < 0) return -1;
    t->up[0].sw = t->sw;
    t->up[0].in_port = t->dst_port;
    t->up[0].in_hop = (u8)fixed;
    t->up[0].out_port = dev->upstream;
    t->up[0].next_hop = (u8)hop;
    t->up[0].credits = hop_credits(dev, P(dev, t->dst_port), pcie);
    t->up[1].sw = t->src_sw;
    t->up[1].in_port = dev->parent_port;
    t->up[1].in_hop = (u8)hop;
    t->up[1].out_port = t->src_port;
    t->up[1].next_hop = (u8)fixed;
    t->up[1].credits = hop_credits(par, plane, pcie);
    return 0;
}

static int pcie_port_enable(struct tb_switch *sw, int port, int on)
{
    u32 v = on ? ADP_PCIE_CS_0_PE : 0;
    struct tb_port *p = P(sw, port);
    if (!p->cap_adap) return -1;
    return port_write(sw, port, p->cap_adap + ADP_PCIE_CS_0, &v, 1);
}

static int usb3_port_enable(struct tb_switch *sw, int port, int on)
{
    u32 v = on ? (ADP_USB3_CS_0_PE | ADP_USB3_CS_0_V) : ADP_USB3_CS_0_V;
    struct tb_port *p = P(sw, port);
    if (!p->cap_adap) return -1;
    return port_write(sw, port, p->cap_adap + ADP_USB3_CS_0, &v, 1);
}

/* USB3 bandwidth units: (512 << scale) bytes per 125 us microframe */
static u32 mbps_to_usb3(u32 mbps, u32 scale)
{
    u32 bytes = mbps * 125 / 8;                     /* bytes per microframe (mbps * 1e6 / 8 / 8000) */
    u32 unit = 512UL << scale;
    return (bytes + unit - 1) / unit;
}

static u32 usb3_to_mbps(u32 bw, u32 scale)
{
    return bw * (512UL << scale) * 8 / 125;
}

static int usb3_cm_request(struct tb_switch *sw, int port, int on)
{
    u32 v, t0;
    struct tb_port *p = P(sw, port);
    if (port_read(sw, port, p->cap_adap + ADP_USB3_CS_2, &v, 1)) return -1;
    if (on) v |= ADP_USB3_CS_2_CMR; else v &= ~ADP_USB3_CS_2_CMR;
    if (port_write(sw, port, p->cap_adap + ADP_USB3_CS_2, &v, 1)) return -1;
    t0 = plat_ms();
    for (;;) {
        if (port_read(sw, port, p->cap_adap + ADP_USB3_CS_1, &v, 1)) return -1;
        if (((v & ADP_USB3_CS_1_HCA) != 0) == (on != 0)) return 0;
        if (plat_ms() - t0 > 1500) return -1;
        mdelay(1);
    }
}

/* ask the USB3 down adapter of a USB4 host for bandwidth; the adapter grants what it can */
static int usb3_allocate_bandwidth(struct tb_tunnel *t)
{
    struct tb_switch *sw = &tb_switches[t->src_sw];
    struct tb_port *p = P(sw, t->src_port);
    u32 v, scale, rate = 10000, ubw, dbw;
    if (!sw->usb4 || !p->cap_adap) return 0;
    if (port_read(sw, t->src_port, p->cap_adap + ADP_USB3_CS_4, &v, 1) == 0) {
        switch (ADP_USB3_CS_4_MSLR(v)) {
        case 0: rate = 5000; break;
        case 1: rate = 10000; break;
        default: rate = 20000; break;
        }
    }
    if (usb3_cm_request(sw, t->src_port, 1)) return -1;
    if (port_read(sw, t->src_port, p->cap_adap + ADP_USB3_CS_3, &v, 1)) goto out;
    scale = v & ADP_USB3_CS_3_SCALE_MASK;
    ubw = mbps_to_usb3(rate, scale);
    dbw = mbps_to_usb3(rate, scale);
    if (port_read(sw, t->src_port, p->cap_adap + ADP_USB3_CS_2, &v, 1)) goto out;
    v &= ~(ADP_USB3_CS_2_AUBW_MASK | ADP_USB3_CS_2_ADBW_MASK);
    v |= (ubw & ADP_USB3_CS_2_AUBW_MASK) | ((dbw << ADP_USB3_CS_2_ADBW_SHIFT) & ADP_USB3_CS_2_ADBW_MASK);
    if (port_write(sw, t->src_port, p->cap_adap + ADP_USB3_CS_2, &v, 1)) goto out;
    if (port_read(sw, t->src_port, p->cap_adap + ADP_USB3_CS_2, &v, 1) == 0) {
        t->bw_up = usb3_to_mbps(v & ADP_USB3_CS_2_AUBW_MASK, scale);
        t->bw_down = usb3_to_mbps((v & ADP_USB3_CS_2_ADBW_MASK) >> ADP_USB3_CS_2_ADBW_SHIFT, scale);
    }
out:
    usb3_cm_request(sw, t->src_port, 0);
    return 0;
}

static struct tb_tunnel *tunnel_find(int type, int sw)
{
    int i;
    for (i = 0; i < TB_MAX_TUNNELS; i++)
        if (tb_tunnels[i].used && tb_tunnels[i].type == type && tb_tunnels[i].sw == sw) return &tb_tunnels[i];
    return NULL;
}

static int adapter_busy(struct tb_switch *sw, int port)
{
    int i;
    for (i = 0; i < TB_MAX_TUNNELS; i++)
        if (tb_tunnels[i].used && tb_tunnels[i].src_sw == sw_index(sw) && tb_tunnels[i].src_port == port) return 1;
    return 0;
}

/* PCIe down adapter of router 'sw' for a device on its lane 'port' (Intel discrete hosts map them) */
static int find_pcie_down(struct tb_switch *sw, int port)
{
    int i, index = 0, phys = (port - 1) / 2;
    if (!sw->depth && sw->vendor == 0x8086) {
        switch (sw->device) {
        case 0x1547: case 0x1548:                           /* Cactus Ridge */
        case 0x1575: case 0x1577: case 0x15BF: case 0x15D2: case 0x15D9:   /* Alpine Ridge */
            index = phys ? 7 : 6; break;
        case 0x1567: case 0x1569:                           /* Falcon Ridge */
            index = phys ? 8 : 6; break;
        case 0x15E8: case 0x15EB:                           /* Titan Ridge */
            index = phys ? 9 : 8; break;
        }
    }
    if (index && index <= sw->nports && P(sw, index)->type == TB_TYPE_PCIE_DOWN && P(sw, index)->cap_adap &&
        !adapter_busy(sw, index))
        return index;
    for (i = 1; i <= sw->nports; i++)
        if (P(sw, i)->valid && P(sw, i)->type == TB_TYPE_PCIE_DOWN && P(sw, i)->cap_adap && !adapter_busy(sw, i))
            return i;
    return 0;
}

static int find_adapter(struct tb_switch *sw, u32 type)
{
    int i;
    for (i = 1; i <= sw->nports; i++)
        if (P(sw, i)->valid && P(sw, i)->type == type && P(sw, i)->cap_adap) return i;
    return 0;
}

static struct tb_tunnel *tunnel_alloc(void)
{
    int i;
    for (i = 0; i < TB_MAX_TUNNELS; i++)
        if (!tb_tunnels[i].used) {
            memset(&tb_tunnels[i], 0, sizeof(tb_tunnels[i]));
            tb_tunnels[i].used = 1;
            return &tb_tunnels[i];
        }
    return NULL;
}

static int tunnel_pcie(struct tb_switch *dev)
{
    struct tb_tunnel *t;
    struct tb_switch *par = &tb_switches[dev->parent - 1];
    int up, down;
    if (opts2 & OPT2_NOTBPCIE) return 0;
    if (tunnel_find(TB_TUNNEL_PCIE, sw_index(dev))) return 0;
    up = find_adapter(dev, TB_TYPE_PCIE_UP);
    if (!up) return 0;
    down = find_pcie_down(par, dev->parent_port);
    if (!down) {
        kprintf("tbt: %s: no free PCIe down adapter on %s\n", dev->name, par->name);
        return -1;
    }
    t = tunnel_alloc();
    if (!t) return -1;
    t->type = TB_TUNNEL_PCIE;
    t->sw = (u8)sw_index(dev);
    t->src_sw = (u8)sw_index(par);
    t->src_port = (u8)down;
    t->dst_port = (u8)up;
    if (tunnel_build(t)) {
        kprintf("tbt: %s: PCIe path not buildable\n", dev->name);
        t->used = 0;
        return -1;
    }
    if (path_activate(t->down, t->nhops, 1) || path_activate(t->up, t->nhops, 1)) {
        path_deactivate(t->down, t->nhops);
        path_deactivate(t->up, t->nhops);
        t->used = 0;
        return -1;
    }
    if (pcie_port_enable(par, down, 1) || pcie_port_enable(dev, up, 1)) {
        kprintf("tbt: %s: PCIe adapters did not enable\n", dev->name);
    }
    kprintf("tbt: %s: PCIe tunnel from %s adapter %d to adapter %d\n", dev->name, par->name, down, up);
    pcihp_request_rescan(600);
    return 0;
}

static int tunnel_usb3(struct tb_switch *dev)
{
    struct tb_tunnel *t;
    int up, down, i;
    struct tb_switch *par = &tb_switches[dev->parent - 1];
    if (opts2 & OPT2_NOTBUSB) return 0;
    if (!dev->usb4 || !par->usb4) return 0;
    if (tunnel_find(TB_TUNNEL_USB3, sw_index(dev))) return 0;
    up = find_adapter(dev, TB_TYPE_USB3_UP);
    if (!up) return 0;
    down = 0;
    for (i = 1; i <= par->nports; i++)
        if (P(par, i)->valid && P(par, i)->type == TB_TYPE_USB3_DOWN && P(par, i)->cap_adap &&
            !adapter_busy(par, i)) { down = i; break; }
    if (!down) {
        kprintf("tbt: %s: no free USB3 down adapter on %s\n", dev->name, par->name);
        return -1;
    }
    t = tunnel_alloc();
    if (!t) return -1;
    t->type = TB_TUNNEL_USB3;
    t->sw = (u8)sw_index(dev);
    t->src_sw = (u8)sw_index(par);
    t->src_port = (u8)down;
    t->dst_port = (u8)up;
    if (tunnel_build(t)) {
        t->used = 0;
        return -1;
    }
    usb3_allocate_bandwidth(t);
    if (path_activate(t->down, t->nhops, 0) || path_activate(t->up, t->nhops, 0)) {
        path_deactivate(t->down, t->nhops);
        path_deactivate(t->up, t->nhops);
        t->used = 0;
        return -1;
    }
    usb3_port_enable(par, down, 1);
    usb3_port_enable(dev, up, 1);
    kprintf("tbt: %s: USB3 tunnel from %s adapter %d to adapter %d, %lu/%lu Mb/s\n", dev->name, par->name,
            down, up, t->bw_up, t->bw_down);
    return 0;
}

static void tunnel_free(struct tb_tunnel *t, int hw)
{
    struct tb_switch *par = &tb_switches[t->src_sw];
    if (hw && par->used) {
        if (t->type == TB_TUNNEL_PCIE) pcie_port_enable(par, t->src_port, 0);
        else usb3_port_enable(par, t->src_port, 0);
        /* the device router is usually gone: only the hops in routers still present */
        path_deactivate(t->down, t->nhops);
        path_deactivate(t->up, t->nhops);
    }
    t->used = 0;
}

/* tunnels the firmware or BIOS already configured (boot from a Thunderbolt disk): record
   them so their adapters and hop ids stay untouched */
static void discover_tunnels(struct tb_switch *dev)
{
    int i, k;
    struct tb_switch *parent = &tb_switches[dev->parent - 1];
    for (i = 1; i <= parent->nports; i++) {
        struct tb_port *p = P(parent, i);
        u32 v[2];
        int type;
        if (!p->valid || adapter_busy(parent, i)) continue;
        if (p->type == TB_TYPE_PCIE_DOWN) type = TB_TUNNEL_PCIE;
        else if (p->type == TB_TYPE_USB3_DOWN) type = TB_TUNNEL_USB3;
        else continue;
        if (hop_read(parent, i, TB_PCI_HOPID, v)) continue;
        if (!(v[0] & HOP_DW0_ENABLE)) continue;
        if ((int)HOP_DW0_OUT(v[0]) != dev->parent_port && (int)HOP_DW0_OUT(v[0]) != P(parent, dev->parent_port)->dual)
            continue;
        {
            struct tb_tunnel *t = tunnel_alloc();
            u8 hop = (u8)HOP_DW0_NEXT(v[0]);
            if (!t) return;
            t->type = (u8)type;
            t->sw = (u8)sw_index(dev);
            t->src_sw = (u8)sw_index(parent);
            t->discovered = 1;
            t->src_port = (u8)i;
            t->dst_port = (u8)find_adapter(dev, type == TB_TUNNEL_PCIE ? TB_TYPE_PCIE_UP : TB_TYPE_USB3_UP);
            t->nhops = 2;
            if (hop >= P(parent, dev->parent_port)->out_hop_next) P(parent, dev->parent_port)->out_hop_next = (u8)(hop + 1);
            if (hop >= P(dev, dev->upstream)->in_hop_next) P(dev, dev->upstream)->in_hop_next = (u8)(hop + 1);
            for (k = 1; k <= dev->nports; k++) {        /* the return path uses the device's upstream lane */
                u32 u[2];
                if (!P(dev, k)->valid || P(dev, k)->type != (type == TB_TUNNEL_PCIE ? TB_TYPE_PCIE_UP : TB_TYPE_USB3_UP))
                    continue;
                if (hop_read(dev, k, TB_PCI_HOPID, u) == 0 && (u[0] & HOP_DW0_ENABLE)) {
                    u8 h2 = (u8)HOP_DW0_NEXT(u[0]);
                    if (h2 >= P(dev, dev->upstream)->out_hop_next) P(dev, dev->upstream)->out_hop_next = (u8)(h2 + 1);
                    if (h2 >= P(parent, dev->parent_port)->in_hop_next) P(parent, dev->parent_port)->in_hop_next = (u8)(h2 + 1);
                }
            }
            kprintf("tbt: %s: %s tunnel already configured by the firmware (adapter %d)\n", dev->name,
                    type == TB_TUNNEL_PCIE ? "PCIe" : "USB3", i);
        }
    }
}

/* ---- devices ---- */
static void remove_device(struct tb_switch *dev, int hw);

static void scan_ports(struct tb_switch *sw);

static int add_device(struct tb_switch *parent, int port)
{
    struct tb_switch *dev = NULL;
    int i, st, t0;
    if (parent->depth >= TB_MAX_DEPTH) return -1;
    for (i = 1; i < TB_MAX_SWITCHES; i++)
        if (!tb_switches[i].used) { dev = &tb_switches[i]; break; }
    if (!dev) {
        kprintf("tbt: too many routers\n");
        return -1;
    }
    /* wait for the lane to train */
    t0 = (int)plat_ms();
    for (;;) {
        st = lane_state(parent, port);
        if (st == LANE_STATE_CL0) break;
        if (st < 0 || st == LANE_STATE_UNPLUGGED || st == LANE_STATE_DISABLED) return -1;
        if ((int)plat_ms() - t0 > 1500) {
            DBG("tbt: port %d of %s: lane state %d, not up\n", port, parent->name, st);
            return -1;
        }
        mdelay(5);
    }
    memset(dev, 0, sizeof(*dev));
    dev->used = 1;
    dev->depth = (u8)(parent->depth + 1);
    dev->parent = (u8)(sw_index(parent) + 1);
    dev->parent_port = (u8)port;
    child_route(parent, port, &dev->route_lo, &dev->route_hi);
    if (switch_enum(dev)) {
        dev->used = 0;
        return -1;
    }
    switch_dump(dev);
    P(parent, port)->remote_sw = (u8)(sw_index(dev) + 1);
    P(parent, port)->remote_port = dev->upstream;
    if (P(parent, port)->dual) {
        P(parent, P(parent, port)->dual)->remote_sw = (u8)(sw_index(dev) + 1);
        P(parent, P(parent, port)->dual)->remote_port = dev->upstream;
    }
    if (dev->upstream <= dev->nports) {
        P(dev, dev->upstream)->remote_sw = (u8)(sw_index(parent) + 1);
        P(dev, dev->upstream)->remote_port = (u8)port;
    }
    discover_tunnels(dev);
    if (switch_configure(dev, parent)) {
        kprintf("tbt: %s: configuration failed\n", dev->name);
    }
    tunnel_usb3(dev);
    tunnel_pcie(dev);
    scan_ports(dev);            /* daisy chained routers */
    return 0;
}

static void scan_ports(struct tb_switch *sw)
{
    int i;
    for (i = 1; i <= sw->nports; i++) {
        struct tb_port *p = P(sw, i);
        int st;
        if (!is_lane(p) || p->link_nr || is_upstream(sw, i) || p->remote_sw) continue;
        st = lane_state(sw, i);
        if (st != LANE_STATE_CL0) continue;
        add_device(sw, i);
    }
}

static void remove_device(struct tb_switch *dev, int hw)
{
    int i;
    struct tb_switch *parent = &tb_switches[dev->parent - 1];
    /* children first */
    for (i = 1; i <= dev->nports; i++) {
        struct tb_port *p = P(dev, i);
        if (p->remote_sw && !is_upstream(dev, i) && tb_switches[p->remote_sw - 1].parent == sw_index(dev) + 1)
            remove_device(&tb_switches[p->remote_sw - 1], 0);
    }
    for (i = 0; i < TB_MAX_TUNNELS; i++)
        if (tb_tunnels[i].used && tb_tunnels[i].sw == sw_index(dev)) tunnel_free(&tb_tunnels[i], hw);
    kprintf("tbt: %s at %08lx%08lx removed\n", dev->name, dev->route_hi, dev->route_lo);
    P(parent, dev->parent_port)->remote_sw = 0;
    if (P(parent, dev->parent_port)->dual) P(parent, P(parent, dev->parent_port)->dual)->remote_sw = 0;
    /* hop ids on the parent's lane become free again */
    P(parent, dev->parent_port)->in_hop_next = P(parent, dev->parent_port)->out_hop_next = TB_PATH_MIN_HOPID;
    dev->used = 0;
    pcihp_request_rescan(50);
}

/* ---- events ---- */
static struct tb_switch *switch_by_route(u32 lo, u32 hi)
{
    int i;
    for (i = 0; i < TB_MAX_SWITCHES; i++)
        if (tb_switches[i].used && tb_switches[i].route_lo == lo && tb_switches[i].route_hi == hi)
            return &tb_switches[i];
    return NULL;
}

static void handle_plug(struct tb_switch *sw, int port, int unplug)
{
    struct tb_port *p;
    if (port < 1 || port > sw->nports) return;
    p = P(sw, port);
    if (!is_lane(p)) {
        DBG("tbt: %s: plug event on %s adapter %d\n", sw->name, port_type_name(p->type), port);
        return;
    }
    if (p->link_nr && p->dual) port = p->dual;     /* the second lane reports the same link */
    p = P(sw, port);
    if (unplug) {
        if (p->remote_sw) remove_device(&tb_switches[p->remote_sw - 1], 1);
    } else {
        if (p->remote_sw) {
            /* already known (both lanes reported): verify the link is still up */
            if (lane_state(sw, port) != LANE_STATE_CL0) remove_device(&tb_switches[p->remote_sw - 1], 1);
            return;
        }
        add_device(sw, port);
    }
}

void tbt_event(u32 pdf, const u32 *pkt, u32 n)
{
    u32 lo, hi;
    struct tb_switch *sw;
    if (n < 2) return;
    hi = pkt[0] & 0x3FFFFF;
    lo = pkt[1];
    switch (pdf) {
    case TB_PKG_EVENT:
        if (n < 3) return;
        tb_ack_plug(lo, hi, (int)(pkt[2] & 0x3F), (pkt[2] & 0x80000000UL) != 0);
        sw = switch_by_route(lo, hi);
        DBG("tbt: %s on route %08lx%08lx port %lu\n", (pkt[2] & 0x80000000UL) ? "unplug" : "plug", hi, lo, pkt[2] & 0x3F);
        if (sw) handle_plug(sw, (int)(pkt[2] & 0x3F), (pkt[2] & 0x80000000UL) != 0);
        break;
    case TB_PKG_ERROR:
        if (n < 3) return;
        switch (pkt[2] & 0xF) {
        case TB_ERR_ACK_PLUG_EVENT:
            break;
        case TB_ERR_ROP_CMPLT: case TB_ERR_POP_CMPLT: case TB_ERR_PCIE_WAKE: case TB_ERR_DP_CON_CHANGE:
        case TB_ERR_DPTX_DISCOVERY: case TB_ERR_LINK_RECOVERY: case TB_ERR_ASYM_LINK: case TB_ERR_DP_BW:
            tb_ack_notify(lo, hi);
            break;
        default:
            DBG("tbt: error %lu from route %08lx%08lx port %lu\n", pkt[2] & 0xF, hi, lo, (pkt[2] >> 8) & 0x3F);
            break;
        }
        break;
    case TB_PKG_XD_REQ:
        break;                              /* host to host links are not supported */
    default:
        DBG("tbt: packet type %lu (%lu dwords)\n", pdf, n);
        break;
    }
}

/* lanes whose state changed without an event (missed while the ring was stopped) */
static void check_links(void)
{
    int s, i;
    for (s = 0; s < TB_MAX_SWITCHES; s++) {
        struct tb_switch *sw = &tb_switches[s];
        if (!sw->used) continue;
        for (i = 1; i <= sw->nports; i++) {
            struct tb_port *p = P(sw, i);
            int st;
            if (!is_lane(p) || p->link_nr || is_upstream(sw, i)) continue;
            st = lane_state(sw, i);
            if (st < 0) {
                if (sw->depth) break;       /* the router itself is gone: its parent handles it */
                continue;
            }
            if (st == LANE_STATE_CL0 && !p->remote_sw) add_device(sw, i);
            else if (st != LANE_STATE_CL0 && st != LANE_STATE_TRAINING && p->remote_sw)
                remove_device(&tb_switches[p->remote_sw - 1], 1);
        }
    }
}

/* ---- start / poll / stop ---- */
static int swcm_start(void)
{
    struct tb_switch *host = HOST;
    memset(tb_switches, 0, sizeof(tb_switches));
    memset(tb_tunnels, 0, sizeof(tb_tunnels));
    host->used = 1;
    if (switch_enum(host)) {
        kprintf("tbt: host router not reachable, Thunderbolt not used\n");
        host->used = 0;
        return -1;
    }
    switch_dump(host);
    switch_configure(host, host);
    tb_mode = TB_MODE_SWCM;
    scan_ports(host);
    return 0;
}

void tbt_start(struct pcidev *list, int n)
{
    int i, gen;
    opts2 = cb->opts2;
    tb_mode = TB_MODE_NONE;
    tbt_on = 0;
    if (opts2 & OPT2_PCIHP) pcihp_init(list, n);
    if (opts2 & OPT2_NOTB) return;
    if (nhi_present()) {
        nhi_restart();                  /* back from Windows: same hardware, tunnels still up */
    } else {
        for (i = 0; i < n; i++)
            if (nhi_probe(&list[i]) == 0) break;
        if (!nhi_present()) return;
    }
    if (!(opts2 & OPT2_PCIHP)) pcihp_init(list, n);
    gen = nhi_icm_generation();
    if (nhi_icm_running()) {
        if (icm_start() == 0) {
            tb_mode = TB_MODE_ICM;
            tbt_on = 1;
        } else {
            kprintf("tbt: firmware connection manager did not answer\n");
        }
    } else if (gen == ICM_GEN_FR || gen == ICM_GEN_TR) {
        kprintf("tbt: firmware connection manager not running (mode %lu); trying native control\n", nhi_fw_mode());
    }
    if (!tbt_on) {
        if (swcm_start() == 0) tbt_on = 1;
    }
    if (tbt_on) kprintf("tbt: %s connection manager active\n", tb_mode == TB_MODE_ICM ? "firmware" : "software");
    next_check_ms = plat_ms() + 2000;
}

void tbt_poll(void)
{
    u32 now;
    if (tbt_on) {
        nhi_dispatch(tb_mode == TB_MODE_ICM ? icm_event : tbt_event);
        if (tb_mode == TB_MODE_ICM) icm_poll();
        now = plat_ms();
        if ((s32)(now - next_check_ms) >= 0) {
            next_check_ms = now + 2000;
            if (tb_mode == TB_MODE_SWCM) check_links();
        }
    }
    pcihp_poll();
}

/* handover to another owner: the control ring stops, tunnels and PCI devices stay as they are */
void tbt_shutdown(void)
{
    if (!nhi_present()) return;
    if (tb_mode == TB_MODE_ICM) icm_stop();
    nhi_stop();
    tbt_on = 0;
}

/* ---- WINF_TBTREE ---- */
static void node_name(struct wintbnode *nd, const char *s)
{
    strlcpy(nd->name, s, sizeof(nd->name));
}

int tbt_api_tree(void *buf, u32 len, u32 *retlen)
{
    struct wintbnode *nd = (struct wintbnode *)buf;
    u32 max = len / sizeof(*nd), n = 0;
    int i, k;
    struct pcidev *np = nhi_pcidev();
    *retlen = 0;
    if (!nhi_present()) return WINE_NODEV;
    if (max == 0) return WINE_SMALL;
    memset(&nd[n], 0, sizeof(*nd));
    nd[n].type = TN_HOST;
    nd[n].vendor = np->vendor;
    nd[n].device = np->device;
    nd[n].flags = (u8)(tb_mode << 4) | (u8)(tbt_on ? TNF_ACTIVE : 0);
    nd[n].bw_up = nhi_hif_version();
    nd[n].bw_down = tb_mode == TB_MODE_ICM ? icm_security_level : nhi_fw_mode();
    nd[n].route_lo = PCI_BDF(np);
    ksnprintf(nd[n].name, sizeof(nd[n].name), "NHI %02x:%02x.%u %s", np->bus, np->dev, np->fn,
              tb_mode == TB_MODE_ICM ? "firmware CM" : tb_mode == TB_MODE_SWCM ? "software CM" : "idle");
    n++;
    if (tb_mode == TB_MODE_ICM) {
        int m = 0;
        int r = icm_api_tree(nd + n, (max - n) * sizeof(*nd), retlen, &m);
        n += (u32)m;
        *retlen = n * sizeof(*nd);
        return r;
    }
    for (i = 0; i < TB_MAX_SWITCHES && n < max; i++) {
        struct tb_switch *sw = &tb_switches[i];
        if (!sw->used) continue;
        memset(&nd[n], 0, sizeof(*nd));
        nd[n].type = TN_ROUTER;
        nd[n].depth = sw->depth;
        nd[n].port = sw->parent_port;
        nd[n].vendor = sw->vendor;
        nd[n].device = sw->device;
        nd[n].route_lo = sw->route_lo;
        nd[n].route_hi = sw->route_hi;
        nd[n].flags = (u8)((sw->usb4 ? TNF_USB4 : 0) | (sw->configured ? TNF_ACTIVE : 0));
        nd[n].nhops = sw->nports;
        node_name(&nd[n], sw->name);
        n++;
        for (k = 1; k <= sw->nports && n < max; k++) {
            struct tb_port *p = P(sw, k);
            if (!p->valid || p->type == TB_TYPE_INACTIVE || p->type == TB_TYPE_NHI) continue;
            if (p->type == TB_TYPE_PORT && p->link_nr) continue;
            memset(&nd[n], 0, sizeof(*nd));
            nd[n].type = TN_ADAPTER;
            nd[n].depth = sw->depth;
            nd[n].port = (u8)k;
            nd[n].route_lo = sw->route_lo;
            nd[n].route_hi = sw->route_hi;
            nd[n].tunnel = (u8)(p->type == TB_TYPE_PORT ? 0 : (p->type >> 20));
            nd[n].src = (u8)(p->type == TB_TYPE_PORT ? (u8)lane_state(sw, k) : 0);
            nd[n].dst = p->remote_sw;
            nd[n].flags = (u8)((p->cap_usb4 ? TNF_USB4 : 0) | (adapter_busy(sw, k) ? TNF_ACTIVE : 0));
            nd[n].bw_up = p->total_credits;
            node_name(&nd[n], port_type_name(p->type));
            n++;
        }
    }
    for (i = 0; i < TB_MAX_TUNNELS && n < max; i++) {
        struct tb_tunnel *t = &tb_tunnels[i];
        if (!t->used) continue;
        memset(&nd[n], 0, sizeof(*nd));
        nd[n].type = TN_TUNNEL;
        nd[n].tunnel = t->type;
        nd[n].src = t->src_port;
        nd[n].dst = t->dst_port;
        nd[n].nhops = t->nhops;
        nd[n].depth = tb_switches[t->sw].depth;
        nd[n].route_lo = tb_switches[t->sw].route_lo;
        nd[n].route_hi = tb_switches[t->sw].route_hi;
        nd[n].flags = (u8)(t->discovered ? TNF_FIRMWARE : 0) | TNF_ACTIVE;
        nd[n].port = tb_switches[t->src_sw].depth;
        nd[n].bw_up = t->bw_up;
        nd[n].bw_down = t->bw_down;
        node_name(&nd[n], t->type == TB_TUNNEL_PCIE ? "PCIe tunnel" : "USB3 tunnel");
        n++;
    }
    *retlen = n * sizeof(*nd);
    return WINE_OK;
}
