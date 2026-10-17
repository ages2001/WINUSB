/* winpdr.c - WINUSB.PDR / WINDISK.PDR (-DPDR_DISK): I/O Supervisor port driver for Windows
   95/98/ME. One DDB per devnode that WINUSB.VXD / WINDISK.VXD created for a disk (USB mass
   storage device, NVMe namespace, IDE / SATA / SD disk), one DCB per disk of it; reads and
   writes go to the VxD (its service table) and complete at once. CD/DVD drives get a SCSI
   bus DCB: the I/O Supervisor's SCSI'izer turns the CD file system's requests into SRBs,
   whose commands the drive executes (CDFS, audio, eject, media change). */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

#pragma pack(push, 1)
struct aep {                    /* AEPHDR */
    u16 func, result;
    u32 ddb;
    u8 lgn, align[3];
};
struct aep_init {
    struct aep hdr;
    u32 refdata;
    u8 flags, max_target, max_lun;
    u32 dcb, hdevnode, regkey;
    u8 align;
};
struct aep_dcb {                /* AEP_inquiry_device, AEP_dcb_config, AEP_dcb_unconfig */
    struct aep hdr;
    u32 dcb;
};
struct isp {
    u16 func, result;
};
struct isp_ddb_create {
    struct isp hdr;
    u16 size;
    u32 ptr;
    u8 flags, pad;
};
struct isp_ddb_dealloc {
    struct isp hdr;
    u32 ddb, dvt;
};
struct isp_gfnd {
    struct isp hdr;
    u32 offset, found;
    u8 type, pad[3];
};
struct isp_calldown {
    struct isp hdr;
    u32 dcb, req, ddb;
    u16 expan_len;
    u32 flags;
    u8 lgn, pad;
};
struct ilb {
    u32 service_rtn, dprintf_rtn, wait_10th_sec, internal_request, io_criteria_rtn, int_io_criteria_rtn;
    u32 dvt, ios_mem_virt, enqueue_iop, dequeue_iop, rsv1, rsv2;
    u16 flags;
    u8 driver_numb, rsv3;
};
struct drp {
    char eyecatch[8];
    u32 lgn, aer, ilb;
    char name[16];
    u8 revision;
    u32 feature_code;
    u16 if_requirements;
    u8 bus_type;
    u16 reg_result;
    u32 reference_data;
    u8 rsv1[2];
    u32 rsv2;
};
struct iosddb {                 /* IOS DDB, followed by our part */
    u32 phys_addr, next, next_init, dcb_ptr;
    u8 number_buses, ios_flags;
    u16 sig;
    u32 dvt, devnode_ptr, rsv;
    u32 dn;                     /* ours: the devnode */
};
struct dcb {                    /* DCB_COMMON + DCB, the fields a port driver touches */
    u32 physical_dcb, expansion_length, ptr_cd, next_dcb, next_logical_dcb;
    u8 drive_lttr_equiv, unit_number;
    u16 tsd_flags;
    u32 vrp_ptr, dmd_flags, device_flags, device_flags2, partition_start, track_table_ptr, bds_ptr, rsv1, rsv2;
    u8 apparent_blk_shift, partition_type;
    u16 sig;
    u8 device_type;
    u32 exclusive_vm;
    u8 disk_bpb_flags, cassoc, sstor_host;
    u16 user_drvlet, rsv3;
    u32 rsv4;                   /* end of DCB_COMMON (76 bytes) */
    u32 max_xfer_len;
    u32 actual_sector_cnt[2];
    u32 actual_blk_size, actual_head_cnt, actual_cyl_cnt, actual_spt;
    u32 next_ddb_dcb, dev_node;
    u8 bus_type, bus_number, queue_freeze, max_sg_elements, io_pend_count, lock_count;
    u16 scsi_vsd_flags;
    u8 scsi_target_id, scsi_lun, scsi_hba, max_sense_data_len;
    u16 srb_ext_size;
    u8 inquiry_flags[8];
    char vendor_id[8], product_id[16], rev_level[4], port_name[8];
    u8 current_unit;
    u32 blocked_iop, vol_unlock_timer;
    u8 access_timer, vol_flags, q_algo, unit_on_ctl;
    u32 port_specific, spindown_timer;
    u32 bdd_next;               /* DCB_BLOCKDEV */
    u8 bdd_major, bdd_minor, bdd_subtype, bdd_int13;
    u32 bdd_flags;
};
struct ior {
    u32 next;
    u16 func, status;
    u32 flags, callback, start_addr[2], xfer_count, buffer_ptr, private_client, private_ios, private_port;
    u32 requestor_usage[5];
    u32 req_req_handle, req_vol_handle, sgd_lin_phys;
    u8 num_sgds, vol_designtr;
    u16 ios_private_1;
    u32 rsv[2];
};
struct iop {
    u32 physical, physical_dcb, original_dcb;
    u16 timer, timer_orig;
    u32 calldown_ptr, callback_ptr, voltrk_private, thread_handle, srb, rsv[2];
    u32 callback_table[12];
    u8 format_head, format_xfer_rate;
    u16 format_track;
    u32 format_num_sectors;
    struct ior ior;
};
struct pdrinfo {                /* VxD service 1 */
    u32 handle;
    u32 sectors_lo, sectors_hi;
    u32 ssize;
    u8 removable, wprot, cdrom, present;
    char model[41];
    char serial[21];
    u8 bios;                    /* BIOS INT 13h drive of the disk, 0 none */
    u8 pad;
    u32 letters;                    /* its DOS drives (bit 0 A:), from IO.SYS at the start */
};
#pragma pack(pop)

