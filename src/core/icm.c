/* icm.c - Intel firmware connection manager (ICM) of Falcon/Alpine/Titan/Maple Ridge and of
   the Thunderbolt controllers integrated since Ice Lake, when the BIOS runs them in firmware
   mode. The firmware enumerates routers and builds the tunnels; the driver announces itself,
   approves the devices that connect and then finds their PCIe functions. */
#include "tbt.h"

u32 icm_security_level;
int icm_generation;

#define ICM_MAX_DEVS 8
struct icm_dev {
    u8 used, approved, boot, depth, link;
    u8 connection_id, connection_key;
    u16 link_info;
    u32 route_lo, route_hi;
    u32 uuid[4];
};
static struct icm_dev devs[ICM_MAX_DEVS];
static int icm_ready;


static u32 icm_hdr(u32 code, u32 flags)
{
    return (code & 0xFF) | ((flags & 0xFF) << 8);
}

static int icm_match(const u32 *r, u32 n, u32 pdf, void *ctx)
{
    u32 code = *(u32 *)ctx;
    if (pdf != TB_PKG_ICM_RESP || n < 1) return 0;
    return (r[0] & 0xFF) == code;
}

/* send an ICM command and wait for its reply (first packet of a multi packet reply) */
static int icm_request(const u32 *req, u32 reqdw, u32 *resp, u32 timeout)
{
    u32 code = req[0] & 0xFF;
    int n = tb_ctl_request(req, reqdw, TB_PKG_ICM_CMD, resp, icm_match, &code, timeout);
    if (n <= 0) return -1;
    if ((resp[0] >> 8) & ICM_FLAGS_ERROR) {
        DBG("icm: command %lx failed\n", code);
        return -2;
    }
    return n;
}

static const char *slevel_name(u32 sl)
{
    switch (sl) {
    case TB_SECURITY_NONE: return "none";
    case TB_SECURITY_USER: return "user authorization";
    case TB_SECURITY_SECURE: return "secure connect";
    case TB_SECURITY_DPONLY: return "DisplayPort only";
    case TB_SECURITY_USBONLY: return "USB only";
    case TB_SECURITY_NOPCIE: return "no PCIe";
    default: return "?";
    }
}

static int driver_ready(void)
{
    u32 req[1], resp[64];
    int n, tries;
    req[0] = icm_hdr(ICM_DRIVER_READY, 0);
    for (tries = 0; tries < 3; tries++) {
        n = icm_request(req, 1, resp, 5000);
        if (n >= 2) break;
    }
    if (n < 2) return -1;
    if (icm_generation == ICM_GEN_FR)
        icm_security_level = (resp[1] >> 16) & 0xF;
    else
        icm_security_level = (resp[1] >> 16) & 0x7;
    kprintf("icm: firmware ready, security level %lu (%s)\n", icm_security_level, slevel_name(icm_security_level));
    return 0;
}

static struct icm_dev *dev_find_uuid(const u32 *uuid)
{
    int i;
    for (i = 0; i < ICM_MAX_DEVS; i++)
        if (devs[i].used && !memcmp(devs[i].uuid, uuid, 16)) return &devs[i];
    return NULL;
}

static struct icm_dev *dev_alloc(void)
{
    int i;
    for (i = 0; i < ICM_MAX_DEVS; i++)
        if (!devs[i].used) {
            memset(&devs[i], 0, sizeof(devs[i]));
            devs[i].used = 1;
            return &devs[i];
        }
    return NULL;
}

static int approve_device(struct icm_dev *d)
{
    u32 req[8], resp[64];
    int n;
    req[0] = icm_hdr(ICM_APPROVE_DEVICE, 0);
    memcpy(req + 1, d->uuid, 16);
    if (icm_generation == ICM_GEN_FR) {
        req[5] = d->connection_key | ((u32)d->connection_id << 8);
        n = icm_request(req, 6, resp, 10000);
    } else {
        req[5] = d->route_lo;
        req[6] = d->route_hi;
        req[7] = d->connection_id;
        n = icm_request(req, 8, resp, 10000);
    }
    if (n < 0) {
        kprintf("icm: device %08lx-%08lx not approved by the firmware\n", d->uuid[0], d->uuid[1]);
        return -1;
    }
    d->approved = 1;
    kprintf("icm: device %08lx-%08lx approved\n", d->uuid[0], d->uuid[1]);
    return 0;
}

