/* winapi.h - structures shared by WINDISK.SYS / WINUSB.SYS, the driver cores (DOS/VxD) and Windows tools.
   Must compile with both 16-bit and 32-bit Open Watcom compilers. */
#ifndef WINAPI_H
#define WINAPI_H

#pragma pack(push, 1)

#define WIN_MAGIC        0x42433357UL    /* 'WINCB' */
#define WIN_VERSION      0x0100
#define WIN_MAXUNITS     24
#define WIN_KQLEN        20
#define WIN_MPLEX_DISK   0xD3            /* INT 2Fh multiplex id of WINDISK.SYS */
#define WIN_MPLEX_USB    0xD4            /* INT 2Fh multiplex id of WINUSB.SYS */
#define WIN_VXD_DISK     0x3D33          /* VxD device id of WINDISK.386 */
#define WIN_VXD_USB      0x3D34          /* VxD device id of WINUSB.386 */
#define WIN_MPLEX_AUDIO  0xD5            /* INT 2Fh multiplex id of WINAUDIO.SYS */
#define WIN_VXD_AUDIO    0x3D35          /* VxD device id of WINAUDIO.386 */
#if !defined(WIN_USB) && !defined(WIN_AUDIO) && !defined(WIN_DISK)
#define WIN_DISK
#endif
#if defined(WIN_USB)
#define WIN_MPLEX        WIN_MPLEX_USB
#define WIN_VXD_ID       WIN_VXD_USB
#elif defined(WIN_AUDIO)
#define WIN_MPLEX        WIN_MPLEX_AUDIO
#define WIN_VXD_ID       WIN_VXD_AUDIO
#else
#define WIN_MPLEX        WIN_MPLEX_DISK
#define WIN_VXD_ID       WIN_VXD_DISK
#endif

/* unit types / state */
#define UT_NONE     0
#define UT_FIXED    1
#define UT_SLOT     2
#define US_PRESENT  0x01
#define US_CHANGED  0x02
#define US_WPROT    0x04
#define US_EJECTED  0x08

struct winunit {                 /* 64 bytes */
    unsigned char  type;
    unsigned char  state;
    unsigned char  disk;        /* core disk index, 0xFF none */
    unsigned char  part;        /* partition number on disk */
    unsigned long  key;         /* disk identity */
    unsigned long  key2;
    unsigned long  lba;         /* partition start, 512-byte sectors (low 32 bits) */
    unsigned long  sectors;     /* partition size, 512-byte sectors */
    unsigned long  serial;      /* volume serial */
    unsigned char  media;       /* media descriptor */
    unsigned char  ptype;       /* partition type byte */
    unsigned char  gen;         /* bumps on every media change */
    unsigned char  lba_hi;      /* partition start, bits 32-39 (disks above 2 TB) */
    unsigned char  bpb[25];     /* 28: DOS 3.31 BPB as returned by BUILD BPB */
    unsigned char  label[11];   /* 53: volume label */
};

/* control block flags */
#define CBF_VXD         0x0001  /* VxD active, route requests to vxd_entry */
#define CBF_DOSCORE     0x0002  /* DOS core image loaded */
#define CBF_REINIT      0x0004  /* hardware must be re-initialized by DOS core */
#define CBF_USB         0x0008  /* DOS core runs USB */
#define CBF_BUSY        0x0010  /* DOS core executing */
#define CBF_VCPI        0x0020  /* DOS core entered through VCPI */
#define CBF_NOCORE      0x0040  /* DOS core failed to start */
#define CBF_KICK        0x0080  /* poll requested */
#define CBF_WHEEL4      0x0100  /* inject IntelliMouse 4-byte packets */
#define CBF_PS2INJ      0x0200  /* 8042 injection available */
#define CBF_AUXWIN      0x0400  /* WINMOUSE.DRV reads the aux port (standard mode) */
#define CBF_AUXBIOS     0x0800  /* BIOS pointing device enabled (DOS mouse driver) */
#define CBF_MBOX        0x1000  /* WINMOUSE.DRV reads the mouse mailbox (standard mode) */
#define CBF_HAVEDISK    0x2000  /* WINDISK found disks: stays resident even without drive letters */
#define CBF_CHARDEV     0x4000  /* WINDISK.SYS resident as a character device (no drive letters) */
#define CBF_FWREQ       0x8000  /* WINAUDIO: the core asks for firmware files (wincb fw table) */

