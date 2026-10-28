/* winusbw.c - WINUSBW.SYS: the WDM bus driver of WINUSB's USB WDM bridge (Windows 98 / ME).
   It sits on the devnode WINUSB.VXD creates for it (WINUSB\WDMBUS) and reports the USB devices
   and interfaces WINDRV has no class driver for as its children, with the ids Windows' own USB
   hub driver would give them (USB\VID_xxxx&PID_xxxx, USB\Class_xx...). Their Windows drivers
   load on these physical device objects and send their USB requests (URBs) here; WINUSB.VXD
   carries them out on its host controller drivers (inc/wdmbr.h, core/usbwdm.c). Isochronous
   transfers are not supported. */
#include <wdm.h>
#include <usb.h>
#include <usbioctl.h>
#include "../inc/wdmbr.h"

/* WINUSB.VXD (wbsvc.asm) */
unsigned long __cdecl wb_present(void);
unsigned long __cdecl wb_version(void);
unsigned long __cdecl wb_attach(struct wbops *o);
int __cdecl wb_list(struct wbfunc *out, int max);
int __cdecl wb_desc(unsigned long h, int type, int index, void *buf, int len);
void __cdecl wb_queue(struct wbreq *r);
void __cdecl wb_cancel(struct wbreq *r);
unsigned long __cdecl wb_frame(unsigned long h);
void __cdecl wb_log(const char *line);
unsigned long __cdecl irq_off(void);
void __cdecl irq_on(unsigned long f);

#define TAG         'DBSU'
#define MAXPDO      32
#define EXT_FDO     0x4F444657UL        /* 'WFDO' */
#define EXT_PDO     0x4F445057UL        /* 'WPDO' */

struct ext {
    ULONG kind;
    PDEVICE_OBJECT self;
    /* FDO */
    PDEVICE_OBJECT lower, pdo;
    int attached;
    /* PDO */
    struct ext *bus;
    struct wbfunc fn;
    int present;                        /* still reported to the PnP manager */
    int started;
    ULONG devpower;
    PIRP parked;                        /* requests for a device that is gone (DriverContext[1] links) */
};

static PDEVICE_OBJECT pdos[MAXPDO];
static struct ext *busx;
static KDPC done_dpc, chg_dpc;
static struct wbreq *doneq;             /* finished requests, completed in done_dpc */
static ULONG pdo_count;
static struct wbops ops;
static int dbg;                         /* WINUSB /D: the requests go to its log */
static ULONG nlog;

/* ---- small helpers (no C library) ---- */
static void mset(void *d, int c, ULONG n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
}

static void mcpy(void *d, const void *s, ULONG n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
}