#define AEP_INITIALIZE      0
#define AEP_BOOT_COMPLETE   2
#define AEP_CONFIG_DCB      3
#define AEP_UNCONFIG_DCB    4
#define AEP_DEVICE_INQUIRY  6
#define AEP_UNINITIALIZE    15
#define AEP_PEND_UNCONFIG_DCB 21
#define AEP_SUCCESS         0
#define AEP_FAILURE         0xFFFF
#define AEP_NO_MORE_DEVICES 2
#define ISP_CREATE_DDB      0
#define ISP_INSERT_CALLDOWN 5
#define ISP_GET_FIRST_NEXT_DCB 8
#define ISP_DEALLOC_DDB     9
#define DRP_MISC_PD         (1UL << 0x13)
#define DRP_FC_1_SEC        0x2000
#define DRP_IF_STD          0xFF
#define DRP_BT_ESDI         0
#define DRP_REMAIN_RESIDENT 1
#define DCB_type_disk       0
#define DCB_type_cdrom      5
#define DCB_BUS_ESDI        0
#define DCB_BUS_SCSI        1
#define DCB_DEV_WRITEABLE   0x02
#define DCB_DEV_REMOVABLE   0x04
#define DCB_DEV_PHYSICAL    0x8000
#define DCB_DEV_RMM         0x2000
#define DCB_DEV_INT13_DRIVE 0x01
#define DCB_dmd_physical    0x08
#define DCB_dmd_prot_mode   0x00100000UL
#define IOR_READ            0
#define IOR_WRITE           1
#define IOR_VERIFY          2
#define IOR_MEDIA_CHECK     5
#define IOR_MEDIA_CHECK_RESET 6
#define IOR_LOAD_MEDIA      7
#define IOR_EJECT_MEDIA     8
#define IOR_LOCK_MEDIA      9
#define IOR_UNLOCK_MEDIA    0x0A
#define IOR_COMPUTE_GEOM    0x0C
#define IOR_FLUSH_DRIVE     0x27
#define IOR_FLUSH_DRIVE_AND_DISCARD 0x28
#define IORF_SCATTER_GATHER 0x02
#define IORF_SRB_VALID      0x8000
#define IORS_SUCCESS        0
#define IORS_INVALID_COMMAND 0x16
#define IORS_MEDIA_ERROR    0x14
#define IORS_NO_DEVICE      0x17
#define IORS_NO_MEDIA       0x18
#define IORS_WRITE_PROTECT  0x1F