/* WINAUDIO: firmware files of the Intel audio DSP, loaded into XMS by WINAUDIO.SYS from its
   directory before the core starts the sound devices; the table takes the place of the
   (unused) drive units of the control block */
struct winfw {
    char           name[13];    /* 8.3 */
    unsigned char  state;       /* WFW_* */
    unsigned short emb;         /* XMS handle */
    unsigned long  phys;        /* 4K aligned */
    unsigned long  size;
};
#define WFW_REQ         1
#define WFW_OK          2
#define WFW_MISSING     3
#define WIN_MAXFW       16
#define CB_FWTAB        448

/* options parsed from CONFIG.SYS line */
#define OPT_USB         0x0001  /* /USB  : USB in DOS and standard mode */
#define OPT_NOAHCI      0x0002  /* /NOAHCI */
#define OPT_NONVME      0x0004  /* /NONVME */
#define OPT_ALL         0x0008  /* /ALL  : also mount partitions on BIOS disks */
#define OPT_VERBOSE     0x0010  /* /V */
#define OPT_DEBUG       0x0020  /* /D    : debug log to port E9 / COM */
#define OPT_NOVXD       0x0040  /* /NOVXD: do not auto-load WINCORE.386 */
#define OPT_NOHID       0x0080  /* /NOHID */
#define OPT_NOMSC       0x0100  /* /NOMSC */
#define OPT_COM1        0x0200  /* /COM1 : debug log on COM1 instead of port E9 */
#define OPT_NOKBC       0x0400  /* /NOKBC: no 8042 injection in DOS mode */
#define OPT_NVME        0x0800  /* /NVME : take over NVMe controllers the BIOS initialized */
#define OPT_NOIDE       0x1000  /* /NOIDE */
#define OPT_PIO         0x2000  /* /PIO  : no IDE bus-master DMA */
#define OPT_WINONLY     0x4000  /* /WIN  : USB only in Windows 386 enhanced mode */
#define OPT_ISA         0x8000  /* /ISA  : also probe the ISA IDE ports 1E8h and 168h */
#define OPT_SCREEN      0x00010000UL    /* /S      : core log on the text screen too */
#define OPT_POLL        0x00020000UL    /* /POLL   : no controller interrupts, polling only */
#define OPT_NOXHCI      0x00040000UL    /* /NOXHCI : leave xHCI controllers to the BIOS */
#define OPT_NOEHCI      0x00080000UL    /* /NOEHCI */
#define OPT_NOOHCI      0x00100000UL    /* /NOOHCI */
#define OPT_NOUHCI      0x00200000UL    /* /NOUHCI */
#define OPT_NOROUTE     0x00400000UL    /* /NOROUTE: no Intel EHCI -> xHCI port switching */
#define OPT_NOHDA       0x00800000UL    /* /NOHDA  : (WINAUDIO) no HD Audio controllers */
#define OPT_NOAC97      0x01000000UL    /* /NOAC97 : no AC'97 controllers */
#define OPT_INVEAPD     0x02000000UL    /* /EAPD   : AC'97 external amplifier on with EAPD = 1 */
#define OPT_NOGENERIC   0x04000000UL    /* /NOGEN  : do not try unknown 04/01 functions as AC'97 */
#define OPT_HDAIMM      0x08000000UL    /* /IMM    : HD Audio immediate commands instead of CORB/RIRB */
#define OPT_NOSYNTH     0x10000000UL    /* /NOSYNTH: no MIDI synthesizer */
#define OPT_POSBUF      0x20000000UL    /* /POSBUF : HD Audio DMA position buffer instead of LPIB */
#define OPT_HDAGPIO     0x40000000UL    /* /GPIO   : HD Audio codec GPIO 0-3 as outputs, high (amplifiers) */
#define OPT_NOI2C       0x80000000UL    /* /NOI2C  : (WINUSB) no HID over I2C touchpads */
/* second option word (wincb.opts2) */
#define OPT2_NOTB       0x0001  /* /NOTB     : (WINUSB) no Thunderbolt / USB4 host support */
#define OPT2_NOTBPCIE   0x0002  /* /NOTBPCIE : no PCIe tunnels (NVMe, xHCI behind a dock) */
#define OPT2_NOTBUSB    0x0004  /* /NOTBUSB  : no USB3 tunnels on USB4 links */
#define OPT2_PCIHP      0x0008  /* /PCIHP    : PCIe hot plug on every hot plug capable port, Thunderbolt or not */
#define OPT2_DOSDISK    0x0010  /* /DOSDISK  : (WINUSB) drive letters from the DOS driver on MS-DOS 7 too */
#define OPT2_NODISK     0x0020  /* set by the DOS driver: no DOS drive letters (Windows 95/98 own the disks) */
#define OPT2_DSP        0x0080  /* /DSP      : (WINAUDIO) HD Audio codecs through the Intel DSP (SST/SOF) too */
#define OPT2_DMIC       0x0100  /* /DMIC     : digital microphones without an NHLT table (IPC3 firmware) */
#define OPT2_NODSP      0x0200  /* /NODSP    : never start the Intel DSP */
#define OPT2_NORELOC    0x0040  /* /NORELOC  : no relocation pass at the start; a BAR above 4G is moved when its driver starts */
#define OPT2_ALL4G      0x0800  /* /ALL4G    : the relocation pass moves every device and bridge window, not only WINDRV's */
#define OPT2_NOPS2      0x0400  /* /NOPS2    : (WINUSB) leave the PS/2 mouse to Windows / the BIOS in 386 enhanced mode */
#define OPT2_NOTP       0x1000  /* /NOTP     : (WINUSB) PS/2 touchpads stay in their mouse mode, no SMBus touchpads */
#define OPT2_NOSMB      0x2000  /* /NOSMB    : (WINUSB) touchpads on PS/2 only, not on SMBus */

