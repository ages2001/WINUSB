/* usbd.c - USBD.SYS of the USB WDM bridge (Windows 98 / ME): the USB descriptor helpers that
   Windows drivers of USB devices import from USBD.SYS (USBD_CreateConfigurationRequestEx,
   USBD_ParseConfigurationDescriptorEx ...). Windows' own USBD.SYS comes with its USB host
   controller drivers; on a machine whose controllers have only ever had WINUSB it is not
   installed, and drivers such as HIDUSB.SYS would not load. The installer and WINUSB.INF put
   this one in place only when there is no USBD.SYS yet; the host controller part of
   Windows' USBD.SYS is not here. */
#include <wdm.h>
#include <usb.h>

unsigned long __cdecl wb_present(void);
unsigned long __cdecl wb_frame(unsigned long h);

typedef struct _USBD_INTERFACE_LIST_ENTRY {
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PUSBD_INTERFACE_INFORMATION Interface;
} USBD_INTERFACE_LIST_ENTRY, *PUSBD_INTERFACE_LIST_ENTRY;

#define IFSIZE(n) (sizeof(USBD_INTERFACE_INFORMATION) + ((n) ? (n) - 1 : 0) * sizeof(USBD_PIPE_INFORMATION) - \
                   ((n) ? 0 : sizeof(USBD_PIPE_INFORMATION)))

static void mset(void *d, int c, ULONG n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
}

PUSB_COMMON_DESCRIPTOR __stdcall USBD_ParseDescriptors(PVOID buf, ULONG len, PVOID start, LONG type)
{
    PUCHAR p = start, end = (PUCHAR)buf + len;
    while (p + 2 <= end && p[0]) {
        if (type == -1 || p[1] == (UCHAR)type) return (PUSB_COMMON_DESCRIPTOR)p;
        p += p[0];
    }
    return NULL;
}

PUSB_INTERFACE_DESCRIPTOR __stdcall USBD_ParseConfigurationDescriptorEx(PUSB_CONFIGURATION_DESCRIPTOR cfg, PVOID start,
                                                                        LONG ifnum, LONG alt, LONG cls, LONG sub,
                                                                        LONG proto)
{
    PUCHAR p = start, end = (PUCHAR)cfg + cfg->wTotalLength;
    while (p + 9 <= end && p[0]) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE && (ifnum == -1 || p[2] == (UCHAR)ifnum) &&
            (alt == -1 || p[3] == (UCHAR)alt) && (cls == -1 || p[5] == (UCHAR)cls) &&
            (sub == -1 || p[6] == (UCHAR)sub) && (proto == -1 || p[7] == (UCHAR)proto))
            return (PUSB_INTERFACE_DESCRIPTOR)p;
        p += p[0];
    }
    return NULL;
}

PUSB_INTERFACE_DESCRIPTOR __stdcall USBD_ParseConfigurationDescriptor(PUSB_CONFIGURATION_DESCRIPTOR cfg,
                                                                      UCHAR ifnum, UCHAR alt)
{
    return USBD_ParseConfigurationDescriptorEx(cfg, cfg, ifnum, alt, -1, -1, -1);
}

ULONG __stdcall USBD_GetInterfaceLength(PUSB_INTERFACE_DESCRIPTOR ifd, PUCHAR end)
{
    PUCHAR p = (PUCHAR)ifd;
    ULONG n = p[0];
    p += p[0];
    while (p + 2 <= end && p[0] && p[1] != USB_INTERFACE_DESCRIPTOR_TYPE) {
        n += p[0];
        p += p[0];
    }
    return n;
}