extern u32 __cdecl ios_register(struct drp *d);
extern void __cdecl pdr_call1(void *fn, void *arg);
extern void __cdecl thk_aer(void);
extern void __cdecl thk_request(void);
extern u32 __cdecl core_get_version(void);
extern int __cdecl core_blk_lookup(u32 dn, u32 unit, struct pdrinfo *out);
extern int __cdecl core_blk_io(u32 h, u32 lba_lo, u32 lba_hi, u32 count, void *buf, u32 write);
extern u32 __cdecl core_blk_status(u32 h);
extern int __cdecl core_cd_media(u32 h);
extern int __cdecl core_cd_command(u32 h, const u8 *cdb, void *buf, u32 len, u8 *sense);
extern void __cdecl pdr_outb(u32 port, u32 v);
extern u32 __cdecl vmm_get_ddb(u32 devid);
#ifdef PDR_DISK
#define CORE_DEVICE_ID  0x3D33          /* WINDISK.VXD */
#define PDR_DDBNAME     "WINDSKPD"
#define PDR_FILE        "WINDISK.PDR"
#define PDR_VENDOR      "WINDISK"
#else
#define CORE_DEVICE_ID  0x3D34          /* WINUSB.VXD */
#define PDR_DDBNAME     "WINUSBPD"
#define PDR_FILE        "WINUSB.PDR"
#define PDR_VENDOR      "WINUSB"
#endif
extern char bss_begin[], bss_end[];
extern u32 WINPDR_DDB[];                /* DDB_Reference_Data at dword 11 */
#pragma aux WINPDR_DDB "*"

static struct ilb ilb;
/* initialized data (a zero would go to the BSS, which the loader does not clear): survives the
   repeated dynamic init calls of VXDLDR */
#define STATE_NEW   0x4E455731UL
#define STATE_REG   0x52454731UL
static u32 regstate = STATE_NEW;
static int trace = 0;           /* set from the VxD options (/D) */
struct drp pdr_drp = { PDR_DDBNAME, DRP_MISC_PD, 0, 0, PDR_FILE, 1, 0, DRP_IF_STD, DRP_BT_ESDI, 0, 0, { 0, 0 }, 0 };
#define drp pdr_drp
static int ndisks;
#define MAXDDB 16
static u32 ddbs[MAXDDB];        /* DDBs created, one per devnode */

static int ddb_find_dn(u32 dn)
{
    int i;
    for (i = 0; i < MAXDDB; i++)
        if (ddbs[i] && ((struct iosddb *)ddbs[i])->dn == dn) return i;
    return -1;
}

static void dbg(const char *s)
{
    while (*s) pdr_outb(0xE9, (u8)*s++);
}

static void dbghex(const char *s, u32 v)
{
    int i;
    dbg(s);
    for (i = 28; i >= 0; i -= 4) pdr_outb(0xE9, (u8)("0123456789abcdef"[(v >> i) & 15]));
    pdr_outb(0xE9, '\n');
}

static void memzero(void *p, u32 n)
{
    u8 *d = p;
    while (n--) *d++ = 0;
}

static void padcopy(char *d, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (*s) d[i] = *s++;
        else d[i] = ' ';
    }
}

static int isp_call(void *packet)
{
    pdr_call1((void *)ilb.service_rtn, packet);
    return ((struct isp *)packet)->result;
}

static void aer_initialize(struct aep_init *a)
{
    struct isp_ddb_create c;
    struct pdrinfo info;
    struct iosddb *ddb;
    int r;
    int slot;
    if (trace) dbghex("pdr: initialize devnode ", a->hdevnode);
    if (!a->hdevnode || !vmm_get_ddb(CORE_DEVICE_ID)) {
        /* loaded by IOS at boot before the VxD: nothing to serve yet */
        a->hdr.result = AEP_FAILURE;
        return;
    }
    /* IOS asks again and again ("another adapter?") until the driver says no */
    if (ddb_find_dn(a->hdevnode) >= 0) {
        a->hdr.result = AEP_FAILURE;
        return;
    }
    for (slot = 0; slot < MAXDDB && ddbs[slot]; slot++) ;
    if (slot >= MAXDDB) {
        a->hdr.result = AEP_FAILURE;
        return;
    }
    if (core_get_version() & 0x10000UL) trace = 1;
    r = core_blk_lookup(a->hdevnode, 0, &info);
    if (r < 0) {
        dbg("pdr: devnode unknown to the VxD\n");
        a->hdr.result = AEP_FAILURE;
        return;
    }
    memzero(&c, sizeof(c));
    c.hdr.func = ISP_CREATE_DDB;
    c.size = sizeof(struct iosddb);
    if (isp_call(&c) || !c.ptr) {
        a->hdr.result = AEP_FAILURE;
        return;
    }
    ddb = (struct iosddb *)c.ptr;
    ddb->dn = a->hdevnode;
    ddb->devnode_ptr = a->hdevnode;
    ddbs[slot] = c.ptr;
    a->hdr.ddb = c.ptr;
    a->max_target = 0;
    a->max_lun = 0;
    a->hdr.result = AEP_SUCCESS;
    dbg("pdr: devnode initialized\n");
}