struct winmount {                /* partition already mounted by DOS through INT 13h */
    unsigned char  biosdrv;
    unsigned char  letter;
    unsigned short rsv;
    unsigned long  start;
};

struct wingeom {                 /* BIOS (translated) geometry of a hard disk */
    unsigned short cyls;
    unsigned char  heads;
    unsigned char  spt;
};

struct wincb {                   /* 2048 bytes, lives in WINDISK.SYS / WINUSB.SYS memory */
    unsigned long  magic;       /* 0 */
    unsigned short version;     /* 4 */
    unsigned short size;        /* 6 */
    unsigned long  vxd_entry;   /* 8   V86 seg:off of VxD callback */
    unsigned long  flags;       /* 12  CBF_* */
    unsigned char  nunits;      /* 16 */
    unsigned char  firstdrive;  /* 17  0 = A: */
    unsigned char  nfixed;      /* 18 */
    unsigned char  nslots;      /* 19 */
    unsigned long  opts;        /* 20  OPT_* */
    unsigned long  core_phys;   /* 24 */
    unsigned long  core_size;   /* 28 */
    unsigned short irqmask;     /* 32  IRQs the DOS core wants */
    unsigned short drvseg;      /* 36-2=34 */
    unsigned short mplex;       /* 36 */
    unsigned short dosver;      /* 38 */
    unsigned long  biosmbr[8];  /* 40  CRC32 of MBR for BIOS disks 80h.. */
    struct wingeom  biosgeom[8]; /* 72  INT 13h AH=08h geometry of BIOS disks 80h.. */
    unsigned char  nbios;       /* 104 */
    unsigned char  nmounted;    /* 105 */
    unsigned char  slotreq;     /* 106 requested slots (/R:n); bit 7: the default, which grows with
                                   the partitions of the removable disks found at the start */
    unsigned char  kbcstat;     /* 107 */
    struct winmount mounted[16]; /* 108 .. 236 */
    unsigned short bpbptr[WIN_MAXUNITS]; /* 236 .. 284 near pointers for INIT */
    unsigned long  cb_lin;      /* 284 linear address seen by DOS core */
    unsigned long  ticks;       /* 288 */
    unsigned long  pollcnt;     /* 292 */
    unsigned long  irqcnt;      /* 296 */
    unsigned short wheel_ctl;   /* 300 bit0: an INT 33h wheel-API client reads the wheel itself */
    unsigned short shirq;       /* 302 IRQs shared with devices of other drivers */
    short          mb_dx;       /* 304 USB mouse mailbox for WINMOUSE.DRV in standard mode */
    short          mb_dy;       /* 306 */
    signed char    mb_dz;       /* 308 wheel, positive = away from the user */
    unsigned char  mb_btn;      /* 309 bit 0 left, 1 right, 2 middle, 3 back (4th), 4 forward (5th) */
    unsigned char  mb_seq;      /* 310 incremented on every update */
    unsigned char  mb_flags;    /* 311 bit 0 absolute position in mb_absx/mb_absy, bits 4-6 buttons 1-3
                                   and bits 1-2 buttons 4-5 pressed since WINMOUSE.DRV last read the mailbox */
    unsigned short ideown[8];   /* 312 IDE channels (command port) owned by the DOS core */
    unsigned short hcbdf[8];    /* 328 USB controllers (bus << 8 | dev << 3 | fn) .. */
    unsigned char  hcirq[8];    /* 344 .. and their IRQs (0xFF none), for the real-mode IRQ code */
    unsigned long  tsc_khz;     /* 352 TSC frequency measured by the DOS core */
    char           vxdpath[92]; /* 356 .. 448 */
    struct winunit  units[WIN_MAXUNITS]; /* 448 .. 1984 */
    unsigned short nvmeown[8];  /* 1984 NVMe controllers (bus<<8|devfn)+1 owned by the DOS core */
    unsigned char  kq_head;     /* 2000 8042 injection queue (DOS mode) */
    unsigned char  kq_tail;     /* 2001 */
    unsigned short kq[20];      /* 2002 low byte data, high byte 8042 command D2h/D3h */
    unsigned short mb_absx;     /* 2042 tablet position 0..65535 */
    unsigned short mb_absy;     /* 2044 */
    unsigned short opts2;       /* 2046 OPT2_* */
};

