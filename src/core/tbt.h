/* tbt.h - Thunderbolt / USB4 host support: NHI, control channel, routers, tunnels */
#ifndef TBT_H
#define TBT_H
#include "core.h"

/* ---- NHI (Native Host Interface) registers, BAR0 ---- */
#define NHI_TX_RING_BASE    0x00000     /* 16 bytes per ring */
#define NHI_RX_RING_BASE    0x08000
#define NHI_TX_OPT_BASE     0x19800     /* 32 bytes per ring */
#define NHI_RX_OPT_BASE     0x29800
#define NHI_RING_NOTIFY     0x38100     /* interrupt status, one bit per ring (tx), then rx */
#define NHI_RING_INT        0x38200     /* interrupt enable */
#define NHI_INT_THROTTLE    0x38c00
#define NHI_HOP_COUNT       0x39640     /* bits 0-9 ring count, bits 16-23 host interface version */
#define NHI_CAPS_VERSION(v) (((v) >> 16) & 0xFF)
#define NHI_RESET           0x39858
#define NHI_RESET_HRR       0x00000001UL    /* host router reset */
#define NHI_DMA_MISC        0x39864
#define NHI_DMA_MISC_INT_AUTO_CLEAR 0x00000004UL
#define NHI_INMAIL_DATA     0x39900
#define NHI_INMAIL_CMD      0x39904
#define NHI_INMAIL_CMD_MASK 0xFF
#define NHI_INMAIL_ERROR    0x40000000UL
#define NHI_INMAIL_OP_REQ   0x80000000UL
#define NHI_OUTMAIL_CMD     0x3993C
#define NHI_OUTMAIL_OPMODE(v)   (((v) >> 8) & 0xF)
#define NHI_OUTMAIL_STATUS(v)   (((v) >> 24) & 0xF)
#define NHI_FW_STS          0x39944
#define NHI_FW_STS_ICM_EN       0x00000001UL
#define NHI_FW_STS_ICM_EN_INV   0x00000002UL
#define NHI_FW_STS_ICM_EN_CPU   0x00000004UL
#define NHI_FW_STS_CIO_RESET    0x40000000UL
#define NHI_FW_STS_NVM_AUTH     0x80000000UL

#define NHI_MB_SAVE_DEVS        0x05        /* in-mailbox commands */
#define NHI_MB_DISCONNECT_PCIE  0x06
#define NHI_MB_DRV_UNLOADS      0x07
#define NHI_MB_DISCONNECT_PA    0x10
#define NHI_MB_DISCONNECT_PB    0x11
#define NHI_MB_ALLOW_ALL_DEVS   0x23
#define NHI_FW_MODE_SAFE        0
#define NHI_FW_MODE_AUTH        1
#define NHI_FW_MODE_EP          2
#define NHI_FW_MODE_CM          3

/* ring option flags */
#define RING_FLAG_E2E       0x10000000UL
#define RING_FLAG_RAW       0x40000000UL
#define RING_FLAG_ENABLE    0x80000000UL
/* descriptor flags (dword 2 bits 20-31) */
#define RDESC_CRC_ERROR     0x001       /* rx */
#define RDESC_COMPLETED     0x002
#define RDESC_POSTED        0x004
#define RDESC_OVERRUN       0x004       /* rx */
#define RDESC_INTERRUPT     0x008

#define TB_FRAME_SIZE       256
#define TB_RING_SIZE        16          /* ring 0 entries, tx and rx */

/* control packet types (PDF, carried in the descriptor eof field of raw rings) */
#define TB_PKG_READ         1
#define TB_PKG_WRITE        2
#define TB_PKG_ERROR        3
#define TB_PKG_NOTIFY_ACK   4
#define TB_PKG_EVENT        5
#define TB_PKG_XD_REQ       6
#define TB_PKG_XD_RESP      7
#define TB_PKG_OVERRIDE     8
#define TB_PKG_RESET        9
#define TB_PKG_ICM_EVENT    10
#define TB_PKG_ICM_CMD      11
#define TB_PKG_ICM_RESP     12
#define TB_PKG_PREP_SLEEP   13

/* config spaces */
#define TB_CFG_HOPS         0
#define TB_CFG_PORT         1
#define TB_CFG_SWITCH       2
#define TB_CFG_COUNTERS     3