static void geometry(u32 lo, u32 hi, u32 *heads, u32 *spt, u32 *cyls)
{
    u32 c;
    *spt = 63;
    *heads = 255;
    if (hi) {
        *cyls = 0xFFFFFFFFUL / (63 * 255);
        return;
    }
    c = lo / (63 * 255);
    if (!c) c = 1;
    *cyls = c;
}

extern u32 __cdecl vmm_init_state(void);

/* a DCB of the real-mode mapper for one of the DOS drives of a BIOS drive: Windows runs those
   volumes in compatibility mode already (the disk's port driver was installed after the
   start); a DCB of ours now would take the open volumes away under the file system */
static int rmm_has(u32 letters)
{
    struct isp_gfnd g;
    u32 cur = 0;
    int n = 0;
    for (;;) {
        struct dcb *d;
        memzero(&g, sizeof(g));
        g.hdr.func = ISP_GET_FIRST_NEXT_DCB;
        g.offset = cur;
        g.type = 0xFF;
        if (isp_call(&g) || !g.found || n++ > 64) return 0;
        cur = g.found;
        d = (struct dcb *)cur;
        if ((d->device_flags & DCB_DEV_RMM) && !(d->device_flags & DCB_DEV_PHYSICAL) &&
            d->drive_lttr_equiv < 26 && (letters & (1UL << d->drive_lttr_equiv)))
            return 1;
    }
}

/* a FAT partition in the partition table of a disk (DOS has its partitions as drives) */
static u8 sec0[4096];
static int fat_parts(u32 h, u32 ssize)
{
    int i;
    if (ssize > sizeof(sec0) || core_blk_io(h, 0, 0, 1, sec0, 0)) return 1;
    if (sec0[510] != 0x55 || sec0[511] != 0xAA) return 0;
    for (i = 0; i < 4; i++) {
        u8 t = sec0[446 + i * 16 + 4];
        if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E ||
            t == 0x05 || t == 0x0F) return 1;
    }
    return 0;
}