static ULONG slen(const char *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

static char *hex4(char *o, unsigned v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    for (i = 3; i >= 0; i--) *o++ = hx[(v >> (i * 4)) & 15];
    return o;
}

static char *hex2(char *o, unsigned v)
{
    static const char hx[] = "0123456789ABCDEF";
    *o++ = hx[(v >> 4) & 15];
    *o++ = hx[v & 15];
    return o;
}

static char *put(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    return o;
}

static char *hex8(char *o, ULONG v)
{
    o = hex4(o, (unsigned)(v >> 16));
    return hex4(o, (unsigned)(v & 0xFFFF));
}

/* "<text> <a> <b>" into WINUSB's log (with /D) */
static void logx(const char *t, ULONG a, ULONG b)
{
    char line[80], *o = line;
    if (!dbg || nlog > 400) return;
    nlog++;
    o = put(o, t);
    *o++ = ' ';
    o = hex8(o, a);
    *o++ = ' ';
    o = hex8(o, b);
    *o = 0;
    wb_log(line);
}

/* ASCII multi-string (strings separated by NUL, a second NUL at the end) -> wide, PagedPool */
static PWCHAR wide(const char *s, ULONG n)
{
    PWCHAR w = ExAllocatePoolWithTag(PagedPool, (n + 1) * sizeof(WCHAR), TAG);
    ULONG i;
    if (!w) return NULL;
    for (i = 0; i < n; i++) w[i] = (WCHAR)(unsigned char)s[i];
    w[n] = 0;
    return w;
}

/* ---- ids of a child ---- */
static char *id_base(char *o, struct wbfunc *f)
{
    o = put(o, "USB\\VID_");
    o = hex4(o, f->vid);
    o = put(o, "&PID_");
    o = hex4(o, f->pid);
    return o;
}

static char *id_mi(char *o, struct wbfunc *f)
{
    if (f->iface != WBF_WHOLE) {
        o = put(o, "&MI_");
        o = hex2(o, f->iface);
    }
    return o;
}

static PWCHAR query_id(struct ext *x, BUS_QUERY_ID_TYPE t)
{
    char b[256], *o = b;
    struct wbfunc *f = &x->fn;
    switch (t) {
    case BusQueryDeviceID:
        o = id_mi(id_base(o, f), f);
        *o++ = 0;
        break;
    case BusQueryHardwareIDs:
        o = id_base(o, f);
        o = put(o, "&REV_");
        o = hex4(o, f->bcd);
        o = id_mi(o, f);
        *o++ = 0;
        o = id_mi(id_base(o, f), f);
        *o++ = 0;
        *o++ = 0;
        break;
    case BusQueryCompatibleIDs:
        o = put(o, "USB\\Class_");
        o = hex2(o, f->cls);
        o = put(o, "&SubClass_");
        o = hex2(o, f->sub);
        o = put(o, "&Prot_");
        o = hex2(o, f->proto);
        *o++ = 0;
        o = put(o, "USB\\Class_");
        o = hex2(o, f->cls);
        o = put(o, "&SubClass_");
        o = hex2(o, f->sub);
        *o++ = 0;
        o = put(o, "USB\\Class_");
        o = hex2(o, f->cls);
        *o++ = 0;
        *o++ = 0;
        break;
    case (BUS_QUERY_ID_TYPE)3:                 /* BusQueryInstanceID */
        o = put(o, f->inst);
        *o++ = 0;
        break;
    default:
        return NULL;
    }
    return wide(b, (ULONG)(o - b));
}

/* ---- request completion ---- */
static NTSTATUS nt_status(long s)
{
    switch (s) {
    case WBS_OK: return STATUS_SUCCESS;
    case WBS_CANCEL: return STATUS_CANCELLED;
    case WBS_GONE: return STATUS_DEVICE_NOT_CONNECTED;
    case WBS_BADPARAM: return STATUS_INVALID_PARAMETER;
    case WBS_NOMEM: return STATUS_INSUFFICIENT_RESOURCES;
    case WBS_TIMEOUT: return STATUS_IO_TIMEOUT;
    }
    return STATUS_UNSUCCESSFUL;
}

static USBD_STATUS usbd_status(long s)
{
    switch (s) {
    case WBS_OK: return USBD_STATUS_SUCCESS;
    case WBS_STALL: return USBD_STATUS_STALL_PID;
    case WBS_CANCEL: return USBD_STATUS_CANCELED;
    case WBS_GONE: return USBD_STATUS_DEVICE_GONE;
    case WBS_BABBLE: return USBD_STATUS_BABBLE_DETECTED;
    case WBS_SHORT: return USBD_STATUS_ERROR_SHORT_TRANSFER;
    case WBS_BADPARAM: return USBD_STATUS_INVALID_PARAMETER;
    case WBS_NOMEM: return USBD_STATUS_INSUFFICIENT_RESOURCES;
    case WBS_TIMEOUT: return USBD_STATUS_TIMEOUT;
    }
    return USBD_STATUS_DEV_NOT_RESPONDING;
}

/* the results of a select request into the URB's interface descriptions */
static void sel_out(PURB u, struct wbsel *s, int iface_only)
{
    PUSBD_INTERFACE_INFORMATION ii = iface_only ? &u->UrbSelectInterface.Interface : &u->UrbSelectConfiguration.Interface;
    int i, k;
    if (!iface_only) u->UrbSelectConfiguration.ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)0xC0F16;
    for (i = 0; i < s->nif; i++) {
        struct wbif *w = &s->ifc[i];
        ULONG room = ii->Length > sizeof(*ii) - sizeof(ii->Pipes) ?
                     (ii->Length - (sizeof(*ii) - sizeof(ii->Pipes))) / sizeof(USBD_PIPE_INFORMATION) : 0;
        ii->Class = w->cls;
        ii->SubClass = w->sub;
        ii->Protocol = w->proto;
        ii->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG)(0x1F000 | (w->ifnum << 8) | w->alt);
        ii->NumberOfPipes = w->npipes;
        for (k = 0; k < w->npipes && (ULONG)k < room; k++) {
            PUSBD_PIPE_INFORMATION p = &ii->Pipes[k];
            p->MaximumPacketSize = w->pipes[k].maxpkt;
            p->EndpointAddress = w->pipes[k].ep;
            p->Interval = w->pipes[k].interval;
            p->PipeType = (USBD_PIPE_TYPE)w->pipes[k].type;
            p->PipeHandle = (USBD_PIPE_HANDLE)w->pipes[k].handle;
            if (!p->MaximumTransferSize || p->MaximumTransferSize > 0x400000UL) p->MaximumTransferSize = 0x10000UL;
        }
        ii = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)ii + ii->Length);
    }
}