/* error codes in TB_PKG_ERROR */
#define TB_ERR_PORT_NOT_CONNECTED   0
#define TB_ERR_LINK                 1
#define TB_ERR_INVALID_SPACE        2
#define TB_ERR_NO_SUCH_PORT         4
#define TB_ERR_ACK_PLUG_EVENT       7
#define TB_ERR_LOOP                 8
#define TB_ERR_HEC                  12
#define TB_ERR_FLOW_CONTROL         13
#define TB_ERR_LOCK                 15
#define TB_ERR_DP_BW                32
#define TB_ERR_ROP_CMPLT            33
#define TB_ERR_POP_CMPLT            34
#define TB_ERR_PCIE_WAKE            35
#define TB_ERR_DP_CON_CHANGE        36
#define TB_ERR_DPTX_DISCOVERY       37
#define TB_ERR_LINK_RECOVERY        38
#define TB_ERR_ASYM_LINK            39
#define TB_ERR_PG_HOT_PLUG          2
#define TB_ERR_PG_HOT_UNPLUG        3

/* ---- router (switch) config space, dwords ---- */
#define ROUTER_CS_0         0       /* vendor:16 device:16 */
#define ROUTER_CS_1         1       /* first cap:8 upstream port:6 max port:6 depth:3 x:1 rev:8 */
#define ROUTER_CS_2         2       /* route lo */
#define ROUTER_CS_3         3       /* route hi:31 enabled:1 */
#define ROUTER_CS_4         4       /* plug events delay:8 cmuv:8 x:8 version:8 */
#define ROUTER_CS_5         5
#define ROUTER_CS_5_SLP     0x00000001UL
#define ROUTER_CS_5_C3S     0x00800000UL
#define ROUTER_CS_5_PTO     0x01000000UL    /* PCIe tunneling on */
#define ROUTER_CS_5_UTO     0x02000000UL    /* USB3 tunneling on */
#define ROUTER_CS_5_HCO     0x04000000UL    /* internal xHCI on */
#define ROUTER_CS_5_CV      0x80000000UL    /* configuration valid */
#define ROUTER_CS_6         6
#define ROUTER_CS_6_SLPR    0x00000001UL
#define ROUTER_CS_6_TNS     0x00000002UL    /* TBT3 not supported */
#define ROUTER_CS_6_HCI     0x00040000UL    /* internal xHCI present */
#define ROUTER_CS_6_CR      0x01000000UL    /* configuration ready */
#define ROUTER_CS_26        0x1A
#define ROUTER_CS_26_OPCODE_MASK 0xFFFF
#define ROUTER_CS_26_STATUS(v)  (((v) >> 24) & 0x3F)
#define ROUTER_CS_26_ONS    0x40000000UL
#define ROUTER_CS_26_OV     0x80000000UL
#define USB4_VERSION_1_0    0x20
#define TB_VERSION_TBT3     0x10

/* router capabilities */
#define TB_SW_CAP_TMU       1
#define TB_SW_CAP_VSE       5
#define TB_VSE_PLUG_EVENTS  1       /* Intel vendor specific capability ids */
#define TB_VSE_TIME2        3
#define TB_VSE_CP_LP        4
#define TB_VSE_LC           6       /* link controller */
#define TB_LC_DESC          0x02
#define TB_LC_SX_CTRL       0x96
#define TB_LC_SX_CTRL_L1C   0x00010000UL
#define TB_LC_SX_CTRL_L2C   0x00100000UL
#define TB_LC_SX_CTRL_UPSTREAM 0x40000000UL

/* ---- adapter (port) config space ---- */
#define ADP_CS_0            0       /* vendor:16 device:16 */
#define ADP_CS_1            1       /* first cap:8 max counters:11 counters:1 x:4 rev:8 */
#define ADP_CS_2            2       /* type:24 version:8 */
#define ADP_CS_3            3       /* x:20 port number:6 x:6 */
#define ADP_CS_4            4       /* nfc credits:20 total buffers:10 x:2 */
#define ADP_CS_4_NFC_MASK   0x000FFFFFUL
#define ADP_CS_4_LCK        0x80000000UL    /* USB4: adapter locked (boot default) */
#define ADP_CS_5_DHP        0x80000000UL    /* USB4: hot plug events disabled */
#define ADP_CS_4_TOTAL(v)   (((v) >> 20) & 0x3FF)
#define ADP_CS_5            5       /* max in hop:11 max out hop:11 */
#define ADP_HEADER_DWORDS   8