static void aer_inquiry(struct aep_dcb *a)
{
    struct dcb *d = (struct dcb *)a->dcb;
    struct iosddb *ddb = (struct iosddb *)a->hdr.ddb;
    struct pdrinfo info;
    int r = core_blk_lookup(ddb->dn, d->unit_on_ctl, &info);
    if (r > 0) {
        a->hdr.result = AEP_NO_MORE_DEVICES;
        return;
    }
    if (r < 0) {
        a->hdr.result = AEP_FAILURE;
        return;
    }
    /* at the start of Windows the I/O Supervisor hands the real-mode volumes over cleanly;
       once Windows runs (SYSSTATE_VXDINITCOMPLETED and later) they may have open files */
    if (info.bios >= 0x80 && vmm_init_state() >= 0x30000000UL && rmm_has(info.letters)) {
        dbghex("pdr: BIOS drive in compatibility mode, ours at the next start of Windows: ", info.bios);
        a->hdr.result = AEP_NO_MORE_DEVICES;
        return;
    }
    if (info.cdrom) {
        /* CD/DVD drive: 2048-byte blocks, removable medium */
        int m = core_cd_media(info.handle);
        d->device_type = DCB_type_cdrom;
        d->bus_type = DCB_BUS_SCSI;
        d->actual_sector_cnt[0] = (info.sectors_lo >> 2) | (info.sectors_hi << 30);
        d->actual_sector_cnt[1] = 0;
        d->actual_blk_size = 2048;
        d->max_xfer_len = 0x10000;
        d->max_sg_elements = 16;
        d->scsi_target_id = d->unit_on_ctl;
        d->device_flags |= DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE;
        d->dmd_flags |= DCB_dmd_physical | DCB_dmd_prot_mode;
        d->inquiry_flags[0] = DCB_type_cdrom;
        d->inquiry_flags[1] = 0x80;
        padcopy(d->vendor_id, PDR_VENDOR, 8);
        padcopy(d->product_id, info.model, 16);
        padcopy(d->rev_level, "1.0", 4);
        padcopy(d->port_name, PDR_VENDOR, 8);
        d->port_specific = info.handle;
        d->rsv2 = m >= 0 ? (u32)m : 0;  /* medium changes seen */
        a->hdr.result = AEP_SUCCESS;
        ndisks++;
        dbg("pdr: CD/DVD drive configured\n");
        return;
    }
    d->device_type = DCB_type_disk;
    d->bus_type = DCB_BUS_ESDI;
    d->actual_sector_cnt[0] = info.sectors_lo;
    d->actual_sector_cnt[1] = info.sectors_hi;
    d->actual_blk_size = 512;
    geometry(info.sectors_lo, info.sectors_hi, &d->actual_head_cnt, &d->actual_spt, &d->actual_cyl_cnt);
    d->max_xfer_len = 0x10000;
    d->max_sg_elements = 16;
    d->scsi_target_id = d->unit_on_ctl;
    d->device_flags |= DCB_DEV_PHYSICAL;
    if (!info.wprot) d->device_flags |= DCB_DEV_WRITEABLE;
    /* a removable disk without FAT partitions (UDF, a FAT or other file system on the whole
       disk, unformatted) is a removable drive: its drive letter is there for every file
       system. Disks with FAT partitions keep a drive letter per partition, as in DOS. */
    if (info.removable && !info.letters && !fat_parts(info.handle, info.ssize))
        d->device_flags |= DCB_DEV_REMOVABLE;
    d->dmd_flags |= DCB_dmd_physical | DCB_dmd_prot_mode;
    if (info.bios >= 0x80) {
        /* one of the BIOS's INT 13h drives: the I/O Supervisor takes its real-mode drive
           letters over (the VxD serves its INT 13h transfers until then) */
        d->device_flags |= DCB_DEV_INT13_DRIVE;
        d->bdd_int13 = info.bios;
        d->bdd_flags |= DCB_DEV_INT13_DRIVE;
        d->unit_number = info.bios;
    }
    d->inquiry_flags[0] = 0;
    d->inquiry_flags[1] = info.removable ? 0x80 : 0;
    padcopy(d->vendor_id, PDR_VENDOR, 8);
    padcopy(d->product_id, info.model, 16);
    padcopy(d->rev_level, "1.0", 4);
    padcopy(d->port_name, PDR_VENDOR, 8);
    d->port_specific = info.handle;
    a->hdr.result = AEP_SUCCESS;
    ndisks++;
    dbg("pdr: disk configured\n");
}

/* /D: every DCB the I/O Supervisor knows, physical and logical */
static void dump_dcbs(void)
{
    struct isp_gfnd g;
    u32 cur = 0;
    int n = 0;
    for (;;) {
        struct dcb *d;
        memzero(&g, sizeof(g));
        g.hdr.func = ISP_GET_FIRST_NEXT_DCB;
        g.offset = cur;
        g.type = 0xFF;
        if (isp_call(&g) || !g.found || n++ > 40) break;
        cur = g.found;
        d = (struct dcb *)cur;
        dbghex("pdr: dcb ", cur);
        dbghex("pdr:   type/unit/letter ", ((u32)d->device_type << 16) | ((u32)d->unit_number << 8) | d->drive_lttr_equiv);
        dbghex("pdr:   flags ", d->device_flags);
        dbghex("pdr:   flags2 ", d->device_flags2);
        dbghex("pdr:   dmd ", d->dmd_flags);
        if (d->device_flags & DCB_DEV_PHYSICAL) {
            dbghex("pdr:   sectors ", d->actual_sector_cnt[0]);
            dbghex("pdr:   int13/bddflags ", ((u32)d->bdd_int13 << 24) | (d->bdd_flags & 0xFFFFFF));
            dbghex("pdr:   bus/target ", ((u32)d->bus_type << 8) | d->scsi_target_id);
        } else {
            dbghex("pdr:   partition start ", d->partition_start);
        }
    }
}

