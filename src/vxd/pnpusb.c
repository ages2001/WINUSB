/* pnpusb.c - Windows 95/98/ME Plug and Play, the WINUSB part: the controllers (USB host
   controllers, USB4 / Thunderbolt host interfaces, LPSS I2C, NVMe) and one child devnode per
   USB device (under the controller: Device Manager sorts them into their classes by the
   compatible ids below and the WINUSB INF files) and per NVMe namespace. */
#include "pnpint.h"

int usb_api(struct winapireq *r, u8 *buf);

int pkg_enumerates(void)
{
    return 1;
}

/* PS/2 keyboard (*PNP03xx) or PS/2 mouse of the 8042 (*PNP0F03 ...) found by the BIOS, ACPI
   or Windows detection: 'K', 'M', 0 */
static int input_kind(const char *id)
{
    static const char *const mice[] = { "PNP0F03", "PNP0F0B", "PNP0F0E", "PNP0F12", "PNP0F13", "PNP0F19",
                                        "PNP0F1C" };
    static const char *const pads[] = { "SYN", "ETD", "LEN0", "DLL", "HPQ" };
    const char *s = id;
    int i;
    while (*s && *s != '\\') s++;
    if (!*s) return 0;
    s++;
    if (*s == '*') s++;
    if (!strncmp(s, "PNP03", 5) && s[5] && s[6] && (s[7] == '\\' || !s[7])) return 'K';
    for (i = 0; i < (int)ARRAY_SIZE(mice); i++)
        if (!strncmp(s, mice[i], 7) && (s[7] == '\\' || !s[7])) return 'M';
    /* PS/2 touchpads under their ACPI ids (PNP0F13 is their compatible id) */
    for (i = 0; i < (int)ARRAY_SIZE(pads); i++)
        if (!strncmp(s, pads[i], strlen(pads[i]))) return 'M';
    return 0;
}

/* WINUSB\VID_xxxx&PID_xxxx\... (USB devices), WINUSB\NVME\... (namespaces), PS/2 keyboard and
   mouse devnodes (WINKBD.INF, WINMOU.INF) */
int pkg_devnode_kind(const char *id)
{
    if (!strncmp(id, "WINUSB\\", 7)) return PK_CHILD;
    if (input_kind(id)) return PK_INPUT;
    return 0;
}

/* options of a PS/2 devnode (its software key) when no controller brought them yet */
static void input_options(u32 dn)
{
    char buf[128];
    u32 len = sizeof(buf) - 1;
    memset(buf, 0, sizeof(buf));
    if (cm_read_registry_value(dn, NULL, "Options", REG_SZ, buf, &len, CM_REGISTRY_SOFTWARE) != CR_SUCCESS)
        buf[0] = 0;
    if (!cb || buf[0]) vxd_standalone_cb(buf[0] ? buf : NULL);
}

/* PS/2 mouse: ps2.c on the IRQ of the devnode; PS/2 keyboard: VKD reads it, the LEDs are kept
   in step with the lock keys (plat_kbd_leds) */
void pkg_child_config(struct pnode *p, u32 func)
{
    int k;
    if (p->kind != PK_INPUT) return;
    k = input_kind(p->id);
    switch (func) {
    case CONFIG_START:
        input_options(p->dn);
        vxd_input_init();
        if (k == 'M') {
            struct cmconfig cfg;
            int irq = 12;
            memset(&cfg, 0, sizeof(cfg));
            if (cm_get_alloc_log_conf(&cfg, p->dn, 0) == CR_SUCCESS && cfg.nirq) irq = cfg.irq[0];
            ps2_start(irq);
        } else {
            vxd_kbd_start();
        }
        p->started = 1;
        break;
    case CONFIG_STOP:
    case CONFIG_REMOVE:
    case CONFIG_SHUTDOWN:
        if (k == 'M') ps2_stop();
        p->started = 0;
        break;
    }
}

/* Device Manager name of a controller from its PCI class code: Windows 95 matches only the
   4-digit PCI\CC_xxxx IDs, so the generic INF entry gets the precise name here */
const char *pkg_desc(u32 bdf)
{
    u32 cls = pci_rd32((int)(bdf >> 8), (int)((bdf >> 3) & 0x1F), (int)(bdf & 7), 8) >> 8;
    u32 vid = pci_rd32((int)(bdf >> 8), (int)((bdf >> 3) & 0x1F), (int)(bdf & 7), 0) & 0xFFFF;
    switch (cls) {
    case 0x0C0300: return "WINUSB USB 1.1 UHCI Universal Host Controller";
    case 0x0C0310: return "WINUSB USB 1.1 OHCI Open Host Controller";
    case 0x0C0320: return "WINUSB USB 2.0 EHCI Enhanced Host Controller";
    case 0x0C0330: return "WINUSB USB 3.x xHCI eXtensible Host Controller";
    case 0x0C0340: return "WINUSB USB4 Host Router (Thunderbolt 4)";
    case 0x0C8000: return vid == 0x8086 ? "WINUSB Intel LPSS I2C Controller (HID over I2C)" : NULL;
    case 0x010802: return "WINUSB NVMe (NVM Express) Controller";
    default:
        if ((cls >> 8) == 0x0880 && vid == 0x8086) return "WINUSB Thunderbolt 3 / USB4 Host Interface (NHI)";
        return NULL;
    }
}