#define TB_TYPE_INACTIVE    0x000000UL
#define TB_TYPE_PORT        0x000001UL  /* lane adapter */
#define TB_TYPE_NHI         0x000002UL
#define TB_TYPE_DP_IN       0x0E0101UL
#define TB_TYPE_DP_OUT      0x0E0102UL
#define TB_TYPE_PCIE_DOWN   0x100101UL
#define TB_TYPE_PCIE_UP     0x100102UL
#define TB_TYPE_USB3_DOWN   0x200101UL
#define TB_TYPE_USB3_UP     0x200102UL

/* adapter capabilities */
#define TB_ADP_CAP_PHY      1       /* lane adapter */
#define TB_ADP_CAP_POWER    2
#define TB_ADP_CAP_TIME1    3
#define TB_ADP_CAP_ADAP     4       /* protocol adapter (PCIe, USB3, DP) */
#define TB_ADP_CAP_VSE      5
#define TB_ADP_CAP_USB4     6

/* lane adapter (cap PHY) */
#define LANE_ADP_CS_0       0
#define LANE_ADP_CS_0_SUPPORTED_WIDTH(v) (((v) >> 20) & 0x3F)
#define LANE_ADP_CS_1       1
#define LANE_ADP_CS_1_LD    0x00004000UL    /* lane disable */
#define LANE_ADP_CS_1_LB    0x00008000UL    /* lane bonding */
#define LANE_ADP_CS_1_SPEED(v)  (((v) >> 16) & 0xF)
#define LANE_ADP_CS_1_WIDTH(v)  (((v) >> 20) & 0x3F)
#define LANE_ADP_CS_1_STATE(v)  (((v) >> 26) & 0xF)
#define LANE_STATE_DISABLED 0
#define LANE_STATE_TRAINING 1
#define LANE_STATE_CL0      2
#define LANE_STATE_UNPLUGGED 7

/* USB4 port (cap USB4) */
#define PORT_CS_18          0x12
#define PORT_CS_18_BE       0x00000001UL    /* bonding enabled */
#define PORT_CS_18_TCM      0x00000002UL
#define PORT_CS_18_CPS      0x00000004UL
#define PORT_CS_19          0x13
#define PORT_CS_19_PC       0x00000008UL    /* port configured */
#define PORT_CS_19_PID      0x00000010UL

/* PCIe adapter (cap ADAP) */
#define ADP_PCIE_CS_0       0
#define ADP_PCIE_CS_0_PE    0x80000000UL    /* path enable */
#define ADP_PCIE_CS_0_LTSSM(v) (((v) >> 24) & 0xF)
/* USB3 adapter (cap ADAP) */
#define ADP_USB3_CS_0       0
#define ADP_USB3_CS_0_V     0x40000000UL    /* valid */
#define ADP_USB3_CS_0_PE    0x80000000UL
#define ADP_USB3_CS_1       1
#define ADP_USB3_CS_1_CUBW_MASK 0x00000FFFUL
#define ADP_USB3_CS_1_CDBW_MASK 0x00FFF000UL
#define ADP_USB3_CS_1_HCA   0x80000000UL    /* CM request acknowledged */
#define ADP_USB3_CS_2       2
#define ADP_USB3_CS_2_AUBW_MASK 0x00000FFFUL
#define ADP_USB3_CS_2_ADBW_SHIFT 12
#define ADP_USB3_CS_2_ADBW_MASK 0x00FFF000UL
#define ADP_USB3_CS_2_CMR   0x80000000UL    /* CM request */
#define ADP_USB3_CS_3       3
#define ADP_USB3_CS_3_SCALE_MASK 0x3F
#define ADP_USB3_CS_4       4
#define ADP_USB3_CS_4_ALR(v)  ((v) & 0x7F)          /* actual link rate */
#define ADP_USB3_CS_4_MSLR(v) (((v) >> 12) & 0x7F)  /* max supported link rate */
#define ADP_USB3_CS_4_ULV   0x00000080UL

/* hop (path) entry: 2 dwords per in-hop id in the HOPS space of the in port */
#define HOP_DW0(next_hop, out_port, credits, enable) \
    ((u32)((next_hop) & 0x7FF) | ((u32)((out_port) & 0x3F) << 11) | ((u32)((credits) & 0xFF) << 17) | \
     ((enable) ? 0x80000000UL : 0))
#define HOP_DW0_NEXT(v)     ((v) & 0x7FF)
#define HOP_DW0_OUT(v)      (((v) >> 11) & 0x3F)
#define HOP_DW0_CREDITS(v)  (((v) >> 17) & 0xFF)
#define HOP_DW0_ENABLE      0x80000000UL
#define HOP_DW1(weight, prio, ifc, efc) \
    ((u32)((weight) & 0xF) | ((u32)((prio) & 7) << 8) | ((ifc) ? 0x01000000UL : 0) | ((efc) ? 0x02000000UL : 0))