static void aer_config(struct aep_dcb *a)
{
    struct isp_calldown c;
    memzero(&c, sizeof(c));
    c.hdr.func = ISP_INSERT_CALLDOWN;
    c.dcb = a->dcb;
    c.req = (u32)thk_request;
    c.ddb = a->hdr.ddb;
    c.flags = DCB_dmd_physical | DCB_dmd_prot_mode;
    c.lgn = a->hdr.lgn;
    a->hdr.result = isp_call(&c) ? AEP_FAILURE : AEP_SUCCESS;
    if (trace) {
        dbghex("pdr: calldown inserted, result ", a->hdr.result);
        dbghex("pdr:     dcb ptr_cd ", ((struct dcb *)a->dcb)->ptr_cd);
        dbghex("pdr:     dcb port_specific ", ((struct dcb *)a->dcb)->port_specific);
        dbghex("pdr:     dcb device_flags ", ((struct dcb *)a->dcb)->device_flags);
        dbghex("pdr:     dcb dmd_flags ", ((struct dcb *)a->dcb)->dmd_flags);
        dbghex("pdr:     dcb int13 ", ((struct dcb *)a->dcb)->bdd_int13);
        dbghex("pdr:     dcb bdd_flags ", ((struct dcb *)a->dcb)->bdd_flags);
    }
}

void __cdecl pdr_aer(struct aep *a)
{
    if (trace) {
        dbghex("pdr: aep ", a->func);
        dbghex("pdr:     ddb ", a->ddb);
        if (a->func == AEP_CONFIG_DCB || a->func == AEP_UNCONFIG_DCB || a->func == AEP_DEVICE_INQUIRY ||
            a->func == AEP_PEND_UNCONFIG_DCB)
            dbghex("pdr:     dcb ", ((struct aep_dcb *)a)->dcb);
    }
    switch (a->func) {
    case AEP_INITIALIZE: aer_initialize((struct aep_init *)a); break;
    case AEP_DEVICE_INQUIRY: aer_inquiry((struct aep_dcb *)a); break;
    case AEP_CONFIG_DCB: aer_config((struct aep_dcb *)a); break;
    case AEP_UNINITIALIZE:
    {
        int i;
        for (i = 0; i < MAXDDB; i++)
            if (ddbs[i] == a->ddb) ddbs[i] = 0;
        dbg("pdr: devnode uninitialized\n");
        a->result = AEP_SUCCESS;
        break;
    }
    case AEP_BOOT_COMPLETE:
        if (trace) dump_dcbs();
        a->result = AEP_SUCCESS;
        break;
    case AEP_UNCONFIG_DCB:
    case AEP_PEND_UNCONFIG_DCB:
        a->result = AEP_SUCCESS;
        break;
    default:
        a->result = AEP_FAILURE;    /* not ours (e.g. AEP_ASSOCIATE_DCB of the TSD) */
        break;
    }
}

/* cnt blocks of the DCB (512 bytes, CD/DVD 2048) at block (hi:lo) */
static int blk_xfer(struct dcb *d, u32 lo, u32 hi, u32 cnt, void *buf, int write)
{
    int sh = d->actual_blk_size == 2048 ? 2 : 0;    /* 512-byte sectors per block: 1 << sh */
    if (sh) {
        hi = (hi << sh) | (lo >> (32 - sh));
        lo <<= sh;
    }
    return core_blk_io(d->port_specific, lo, hi, cnt << sh, buf, (u32)write);
}

static u16 do_io(struct dcb *d, struct ior *r, int write)
{
    u32 lba = r->start_addr[0], hi = r->start_addr[1];
    int rc;
    if (write && !(d->device_flags & DCB_DEV_WRITEABLE)) return IORS_WRITE_PROTECT;
    if (r->flags & IORF_SCATTER_GATHER) {
        /* logical scatter list: (blocks, linear address) pairs, a zero count ends it */
        u32 *e = (u32 *)r->buffer_ptr, left = r->xfer_count;
        while (left && e[0]) {
            u32 cnt = e[0];
            if (cnt > left) cnt = left;
            rc = blk_xfer(d, lba, hi, cnt, (void *)e[1], write);
            if (rc) return rc == -2 ? IORS_NO_DEVICE : IORS_MEDIA_ERROR;
            if (lba + cnt < lba) hi++;          /* past 2^32 blocks */
            lba += cnt;
            left -= cnt;
            e += 2;
        }
        return left ? IORS_MEDIA_ERROR : IORS_SUCCESS;
    }
    rc = blk_xfer(d, lba, hi, r->xfer_count, (void *)r->buffer_ptr, write);
    if (rc) return rc == -2 ? IORS_NO_DEVICE : IORS_MEDIA_ERROR;
    return IORS_SUCCESS;
}

