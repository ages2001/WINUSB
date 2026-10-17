/* pnp.c - Windows 95/98/ME Plug and Play, the generic part: the VxD is the DevLoader of its PCI
   controllers (CONFIGMG hands them over with CONFIG_START/STOP), the enumerator of their child
   devnodes (USB devices, HD Audio codecs, disks: the package part in pnpusb.c / pnpaud.c /
   pnpdisk.c says which exist) and the driver of those children. */
#include "pnpint.h"

void core_resume(void);

int pnp_active;

struct pnode pnp_nodes[MAXNODE];
int pnp_nnodes;
static u32 batch_timer;
static int batch_due;
static int options_read;
static int in_sync;
static int shutting_down;           /* Windows shuts down: no more child devnodes */

struct pnode *pnp_node_by_dn(u32 dn)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++)
        if (pnp_nodes[i].dn == dn) return &pnp_nodes[i];
    return NULL;
}

struct pnode *pnp_node_by_bdf(u32 bdf)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++)
        if (pnp_nodes[i].dn && pnp_nodes[i].kind == PK_PCI && pnp_nodes[i].bdf == bdf) return &pnp_nodes[i];
    return NULL;
}

struct pnode *pnp_node_alloc(void)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++)
        if (!pnp_nodes[i].dn) {
            memset(&pnp_nodes[i], 0, sizeof(pnp_nodes[0]));
            return &pnp_nodes[i];
        }
    if (pnp_nnodes >= MAXNODE) return NULL;
    memset(&pnp_nodes[pnp_nnodes], 0, sizeof(pnp_nodes[0]));
    return &pnp_nodes[pnp_nnodes++];
}

u32 pnp_hexval(const char *s, int n)
{
    u32 v = 0;
    while (n--) {
        char c = *s++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
    }
    return v;
}

const char *pnp_findstr(const char *s, const char *key)
{
    int n = (int)strlen(key);
    for (; *s; s++)
        if (!strncmp(s, key, n)) return s + n;
    return NULL;
}

/* PCI\VEN_8086&DEV_7020&...\BUS_00&DEV_01&FUNC_02 -> bus << 8 | dev << 3 | fn */
static int parse_pci_id(const char *id, u32 *bdf)
{
    const char *b = pnp_findstr(id, "BUS_"), *d = pnp_findstr(id, "\\BUS_"), *f = pnp_findstr(id, "FUNC_");
    const char *dv;
    if (!b || !f || !d) return -1;
    dv = pnp_findstr(d, "&DEV_");
    if (!dv) return -1;
    *bdf = (pnp_hexval(b, 2) << 8) | (pnp_hexval(dv, 2) << 3) | pnp_hexval(f, 2);
    return 0;
}

/* ---- options: Enum\...\Options (REG_SZ) of the first controller's software key ---- */
static void read_options(u32 dn)
{
    char buf[128];
    u32 len = sizeof(buf) - 1;
    const char *o = "";
    if (options_read) return;
    options_read = 1;
    memset(buf, 0, sizeof(buf));
    /* empty: SYSTEM.INI [WINUSB] / [WINAUDIO] / [WINDISK] Options= */
    if (cm_read_registry_value(dn, NULL, "Options", REG_SZ, buf, &len, CM_REGISTRY_SOFTWARE) == CR_SUCCESS && buf[0])
        o = buf;
    else
        o = NULL;
    if (vxd_standalone_cb(o) == 0) kprintf("pnp: options \"%s\"\n", o ? o : "(SYSTEM.INI)");
}

/* ---- the controllers are started together a little after the last CONFIG_START ---- */
extern void __cdecl thk_pnpbatch(void);

#ifndef WIN_DISK
static void batch_schedule(void)
{
    if (batch_timer) vmm_cancel_timeout(batch_timer);
    batch_timer = vmm_set_timeout(250, (void *)thk_pnpbatch, 0);
    batch_due = 1;
}
#endif

static void batch_run(void)
{
    u32 bdfs[32];
    int i, n = 0, r;
    batch_timer = 0;
    batch_due = 0;
    if (!vxd_core_up()) vxd_core_start_pnp();
    if (!vxd_core_up()) return;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        if (p->dn && p->kind == PK_PCI && p->started == 1 && n < 32) {
            bdfs[n++] = p->bdf;
            p->started = 2;
        }
    }
    if (!n) return;
    r = core_pnp_start(bdfs, n);
    kprintf("pnp: %d controller(s) started\n", r);
}