static struct pnode *node_by_usbid(u16 id)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++)
        if (pnp_nodes[i].dn && pnp_nodes[i].usbid == id && pnp_nodes[i].kind != PK_PCI) return &pnp_nodes[i];
    return NULL;
}

static struct winusbnode *tree_find(struct winusbnode *t, int n, u16 id)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i].id == id) return &t[i];
    return NULL;
}

/* compatible ids by function, the most precise first; each has its own INF entry (and class):
   USB hubs and other devices: WINUSB.INF (Universal Serial Bus controllers), keyboards
   (Keyboard), mice (Mouse), tablets / touch screens / other HID (Human Interface Devices):
   WINUSBH.INF, audio and MIDI: WINUSBA.INF (Sound, video and game controllers), cameras:
   WINUSBV.INF (Imaging devices), mass storage: WINUSBD.INF (Disk drives) */
static const char *compat_of(const struct winusbnode *d)
{
    switch (d->driver) {
    case UD_HUB:
        if (d->speed >= 3 || d->bcdusb >= 0x0300) return "WINUSB\\HUB30,WINUSB\\HUB";
        if (d->speed == 2) return "WINUSB\\HUB20,WINUSB\\HUB";
        return "WINUSB\\HUB11,WINUSB\\HUB";
    case UD_KBD: return "WINUSB\\KEYBOARD,WINUSB\\HID";
    case UD_MOUSE: return "WINUSB\\MOUSE,WINUSB\\HID";
    case UD_TABLET: return "WINUSB\\TABLET,WINUSB\\HID";
    case UD_HIDOTHER: return "WINUSB\\HID";
    case UD_BOT: return "WINUSB\\BOT,WINUSB\\STORAGE";
    case UD_UAS: return "WINUSB\\UAS,WINUSB\\STORAGE";
    case UD_AUDIO: return d->rsv >= 2 ? "WINUSB\\AUDIO20,WINUSB\\AUDIO" : "WINUSB\\AUDIO10,WINUSB\\AUDIO";
    case UD_MIDI: return "WINUSB\\MIDI,WINUSB\\AUDIO";
    case UD_VIDEO: return "WINUSB\\VIDEO";
    default: return "WINUSB\\DEVICE";
    }
}

/* instance: controller location and the port path, stable for the same socket */
static void instance_of(struct winusbnode *t, int n, struct winusbnode *d, char *out, u32 max)
{
    u8 ports[8];
    int np = 0, i;
    struct winusbnode *cur = d, *hc = NULL;
    char tmp[16];
    while (cur && np < 8) {
        ports[np++] = cur->port;
        if (cur->parent & 0x4000) {
            hc = tree_find(t, n, cur->parent);
            break;
        }
        cur = tree_find(t, n, cur->parent);
    }
    ksnprintf(out, max, "B%02lXD%02lXF%lX", hc ? hc->pciloc >> 8 : 0, hc ? (hc->pciloc >> 3) & 0x1F : 0,
              hc ? hc->pciloc & 7 : 0);
    for (i = np - 1; i >= 0; i--) {
        ksnprintf(tmp, sizeof(tmp), "%c%u", i == np - 1 ? 'P' : '.', ports[i]);
        strlcat(out, tmp, max);
    }
}

static void create_child(struct winusbnode *t, int n, struct winusbnode *d)
{
    struct pnode *parent = NULL, *p;
    char inst[48], id[160], hw[160], desc[80];
    struct winusbnode *cur = d;
    int depth;
    /* all devices sit under the controller devnode (the one with the enumerator) */
    for (depth = 0; cur && depth < 8 && !(cur->parent & 0x4000); depth++) cur = tree_find(t, n, cur->parent);
    if (cur) {
        struct winusbnode *hc = tree_find(t, n, cur->parent);
        if (hc) parent = pnp_node_by_bdf(hc->pciloc);
    }
    if (!parent) return;                /* parent devnode not there yet: next scan */
    instance_of(t, n, d, inst, sizeof(inst));
    ksnprintf(id, sizeof(id), "WINUSB\\VID_%04X&PID_%04X\\%s", d->vid, d->pid, inst);
    ksnprintf(hw, sizeof(hw), "WINUSB\\VID_%04X&PID_%04X&REV_%04X,WINUSB\\VID_%04X&PID_%04X", d->vid, d->pid, d->bcd,
              d->vid, d->pid);
    desc[0] = 0;
    if (d->product[0]) ksnprintf(desc, sizeof(desc), "%s%s%s", d->manuf, d->manuf[0] ? " " : "", d->product);
    p = pnp_child(parent, id, hw, compat_of(d), desc,
                  (u8)((d->driver == UD_BOT || d->driver == UD_UAS) ? PK_IOSDISK : PK_CHILD), d->id);
    if (p) p->usbid = d->id;
}