/* SCSI request block (the SCSI'izer gives one for the requests of a SCSI bus DCB) */
struct srb {
    u16 length;
    u8 function, srb_status, scsi_status, path_id, target_id, lun;
    u8 queue_tag, queue_action, cdb_length, sense_length;
    u32 flags, data_length, timeout, data_buffer, sense_buffer, next, original, extension, sort_key;
    u8 cdb[16];
};
#define SRB_STATUS_SUCCESS      0x01
#define SRB_STATUS_ERROR        0x04
#define SRB_STATUS_INVALID_REQUEST 0x06
#define SRB_STATUS_NO_DEVICE    0x08
#define SRB_STATUS_AUTOSENSE_VALID 0x80
#define SRB_FLAGS_DATA_OUT      0x80

/* SRB data given as a scatter list of the IOR: read here first */
#define SRB_XBUF 0x10000
static u8 xbuf[SRB_XBUF];

/* the CD/DVD drive executes the SRB's command (data from the drive only, into the SRB's
   buffer, the IOR's buffer, or the IOR's scatter list of (blocks, linear address) pairs) */
static u16 do_srb(struct dcb *d, struct srb *b, struct ior *ir)
{
    u8 sense[18];
    u32 buf = b->data_buffer, len = b->data_length;
    int r, sg = 0;
    if (trace) {
        dbghex("pdr:     srb func/cdb0 ", ((u32)b->function << 8) | b->cdb[0]);
        dbghex("pdr:     srb flags ", b->flags);
        dbghex("pdr:     srb len ", len);
        dbghex("pdr:     srb buf ", buf);
    }
    b->scsi_status = 0;
    if (b->function != 0) {                     /* not EXECUTE_SCSI: nothing to do here */
        b->srb_status = SRB_STATUS_SUCCESS;
        return IORS_SUCCESS;
    }
    if ((b->flags & SRB_FLAGS_DATA_OUT) && len) {
        b->srb_status = SRB_STATUS_INVALID_REQUEST;
        return IORS_INVALID_COMMAND;
    }
    if (len && (ir->flags & IORF_SCATTER_GATHER) && (!buf || buf == ir->buffer_ptr)) {
        /* the IOR's scatter list (Windows 98 puts its address into the SRB too) */
        if (len > SRB_XBUF) {
            b->srb_status = SRB_STATUS_INVALID_REQUEST;
            return IORS_INVALID_COMMAND;
        }
        buf = (u32)xbuf;
        sg = 1;
    } else if (!buf && len) {
        buf = ir->buffer_ptr;
    }
    sense[2] = sense[12] = sense[13] = 0;
    r = core_cd_command(d->port_specific, b->cdb, (void *)buf, buf ? len : 0, sense);
    if (trace) dbghex("pdr:     result/key/asc ", ((u32)(r & 0xFF) << 24) | ((u32)(sense[2] & 0x0F) << 16) | ((u32)sense[12] << 8) | sense[13]);
    if (r == 0) {
        if (sg) {
            /* the unit of the counts: blocks of the drive (Windows 95), 512-byte sectors or
               bytes, whichever adds up to the length */
            u32 *e = (u32 *)ir->buffer_ptr, off = 0, sum = 0, unit = 1, k;
            for (k = 0; k < 64 && e[2 * k]; k++) sum += e[2 * k];
            if (sum * d->actual_blk_size == len) unit = d->actual_blk_size;
            else if (sum * 512 == len) unit = 512;
            if (trace) dbghex("pdr:     sg unit ", unit);
            for (; e[0] && off < len; e += 2) {
                u32 n = e[0] * unit, i;
                if (n > len - off) n = len - off;
                for (i = 0; i < n; i++) ((u8 *)e[1])[i] = xbuf[off + i];
                off += n;
            }
        }
        b->srb_status = SRB_STATUS_SUCCESS;
        return IORS_SUCCESS;
    }
    if (r == 1) {
        b->scsi_status = 2;                     /* CHECK CONDITION */
        b->srb_status = SRB_STATUS_ERROR;
        if (b->sense_buffer && b->sense_length) {
            u32 i, n = b->sense_length < sizeof(sense) ? b->sense_length : sizeof(sense);
            for (i = 0; i < n; i++) ((u8 *)b->sense_buffer)[i] = sense[i];
            b->srb_status |= SRB_STATUS_AUTOSENSE_VALID;
        }
        return IORS_SUCCESS;
    }
    b->srb_status = SRB_STATUS_NO_DEVICE;
    return IORS_NO_DEVICE;
}