/* follows wincb in WINUSB.SYS (DOS mode with /USB): the bytes of a PS/2 touchpad that the
   core drives in its own protocol, caught on IRQ 12, and the packets the core makes for the
   pointing device BIOS callback of the DOS mouse driver */
#define WIN_CBSIZE      2048
#define WIN_AUXQ        64
#define WIN_AUXPK       16
struct wincbx {
    unsigned char  mode;        /* 0 bit 0: aux bytes go to q */
    unsigned char  bios;        /* 1 inside a pointing device BIOS call: aux bytes to the BIOS */
    unsigned char  qhead;       /* 2 written on IRQ 12 */
    unsigned char  qtail;       /* 3 read by the core */
    unsigned char  bioscnt;     /* 4 pointing device BIOS calls so far */
    unsigned char  phead;       /* 5 packets: written by the core */
    unsigned char  ptail;       /* 6 delivered by WINUSB.SYS */
    unsigned char  psize;       /* 7 packet size the DOS driver asked for (C205h BH) */
    unsigned long  cbfn;        /* 8 its callback (C207h ES:BX) */
    unsigned char  q[WIN_AUXQ]; /* 12 */
    unsigned char  pk[WIN_AUXPK][4];    /* 76 status, X, Y, Z */
};

/* parameters passed by the platform to core_init */
struct core_params {
    unsigned long pool_lin;
    unsigned long pool_phys;
    unsigned long pool_size;
    unsigned long cb_lin;
    unsigned long mode;         /* 1 DOS, 2 VxD */
    unsigned long flags;
    unsigned long mbr_lin;      /* DOS: MBR sectors of BIOS disks 80h.. (nbios * 512 bytes) */
    unsigned long rsv[2];
};

/* ---- application API (INT 2Fh AH=mplex AL=01h, ES:BX -> winapireq) ---- */
#define WINF_INFO        0x0001
#define WINF_USBTREE     0x0010
#define WINF_USBDESC     0x0011
#define WINF_USBEJECT    0x0012
#define WINF_TBTREE      0x0013  /* buf: wintbnode array (Thunderbolt / USB4 host, routers, tunnels) */
#define WINF_DISKS       0x0020
#define WINF_RAWREAD     0x0021
#define WINF_CDREQ       0x0023  /* buf: DOS request header for the CD-ROM device (WINCD.SYS) */
#define WINF_RAWIO       0x0024  /* arg0 disk | 80000000h write, arg1:arg2 LBA, buf/buflen: sectors */
#define WINF_UNITS       0x0022  /* buf: winunit array (nunits entries), first drive in wininfo */
#define WINF_LOG         0x0030
#define WINF_SETOPT      0x0040
#define WINF_FLUSH       0x0041

#define WINF_AUD_DEVS    0x0050  /* buf: winauddev array */
#define WINF_AUD_OPEN    0x0051  /* arg0 dev | dir << 8, arg1 rate, arg2 bits | ch << 8 | float << 16,
                                   buf: winring (seg:off) -> retlen stream id */
#define WINF_AUD_CTL     0x0052  /* arg0 stream, arg1 AC_*, arg2 value */
#define WINF_AUD_MIX     0x0053  /* arg0 dev, arg1 MX_* (| 0x80000000 set), arg2 value -> retlen value */
#define WINF_AUD_MIDI    0x0054  /* arg0 dev (0xFF synthesizer), arg1 short message or buf/buflen */