/* NVMe namespaces: one child devnode per disk under the controller devnode */
static void sync_nvme(void)
{
    int i;
    for (i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        struct pnode *parent, *p;
        char id[96];
        if (d->kind != DK_NVME) continue;
        parent = pnp_node_by_bdf(d->key >> 16);
        if (!parent) continue;
        ksnprintf(id, sizeof(id), "WINUSB\\NVME\\B%02lXD%02lXF%lXN%lu", d->key >> 24, (d->key >> 19) & 0x1F,
                  (d->key >> 16) & 7, d->key & 0x7FFF);
        p = pnp_child(parent, id, "WINUSB\\NVME", "WINUSB\\NVME", d->model, PK_IOSDISK, d->key);
        if (p) p->bdf = d->key;         /* controller and namespace */
    }
}

/* The PS/2 mouse devnode with Windows' own driver (VMOUSE with MSMOUSE.VXD) gets the WINMOU.INF
   driver: WINUSB.VXD is its device loader from the next start on. Installing WINUSB makes
   WINMOUSE.DRV the mouse driver, after which Windows starts VMOUSE without MSMOUSE.VXD and
   shows the PS/2 mouse with code 8. Not with /NOPS2, and only once per start. */
static const char mou_desc[] = "WINUSB PS/2 Mouse (IntelliMouse Explorer, 5 buttons)";

static int same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return !*a && !*b;
}

static void ps2_adopt_node(u32 dn)
{
    static const char loader[] = "WINUSB.VXD";
    static const char drvdesc[] = "WINDRV WINUSB 1.0 PS/2 mouse";
    char buf[64];
    u32 len = sizeof(buf) - 1;
    memset(buf, 0, sizeof(buf));
    if (cm_read_registry_value(dn, NULL, "DevLoader", REG_SZ, buf, &len, CM_REGISTRY_SOFTWARE) != CR_SUCCESS)
        return;                         /* no driver installed: Windows offers WINMOU.INF itself */
    if (same_nocase(buf, loader)) return;
    cm_write_registry_value(dn, NULL, "DevLoader", REG_SZ, loader, sizeof(loader), CM_REGISTRY_SOFTWARE);
    cm_write_registry_value(dn, NULL, "DriverDesc", REG_SZ, drvdesc, sizeof(drvdesc), CM_REGISTRY_SOFTWARE);
    cm_write_registry_value(dn, NULL, "InfPath", REG_SZ, "WINMOU.INF", 11, CM_REGISTRY_SOFTWARE);
    cm_write_registry_value(dn, NULL, "InfSection", REG_SZ, "WINMOU.PS2", 11, CM_REGISTRY_SOFTWARE);
    cm_write_registry_value(dn, NULL, "Options", REG_SZ, "", 1, CM_REGISTRY_SOFTWARE);
    pnp_set_desc(dn, mou_desc);
    kprintf("pnp: PS/2 mouse devnode %08lx: device loader %s -> %s\n", dn, buf, loader);
}

static void ps2_adopt_tree(u32 dn, int depth)
{
    char id[128];
    u32 c;
    if (depth > 12) return;
    memset(id, 0, sizeof(id));
    if (cm_get_device_id(dn, id, sizeof(id) - 1, 0) == CR_SUCCESS && input_kind(id) == 'M') ps2_adopt_node(dn);
    if (cm_get_child(&c, dn, 0) != CR_SUCCESS) return;
    do {
        ps2_adopt_tree(c, depth + 1);
    } while (cm_get_sibling(&c, c, 0) == CR_SUCCESS);
}

static void ps2_adopt(void)
{
    static int done;
    u32 root;
    if (done) return;
    done = 1;
    if (cb && (cb->opts2 & OPT2_NOPS2)) return;
    if (cm_locate_devnode(&root, NULL, 0) == CR_SUCCESS) ps2_adopt_tree(root, 0);
}

void pkg_sync(void)
{
    struct winapireq rq;
    struct winusbnode *t;
    int n, i;
    ps2_adopt();
    sync_nvme();
    t = vmm_heap_alloc(64 * sizeof(*t));
    if (!t) return;
    memset(&rq, 0, sizeof(rq));
    rq.fn = WINF_USBTREE;
    rq.buflen = 64 * sizeof(*t);
    if (usb_api(&rq, (u8 *)t) != WINE_OK) {
        /* no view of the tree: keep the children */
        for (i = 0; i < pnp_nnodes; i++) pnp_nodes[i].seen = 1;
        vmm_heap_free(t);
        return;
    }
    n = (int)(rq.retlen / sizeof(*t));
    for (i = 0; i < n; i++) {
        struct pnode *p;
        if (t[i].type != UN_DEV) continue;
        p = node_by_usbid(t[i].id);
        if (p) {
            p->seen = 1;
            continue;
        }
        create_child(t, n, &t[i]);
    }
    vmm_heap_free(t);
}

/* disks of a devnode for the port driver (pnpblk.c) */
int pkg_blk_owned(struct pnode *p, struct blkdev *d)
{
    if (p->kind == PK_PCI) return d->kind == DK_NVME && (d->key >> 16) == p->bdf;
    if (!p->usbid) return d->kind == DK_NVME && d->key == p->bdf;
    return d->kind == DK_USB && d->owner == p->usbid;
}
