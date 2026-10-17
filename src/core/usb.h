/* usb.h - WINCORE USB stack internal interfaces */
#ifndef USB_H
#define USB_H

#include "core.h"

#define USB_LS      0
#define USB_FS      1
#define USB_HS      2
#define USB_SS      3
#define USB_SSP     4

#define HC_UHCI     1
#define HC_OHCI     2
#define HC_EHCI     3
#define HC_XHCI     4

#define EP_CTRL     0
#define EP_ISO      1
#define EP_BULK     2
#define EP_INT      3

/* transfer status */
#define XS_PENDING  1
#define XS_OK       0
#define XS_STALL    (-2)
#define XS_ERROR    (-3)
#define XS_TIMEOUT  (-4)
#define XS_CANCEL   (-5)
#define XS_BABBLE   (-6)
#define XS_GONE     (-7)

/* normalized port status */
#define PS_CONNECT  0x0001
#define PS_ENABLE   0x0002
#define PS_RESET    0x0004
#define PS_POWER    0x0008
#define PS_OVERCUR  0x0010
#define PS_CCHANGE  0x0020      /* connect change */
#define PS_OTHERCHG 0x0040      /* any other change bit */
#define PS_OWNER    0x0080      /* EHCI: owned by companion */
#define PS_LINEK    0x0100      /* EHCI: K-state, low speed device */
#define PS_SPEEDSH  12
#define PS_SPEED(s) (((s) >> PS_SPEEDSH) & 7)

struct usb_hc;
struct usb_dev;
struct usb_pipe;
struct usb_xfer;
struct usb_hub;

typedef void (*xfer_cb)(struct usb_xfer *x);

struct usb_xfer {
    struct usb_pipe *pipe;
    u8 setup[8];
    u8 *buf;
    u32 len;
    u32 actual;
    volatile int status;
    u16 stream;
    u8 in;
    u8 queued;
    xfer_cb cb;
    void *priv;
    void *hcpriv;
    struct usb_xfer *next;
    u32 deadline;
};

struct usb_pipe {
    struct usb_dev *dev;
    u8 ep;              /* endpoint address with direction bit */
    u8 type;
    u16 maxpkt;
    u8 interval;
    u8 toggle;
    u8 maxburst;
    u8 mult;
    u8 maxstreams;      /* log2 of streams requested (UAS), 0 none */
    u8 open;
    u16 rsv;
    void *hcpriv;
};

/* isochronous stream: the host controller keeps packets scheduled ahead of the frame
   counter, asks the class driver for OUT data (fill) and hands back IN data (done) */
struct usb_iso {
    struct usb_pipe *pipe;
    u8 in;
    u8 running;
    u16 pktsize;                /* bytes per service interval (max packet * transactions) */
    u32 period_us;              /* time between packets */
    u32 ahead_us;               /* how far ahead packets are scheduled */
    u32 maxbytes;               /* packet buffer limit (0: 256K) */
    int npkt;                   /* packet slots */
    u8 *buf;                    /* npkt * pktsize, DMA memory */
    int (*fill)(struct usb_iso *s, u8 *dst, int max);   /* OUT: bytes for the next packet */
    void (*done)(struct usb_iso *s, u8 *src, int len);  /* IN: data of a received packet */
    void *priv;
    void *hcpriv;
    u32 packets, errors, missed;
    struct usb_iso *next;
};

struct usb_hubops {
    int (*nports)(struct usb_hub *h);
    u32 (*status)(struct usb_hub *h, int port);
    void (*clear)(struct usb_hub *h, int port, u32 status);
    int (*reset)(struct usb_hub *h, int port);      /* blocking, returns 0 when enabled */
    void (*disable)(struct usb_hub *h, int port);
    int (*handoff)(struct usb_hub *h, int port);    /* EHCI companion release */
};

#define MAXPORTS 32

/* port enumeration state */
#define PST_EMPTY     0
#define PST_DEBOUNCE  1
#define PST_ACTIVE    2
#define PST_FAILED    3
#define PST_COMPANION 4
#define PST_EJECTED   5

struct usb_port {
    u8 state;
    u8 tries;
    u16 rsv;
    u32 deadline;
    struct usb_dev *dev;
};

struct usb_hub {
    struct usb_hc *hc;
    struct usb_dev *dev;        /* NULL for root hub */
    struct usb_hubops *ops;
    int nports;
    int super;                  /* USB3 hub */
    int ttthink;
    int mtt;
    int pwrgood_ms;
    u32 next_scan;
    u8 changed;                 /* status change seen (external hubs) */
    u8 depth;
    u8 *intbuf;
    struct usb_xfer *intx;
    struct usb_port port[MAXPORTS + 1];
    struct usb_hub *nexthub;
    void *priv;
};