static void complete(PIRP irp, struct wbreq *r)
{
    PURB u = URB_FROM_IRP(irp);
    NTSTATUS st = nt_status(r->status);
    logx("urb done", u->UrbHeader.Function, (ULONG)r->status);
    switch (u->UrbHeader.Function) {
    case URB_FUNCTION_SELECT_CONFIGURATION:
        if (r->status == WBS_OK && r->op == WBR_SELCFG) sel_out(u, (struct wbsel *)r->buf, 0);
        break;
    case URB_FUNCTION_SELECT_INTERFACE:
        if (r->status == WBS_OK) sel_out(u, (struct wbsel *)r->buf, 1);
        break;
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SYNC_RESET_PIPE:
    case URB_FUNCTION_SYNC_CLEAR_STALL:
        break;
    default:
        /* transfers: all share the layout of the control transfer */
        u->UrbControlTransfer.TransferBufferLength = r->actual;
        if (r->status == WBS_SHORT) st = STATUS_UNSUCCESSFUL;
        break;
    }
    u->UrbHeader.Status = usbd_status(r->status);
    if (r->op == WBR_SELCFG || r->op == WBR_SELIF) ExFreePool(r->buf);
    ExFreePool(r);
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

/* Transfers for a device that is gone wait here until their driver cancels them or the PDO is
   removed: completed at once, a class driver such as HIDCLASS sends its reads again right away
   and the loop would starve the worker thread that tells the PnP manager */
static VOID __stdcall parked_cancel(PDEVICE_OBJECT dev, PIRP irp)
{
    struct ext *x = dev->DeviceExtension;
    PIRP *pp;
    unsigned long f;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    f = irq_off();
    for (pp = &x->parked; *pp; pp = (PIRP *)&(*pp)->Tail.Overlay.DriverContext[1])
        if (*pp == irp) { *pp = irp->Tail.Overlay.DriverContext[1]; break; }
    irq_on(f);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static void park(struct ext *x, PIRP irp)
{
    unsigned long f;
    PURB u = URB_FROM_IRP(irp);
    u->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
    IoSetCancelRoutine(irp, parked_cancel);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return;
    }
    f = irq_off();
    irp->Tail.Overlay.DriverContext[1] = x->parked;
    x->parked = irp;
    irq_on(f);
}

static void unpark_all(struct ext *x)
{
    PIRP irp;
    unsigned long f;
    for (;;) {
        f = irq_off();
        irp = x->parked;
        if (irp) x->parked = irp->Tail.Overlay.DriverContext[1];
        irq_on(f);
        if (!irp) break;
        if (!IoSetCancelRoutine(irp, NULL)) continue;      /* its cancel routine has it */
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static VOID __stdcall done_dpc_fn(PKDPC d, PVOID c, PVOID a, PVOID b)
{
    struct wbreq *q, *r;
    unsigned long f;
    (void)d; (void)c; (void)a; (void)b;
    f = irq_off();
    q = doneq;
    doneq = NULL;
    irq_on(f);
    /* oldest first */
    {
        struct wbreq *rev = NULL;
        while (q) { r = q; q = q->next; r->next = rev; rev = r; }
        q = rev;
    }
    while (q) {
        PIRP irp;
        r = q;
        q = q->next;
        irp = r->ctx;
        if (!irp) {
            if (r->op == WBR_SELCFG || r->op == WBR_SELIF) ExFreePool(r->buf);
            ExFreePool(r);
            continue;
        }
        IoSetCancelRoutine(irp, NULL);
        if (r->status == WBS_GONE && !irp->Cancel &&
            ((PURB)URB_FROM_IRP(irp))->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
            struct ext *x = IoGetCurrentIrpStackLocation(irp)->DeviceObject->DeviceExtension;
            if (r->op == WBR_SELCFG || r->op == WBR_SELIF) ExFreePool(r->buf);
            ExFreePool(r);
            park(x, irp);
            continue;
        }
        complete(irp, r);
    }
}

/* WINUSB.VXD: a request finished (event time) */
static void __stdcall vxd_done(void *ctx, struct wbreq *r)
{
    unsigned long f;
    (void)ctx;
    f = irq_off();
    r->next = doneq;
    doneq = r;
    irq_on(f);
    KeInsertQueueDpc(&done_dpc, NULL, NULL);
}

/* the PnP manager asked to look at the children again: from a worker thread (Windows 98's
   NTKERN does not act on it at DPC time) */
static WORK_QUEUE_ITEM chg_wi;
static volatile LONG chg_queued;

static VOID __stdcall chg_work(PVOID c)
{
    (void)c;
    chg_queued = 0;
    logx("bus changed", 0, 0);
    if (busx && busx->attached) IoInvalidateDeviceRelations(busx->pdo, BusRelations);
}

static VOID __stdcall chg_dpc_fn(PKDPC d, PVOID c, PVOID a, PVOID b)
{
    (void)d; (void)c; (void)a; (void)b;
    logx("bus dpc", (ULONG)chg_queued, 0);
    if (chg_queued) return;
    chg_queued = 1;
    ExQueueWorkItem(&chg_wi, CriticalWorkQueue);
}

/* WINUSB.VXD: functions came or went */
static void __stdcall vxd_changed(void *ctx)
{
    BOOLEAN q;
    (void)ctx;
    q = KeInsertQueueDpc(&chg_dpc, NULL, NULL);
    logx("bus kick", (ULONG)q, 0);
}

static VOID __stdcall cancel_fn(PDEVICE_OBJECT dev, PIRP irp)
{
    struct wbreq *r = irp->Tail.Overlay.DriverContext[0];
    (void)dev;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    if (r) wb_cancel(r);
}

/* ---- URBs ---- */
static void *xfer_buf(PURB u)
{
    struct _URB_CONTROL_TRANSFER *t = &u->UrbControlTransfer;
    if (t->TransferBuffer) return t->TransferBuffer;
    if (t->TransferBufferMDL) return MmGetSystemAddressForMdl(t->TransferBufferMDL);
    return NULL;
}

static void setup(struct wbreq *r, int rt, int req, unsigned val, unsigned idx, ULONG len)
{
    r->setup[0] = (unsigned char)rt;
    r->setup[1] = (unsigned char)req;
    r->setup[2] = (unsigned char)val;
    r->setup[3] = (unsigned char)(val >> 8);
    r->setup[4] = (unsigned char)idx;
    r->setup[5] = (unsigned char)(idx >> 8);
    r->setup[6] = (unsigned char)len;
    r->setup[7] = (unsigned char)(len >> 8);
}

static NTSTATUS urb_fail(PIRP irp, PURB u, USBD_STATUS us, NTSTATUS st)
{
    u->UrbHeader.Status = us;
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS urb_ok(PIRP irp, PURB u)
{
    return urb_fail(irp, u, USBD_STATUS_SUCCESS, STATUS_SUCCESS);
}

/* the interfaces of a select request into a wbsel */
static struct wbsel *sel_in(PURB u, int iface_only)
{
    struct wbsel *s = ExAllocatePoolWithTag(NonPagedPool, sizeof(*s), TAG);
    PUSBD_INTERFACE_INFORMATION ii;
    PUCHAR end = (PUCHAR)u + u->UrbHeader.Length;
    int n, max;
    if (!s) return NULL;
    mset(s, 0, sizeof(*s));
    if (iface_only) {
        ii = &u->UrbSelectInterface.Interface;
        max = 1;
    } else {
        ii = &u->UrbSelectConfiguration.Interface;
        s->cfgval = u->UrbSelectConfiguration.ConfigurationDescriptor->bConfigurationValue;
        max = u->UrbSelectConfiguration.ConfigurationDescriptor->bNumInterfaces;
    }
    for (n = 0; n < max && n < WB_MAXIF && (PUCHAR)ii + 8 <= end && ii->Length; n++) {
        s->ifc[n].ifnum = ii->InterfaceNumber;
        s->ifc[n].alt = ii->AlternateSetting;
        ii = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)ii + ii->Length);
    }
    s->nif = (unsigned char)n;
    return s;
}

static NTSTATUS submit_urb(struct ext *x, PIRP irp)
{
    PURB u = URB_FROM_IRP(irp);
    struct _URB_CONTROL_TRANSFER *t = &u->UrbControlTransfer;
    struct wbreq *r;
    unsigned short fn = u->UrbHeader.Function;
    int rt = -1, recip = 0;
    if (!x->present) {
        /* reads and writes wait (see park), everything else fails at once: a driver waiting
           for it in a worker thread would block the thread for good */
        if (fn != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
            return urb_fail(irp, u, USBD_STATUS_DEVICE_GONE, STATUS_DEVICE_NOT_CONNECTED);
        IoMarkIrpPending(irp);
        park(x, irp);
        return STATUS_PENDING;
    }
    if (fn == URB_FUNCTION_GET_CURRENT_FRAME_NUMBER) {
        u->UrbGetCurrentFrameNumber.FrameNumber = wb_frame(x->fn.handle);
        return urb_ok(irp, u);
    }
    if (fn == URB_FUNCTION_ISOCH_TRANSFER || fn == URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL ||
        fn == URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL || fn == URB_FUNCTION_GET_FRAME_LENGTH ||
        fn == URB_FUNCTION_SET_FRAME_LENGTH)
        return urb_fail(irp, u, USBD_STATUS_NOT_SUPPORTED, STATUS_NOT_SUPPORTED);
    r = ExAllocatePoolWithTag(NonPagedPool, sizeof(*r), TAG);
    if (!r) return urb_fail(irp, u, USBD_STATUS_INSUFFICIENT_RESOURCES, STATUS_INSUFFICIENT_RESOURCES);
    mset(r, 0, sizeof(*r));
    r->magic = WBR_MAGIC;
    r->func = x->fn.handle;
    r->ctx = irp;
    r->op = WBR_XFER;
    switch (fn) {
    case URB_FUNCTION_SELECT_CONFIGURATION:
        if (!u->UrbSelectConfiguration.ConfigurationDescriptor) {
            r->op = WBR_UNCONFIG;
            break;
        }
        r->op = WBR_SELCFG;
        r->buf = (unsigned char *)sel_in(u, 0);
        if (!r->buf) { ExFreePool(r); return urb_fail(irp, u, USBD_STATUS_INSUFFICIENT_RESOURCES, STATUS_INSUFFICIENT_RESOURCES); }
        break;
    case URB_FUNCTION_SELECT_INTERFACE:
        r->op = WBR_SELIF;
        r->buf = (unsigned char *)sel_in(u, 1);
        if (!r->buf) { ExFreePool(r); return urb_fail(irp, u, USBD_STATUS_INSUFFICIENT_RESOURCES, STATUS_INSUFFICIENT_RESOURCES); }
        break;
    case URB_FUNCTION_ABORT_PIPE:
        r->op = WBR_ABORTPIPE;
        r->pipe = (unsigned long)u->UrbPipeRequest.PipeHandle;
        break;
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SYNC_RESET_PIPE:
    case URB_FUNCTION_SYNC_CLEAR_STALL:
        r->op = WBR_RESETPIPE;
        r->pipe = (unsigned long)u->UrbPipeRequest.PipeHandle;
        break;
    case URB_FUNCTION_CONTROL_TRANSFER:
        mcpy(r->setup, t->SetupPacket, 8);
        if (!(t->TransferFlags & USBD_DEFAULT_PIPE_TRANSFER)) r->pipe = (unsigned long)t->PipeHandle;
        r->flags = WBRF_SHORTOK;
        break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        r->pipe = (unsigned long)u->UrbBulkOrInterruptTransfer.PipeHandle;
        r->flags = ((u->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) ? WBRF_IN : 0) |
                   ((u->UrbBulkOrInterruptTransfer.TransferFlags & USBD_SHORT_TRANSFER_OK) ? WBRF_SHORTOK : 0);
        if (!r->pipe) { ExFreePool(r); return urb_fail(irp, u, USBD_STATUS_INVALID_PIPE_HANDLE, STATUS_INVALID_PARAMETER); }
        break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE: rt = 0x80; goto desc;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE: rt = 0x81; goto desc;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT: rt = 0x82; goto desc;
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE: rt = 0x00; goto desc;
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE: rt = 0x01; goto desc;
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT: rt = 0x02;
    desc:
    {
        struct _URB_CONTROL_DESCRIPTOR_REQUEST *d = &u->UrbControlDescriptorRequest;
        setup(r, rt, (rt & 0x80) ? 6 : 7, (unsigned)((d->DescriptorType << 8) | d->Index), d->LanguageId,
              d->TransferBufferLength);
        r->flags = WBRF_SHORTOK;
        break;
    }
    case URB_FUNCTION_SET_FEATURE_TO_DEVICE: rt = 0x00; goto feat;
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE: rt = 0x01; goto feat;
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT: rt = 0x02; goto feat;
    case URB_FUNCTION_SET_FEATURE_TO_OTHER: rt = 0x03; goto feat;
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE: rt = 0x100; goto feat;
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE: rt = 0x101; goto feat;
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT: rt = 0x102; goto feat;
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER: rt = 0x103;
    feat:
    {
        struct _URB_CONTROL_FEATURE_REQUEST *q = &u->UrbControlFeatureRequest;
        setup(r, rt & 0xFF, (rt & 0x100) ? 1 : 3, q->FeatureSelector, q->Index, 0);
        break;
    }
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE: rt = 0x80; goto status;
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE: rt = 0x81; goto status;
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT: rt = 0x82; goto status;
    case URB_FUNCTION_GET_STATUS_FROM_OTHER: rt = 0x83;
    status:
        setup(r, rt, 0, 0, u->UrbControlGetStatusRequest.Index, 2);
        r->flags = WBRF_SHORTOK;
        break;
    case URB_FUNCTION_VENDOR_DEVICE: rt = 0x40; recip = 0; goto vc;
    case URB_FUNCTION_VENDOR_INTERFACE: rt = 0x40; recip = 1; goto vc;
    case URB_FUNCTION_VENDOR_ENDPOINT: rt = 0x40; recip = 2; goto vc;
    case URB_FUNCTION_VENDOR_OTHER: rt = 0x40; recip = 3; goto vc;
    case URB_FUNCTION_CLASS_DEVICE: rt = 0x20; recip = 0; goto vc;
    case URB_FUNCTION_CLASS_INTERFACE: rt = 0x20; recip = 1; goto vc;
    case URB_FUNCTION_CLASS_ENDPOINT: rt = 0x20; recip = 2; goto vc;
    case URB_FUNCTION_CLASS_OTHER: rt = 0x20; recip = 3;
    vc:
    {
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *v = &u->UrbControlVendorClassRequest;
        int in = (v->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        setup(r, (in ? 0x80 : 0) | rt | recip | (v->RequestTypeReservedBits & 0x1C), v->Request, v->Value, v->Index,
              v->TransferBufferLength);
        r->flags = WBRF_SHORTOK;
        break;
    }
    case URB_FUNCTION_GET_CONFIGURATION:
        setup(r, 0x80, 8, 0, 0, 1);
        r->flags = WBRF_SHORTOK;
        break;
    case URB_FUNCTION_GET_INTERFACE:
        setup(r, 0x81, 0x0A, 0, u->UrbControlGetInterfaceRequest.Interface, 1);
        r->flags = WBRF_SHORTOK;
        break;
    default:
        ExFreePool(r);
        return urb_fail(irp, u, USBD_STATUS_INVALID_URB_FUNCTION, STATUS_INVALID_PARAMETER);
    }
    if (r->op == WBR_XFER) {
        r->buf = xfer_buf(u);
        r->len = t->TransferBufferLength;
        if (r->len && !r->buf) {
            ExFreePool(r);
            return urb_fail(irp, u, USBD_STATUS_INVALID_PARAMETER, STATUS_INVALID_PARAMETER);
        }
        if (!r->pipe && r->len > (ULONG)(r->setup[6] | (r->setup[7] << 8))) r->len = r->setup[6] | (r->setup[7] << 8);
    }
    u->UrbHeader.Status = USBD_STATUS_PENDING;
    irp->Tail.Overlay.DriverContext[0] = r;
    IoMarkIrpPending(irp);
    IoSetCancelRoutine(irp, cancel_fn);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        /* cancelled before it got here: it still goes through the VxD (one completion path) */
        r->cancel = 1;
    }
    wb_queue(r);
    return STATUS_PENDING;
}

static NTSTATUS pdo_internal(struct ext *x, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_SUCCESS;
    logx("pdo ioctl", s->Parameters.DeviceIoControl.IoControlCode,
         s->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB ?
         ((PURB)s->Parameters.Others.Argument1)->UrbHeader.Function : 0);
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:
        return submit_urb(x, irp);
    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
        *(PULONG)s->Parameters.Others.Argument1 = x->present ? (USBD_PORT_ENABLED | USBD_PORT_CONNECTED) : 0;
        break;
    case IOCTL_INTERNAL_USB_RESET_PORT:
    case IOCTL_INTERNAL_USB_ENABLE_PORT:
    case IOCTL_INTERNAL_USB_CYCLE_PORT:
        st = x->present ? STATUS_SUCCESS : STATUS_DEVICE_NOT_CONNECTED;
        break;
    case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
        if (s->Parameters.Others.Argument1) *(PDEVICE_OBJECT *)s->Parameters.Others.Argument1 = x->self;
        if (s->Parameters.Others.Argument2) *(PDEVICE_OBJECT *)s->Parameters.Others.Argument2 = x->self;
        break;
    case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
        if (s->Parameters.Others.Argument1) (*(PULONG)s->Parameters.Others.Argument1)++;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
    }
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ---- children ---- */
static PDEVICE_OBJECT new_pdo(struct ext *bus, struct wbfunc *f)
{
    PDEVICE_OBJECT d;
    struct ext *x;
    WCHAR name[32];
    UNICODE_STRING us;
    static const char pre[] = "\\Device\\WinUsbD-";
    ULONG n = pdo_count++, i, k = 0;
    char num[12];
    int j = 0;
    for (i = 0; pre[i]; i++) name[k++] = (WCHAR)pre[i];
    do { num[j++] = (char)('0' + n % 10); n /= 10; } while (n && j < 10);
    while (j) name[k++] = (WCHAR)num[--j];
    name[k] = 0;
    us.Buffer = name;
    us.Length = (USHORT)(k * sizeof(WCHAR));
    us.MaximumLength = (USHORT)(us.Length + sizeof(WCHAR));
    if (IoCreateDevice(bus->self->DriverObject, sizeof(struct ext), &us, FILE_DEVICE_UNKNOWN, 0, FALSE, &d) != STATUS_SUCCESS)
        return NULL;
    x = d->DeviceExtension;
    mset(x, 0, sizeof(*x));
    x->kind = EXT_PDO;
    x->self = d;
    x->bus = bus;
    mcpy(&x->fn, f, sizeof(*f));
    x->present = 1;
    x->devpower = PowerDeviceD0;
    d->Flags |= DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    return d;
}

static NTSTATUS bus_relations(struct ext *bus, PIRP irp)
{
    static struct wbfunc list[MAXPDO];
    int n = wb_list(list, MAXPDO), i, k, cnt = 0;
    PDEVICE_RELATIONS rel, old = (PDEVICE_RELATIONS)irp->IoStatus.Information;
    /* the PDOs against the functions the VxD has now */
    for (k = 0; k < MAXPDO; k++) {
        struct ext *x;
        if (!pdos[k]) continue;
        x = pdos[k]->DeviceExtension;
        x->present = 0;
        for (i = 0; i < n; i++)
            if (list[i].handle == x->fn.handle) { x->present = 1; list[i].handle = 0; break; }
    }
    for (i = 0; i < n; i++) {
        if (!list[i].handle) continue;
        for (k = 0; k < MAXPDO && pdos[k]; k++) ;
        if (k == MAXPDO) break;
        pdos[k] = new_pdo(bus, &list[i]);
    }
    for (k = 0; k < MAXPDO; k++)
        if (pdos[k] && ((struct ext *)pdos[k]->DeviceExtension)->present) cnt++;
    if (old) cnt += old->Count;
    rel = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + cnt * sizeof(PDEVICE_OBJECT), TAG);
    if (!rel) return STATUS_INSUFFICIENT_RESOURCES;
    rel->Count = 0;
    if (old) {
        for (i = 0; i < (int)old->Count; i++) rel->Objects[rel->Count++] = old->Objects[i];
        ExFreePool(old);
    }
    for (k = 0; k < MAXPDO; k++) {
        if (!pdos[k] || !((struct ext *)pdos[k]->DeviceExtension)->present) continue;
        ObReferenceObject(pdos[k]);
        rel->Objects[rel->Count++] = pdos[k];
    }
    irp->IoStatus.Information = (ULONG_PTR)rel;
    return STATUS_SUCCESS;
}

static void pdo_delete(struct ext *x)
{
    int k;
    for (k = 0; k < MAXPDO; k++)
        if (pdos[k] == x->self) pdos[k] = NULL;
    IoDeleteDevice(x->self);
}

static NTSTATUS pdo_pnp(struct ext *x, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = irp->IoStatus.Status;
    logx("pdo pnp", s->MinorFunction, x->fn.handle);
    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        x->started = 1;
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_SURPRISE_REMOVAL:
        unpark_all(x);
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        x->started = 0;
        unpark_all(x);
        if (x->present) {
            /* driver unloaded or device disabled: its pipes are closed, the PDO stays */
            struct wbreq *r = ExAllocatePoolWithTag(NonPagedPool, sizeof(*r), TAG);
            if (r) {
                mset(r, 0, sizeof(*r));
                r->magic = WBR_MAGIC;
                r->op = WBR_UNCONFIG;
                r->func = x->fn.handle;
                wb_queue(r);            /* no IRP: freed when it is done */
            }
            irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        pdo_delete(x);
        return STATUS_SUCCESS;
    case IRP_MN_QUERY_ID:
    {
        PWCHAR w = query_id(x, s->Parameters.QueryId.IdType);
        if (w) {
            irp->IoStatus.Information = (ULONG_PTR)w;
            st = STATUS_SUCCESS;
        }
        break;
    }
    case IRP_MN_QUERY_DEVICE_TEXT:
        if (s->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription) {
            char b[96], *o = b;
            if (x->fn.product[0]) o = put(o, x->fn.product);
            else o = put(o, "USB Device");
            *o = 0;
            irp->IoStatus.Information = (ULONG_PTR)wide(b, slen(b));
            st = irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        } else if (s->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation) {
            irp->IoStatus.Information = (ULONG_PTR)wide(x->fn.inst, slen(x->fn.inst));
            st = irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        }
        break;
    case IRP_MN_QUERY_CAPABILITIES:
    {
        PDEVICE_CAPABILITIES c = s->Parameters.DeviceCapabilities.Capabilities;
        c->Removable = 1;
        c->SurpriseRemovalOK = 1;
        c->UniqueID = 0;
        c->Address = x->fn.port;
        c->UINumber = x->fn.port;
        c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        c->DeviceState[PowerSystemSleeping1] = PowerDeviceD3;
        c->DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
        c->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
        c->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
        c->DeviceState[PowerSystemShutdown] = PowerDeviceD3;
        c->SystemWake = PowerSystemUnspecified;
        c->DeviceWake = PowerDeviceUnspecified;
        st = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS rel = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), TAG);
            if (rel) {
                rel->Count = 1;
                rel->Objects[0] = x->self;
                ObReferenceObject(x->self);
                irp->IoStatus.Information = (ULONG_PTR)rel;
                st = STATUS_SUCCESS;
            } else {
                st = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION:
    {
        PPNP_BUS_INFORMATION b = ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION), TAG);
        static const GUID usbguid = { 0x9d7debbc, 0xc85d, 0x11d1, { 0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a } };
        if (b) {
            mcpy(&b->BusTypeGuid, &usbguid, sizeof(GUID));
            b->LegacyBusType = PNPBus;
            b->BusNumber = 0;
            irp->IoStatus.Information = (ULONG_PTR)b;
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_INSUFFICIENT_RESOURCES;
        }
        break;
    }
    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        st = STATUS_SUCCESS;
        break;
    default:
        break;
    }
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS pdo_power(struct ext *x, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = irp->IoStatus.Status;
    logx("pdo power", s->MinorFunction, s->Parameters.Power.State.DeviceState);
    switch (s->MinorFunction) {
    case IRP_MN_SET_POWER:
        if (s->Parameters.Power.Type == DevicePowerState) {
            x->devpower = s->Parameters.Power.State.DeviceState;
            PoSetPowerState(x->self, DevicePowerState, s->Parameters.Power.State);
        }
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_POWER:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_WAIT_WAKE:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    PoStartNextPowerIrp(irp);
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ---- the bus FDO ---- */
static NTSTATUS __stdcall sync_done(PDEVICE_OBJECT d, PIRP irp, PVOID ev)
{
    (void)d; (void)irp;
    KeSetEvent((PKEVENT)ev, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS call_lower_sync(struct ext *x, PIRP irp)
{
    KEVENT ev;
    NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, sync_done, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(x->lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

static NTSTATUS pass_down(struct ext *x, PIRP irp)
{
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(x->lower, irp);
}

static NTSTATUS fdo_pnp(struct ext *x, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;
    int k;
    logx("bus pnp", s->MinorFunction, s->MinorFunction == IRP_MN_QUERY_DEVICE_RELATIONS ?
         s->Parameters.QueryDeviceRelations.Type : 0);
    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = call_lower_sync(x, irp);
        if (NT_SUCCESS(st)) {
            if (wb_present()) dbg = (wb_version() & 0x10000UL) != 0;
            if (!wb_present() || !(wb_version() & 0x20000UL)) {
                DbgPrint("WINUSBW: WINUSB.VXD without the WDM bridge\n");
                st = STATUS_DEVICE_CONFIGURATION_ERROR;
            } else {
                ops.size = sizeof(ops);
                ops.done = vxd_done;
                ops.changed = vxd_changed;
                ops.ctx = x;
                if (wb_attach(&ops)) x->attached = 1;
                else st = STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
        irp->IoStatus.Status = st;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == BusRelations && x->attached) {
            st = bus_relations(x, irp);
            irp->IoStatus.Status = st;
            if (!NT_SUCCESS(st)) {
                IoCompleteRequest(irp, IO_NO_INCREMENT);
                return st;
            }
        }
        return pass_down(x, irp);
    case IRP_MN_REMOVE_DEVICE:
        if (x->attached) wb_attach(NULL);
        x->attached = 0;
        for (k = 0; k < MAXPDO; k++)
            if (pdos[k]) {
                IoDeleteDevice(pdos[k]);
                pdos[k] = NULL;
            }
        irp->IoStatus.Status = STATUS_SUCCESS;
        st = pass_down(x, irp);
        IoDetachDevice(x->lower);
        busx = NULL;
        IoDeleteDevice(x->self);
        return st;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        irp->IoStatus.Status = STATUS_SUCCESS;
        return pass_down(x, irp);
    }
    return pass_down(x, irp);
}

static NTSTATUS __stdcall dispatch_pnp(PDEVICE_OBJECT d, PIRP irp)
{
    struct ext *x = d->DeviceExtension;
    return x->kind == EXT_FDO ? fdo_pnp(x, irp) : pdo_pnp(x, irp);
}

static NTSTATUS __stdcall dispatch_power(PDEVICE_OBJECT d, PIRP irp)
{
    struct ext *x = d->DeviceExtension;
    if (x->kind == EXT_PDO) return pdo_power(x, irp);
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(x->lower, irp);
}

static NTSTATUS __stdcall dispatch_internal(PDEVICE_OBJECT d, PIRP irp)
{
    struct ext *x = d->DeviceExtension;
    NTSTATUS st;
    if (x->kind == EXT_PDO) return pdo_internal(x, irp);
    st = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS __stdcall dispatch_other(PDEVICE_OBJECT d, PIRP irp)
{
    struct ext *x = d->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;
    if (x->kind == EXT_FDO && s->MajorFunction == IRP_MJ_SYSTEM_CONTROL) return pass_down(x, irp);
    st = (s->MajorFunction == IRP_MJ_CREATE || s->MajorFunction == IRP_MJ_CLOSE) ? STATUS_SUCCESS :
         (s->MajorFunction == IRP_MJ_SYSTEM_CONTROL ? irp->IoStatus.Status : STATUS_INVALID_DEVICE_REQUEST);
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS __stdcall add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT d;
    struct ext *x;
    NTSTATUS st;
    if (busx) return STATUS_DEVICE_CONFIGURATION_ERROR;    /* one bridge */
    st = IoCreateDevice(drv, sizeof(struct ext), NULL, FILE_DEVICE_BUS_EXTENDER, 0, FALSE, &d);
    if (!NT_SUCCESS(st)) return st;
    x = d->DeviceExtension;
    mset(x, 0, sizeof(*x));
    x->kind = EXT_FDO;
    x->self = d;
    x->pdo = pdo;
    x->lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!x->lower) {
        IoDeleteDevice(d);
        return STATUS_NO_SUCH_DEVICE;
    }
    busx = x;
    d->Flags |= DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID __stdcall unload(PDRIVER_OBJECT drv)
{
    (void)drv;
}

NTSTATUS __stdcall DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    int i;
    (void)path;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = dispatch_other;
    drv->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    drv->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = dispatch_internal;
    drv->DriverExtension->AddDevice = add_device;
    drv->DriverUnload = unload;
    KeInitializeDpc(&done_dpc, done_dpc_fn, NULL);
    KeInitializeDpc(&chg_dpc, chg_dpc_fn, NULL);
    ExInitializeWorkItem(&chg_wi, chg_work, NULL);
    return STATUS_SUCCESS;
}
