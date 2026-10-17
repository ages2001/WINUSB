/* vxd.h - VMM / VxD services (vxdent.asm) shared by the VxD platform files */
#ifndef VXD_H
#define VXD_H
#include "core.h"

struct crs {                    /* Client_Reg_Struc */
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax, err, eip;
    u16 cs, cs_p;
    u32 eflags, esp;
    u16 ss, ss_p, es, es_p, ds, ds_p, fs, fs_p, gs, gs_p;
    u32 alt[9];
};

extern u32 __cdecl vmm_sys_vm(void);
extern u32 __cdecl vmm_cur_vm(void);
extern u32 __cdecl vmm_system_time(void);
extern u32 __cdecl vmm_version(void);
extern u32 __cdecl vmm_set_timeout(u32 ms, void *func, u32 ref);
extern void __cdecl vmm_cancel_timeout(u32 h);
extern u32 __cdecl vmm_global_event(void *func, u32 ref);
extern void __cdecl vmm_begin_crit(void);
extern void __cdecl vmm_end_crit(void);
extern void * __cdecl vmm_heap_alloc(u32 size);
extern void __cdecl vmm_heap_free(void *p);
extern void __cdecl thk_cmconfig(void);
extern void __cdecl thk_hwint(void);

struct vid {                    /* VPICD_IRQ_Descriptor */
    u16 irq, options;
    u32 hw_int, virt_int, eoi, mask_change, iret, iret_timeout;
};
extern u32 __cdecl vpicd_virtualize(struct vid *v);
extern void __cdecl vpicd_phys_eoi(u32 h);
extern void __cdecl vpicd_force_default(u32 h);
extern void __cdecl vpicd_unmask(u32 h);
extern void __cdecl vpicd_mask(u32 h);
extern u32 __cdecl vkd_force_keys(u8 *buf, u32 n);
extern u32 __cdecl vkd_kbd_owner(void);
extern void __cdecl thk_cmenum(void);
extern void __cdecl thk_appy(void);

/* ---- Configuration Manager (Windows 95/98/ME) ---- */
#define CR_SUCCESS              0
#define CR_DEFAULT              1
#define CR_INVALID_DEVNODE      0x05
#define CR_FAILURE              0x13
#define CR_NO_SUCH_VALUE        0x25
#define CONFIG_FILTER           0
#define CONFIG_START            1
#define CONFIG_STOP             2
#define CONFIG_TEST             3
#define CONFIG_REMOVE           4
#define CONFIG_ENUMERATE        5
#define CONFIG_SETUP            6
#define CONFIG_CALLBACK         7
#define CONFIG_APM              8
#define CONFIG_TEST_FAILED      9
#define CONFIG_TEST_SUCCEEDED   0x0A
#define CONFIG_VERIFY_DEVICE    0x0B
#define CONFIG_PREREMOVE        0x0C
#define CONFIG_SHUTDOWN         0x0D
#define CONFIG_PREREMOVE2       0x0E
#define CONFIG_READY            0x0F
#define CONFIG_PRESHUTDOWN      0x12
#define CONFIG_APM_TEST_STANDBY 0
#define CONFIG_APM_TEST_SUSPEND 1
#define CONFIG_APM_RESUME_STANDBY 6
#define CONFIG_APM_RESUME_SUSPEND 7
#define CONFIG_APM_RESUME_CRITICAL 8
#define CM_REGISTER_DEVICE_DRIVER_DISABLEABLE 1
#define CM_REGISTER_DEVICE_DRIVER_REMOVABLE   2
#define CM_CREATE_DEVNODE_NORMAL 0
#define CM_REGISTER_ENUMERATOR_HARDWARE 1
#define CR_ALREADY_SUCH_DEVNODE 0x10
#define CM_REMOVE_UI_OK         0
#define CM_REGISTRY_HARDWARE    0
#define CM_REGISTRY_SOFTWARE    1
#define DN_STARTED              0x08
#define DN_HAS_PROBLEM          0x400
#define REG_SZ                  1
#define REG_BINARY              3
#define DLVXD_LOAD_DEVLOADER    1
#define DLVXD_LOAD_DRIVER       2