void __cdecl vxd_pnpbatch(u32 vm, struct crs *c)
{
    (void)vm; (void)c;
    vmm_begin_crit();
    batch_run();
    vmm_end_crit();
}

void pnp_set_desc(u32 dn, const char *d)
{
    char cur[128];
    u32 len = sizeof(cur) - 1;
    if (!d) return;
    memset(cur, 0, sizeof(cur));
    if (cm_read_registry_value(dn, NULL, "DeviceDesc", REG_SZ, cur, &len, CM_REGISTRY_HARDWARE) == CR_SUCCESS &&
        !strncmp(cur, d, sizeof(cur)))
        return;
    cm_write_registry_value(dn, NULL, "DeviceDesc", REG_SZ, d, (u32)strlen(d) + 1, CM_REGISTRY_HARDWARE);
}

/* ---- Configuration Manager calls (thk_cmconfig in vxdent.asm) ---- */
u32 __cdecl vxd_cmconfig(u32 func, u32 sub, u32 dn, u32 ref, u32 flags)
{
    struct pnode *p = pnp_node_by_dn(dn);
    (void)ref; (void)flags;
    if (cb && (cb->opts & OPT_DEBUG)) kprintf("pnp: config %lu/%lu devnode %08lx\n", func, sub, dn);
    if (!p) return CR_DEFAULT;
    if (func == CONFIG_SHUTDOWN || func == CONFIG_PRESHUTDOWN) shutting_down = 1;
    if (p->kind != PK_PCI) {
        pkg_child_config(p, func);
        if (func == CONFIG_REMOVE) {
            p->dn = 0;
            p->usbid = 0;
        }
        return func == CONFIG_START || func == CONFIG_TEST || func == CONFIG_STOP || func == CONFIG_REMOVE ||
               func == CONFIG_PREREMOVE || func == CONFIG_SHUTDOWN || func == CONFIG_PRESHUTDOWN ||
               func == CONFIG_APM ? CR_SUCCESS : CR_DEFAULT;
    }
    switch (func) {
    case CONFIG_START: {
        struct cmconfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (cm_get_alloc_log_conf(&cfg, dn, 0) == CR_SUCCESS)
            kprintf("pnp: start %02lx:%02lx.%lu, %u memory window(s) at %08lx, %u I/O range(s), IRQ %d\n",
                    p->bdf >> 8, (p->bdf >> 3) & 0x1F, p->bdf & 7, cfg.nmem, cfg.nmem ? cfg.membase[0] : 0,
                    cfg.nio, cfg.nirq ? cfg.irq[0] : -1);
        else
            kprintf("pnp: start %02lx:%02lx.%lu\n", p->bdf >> 8, (p->bdf >> 3) & 0x1F, p->bdf & 7);
        read_options(dn);
        pnp_set_desc(dn, pkg_desc(p->bdf));
        if (p->started != 2) {
            p->started = 1;
#ifdef WIN_DISK
            /* disk controllers start at once: the I/O Supervisor must find the disks (the
               boot disk among them) while the Configuration Manager still enumerates */
            vmm_begin_crit();
            batch_run();
            vmm_end_crit();
#else
            batch_schedule();
#endif
        }
        return CR_SUCCESS;
    }
    case CONFIG_TEST:
        return CR_SUCCESS;
    case CONFIG_STOP:
    case CONFIG_PREREMOVE:
    case CONFIG_REMOVE:
        if (p->started == 2) {
            kprintf("pnp: stop %02lx:%02lx.%lu\n", p->bdf >> 8, (p->bdf >> 3) & 0x1F, p->bdf & 7);
            vmm_begin_crit();
            core_pnp_stop(p->bdf);
            vmm_end_crit();
        }
        p->started = 0;
        if (func == CONFIG_REMOVE) {
            p->dn = 0;              /* the devnode is gone */
            p->usbid = 0;
        }
        return CR_SUCCESS;
    case CONFIG_SHUTDOWN:
    case CONFIG_PRESHUTDOWN:
        return CR_SUCCESS;
    case CONFIG_APM:
        if (sub == CONFIG_APM_RESUME_SUSPEND || sub == CONFIG_APM_RESUME_CRITICAL) {
            /* hardware lost its state: controllers are started again */
#ifdef WIN_DISK
            /* the disks stay (the I/O Supervisor holds them open): only the controller
               state is set up again */
            if (p->started == 2) {
                vmm_begin_crit();
                core_resume();
                vmm_end_crit();
            }
#else
            if (p->started == 2) {
                vmm_begin_crit();
                core_pnp_stop(p->bdf);
                vmm_end_crit();
                p->started = 1;
                batch_schedule();
            }
#endif
        }
        return CR_SUCCESS;
    default:
        return CR_DEFAULT;
    }
}