PURB __stdcall USBD_CreateConfigurationRequestEx(PUSB_CONFIGURATION_DESCRIPTOR cfg, PUSBD_INTERFACE_LIST_ENTRY list)
{
    ULONG size = sizeof(struct _URB_SELECT_CONFIGURATION) - sizeof(USBD_INTERFACE_INFORMATION), n;
    PUSBD_INTERFACE_LIST_ENTRY e;
    PURB u;
    PUCHAR o;
    for (e = list; e->InterfaceDescriptor; e++) size += IFSIZE(e->InterfaceDescriptor->bNumEndpoints);
    if (size < sizeof(struct _URB_SELECT_CONFIGURATION)) size = sizeof(struct _URB_SELECT_CONFIGURATION);
    u = ExAllocatePoolWithTag(NonPagedPool, size, 'DBSU');
    if (!u) return NULL;
    mset(u, 0, size);
    u->UrbHeader.Length = (USHORT)size;
    u->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    u->UrbSelectConfiguration.ConfigurationDescriptor = cfg;
    o = (PUCHAR)&u->UrbSelectConfiguration.Interface;
    for (e = list; e->InterfaceDescriptor; e++) {
        PUSBD_INTERFACE_INFORMATION ii = (PUSBD_INTERFACE_INFORMATION)o;
        ULONG k;
        n = e->InterfaceDescriptor->bNumEndpoints;
        ii->Length = (USHORT)IFSIZE(n);
        ii->InterfaceNumber = e->InterfaceDescriptor->bInterfaceNumber;
        ii->AlternateSetting = e->InterfaceDescriptor->bAlternateSetting;
        ii->NumberOfPipes = n;
        for (k = 0; k < n; k++) ii->Pipes[k].MaximumTransferSize = PAGE_SIZE;
        e->Interface = ii;
        o += ii->Length;
    }
    return u;
}

/* the old form: every interface (alternate setting 0) of the configuration */
PURB __stdcall USBD_CreateConfigurationRequest(PUSB_CONFIGURATION_DESCRIPTOR cfg, PUSHORT size)
{
    USBD_INTERFACE_LIST_ENTRY l[33];
    PURB u;
    int n = 0;
    PUCHAR from = (PUCHAR)cfg + cfg->bLength;
    PUSB_INTERFACE_DESCRIPTOR ifd;
    while (n < 32 && (ifd = USBD_ParseConfigurationDescriptorEx(cfg, from, -1, 0, -1, -1, -1)) != NULL) {
        l[n].InterfaceDescriptor = ifd;
        l[n].Interface = NULL;
        n++;
        from = (PUCHAR)ifd + ifd->bLength;
    }
    l[n].InterfaceDescriptor = NULL;
    u = USBD_CreateConfigurationRequestEx(cfg, l);
    if (size) *size = u ? u->UrbHeader.Length : 0;
    return u;
}

VOID __stdcall USBD_GetUSBDIVersion(PUSBD_VERSION_INFORMATION v)
{
    if (!v) return;
    v->USBDI_Version = 0x00000200;
    v->Supported_USB_Version = 0x0200;
}

/* bits per frame of one transaction (with bit stuffing), as Windows' USBD computes it */
ULONG __stdcall USBD_CalculateUsbBandwidth(ULONG maxpkt, UCHAR type, BOOLEAN lowspeed)
{
    ULONG ovh = type == 1 ? 9 : 13;     /* isochronous / other */
    ULONG bw = (maxpkt + ovh) * 8 * 7 / 6;
    return lowspeed ? bw * 8 : bw;
}

NTSTATUS __stdcall USBD_QueryBusTime(PDEVICE_OBJECT hub, PULONG frame)
{
    (void)hub;
    if (!frame) return STATUS_INVALID_PARAMETER;
    *frame = wb_present() ? wb_frame(0) : 0;
    return STATUS_SUCCESS;
}

VOID __stdcall USBD_RegisterHcFilter(PDEVICE_OBJECT hc, PDEVICE_OBJECT filter)
{
    (void)hc; (void)filter;
}

NTSTATUS __stdcall USBD_GetPdoRegistryParameter(PDEVICE_OBJECT pdo, PVOID buf, ULONG len, PWCHAR key, ULONG keylen)
{
    (void)pdo; (void)buf; (void)len; (void)key; (void)keylen;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    (void)drv; (void)path;
    return STATUS_SUCCESS;
}