#define MAX_MEM_REGISTERS 9
#define MAX_IO_PORTS 20
#define MAX_IRQS 7
#define MAX_DMA_CHANNELS 7
#pragma pack(push, 1)
struct cmconfig {               /* CMCONFIG of CM_Get_Alloc_Log_Conf */
    u16 nmem;
    u32 membase[MAX_MEM_REGISTERS];
    u32 memlen[MAX_MEM_REGISTERS];
    u16 memattr[MAX_MEM_REGISTERS];
    u16 nio;
    u16 iobase[MAX_IO_PORTS];
    u16 iolen[MAX_IO_PORTS];
    u16 nirq;
    u8 irq[MAX_IRQS];
    u8 irqattr[MAX_IRQS];
    u16 ndma;
    u8 dma[MAX_DMA_CHANNELS];
    u16 dmaattr[MAX_DMA_CHANNELS];
    u8 rsv[3];
};
#pragma pack(pop)

extern u32 __cdecl cm_get_version(void);
extern u32 __cdecl cm_locate_devnode(u32 *pdn, const char *id, u32 flags);
extern u32 __cdecl cm_get_parent(u32 *pdn, u32 dn, u32 flags);
extern u32 __cdecl cm_get_child(u32 *pdn, u32 dn, u32 flags);
extern u32 __cdecl cm_get_sibling(u32 *pdn, u32 dn, u32 flags);
extern u32 __cdecl cm_get_device_id(u32 dn, char *buf, u32 len, u32 flags);
extern u32 __cdecl cm_create_devnode(u32 *pdn, const char *id, u32 parent, u32 flags);
extern u32 __cdecl cm_remove_subtree(u32 dn, u32 flags);
extern u32 __cdecl cm_register_device_driver(u32 dn, void *handler, u32 ref, u32 flags);
extern u32 __cdecl cm_register_enumerator(u32 dn, void *handler, u32 flags);
extern u32 __cdecl cm_call_at_appy_time(void *handler, u32 ref, u32 flags);
extern u32 __cdecl cm_get_devnode_status(u32 *status, u32 *problem, u32 dn, u32 flags);
extern u32 __cdecl cm_get_alloc_log_conf(struct cmconfig *c, u32 dn, u32 flags);
extern u32 __cdecl cm_read_registry_value(u32 dn, const char *subkey, const char *name, u32 type, void *buf,
                                          u32 *len, u32 flags);
extern u32 __cdecl cm_write_registry_value(u32 dn, const char *subkey, const char *name, u32 type,
                                           const void *buf, u32 len, u32 flags);
extern u32 __cdecl cm_set_private_problem(u32 dn, u32 ref, u32 flags);

/* ---- platvxd.c ---- */
int vxd_standalone_cb(const char *options);     /* control block without a DOS driver */
int vxd_core_up(void);
struct blkdev;
int vxd_bios_unit(struct blkdev *d);     /* BIOS INT 13h drive of a disk (Windows 9x), 0 none */
u32 vxd_bios_letters(int unit);
int pnp_shutting_down(void);            /* Windows 9x shuts down (CONFIG_PRESHUTDOWN seen) */         /* DOS drives of BIOS drive unit (bit 0 A:) */
void vxd_core_start_pnp(void);                  /* start the core in Plug and Play mode */

void vxd_input_init(void);                      /* system VM known: mouse events can be delivered */
void vxd_ps2_mouse(int dx, int dy, int dz, int dw, int buttons);   /* PS/2 mouse packet */
void vxd_kbd_start(void);                       /* the PS/2 keyboard devnode is ours */

/* ---- ps2.c ---- */
int ps2_start(int irq);         /* PS/2 mouse driven by the VxD from now on: 0 */
void ps2_stop(void);            /* mouse back in its power-on mode for the BIOS / DOS drivers */
int ps2_is_irq(u32 h);
u32 ps2_hwint(u32 h);           /* VPICD hardware interrupt: 0 = it was ours */
u32 ps2_status(void);
void ps2_reclaim(void);         /* IRQ 12 virtualized again: our handler runs before the others */           /* bit 0 running, bit 1 wheel, bit 2 buttons 4 and 5 */
int ps2_kbd_leds(u8 leds);      /* PS/2 keyboard LEDs: bit 0 Scroll, 1 Num, 2 Caps Lock */

/* ---- pnp.c ---- */
void pnp_init(void);
u32 pnp_new_devnode(u32 dn, u32 loadtype);
void pnp_tick(void);                            /* every 10 ms from the timer event */
void pnp_exit(void);
extern int pnp_active;

#endif