void __cdecl pdr_request(struct iop *p)
{
    struct ior *r = &p->ior;
    struct dcb *d = (struct dcb *)p->physical_dcb;
    u32 *cb;
    if (trace) {
        dbghex("pdr: ior func ", r->func);
        dbghex("pdr:     dcb ", p->physical_dcb);
        dbghex("pdr:     flags ", r->flags);
        dbghex("pdr:     sector ", r->start_addr[0]);
        dbghex("pdr:     count ", r->xfer_count);
        if (d->device_type == DCB_type_cdrom) {
            dbghex("pdr:     buffer ", r->buffer_ptr);
            dbghex("pdr:     srb ", p->srb);
        }
    }
    if ((r->flags & IORF_SRB_VALID) && p->srb && d->device_type == DCB_type_cdrom) {
        r->status = do_srb(d, (struct srb *)p->srb, r);
        goto done;
    }
    switch (r->func) {
    case IOR_READ: r->status = do_io(d, r, 0); break;
    case IOR_WRITE: r->status = do_io(d, r, 1); break;
    case IOR_VERIFY:
    case IOR_LOAD_MEDIA:
    case IOR_EJECT_MEDIA:
    case IOR_LOCK_MEDIA:
    case IOR_UNLOCK_MEDIA:
    case IOR_FLUSH_DRIVE:
    case IOR_FLUSH_DRIVE_AND_DISCARD:
    case IOR_MEDIA_CHECK_RESET:
    case IOR_COMPUTE_GEOM:
        r->status = IORS_SUCCESS;
        break;
    case IOR_MEDIA_CHECK:
        r->status = core_blk_status(d->port_specific) ? IORS_SUCCESS : IORS_NO_MEDIA;
        break;
    default:
        r->status = IORS_INVALID_COMMAND;
        break;
    }
done:
    /* complete: the next callback entry down the IOP callback table */
    if (trace) dbghex("pdr:     status ", r->status);
    p->callback_ptr -= 8;
    cb = (u32 *)p->callback_ptr;
    pdr_call1((void *)cb[0], p);
}

u32 __cdecl pdr_control(u32 msg, u32 ebx, u32 edx)
{
    (void)ebx; (void)edx;
    /* DDB_Reference_Data: the DRP, as the DDK port drivers have it; the I/O Supervisor reads
       its logical group number when it loads the driver */
    WINPDR_DDB[11] = (u32)&drp;
    switch (msg) {
    case 0x1B:                  /* Sys_Dynamic_Device_Init */
        /* IOS loads the driver again for every devnode (and again and again while the
           AEP_INITIALIZE of a devnode succeeds): one registration serves them all */
        if (regstate == STATE_REG) return 0;
        regstate = STATE_REG;
        memzero(bss_begin, (u32)(bss_end - bss_begin));
        memzero(&ilb, sizeof(ilb));
        drp.aer = (u32)thk_aer;
        drp.ilb = (u32)&ilb;
        drp.reg_result = 0;
        ios_register(&drp);
        if (drp.reg_result != DRP_REMAIN_RESIDENT) {
            dbg("pdr: IOS refused the registration\n");
            return 1;
        }
        dbg("pdr: registered with IOS\n");
        return 0;
    case 0x1C:                  /* Sys_Dynamic_Device_Exit */
        return 0;
    }
    return 0;
}