static void device_connected(const u32 *p, u32 n)
{
    struct icm_dev *d;
    u32 flags = (p[0] >> 8) & 0xFF, link_info, sl = ICM_FLAGS_SLEVEL(flags);
    int fresh = 0;
    if (n < (u32)(icm_generation == ICM_GEN_FR ? 6 : 8)) return;
    d = dev_find_uuid(p + 1);
    if (!d) {
        d = dev_alloc();
        if (!d) {
            kprintf("icm: too many devices\n");
            return;
        }
        memcpy(d->uuid, p + 1, 16);
        fresh = 1;
    }
    if (icm_generation == ICM_GEN_FR) {
        d->connection_key = (u8)p[5];
        d->connection_id = (u8)(p[5] >> 8);
        link_info = p[5] >> 16;
        d->link = (u8)ICM_LINK_INFO_LINK(link_info);
        d->depth = (u8)ICM_LINK_INFO_DEPTH(link_info);
        d->route_lo = d->link;
        d->route_hi = 0;
    } else {
        d->route_lo = p[5];
        d->route_hi = p[6];
        d->connection_id = (u8)p[7];
        link_info = p[7] >> 16;
        d->link = (u8)ICM_LINK_INFO_LINK(link_info);
        d->depth = (u8)ICM_LINK_INFO_DEPTH(link_info);
    }
    d->link_info = (u16)link_info;
    d->approved = (link_info & ICM_LINK_INFO_APPROVED) != 0;
    d->boot = (link_info & ICM_LINK_INFO_BOOT) != 0;
    kprintf("icm: device %08lx-%08lx on link %u depth %u%s%s%s\n", d->uuid[0], d->uuid[1], d->link, d->depth,
            d->approved ? ", approved" : "", d->boot ? ", boot" : "", (link_info & ICM_LINK_INFO_REJECTED) ? ", rejected" : "");
    if (!d->approved && !(link_info & ICM_LINK_INFO_REJECTED)) {
        switch (sl) {
        case TB_SECURITY_NONE:
        case TB_SECURITY_USER:
            approve_device(d);
            break;
        case TB_SECURITY_SECURE:
            kprintf("icm: security level 'secure connect' needs a stored key: set the BIOS Thunderbolt security\n"
                    "icm: level to 'no security' or 'user authorization' for this driver\n");
            break;
        default:
            kprintf("icm: security level %lu: PCIe tunnels are disabled by the BIOS\n", sl);
            break;
        }
    }
    (void)fresh;
    /* the firmware builds the tunnels; the PCIe functions appear a little later */
    pcihp_request_rescan(d->approved ? 800 : 2000);
}

static void device_disconnected(const u32 *p, u32 n)
{
    int i;
    if (icm_generation == ICM_GEN_FR) {
        u32 link_info = n >= 2 ? p[1] >> 16 : 0;
        for (i = 0; i < ICM_MAX_DEVS; i++)
            if (devs[i].used && devs[i].link == ICM_LINK_INFO_LINK(link_info) &&
                devs[i].depth == ICM_LINK_INFO_DEPTH(link_info)) devs[i].used = 0;
    } else {
        if (n < 3) return;
        for (i = 0; i < ICM_MAX_DEVS; i++)
            if (devs[i].used && devs[i].route_lo == p[1] && devs[i].route_hi == p[2]) devs[i].used = 0;
    }
    kprintf("icm: device disconnected\n");
    pcihp_request_rescan(100);
}

void icm_event(u32 pdf, const u32 *pkt, u32 n)
{
    u32 code;
    if (n < 1) return;
    if (pdf != TB_PKG_ICM_EVENT) {
        if (pdf == TB_PKG_EVENT && n >= 3) {
            /* routers still send plug events; the firmware handles them, we only acknowledge */
            tb_ack_plug(pkt[1], pkt[0] & 0x3FFFFF, (int)(pkt[2] & 0x3F), (pkt[2] & 0x80000000UL) != 0);
        } else if (pdf == TB_PKG_ERROR && n >= 3 && (pkt[2] & 0xF) >= TB_ERR_DP_BW) {
            tb_ack_notify(pkt[1], pkt[0] & 0x3FFFFF);
        } else {
            DBG("icm: packet type %lu ignored\n", pdf);
        }
        return;
    }
    code = pkt[0] & 0xFF;
    switch (code) {
    case ICM_EVENT_DEVICE_CONNECTED: device_connected(pkt, n); break;
    case ICM_EVENT_DEVICE_DISCONNECTED: device_disconnected(pkt, n); break;
    case ICM_EVENT_XDOMAIN_CONNECTED:
    case ICM_EVENT_XDOMAIN_DISCONNECTED:
        DBG("icm: host to host link event ignored\n");
        break;
    case ICM_EVENT_DP_CONFIG_CHANGED:
        break;
    default:
        DBG("icm: event %lx\n", code);
        break;
    }
}

int icm_start(void)
{
    u32 mode = nhi_fw_mode();
    icm_generation = nhi_icm_generation();
    if (!icm_generation) icm_generation = ICM_GEN_ICL;
    memset(devs, 0, sizeof(devs));
    icm_ready = 0;
    if (mode != NHI_FW_MODE_CM) {
        kprintf("icm: firmware in mode %lu (%s), not the connection manager\n", mode,
                mode == NHI_FW_MODE_SAFE ? "safe" : mode == NHI_FW_MODE_AUTH ? "authentication" : "endpoint");
        return -1;
    }
    if (driver_ready()) return -1;
    icm_ready = 1;
    /* devices connected at boot already have their PCIe functions enumerated by the BIOS */
    pcihp_request_rescan(300);
    return 0;
}

void icm_poll(void)
{
}

void icm_stop(void)
{
    icm_ready = 0;
}

int icm_api_tree(void *buf, u32 len, u32 *retlen, int *count)
{
    struct wintbnode *nd = (struct wintbnode *)buf;
    u32 max = len / sizeof(*nd), n = 0;
    int i;
    for (i = 0; i < ICM_MAX_DEVS && n < max; i++) {
        struct icm_dev *d = &devs[i];
        if (!d->used) continue;
        memset(&nd[n], 0, sizeof(*nd));
        nd[n].type = TN_ICMDEV;
        nd[n].depth = d->depth;
        nd[n].port = d->link;
        nd[n].route_lo = d->route_lo;
        nd[n].route_hi = d->route_hi;
        nd[n].flags = (u8)((d->approved ? TNF_APPROVED : 0) | (d->boot ? TNF_FIRMWARE : 0) | TNF_ACTIVE);
        nd[n].bw_up = d->uuid[0];
        nd[n].bw_down = d->uuid[1];
        ksnprintf(nd[n].name, sizeof(nd[n].name), "%08lx-%08lx", d->uuid[0], d->uuid[1]);
        n++;
    }
    *count = (int)n;
    *retlen = n * sizeof(*nd);
    return WINE_OK;
}