struct usb_hcops {
    const char *name;
    u8 type;
    int (*start)(struct usb_hc *hc);
    void (*stop)(struct usb_hc *hc);
    int (*dev_init)(struct usb_hc *hc, struct usb_dev *d);     /* addressing; 0 ok */
    int (*dev_update)(struct usb_hc *hc, struct usb_dev *d);   /* maxpkt0 / hub info changed */
    void (*dev_free)(struct usb_hc *hc, struct usb_dev *d);
    int (*pipe_open)(struct usb_hc *hc, struct usb_pipe *p);
    void (*pipe_close)(struct usb_hc *hc, struct usb_pipe *p);
    int (*submit)(struct usb_hc *hc, struct usb_xfer *x);
    void (*cancel)(struct usb_hc *hc, struct usb_xfer *x);
    void (*poll)(struct usb_hc *hc);
    int (*irq)(struct usb_hc *hc);
    int (*clear_halt)(struct usb_hc *hc, struct usb_pipe *p);
    int (*config_done)(struct usb_hc *hc, struct usb_dev *d);  /* xHCI configure endpoints */
    int (*iso_start)(struct usb_hc *hc, struct usb_iso *s);
    void (*iso_stop)(struct usb_hc *hc, struct usb_iso *s);
    void (*iso_service)(struct usb_hc *hc, struct usb_iso *s);  /* retire and queue packets */
};

struct usb_hc {
    struct usb_hcops *ops;
    struct pcidev pci;
    void *priv;
    u8 *mmio;
    u16 iobase;
    u8 irq;
    u8 index;
    u8 addrmap[16];             /* 128 addresses */
    struct usb_hub root;
    struct usb_hc *companion_of;  /* UHCI/OHCI: the EHCI owning the same ports */
    u32 errors;
    u32 irqs;
    volatile u32 pending;
    u8 nostreams;
    u8 intx_later;              /* DOS: INTx switched on at the first poll */               /* xHCI with broken stream support: UAS devices use BOT */
    char name[24];
};

/* class drivers */
struct usb_class {
    const char *name;
    int (*probe)(struct usb_dev *d, u8 *iface, u8 *cfg_end);   /* 0 = bound */
    void (*detach)(struct usb_dev *d);
    void (*poll)(void);
};

struct usb_dev {
    struct usb_hc *hc;
    struct usb_hub *hub;        /* hub this device is plugged into */
    struct usb_dev *parent;     /* parent hub device, NULL on root port */
    u8 port;
    u8 speed;
    u8 addr;
    u8 depth;
    u8 slot;
    u8 rootport;
    u8 config;
    u8 gone;
    u32 route;
    struct usb_dev *tthub;      /* high-speed hub with the transaction translator */
    u8 ttport;
    u8 driver;
    u8 drvinfo;                 /* detail of the driver: UAC version, ... (winusbnode.rsv) */
    u16 id;
    struct usb_pipe ep0;
    u8 ddesc[18];
    u8 *cdesc;
    u16 cdesc_len;
    u16 maxpower;
    u8 bos[64];
    u16 bos_len;
    char manuf[32];
    char product[40];
    char serial[24];
    struct usb_class *cls[4];
    void *clspriv[4];
    int ncls;
    struct usb_hub *ownhub;     /* when this device is a hub */
    u32 errors, xfers;
    struct usb_dev *next;
    void *hcpriv;
    u8 ejected;
    u8 uas;
};

