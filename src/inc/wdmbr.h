/* wdmbr.h - the USB WDM bridge of WINUSB for Windows 98 / ME: the interface between
   WINUSB.VXD (core/usbwdm.c, services in the VxD's service table) and its WDM bus driver
   WINUSBW.SYS (src/wdm). The USB devices (or interfaces) no WINDRV class driver serves are
   given to WINUSBW.SYS, which reports them to Windows as USB\VID_xxxx&PID_xxxx devices: their
   own Windows drivers (the device's vendor driver, Windows' HID, printer, scanner drivers)
   load on top and send their USB requests (URBs) down, carried out by WINUSB's host
   controller drivers. Shared by both sides; plain C types, 4-byte packing. */
#ifndef WDMBR_H
#define WDMBR_H

#pragma pack(push, 4)

#define WDMBR_VERSION   0x0100

/* a bridged function: a whole device, or one interface of a composite device */
#define WBF_WHOLE       0xFF            /* iface value of a whole device */
struct wbfunc {
    unsigned long  handle;              /* slot | generation << 8 */
    unsigned short vid, pid, bcd, bcdusb;
    unsigned char  cls, sub, proto;     /* device class, or the interface's */
    unsigned char  iface;               /* interface number, WBF_WHOLE for the device */
    unsigned char  speed;               /* 0 low 1 full 2 high 3 super 4 super+ */
    unsigned char  port;
    unsigned char  ncfg;                /* configurations */
    unsigned char  rsv;
    char           inst[28];            /* instance: controller and port path */
    char           manuf[32];
    char           product[40];
    char           serial[24];
};

/* request operations */
#define WBR_XFER        1               /* transfer (control on pipe 0) */
#define WBR_SELCFG      2               /* SET_CONFIGURATION and pipes of the interfaces (buf: wbsel) */
#define WBR_SELIF       3               /* SET_INTERFACE and pipes of one interface (buf: wbsel, 1 interface) */
#define WBR_RESETPIPE   4               /* CLEAR_FEATURE(ENDPOINT_HALT), toggle reset */
#define WBR_ABORTPIPE   5               /* cancel every transfer of the pipe */
#define WBR_UNCONFIG    6               /* pipes closed (SET_CONFIGURATION 0 for a whole device) */

/* flags */
#define WBRF_IN         0x0001
#define WBRF_SHORTOK    0x0002

/* status */
#define WBS_OK          0
#define WBS_PENDING     1
#define WBS_STALL       (-2)
#define WBS_ERROR       (-3)
#define WBS_TIMEOUT     (-4)
#define WBS_CANCEL      (-5)
#define WBS_BABBLE      (-6)
#define WBS_GONE        (-7)
#define WBS_SHORT       (-8)            /* less data than asked and WBRF_SHORTOK not set */
#define WBS_BADPARAM    (-9)
#define WBS_NOMEM       (-10)

struct wbreq {
    unsigned long  magic;               /* WBR_MAGIC */
    unsigned long  op;                  /* WBR_* */
    unsigned long  func;                /* wbfunc.handle */
    unsigned long  pipe;                /* pipe handle, 0: the default control pipe */
    unsigned char  setup[8];            /* control transfers */
    unsigned char *buf;                 /* data: a locked system address */
    unsigned long  len;
    unsigned long  actual;
    unsigned long  flags;               /* WBRF_* */
    volatile long  status;              /* WBS_* */
    void          *ctx;                 /* the bus driver's (its IRP) */
    volatile long  cancel;              /* set by wb_cancel */
    struct wbreq  *next;                /* queue link (VxD) */
    void          *vx[4];               /* VxD private */
};
#define WBR_MAGIC       0x52514257UL    /* 'WBQR' */

/* WBR_SELCFG / WBR_SELIF: wbreq.buf points to a wbsel */
#define WB_MAXIF        16
#define WB_MAXEP        16
struct wbpipe {
    unsigned long  handle;              /* out */
    unsigned short maxpkt;              /* out: bytes per packet (with the transactions per frame) */
    unsigned char  ep;                  /* out: endpoint address */
    unsigned char  type;                /* out: 0 control 1 iso 2 bulk 3 interrupt */
    unsigned char  interval;            /* out */
    unsigned char  rsv[3];
};
struct wbif {
    unsigned char  ifnum, alt;          /* in */
    unsigned char  cls, sub, proto;     /* out */
    unsigned char  npipes;              /* out */
    unsigned char  rsv[2];
    struct wbpipe  pipes[WB_MAXEP];
};
struct wbsel {
    unsigned char  cfgval;              /* in: bConfigurationValue (WBR_SELCFG) */
    unsigned char  nif;                 /* in */
    unsigned char  rsv[2];
    struct wbif    ifc[WB_MAXIF];
};

/* the bus driver's callbacks, registered with wb_attach; called at event time */
struct wbops {
    unsigned long  size;
    void (__stdcall *done)(void *ctx, struct wbreq *r);    /* a request finished */
    void (__stdcall *changed)(void *ctx);                  /* functions came or went */
    void          *ctx;
};

/* services of WINUSB.VXD for the bus driver (cdecl, the VxD's service table; vxdent.asm):
   6  unsigned long wb_attach(struct wbops *ops)      -> WDMBR_VERSION, 0: no bridge (NULL detaches)
   7  int wb_list(struct wbfunc *out, int max)        -> number of functions
   8  int wb_desc(unsigned long func, int type, int index, void *buf, int len)
                                                      -> bytes of a cached descriptor: device (1),
                                                         configuration (2, index 0), -1 none
   9  void wb_queue(struct wbreq *r)                  -> runs at the next poll; done() follows
   10 void wb_cancel(struct wbreq *r)                 -> done() with WBS_CANCEL follows
   11 unsigned long wb_frame(unsigned long func)      -> current frame number (1 ms)
   12 void wb_log(const char *line)                   -> into WINUSB's log (/D) */
#define WB_SVC_ATTACH   6
#define WB_SVC_LIST     7
#define WB_SVC_DESC     8
#define WB_SVC_QUEUE    9
#define WB_SVC_CANCEL   10
#define WB_SVC_FRAME    11
#define WB_SVC_LOG      12

#pragma pack(pop)

#endif