#define HOP_DW1_PENDING     0x10000000UL
#define TB_PATH_MIN_HOPID   8
#define TB_PCI_HOPID        8       /* fixed hop ids of protocol adapters */
#define TB_USB3_HOPID       8
#define TB_PCI_PRIORITY     3
#define TB_PCI_WEIGHT       1
#define TB_USB3_PRIORITY    3
#define TB_USB3_WEIGHT      2
#define TB_MIN_PCIE_CREDITS 6
#define TB_MAX_PCIE_CREDITS 32
#define TB_MAX_USB3_CREDITS 32

/* ---- ICM (firmware connection manager) messages ---- */
#define ICM_GET_TOPOLOGY    0x01
#define ICM_DRIVER_READY    0x03
#define ICM_APPROVE_DEVICE  0x04
#define ICM_CHALLENGE_DEVICE 0x05
#define ICM_ADD_DEVICE_KEY  0x06
#define ICM_GET_ROUTE       0x0A
#define ICM_APPROVE_XDOMAIN 0x10
#define ICM_DISCONNECT_XDOMAIN 0x11
#define ICM_PREBOOT_ACL     0x18
#define ICM_USB4_SWITCH_OP  0x20
#define ICM_EVENT_DEVICE_CONNECTED      0x03
#define ICM_EVENT_DEVICE_DISCONNECTED   0x04
#define ICM_EVENT_XDOMAIN_CONNECTED     0x06
#define ICM_EVENT_XDOMAIN_DISCONNECTED  0x07
#define ICM_EVENT_DP_CONFIG_CHANGED     0x08
#define ICM_FLAGS_ERROR     0x01
#define ICM_FLAGS_NO_KEY    0x02
#define ICM_FLAGS_SLEVEL(f) (((f) >> 3) & 3)
#define ICM_FLAGS_DUAL_LANE 0x20
#define ICM_FLAGS_SPEED_GEN3 0x80
#define ICM_LINK_INFO_LINK(v)   ((v) & 0x7)
#define ICM_LINK_INFO_DEPTH(v)  (((v) >> 4) & 0xF)
#define ICM_LINK_INFO_APPROVED  0x0100
#define ICM_LINK_INFO_REJECTED  0x0200
#define ICM_LINK_INFO_BOOT      0x0400
#define TB_SECURITY_NONE    0
#define TB_SECURITY_USER    1
#define TB_SECURITY_SECURE  2
#define TB_SECURITY_DPONLY  3
#define TB_SECURITY_USBONLY 4
#define TB_SECURITY_NOPCIE  5

/* ---- structures ---- */
#define TB_MAX_PORTS        64
#define TB_MAX_SWITCHES     8
#define TB_MAX_TUNNELS      16
#define TB_MAX_HOPS         8       /* per path (depth) */
#define TB_MAX_DEPTH        6

struct tb_port {
    u32 type;               /* TB_TYPE_* */
    u8 num;
    u8 valid;               /* config space readable */
    u8 cap_phy, cap_adap, cap_usb4;
    u8 link_nr;             /* lane 0 / lane 1 of a dual-lane link */
    u8 dual;                /* partner lane adapter number (0 none) */
    u8 remote_sw;           /* index into switches + 1 of the router on the other end, 0 none */
    u8 remote_port;
    u8 in_hop_next, out_hop_next;   /* simple hop id allocation */
    u16 total_credits;      /* buffers of this adapter */
    u16 used_credits;
    u16 max_in_hop, max_out_hop;
    u32 cs0;                /* lane adapter: LANE_ADP_CS_0 */
};

struct tb_switch {
    u8 used;
    u8 depth;
    u8 upstream;            /* upstream lane adapter number */
    u8 nports;              /* max port number */
    u8 usb4;                /* USB4 router (else TBT 1-3) */
    u8 parent;              /* index of parent router + 1 (0 for the host) */
    u8 parent_port;         /* lane adapter of the parent this router hangs on */
    u8 generation;
    u16 cap_plug, cap_lc;   /* Intel vendor caps (switch config space) */
    u8 configured;
    u16 vendor, device;
    u32 route_lo, route_hi;
    u32 cs4;
    u32 max_pcie_credits, max_usb3_credits;
    struct tb_port ports[TB_MAX_PORTS];
    char name[20];
};