/* USB video class cameras (WINUSB) */
#define WINF_CAM_DEVS    0x0060  /* buf: wincamdev array */
#define WINF_CAM_MODES   0x0061  /* arg0 camera: buf: wincammode array */
#define WINF_CAM_START   0x0062  /* arg0 camera, arg1 mode */
#define WINF_CAM_STOP    0x0063  /* arg0 camera */
#define WINF_CAM_FRAME   0x0064  /* arg0 camera, arg1 offset into the frame: offset 0 takes the newest
                                   frame (winframe header, then data), later offsets continue it;
                                   WINE_BUSY when no new frame is there */

#define WINE_OK          0
#define WINE_BADFN       1
#define WINE_NODEV       2
#define WINE_IO          3
#define WINE_SMALL       4
#define WINE_BUSY        5
#define WINE_NOCORE      6

struct winapireq {               /* 32 bytes */
    unsigned short fn;
    unsigned short status;
    unsigned long  arg0;
    unsigned long  arg1;
    unsigned long  arg2;
    unsigned long  buflen;
    unsigned long  retlen;
    unsigned long  buf;         /* real-mode seg:off (or linear when called inside the VxD) */
    unsigned long  rsv;
};

/* ---- sound ---- */
#define WINRING_MAGIC    0x47523357UL    /* 'WINRG' */
struct winring {                 /* 32-byte header + data, in conventional memory */
    unsigned long  magic;
    unsigned long  size;        /* data bytes, multiple of the frame size */
    unsigned long  wr;          /* producer: total bytes written */
    unsigned long  rd;          /* consumer: total bytes taken */
    unsigned long  pos;         /* playback: bytes heard so far; capture: bytes recorded */
    unsigned long  flags;       /* RF_* */
    unsigned long  rsv[2];
};
#define RF_UNDERRUN     0x0001
#define RF_OVERRUN      0x0002
#define RF_GONE         0x0004  /* device removed */

#define WINAC_OUT        0x01    /* winauddev.caps */
#define WINAC_IN         0x02
#define WINAC_MIDI       0x04    /* MIDI output (USB MIDI) */
#define WINAC_SYNTH      0x08    /* built-in synthesizer on this device */

#define AS_MIC          0x01    /* input sources */
#define AS_LINE         0x02
#define AS_CD           0x04
#define AS_AUX          0x08
#define AS_MIX          0x10

#define MX_MASTER       0       /* volumes: left | right << 16, 0..65535 */
#define MX_WAVE         1
#define MX_SYNTH        2
#define MX_INGAIN       3
#define MX_CD           4
#define MX_LINE         5
#define MX_COUNT        6
#define MX_SRC          0x100   /* WINF_AUD_MIX: input source (AS_*) */
#define MX_MUTE         0x101   /* WINF_AUD_MIX: master mute */

#define AC_START        1       /* WINF_AUD_CTL */
#define AC_PAUSE        2
#define AC_RESET        3       /* stop and forget queued data */
#define AC_CLOSE        4
#define AC_VOLUME       5       /* stream volume: left | right << 16 */

struct winauddev {               /* 64 bytes */
    unsigned char  id;
    unsigned char  type;        /* 1 HDA 2 AC'97 3 USB */
    unsigned char  caps;        /* WINAC_* */
    unsigned char  insrcs;      /* AS_* */
    unsigned long  rate;        /* hardware sample rate */
    unsigned short vid, pid;
    unsigned char  insrc;
    unsigned char  hwvol;
    unsigned char  gone;
    unsigned char  rsv;
    char           name[48];
};

struct wininfo {                 /* WINF_INFO */
    unsigned short version;
    unsigned short mode;        /* 1 DOS core, 2 VxD */
    unsigned long  flags;
    unsigned char  ndisks;
    unsigned char  nunits;
    unsigned char  firstdrive;
    unsigned char  nhc;
    unsigned char  nusbdev;
    unsigned char  ncd;         /* CD/DVD drives (WINDISK: DOS CD-ROM device units) */
    unsigned char  rsv[2];
    unsigned long  poolfree;
    unsigned long  uptime_ms;
    char           build[32];
};