/* PNP_NEW_DEVNODE: CONFIGMG loaded us for this devnode (DevLoader= in the INF) */
u32 pnp_new_devnode(u32 dn, u32 loadtype)
{
    struct pnode *p = pnp_node_by_dn(dn);
    char id[200];
    u32 r;
    (void)loadtype;
    pnp_active = 1;
    memset(id, 0, sizeof(id));
    if (cm_get_device_id(dn, id, sizeof(id) - 1, 0) != CR_SUCCESS) id[0] = 0;
    if (!p) {
        int k = 0;
        u32 bdf = 0;
        if (!strncmp(id, "PCI\\", 4) && parse_pci_id(id, &bdf) == 0) k = PK_PCI;
        else k = pkg_devnode_kind(id);
        if (!k) {
            kprintf("pnp: devnode %08lx \"%s\" is not ours\n", dn, id);
            return CR_FAILURE;
        }
        p = pnp_node_alloc();
        if (!p) return CR_FAILURE;
        p->dn = dn;
        p->kind = (u8)k;
        p->bdf = bdf;
        strlcpy(p->id, id, sizeof(p->id));
    }
    if (!p->registered) {
        r = cm_register_device_driver(dn, (void *)thk_cmconfig, 0,
                                      CM_REGISTER_DEVICE_DRIVER_DISABLEABLE | CM_REGISTER_DEVICE_DRIVER_REMOVABLE);
        if (r != CR_SUCCESS) {
            kprintf("pnp: CM_Register_Device_Driver failed (%lx)\n", r);
            return r;
        }
        p->registered = 1;
        /* the children are ours: without an enumerator the Configuration Manager would drop
           them when it enumerates the controller again */
        if ((p->kind == PK_PCI || p->kind == PK_LEGACY) && pkg_enumerates()) {
            r = cm_register_enumerator(dn, (void *)thk_cmenum, CM_REGISTER_ENUMERATOR_HARDWARE);
            if (r != CR_SUCCESS) kprintf("pnp: CM_Register_Enumerator failed (%lx)\n", r);
        }
    }
    kprintf("pnp: devnode %08lx %s\n", dn, id);
    return CR_SUCCESS;
}

/* ---- child devnodes ---- */
struct pnode *pnp_child_by_key(u8 kind, u32 key)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        if (p->dn && p->parent && p->kind == kind && p->key == key && !p->usbid) {
            p->seen = 1;
            return p;
        }
    }
    return NULL;
}

struct pnode *pnp_child(struct pnode *parent, const char *id, const char *hwids, const char *compat,
                        const char *desc, u8 kind, u32 key)
{
    struct pnode *p;
    u32 dn = 0, r;
    int i;
    for (i = 0; i < pnp_nnodes; i++) {
        p = &pnp_nodes[i];
        if (p->dn && p->parent && !strncmp(p->id, id, sizeof(p->id))) {
            p->seen = 1;
            return p;
        }
    }
    if (!parent || !parent->dn) return NULL;
    p = pnp_node_alloc();
    if (!p) return NULL;
    r = cm_create_devnode(&dn, id, parent->dn, CM_CREATE_DEVNODE_NORMAL);
    if ((r != CR_SUCCESS && r != CR_ALREADY_SUCH_DEVNODE) || !dn) {
        kprintf("pnp: cannot create devnode for %s (%lx)\n", id, r);
        return NULL;
    }
    p->dn = dn;
    p->parent = parent->dn;
    p->kind = kind;
    p->key = key;
    p->seen = 1;
    strlcpy(p->id, id, sizeof(p->id));
    if (hwids) cm_write_registry_value(dn, NULL, "HardwareID", REG_SZ, hwids, (u32)strlen(hwids) + 1, CM_REGISTRY_HARDWARE);
    if (compat)
        cm_write_registry_value(dn, NULL, "CompatibleIDs", REG_SZ, compat, (u32)strlen(compat) + 1, CM_REGISTRY_HARDWARE);
    if (desc && desc[0]) pnp_set_desc(dn, desc);
    kprintf("pnp: devnode %08lx created: %s (%s)\n", dn, id, compat ? compat : hwids ? hwids : "");
    return p;
}