/* usb.c */
extern struct usb_hc *hcs[];
extern int nhc;
extern struct usb_dev *usb_devices;
struct usb_hc *usb_add_hc(struct usb_hcops *ops, struct pcidev *p, void *priv);
int usb_control(struct usb_dev *d, u8 rt, u8 req, u16 val, u16 idx, void *buf, u16 len);
int usb_xfer_sync(struct usb_pipe *p, void *buf, u32 len, int in, u32 timeout_ms, u32 *actual);
int usb_xfer_sync_stream(struct usb_pipe *p, void *buf, u32 len, int in, u32 timeout_ms, u32 *actual, u16 stream);
int usb_submit(struct usb_xfer *x);
void usb_cancel(struct usb_xfer *x);
int usb_wait(struct usb_xfer *x, u32 timeout_ms);
int usb_pipe_open(struct usb_dev *d, struct usb_pipe *p, u8 *epdesc);
void usb_pipe_close(struct usb_pipe *p);
int usb_clear_halt(struct usb_pipe *p);
struct usb_hub *usb_new_hub(struct usb_hc *hc, struct usb_dev *dev, struct usb_hubops *ops, int nports);
void usb_free_hub(struct usb_hub *h);
void usb_port_gone(struct usb_hub *h, int port);
u8 *usb_find_desc(u8 *p, u8 *end, u8 type);
void usb_hc_kick(struct usb_hc *hc);
void usb_hc_poll_all(void);
int usb_addr_alloc(struct usb_hc *hc);
void usb_addr_free(struct usb_hc *hc, int a);
u32 speed_maxpkt0(int speed);
int usb_iso_start(struct usb_iso *s);       /* pipe open; fills in npkt and buf */
void usb_iso_stop(struct usb_iso *s);
void usb_iso_late(struct usb_iso *s);
int usb_set_interface(struct usb_dev *d, int ifnum, int alt);

/* host controller drivers */
int uhci_attach(struct pcidev *p);
int ohci_attach(struct pcidev *p);
int ehci_attach(struct pcidev *p);
int xhci_attach(struct pcidev *p);
void uhci_early(struct pcidev *p);
void ohci_early(struct pcidev *p);
void ehci_early(struct pcidev *p);
void xhci_early(struct pcidev *p);
void *usb_map(u32 phys, u32 len);
void usb_irq_setup(struct usb_hc *hc);

/* class drivers */
extern struct usb_class hub_class;
extern struct usb_class hid_class;
extern struct usb_class msc_class;
extern struct usb_class uaudio_class;
extern struct usb_class uvc_class;
int uvc_api(struct winapireq *r, u8 *buf);
void uaudio_tick(void);
void hid_tick(void);
struct hiddev;
struct hiddev *hid_mouse_new(const u8 *rd, int n);
void hid_mouse_input(struct hiddev *h, const u8 *r, u32 len);
void hid_mouse_free(struct hiddev *h);
/* ptp.c: precision touchpads */
struct ptp;
int ptp_is_touchpad(const u8 *rd, int n);
struct ptp *ptp_new(const u8 *rd, int n);
void ptp_free(struct ptp *p);
int ptp_feature(struct ptp *p, int which, u8 *buf, int max);
void ptp_input(struct ptp *p, const u8 *r, u32 len);
void ptp_tick(void);
struct ptp_contact {
    u8 id;
    s32 x, y;                   /* y grows downwards */
};
struct ptp *ptp_new_abs(s32 xmin, s32 xmax, s32 ymin, s32 ymax, u32 xum, u32 yum,
                        void (*out)(int dx, int dy, int dz, int dw, int buttons));
void ptp_contacts(struct ptp *p, const struct ptp_contact *c, int n, int buttons);
/* ps2tp.c: Synaptics / ELAN touchpads on the PS/2 aux port */
struct ps2io {
    int (*cmd)(u8 c);           /* a byte to the aux device: 0 when acknowledged */
    int (*read)(u32 ms);        /* the next aux byte, -1 after ms */
};
int ps2tp_probe(const struct ps2io *io, void (*out)(int dx, int dy, int dz, int dw, int buttons));
int ps2tp_reprogram(const struct ps2io *io);
int ps2tp_active(void);
int ps2tp_kind(u8 *smbaddr);
const char *ps2tp_name(void);
void ps2tp_off(void);
void ps2tp_byte(u8 b);
u32 ps2tp_stats(u32 *bad);
/* kbc.c: the 8042 from the aux side (commands polled, aux interrupt off) */
extern void (*kbc_stray)(u8 b);
extern const struct ps2io kbc_io;
int kbc_wait_in(void);
int kbc_cmd(u8 c);
int kbc_write(u8 c, u8 d);
int kbc_read(int aux, u32 ms);
void kbc_flush(void);
void kbc_kbd_restore(void);
void kbc_kbd_forget(void);
int kbc_begin(u8 *ccb);
void kbc_end(u8 ccb, int aux_int);
int aux_cmd(u8 c);
int aux_read(u32 ms);
/* smbtp.c: touchpads on SMBus (Synaptics RMI4, ELAN) */
int smbtp_start(int kind, u8 addr, void (*out)(int dx, int dy, int dz, int dw, int buttons));
void smbtp_stop(void);
void smbtp_poll(void);
int smbtp_active(void);
void msc_tick(void);
int msc_eject(struct usb_dev *d);

#endif