struct windiskinfo {             /* WINF_DISKS, one per disk */
    unsigned char  index;
    unsigned char  kind;        /* 1 AHCI 2 NVMe 3 USB 4 IDE 5 SD 6 MMC/eMMC */
    unsigned char  present;
    unsigned char  shared;
    unsigned long  sectors_lo;
    unsigned long  sectors_hi;
    unsigned long  ssize;
    unsigned long  key;
    unsigned long  reads;
    unsigned long  writes;
    unsigned long  errors;
    char           model[41];
    char           serial[21];
    char           letters[10];
    unsigned short usbdev;      /* USB device id (winusbnode.id) of a USB disk, 0 otherwise */
};

#define CAM_YUY2        1       /* wincammode.type */
#define CAM_MJPEG       2
#define CAM_NV12        3

struct wincamdev {               /* 48 bytes */
    unsigned char  id;
    unsigned char  nmodes;
    unsigned char  gone;
    unsigned char  active;      /* streaming, mode + 1 */
    unsigned short usbdev;      /* winusbnode.id */
    char           name[42];
};

struct wincammode {              /* 16 bytes */
    unsigned char  fmt, frame, type, rsv;
    unsigned short w, h;
    unsigned long  interval;    /* 100 ns units */
    unsigned long  maxsize;     /* bytes of a frame, 0: too large for the driver */
};

struct winframe {                /* 16 bytes in front of the frame data */
    unsigned long  seq;
    unsigned long  size;
    unsigned short w, h;
    unsigned char  type, rsv[3];
};

/* Thunderbolt / USB4 node, WINF_TBTREE returns an array of these; the first is the host */
#define TN_HOST     1           /* bw_up: host interface version, bw_down: firmware mode / security level */
#define TN_ROUTER   2           /* nhops: adapter count */
#define TN_ADAPTER  3           /* tunnel: 0 lane (src: lane state) 1 PCIe 2 USB3 14 DisplayPort, dst: remote router */
#define TN_TUNNEL   4           /* tunnel: 1 PCIe 2 USB3, src/dst adapters, bw: USB3 Mb/s */
#define TN_ICMDEV   5           /* device known to the firmware CM: bw_up/bw_down first UUID words */
#define TNF_USB4     0x01
#define TNF_ACTIVE   0x02
#define TNF_FIRMWARE 0x04       /* tunnel built by the firmware / BIOS; ICM device: boot device */
#define TNF_APPROVED 0x08
#define TNF_MODE(f)  (((f) >> 4) & 3)   /* host: 1 software CM, 2 firmware CM */

struct wintbnode {               /* 64 bytes */
    unsigned char  type;        /* TN_* */
    unsigned char  depth;
    unsigned char  port;        /* adapter number / parent lane of a router / ICM link */
    unsigned char  flags;       /* TNF_* */
    unsigned short vendor, device;
    unsigned long  route_lo, route_hi;  /* host: PCI bus<<8|devfn */
    unsigned char  tunnel, src, dst, nhops;
    unsigned long  bw_up, bw_down;
    char           name[36];
};

/* USB tree node, WINF_USBTREE returns an array of these */
#define UN_HC       1
#define UN_DEV      2
#define UN_PORT     3

#define UD_NONE     0
#define UD_HUB      1
#define UD_KBD      2
#define UD_MOUSE    3
#define UD_BOT      4
#define UD_UAS      5
#define UD_HIDOTHER 6
#define UD_AUDIO    7
#define UD_MIDI     8
#define UD_TABLET   9
#define UD_VIDEO    10

struct winusbnode {              /* 160 bytes */
    unsigned char  type;        /* UN_* */
    unsigned char  depth;
    unsigned char  speed;       /* 0 low 1 full 2 high 3 super 4 super+ */
    unsigned char  port;        /* port on parent (1-based) */
    unsigned short id;
    unsigned short parent;
    unsigned short vid, pid, bcd, bcdusb;
    unsigned char  cls, sub, proto, addr;
    unsigned char  nports;
    unsigned char  driver;      /* UD_* */
    unsigned char  hctype;      /* 1 UHCI 2 OHCI 3 EHCI 4 xHCI */
    unsigned char  config;
    unsigned short maxpower;    /* mA */
    unsigned short iface_count;
    unsigned long  portstatus;
    unsigned long  errors;
    unsigned long  xfers;
    unsigned long  pciloc;      /* controllers: bus<<8|devfn */
    unsigned long  mmio;
    unsigned char  irq;
    unsigned char  luns;
    unsigned char  slot;        /* xHCI slot id */
    unsigned char  rsv;         /* driver detail: USB Audio class version (1, 2) */
    char           letters[8];
    char           manuf[32];
    char           product[40];
    char           serial[24];
};

#pragma pack(pop)

#endif