#define TB_TUNNEL_PCIE      1
#define TB_TUNNEL_USB3      2
struct tb_hop {
    u8 sw;                  /* router index */
    u8 in_port, in_hop;
    u8 out_port, next_hop;
    u8 credits;
};
struct tb_tunnel {
    u8 used;
    u8 type;
    u8 sw;                  /* device router the tunnel ends in */
    u8 src_sw;              /* router the tunnel starts in (the device's parent) */
    u8 discovered;          /* set up by the firmware / BIOS, not by us */
    u8 src_port, dst_port;  /* parent adapter, device adapter */
    u8 nhops;
    struct tb_hop down[TB_MAX_HOPS];    /* host -> device */
    struct tb_hop up[TB_MAX_HOPS];      /* device -> host */
    u32 bw_up, bw_down;     /* USB3 allocated bandwidth, Mb/s */
};

/* nhi.c */
struct nhi;
int nhi_probe(struct pcidev *p);            /* 0 = usable */
void nhi_stop(void);
int nhi_present(void);
int nhi_icm_running(void);
u32 nhi_fw_mode(void);
u32 nhi_hif_version(void);
u32 nhi_fw_sts(void);
u16 nhi_device_id(void);
struct pcidev *nhi_pcidev(void);
int nhi_mailbox(u32 cmd, u32 data);
int tb_cfg_read(u32 route_lo, u32 route_hi, int port, int space, u32 offset, u32 *buf, u32 len);
int tb_cfg_write(u32 route_lo, u32 route_hi, int port, int space, u32 offset, const u32 *buf, u32 len);
int tb_cfg_reset(u32 route_lo, u32 route_hi);
int tb_ctl_tx(const u32 *data, u32 dwords, u32 pdf);
int tb_ctl_poll(u32 *buf, u32 *dwords, u32 *pdf);  /* 1 = packet in buf (max 64 dwords) */
typedef int (*tb_match_fn)(const u32 *resp, u32 dwords, u32 pdf, void *ctx);
int tb_ctl_request(const u32 *req, u32 reqdw, u32 pdf, u32 *resp, tb_match_fn match, void *ctx, u32 timeout);
void nhi_dispatch(void (*handler)(u32 pdf, const u32 *pkt, u32 dwords));
void nhi_restart(void);
int nhi_icm_generation(void);
void tb_ack_plug(u32 route_lo, u32 route_hi, int port, int unplug);
void tb_ack_notify(u32 route_lo, u32 route_hi);
extern u32 tb_last_error;                   /* TB_ERR_* of the last failed config access */
extern u32 tb_cfg_timeout;                  /* ms */

/* tbt.c: software connection manager */
void tbt_start(struct pcidev *list, int n);
void tbt_poll(void);
void tbt_shutdown(void);
void tbt_event(u32 pdf, const u32 *pkt, u32 dwords);
int tbt_api_tree(void *buf, u32 len, u32 *retlen);
extern struct tb_switch tb_switches[TB_MAX_SWITCHES];
extern struct tb_tunnel tb_tunnels[TB_MAX_TUNNELS];
extern u32 tb_mode;                         /* TB_MODE_* */
#define TB_MODE_NONE    0
#define TB_MODE_SWCM    1
#define TB_MODE_ICM     2

/* icm.c: firmware connection manager */
int icm_start(void);
void icm_poll(void);
void icm_stop(void);
void icm_event(u32 pdf, const u32 *pkt, u32 dwords);
int icm_api_tree(void *buf, u32 len, u32 *retlen, int *n);
extern u32 icm_security_level;
extern int icm_generation;
#define ICM_GEN_FR  1   /* Light/Cactus/Falcon/Win/Alpine Ridge */
#define ICM_GEN_TR  2   /* Titan Ridge, Maple Ridge */
#define ICM_GEN_ICL 3   /* Ice Lake and later integrated */

/* pcihp.c: PCI hot-plug resources for tunneled devices */
void pcihp_init(struct pcidev *list, int n);
int pcihp_rescan(void);                     /* new devices found and started */
void pcihp_request_rescan(u32 delay_ms);
void pcihp_boot(void);
void pcihp_check_removed(void);
int pcihp_behind_hotplug(int bus);          /* a hot-plug capable downstream port is above this bus */
int pcihp_is_tbt_device(struct pcidev *p);
void pcihp_poll(void);
int pcihp_count(void);

#endif
