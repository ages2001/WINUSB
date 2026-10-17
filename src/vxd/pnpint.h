/* pnpint.h - Windows 95/98/ME Plug and Play inside the VxD: the generic part (pnp.c: devnode
   table, Configuration Manager callbacks, child devnodes) and the package part (pnpusb.c,
   pnpaud.c, pnpdisk.c: which devnodes are ours, which child devnodes exist). */
#ifndef PNPINT_H
#define PNPINT_H
#include "vxd.h"
#include "winapi.h"

#define PK_PCI      1           /* PCI function: controller (DevLoader = this VxD) */
#define PK_CHILD    2           /* child devnode served by this VxD (DevLoader = this VxD) */
#define PK_IOSDISK  3           /* child devnode of the I/O Supervisor (DevLoader = *IOS, our .PDR) */
#define PK_LEGACY   4           /* devnode of another enumerator (BIOS / ACPI) we drive */
#define PK_INPUT    5           /* PS/2 keyboard or mouse of another enumerator, no children */

#define MAXNODE 64
struct pnode {
    u32 dn;
    u32 parent;                 /* devnode of the parent (children) */
    u32 bdf;                    /* PCI function (PK_PCI); package key otherwise */
    u8 kind, started, registered, gone;
    u16 usbid;                  /* USB device id of a WINUSB child devnode */
    u16 seen;                   /* child still present in the last scan */
    u32 laststatus;
    u32 key;                    /* package key of a child (codec address, disk ...) */
    u8 state;                   /* package state of a child (last name written ...) */
    u8 rsv[3];
    u32 cfgh, cfgref;           /* PK_IOSDISK of a BIOS drive: the I/O Supervisor's handler */
    char id[96];
};
extern struct pnode pnp_nodes[MAXNODE];
extern int pnp_nnodes;

struct pnode *pnp_node_by_dn(u32 dn);
struct pnode *pnp_node_by_bdf(u32 bdf);                 /* started or not, PK_PCI */
struct pnode *pnp_node_alloc(void);
u32 pnp_hexval(const char *s, int n);
const char *pnp_findstr(const char *s, const char *key);
/* a child devnode under 'parent' (created now or kept when it exists): hardware and
   compatible ids comma separated, desc may be NULL */
struct pnode *pnp_child(struct pnode *parent, const char *id, const char *hwids, const char *compat,
                        const char *desc, u8 kind, u32 key);
struct pnode *pnp_child_by_key(u8 kind, u32 key);        /* an existing child, marked seen */
void pnp_set_desc(u32 dn, const char *desc);             /* DeviceDesc of the hardware key */

/* package hooks */
int pkg_devnode_kind(const char *id);   /* PK_* for a devnode this package serves, 0 not ours */
int pkg_enumerates(void);               /* the PCI devnodes are enumerators of children */
void pkg_sync(void);                    /* call pnp_child() for every child that exists now */
const char *pkg_desc(u32 bdf);          /* Device Manager name of a controller, NULL: the INF's */
void pkg_child_config(struct pnode *p, u32 func);   /* CONFIG_* of a PK_CHILD / PK_LEGACY / PK_INPUT devnode */

#endif