/* all children against what the package sees now: new ones created, vanished ones removed */
static void pnp_sync(void)
{
    int i;
    u32 r;
    if (in_sync) return;        /* CM_Remove_SubTree / CM_Create_DevNode call the enumerator back */
    in_sync = 1;
    for (i = 0; i < pnp_nnodes; i++) pnp_nodes[i].seen = 0;
    pkg_sync();
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        u32 dn = p->dn;
        if (!dn || !p->parent || p->kind == PK_PCI || p->kind == PK_LEGACY || p->seen) continue;
        /* CM_Remove_SubTree tells the applications: only at application time */
        p->dn = 0;
        p->usbid = 0;
        r = cm_call_at_appy_time((void *)thk_appy, dn, 0);
        kprintf("pnp: %s gone, devnode %08lx removed (%lx)\n", p->id, dn, r);
    }
    in_sync = 0;
}

/* CONFIG_ENUMERATE: the Configuration Manager clears the marks of the children and removes the
   ones we do not create (again) now */
u32 __cdecl vxd_cmenum(u32 func, u32 sub, u32 dn, u32 about, u32 flags)
{
    int i;
    (void)sub; (void)about; (void)flags;
    if (cb && (cb->opts & OPT_DEBUG)) {
        char id[128];
        memset(id, 0, sizeof(id));
        if (about) cm_get_device_id(about, id, sizeof(id) - 1, 0);
        kprintf("pnp: enumerator %lu/%lu devnode %08lx about %08lx %s\n", func, sub, dn, about, id);
    }
    if (func != CONFIG_ENUMERATE) return CR_DEFAULT;
    if (vxd_core_up() && !shutting_down) pnp_sync();
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        u32 ndn = 0, r;
        if (!p->dn || p->kind == PK_PCI || p->kind == PK_LEGACY || p->parent != dn) continue;
        r = cm_create_devnode(&ndn, p->id, dn, CM_CREATE_DEVNODE_NORMAL);
        if (r != CR_SUCCESS && r != CR_ALREADY_SUCH_DEVNODE) kprintf("pnp: enumerate: %s: %lx\n", p->id, r);
    }
    return CR_SUCCESS;
}

void __cdecl vxd_appy(u32 dn)
{
    u32 r = cm_remove_subtree(dn, CM_REMOVE_UI_OK);
    /* CR_INVALID_DEVNODE: the Configuration Manager removed it already (parent removed) */
    if (r != CR_SUCCESS && r != CR_INVALID_DEVNODE)
        kprintf("pnp: CM_Remove_SubTree(%08lx) failed (%lx)\n", dn, r);
}

int pnp_shutting_down(void)
{
    return shutting_down;
}

void pnp_init(void)
{
    memset(pnp_nodes, 0, sizeof(pnp_nodes));
    pnp_nnodes = 0;
    batch_timer = 0;
    options_read = 0;
    pnp_active = 0;
    in_sync = 0;
    shutting_down = 0;
    kprintf("pnp: Configuration Manager %lx\n", cm_get_version());
}

void pnp_tick(void)
{
    static u32 last_sync_ms;
    u32 now;
    int i;
    if (!pnp_active || !vxd_core_up() || batch_due || !pkg_enumerates() || shutting_down) return;
    now = plat_ms();
    if (now - last_sync_ms < 500) return;
    last_sync_ms = now;
    pnp_sync();
    for (i = 0; i < pnp_nnodes; i++) {
        u32 st = 0, pr = 0, r;
        if (!pnp_nodes[i].dn) continue;
        r = cm_get_devnode_status(&st, &pr, pnp_nodes[i].dn, 0);
        if (r == CR_INVALID_DEVNODE) {
            /* removed behind our back (Device Manager): created again at the next scan */
            kprintf("pnp: devnode %08lx is gone\n", pnp_nodes[i].dn);
            pnp_nodes[i].dn = 0;
            pnp_nodes[i].usbid = 0;
            continue;
        }
        if (r != CR_SUCCESS || st != pnp_nodes[i].laststatus) {
            if (cb->opts & OPT_DEBUG)
                kprintf("pnp: devnode %08lx status %lx problem %lu (cr %lx)\n", pnp_nodes[i].dn, st, pr, r);
            pnp_nodes[i].laststatus = st;
        }
    }
}

void pnp_exit(void)
{
    if (batch_timer) vmm_cancel_timeout(batch_timer);
    batch_timer = 0;
}
