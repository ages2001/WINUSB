/* platvxd.c - driver core platform layer for the Windows 3.1 enhanced mode VxD and the
   Windows 95/98/ME Plug and Play VxD (pnp.c) */
#include "vxd.h"

u32 plat_mode = PM_VXD;

extern u32 __cdecl vmm_page_alloc(u32 npages, u32 align, u32 maxphys, u32 flags, u32 *phys, u32 *lin);
extern u32 __cdecl vmm_map_phys(u32 phys, u32 len);
extern u32 __cdecl vmm_alloc_v86_cb(void *func, u32 ref);
extern u32 __cdecl vmm_priority_event(u32 boost, u32 vm, u32 flags, u32 ref, void *func, u32 timeout);
extern void __cdecl vmm_begin_nest(struct crs *c);
extern void __cdecl vmm_end_nest(struct crs *c);
extern void __cdecl vmm_resume_exec(struct crs *c);
extern void __cdecl vmm_far_call(u32 seg, u32 off, struct crs *c);
extern void __cdecl vmm_exec_int(u32 n, struct crs *c);
extern void __cdecl vmm_save_client(void *buf, struct crs *c);
extern void __cdecl vmm_restore_client(void *buf, struct crs *c);
extern void __cdecl vtd_min_period(u32 ms);
extern u32 __cdecl cpu_rdtsc_lo(void);
extern u64 __cdecl cpu_rdtsc64(void);
extern u32 __cdecl vmm_hook_v86_int(u32 n, void *f);
extern u32 __cdecl vmm_profile_bool(const char *key, u32 def);
extern const char * __cdecl vmm_profile_string(const char *section, const char *key, const char *def);
extern u32 __cdecl vmm_init_state(void);
static int profile_ok(void);
extern void __cdecl thk_timer(void);
extern void __cdecl thk_event(void);
extern void __cdecl thk_vmevent(void);
extern void __cdecl thk_v86api(void);
extern u32 vxd_refdata;

void core_resume(void);

static u32 cb_far;              /* real-mode seg:off of the control block */
static u32 sysvm, cur_high;
static u32 tsc_per_us;
static int started;
static volatile u32 ev_pending;
#define EV_POLL  1
#define EV_KEYS  2
static u8 keyq[256];
static volatile u32 keyq_head, keyq_tail;
static u16 com_port;

/* mouse delivery to WINMOUSE.DRV */
static u32 mcb_sel, mcb_off;    /* PM callback in the system VM */
static volatile s32 m_dx, m_dy, m_dz, m_dw;
static volatile u32 m_buttons, m_last_buttons;
static volatile u32 m_usb_buttons, m_ps2_buttons;
/* button changes waiting for delivery: a click (or both clicks of a double click) between
   two deliveries is delivered press by release, nothing is merged */
#define MBQLEN 16
static volatile u8 m_bq[MBQLEN];
static volatile u32 m_bq_head, m_bq_tail, m_bq_last;
static volatile u32 m_pending;
static volatile u32 m_absx, m_absy, m_absnew;
static u8 saved_client[0x6C];

#define MAXIRQ 8
static struct vid vids[MAXIRQ];
static u32 irqh[MAXIRQ];
static int irqn[MAXIRQ];
static int nirq;

void plat_screen(int on)
{
    (void)on;
}

void plat_putc(char c)
{
    if (c == '\n') plat_putc('\r');
    outb(0xE9, (u8)c);
    if (com_port) {
        int i;
        for (i = 0; i < 10000 && !(inb((u16)(com_port + 5)) & 0x20); i++) ;
        outb(com_port, (u8)c);
    }
}

static u64 tsc_base;
static u32 ms_base, tsc_per_ms;

/* TSC once calibrated: VMM system time does not advance while interrupts are disabled
   (BlockDev commands run that way) */
u32 plat_ms(void)
{
    if (!tsc_per_ms) return vmm_system_time();
    return ms_base + (u32)((cpu_rdtsc64() - tsc_base) / tsc_per_ms);
}

void plat_udelay(u32 us)
{
    if (tsc_per_us) {
        u32 t0 = cpu_rdtsc_lo(), want = us * tsc_per_us;
        while (cpu_rdtsc_lo() - t0 < want) cpu_pause();
    } else {
        while (us--) inb(0x80);
    }
}

void *plat_mmio(u32 phys, u32 len)
{
    u32 lin = vmm_map_phys(phys, len);
    if (lin == 0xFFFFFFFFUL) {
        kprintf("vxd: cannot map %08lx\n", phys);
        return NULL;
    }
    return (void *)lin;
}

void *plat_sysrm(u32 segoff)
{
    if (!sysvm) return NULL;
    return (void *)(*(u32 *)(sysvm + 4) + ((segoff >> 16) << 4) + (segoff & 0xFFFF));
}

void *plat_rm(u32 segoff)
{
    return (void *)(cur_high + ((segoff >> 16) << 4) + (segoff & 0xFFFF));
}

/* the sound bridge: Windows 95/98/ME through the other VxD's DDB, else (Windows 3.x with the
   DOS drivers) the control blocks the DOS drivers linked */
extern u32 vxd_link[2];
u32 __cdecl vmm_get_ddb(u32 id);

struct winbridge *plat_bridge_peer(void)
{
    struct winbridge *own = (struct winbridge *)((u8 *)cb + WIN_BROFF);
#ifdef WIN_AUDIO
    u32 id = WIN_VXD_USB;
#else
    u32 id = WIN_VXD_AUDIO;
#endif
    if (vmm_version() >= 0x400) {
        u32 ddb = vmm_get_ddb(id);
        if (ddb && ((u32 *)(ddb + 80))[0] == WINBR_MAGIC && ((u32 *)(ddb + 80))[1])
            return (struct winbridge *)((u32 *)(ddb + 80))[1];
    }
    if (!own->peer) return NULL;
    return (struct winbridge *)((u8 *)plat_sysrm(own->peer) + WIN_BROFF);
}

void *plat_bridge_ring(struct winbridge *p)
{
    return (void *)p->lin;
}

void plat_bridge_publish(struct winbridge *own)
{
    vxd_link[1] = (u32)own;
}

static volatile u32 lock_ms;    /* a lock key came from a USB keyboard at this time ... */
static volatile int lock_on;    /* ... if set */
#ifdef WIN_USB
static u8 ps2_leds = 0xFF;      /* USB LED bits last given to the PS/2 keyboard */
#endif

void plat_key(const u8 *sc, int n)
{
    int i;
    /* Caps, Num and Scroll Lock make codes (not the E1 1D 45 of Pause) */
    for (i = 0; i < n; i++)
        if ((sc[i] == 0x3A || sc[i] == 0x45 || sc[i] == 0x46) && !(i && (sc[i - 1] == 0xE0 || sc[i - 1] == 0x1D)))
            lock_ms = plat_ms(), lock_on = 1;
    /* a whole key sequence or nothing (E0 prefixes, releases) */
    if ((u32)n > (keyq_tail + sizeof(keyq) - keyq_head - 1) % sizeof(keyq)) return;
    while (n--) {
        keyq[keyq_head] = *sc++;
        keyq_head = (keyq_head + 1) % sizeof(keyq);
    }
    if (!(ev_pending & EV_KEYS)) {
        ev_pending |= EV_KEYS;
        vmm_global_event((void *)thk_event, 0);
    }
}

/* motion of all mice is added up; the buttons of the PS/2 mouse and of the USB mice are kept
   apart, so a button held on one is not released by a report of the other.
   Buttons: bit 0 left, 1 right, 2 middle, 3 back (4th), 4 forward (5th). */
static void mouse_event(int dx, int dy, int dz, int dw)
{
    u32 nb = m_usb_buttons | m_ps2_buttons;
    m_dx += dx; m_dy += dy; m_dz += dz; m_dw += dw;
    if (nb != m_bq_last) {
        u32 nh = (m_bq_head + 1) % MBQLEN;
        if (nh == m_bq_tail) m_bq_head = (m_bq_head + MBQLEN - 1) % MBQLEN, nh = (m_bq_head + 1) % MBQLEN;
        m_bq[m_bq_head] = (u8)nb;
        m_bq_head = nh;
        m_bq_last = nb;
    }
    m_buttons = nb;
    if (!mcb_sel || !sysvm) return;
    if (!m_pending) {
        m_pending = 1;
        /* High_Pri_Device_Boost, PEF_Wait_For_STI | PEF_Always_Sched */
        vmm_priority_event(0x00001000UL, sysvm, 0x09, 0, (void *)thk_vmevent, 0);
    }
}

void plat_mouse(int dx, int dy, int dz, int dw, int buttons)
{
    u32 f = irq_save();
    m_usb_buttons = (u32)buttons & 0x1F;
    mouse_event(dx, dy, dz, dw);
    irq_restore(f);
}

void plat_mouse_abs(int x, int y, int dz, int dw, int buttons)
{
    u32 f = irq_save();
    m_absx = (u32)x;
    m_absy = (u32)y;
    m_absnew = 1;
    m_usb_buttons = (u32)buttons & 0x1F;
    mouse_event(0, 0, dz, dw);
    irq_restore(f);
}

/* PS/2 mouse packet (ps2.c, hardware interrupt time) */
void vxd_ps2_mouse(int dx, int dy, int dz, int dw, int buttons)
{
    u32 f = irq_save();
    m_ps2_buttons = (u32)buttons & 0x1F;
    mouse_event(dx, dy, dz, dw);
    irq_restore(f);
}

#ifdef WIN_USB
extern void (*kbc_kbd_sink)(const u8 *b, int n);
static void vxd_kbd_sink(const u8 *b, int n);
#endif

void vxd_input_init(void)
{
    if (!sysvm) sysvm = vmm_sys_vm();
#ifdef WIN_USB
    kbc_kbd_sink = vxd_kbd_sink;
#endif
}

static int kbd_ps2;

void vxd_kbd_start(void)
{
    kbd_ps2 = 1;
}

#ifdef WIN_USB
/* keyboard bytes read while the 8042 served the mouse: to VKD like the USB keys (the
   acknowledges of a keyboard command are no keys) */
static void vxd_kbd_sink(const u8 *b, int n)
{
    u8 k[32];
    int i, m = 0;
    for (i = 0; i < n && m < (int)sizeof(k); i++)
        if (b[i] != 0xFA && b[i] != 0xFE) k[m++] = b[i];
    if (m) plat_key(k, m);
}
#endif

int plat_irq_attach(int irq)
{
    int i;
    for (i = 0; i < nirq; i++) if (irqn[i] == irq) return 0;
    if (nirq >= MAXIRQ || irq <= 0 || irq > 15) return -1;
    vids[nirq].irq = (u16)irq;
    vids[nirq].options = 2;            /* VPICD_Opt_Can_Share */
    vids[nirq].hw_int = (u32)thk_hwint;
    vids[nirq].iret_timeout = 500;
    irqh[nirq] = vpicd_virtualize(&vids[nirq]);
    if (!irqh[nirq]) {
        kprintf("vxd: IRQ %d cannot be virtualized, polling only\n", irq);
        return -1;
    }
    irqn[nirq] = irq;
    vpicd_unmask(irqh[nirq]);
    kprintf("vxd: IRQ %d attached\n", irq);
    nirq++;
    return 0;
}

void plat_irq_detach(int irq)
{
    int i;
    for (i = 0; i < nirq; i++)
        if (irqn[i] == irq && irqh[i]) vpicd_mask(irqh[i]);
}

/* the PS/2 port is ps2.c's in Windows */
int plat_aux_irq(void)
{
    return 0;
}

void plat_aux_poll(void)
{
}

void plat_kick(void)
{
    if (!(ev_pending & EV_POLL)) {
        ev_pending |= EV_POLL;
        vmm_global_event((void *)thk_event, 0);
    }
}

void plat_unit_changed(int unit)
{
    (void)unit;
}

/* ---- callbacks from vxdent.asm ---- */
u32 __cdecl vxd_hwint(u32 h)
{
    int i, r = 0;
#ifdef WIN_USB
    if (ps2_is_irq(h)) return ps2_hwint(h);
#endif
    for (i = 0; i < nirq; i++) {
        if (irqh[i] == h) {
            r = core_irq(irqn[i]);
            break;
        }
    }
    if (!r) return 1;           /* not ours: pass to the next sharer */
    vpicd_phys_eoi(h);
    plat_kick();
    return 0;
}

/* VKD hands forced keys to the keyboard focus VM: not before every VxD has initialized
   (static load of Windows 95/98/ME) and not while that VM is being created or cannot run
   (Windows would end a starting MS-DOS box as "must run in MS-DOS mode"); until then they
   wait in the queue and the timer's poll events bring them later */
#define VMSTAT_CREATING         0x0004
#define VMSTAT_NOT_EXECUTEABLE  0x0010
static int keys_ready(void)
{
    u32 vm;
    if (vmm_version() >= 0x400 && vmm_init_state() < 0x20000000UL) return 0;
    vm = vkd_kbd_owner();
    return !vm || !(*(volatile u32 *)vm & (VMSTAT_CREATING | VMSTAT_NOT_EXECUTEABLE));
}

void __cdecl vxd_event(u32 vm, struct crs *c)
{
    (void)vm; (void)c;
    if (ev_pending & EV_POLL) {
        ev_pending &= ~EV_POLL;
        core_poll();
        if (pnp_active) pnp_tick();
    }
    if ((ev_pending & EV_KEYS) && keys_ready()) {
        u8 buf[64];
        u32 n = 0, left;
        ev_pending &= ~EV_KEYS;
        while (keyq_tail != keyq_head && n < sizeof(buf)) {
            buf[n++] = keyq[keyq_tail];
            keyq_tail = (keyq_tail + 1) % sizeof(keyq);
        }
        if (n) {
            left = vkd_force_keys(buf, n);
            if (left) {
                /* VKD's buffer is full: the rest waits for the next poll */
                keyq_tail = (keyq_tail + sizeof(keyq) - left) % sizeof(keyq);
                ev_pending |= EV_KEYS;
            }
        }
    }
}

#if defined(WIN_AUDIO) || defined(WIN_USB)
static u32 timer_ms = 10;            /* sound: rings refilled, USB audio packets queued every 10 ms */
#else
static u32 timer_ms = 16;
#endif

/* The poll runs as a global event: never in the middle of an API call or another poll */
void __cdecl vxd_timer(u32 vm, struct crs *c)
{
    (void)vm; (void)c;
    plat_kick();
    vmm_set_timeout(timer_ms, (void *)thk_timer, 0);
}

/* deliver accumulated mouse motion to WINMOUSE.DRV in the system VM */
void __cdecl vxd_vmevent(u32 vm, struct crs *c)
{
    s32 dx, dy, dz, dw;
    u32 b, f, abs;
    (void)vm;
    for (;;) {
        f = irq_save();
        dx = m_dx; dy = m_dy; dz = m_dz; dw = m_dw;
        /* the next button change in order, with the motion so far */
        if (m_bq_tail != m_bq_head) {
            b = m_bq[m_bq_tail];
            m_bq_tail = (m_bq_tail + 1) % MBQLEN;
        } else {
            b = m_last_buttons;
        }
        m_dx = m_dy = m_dz = m_dw = 0;
        abs = m_absnew;
        m_absnew = 0;
        if (abs) {
            dx = (s32)m_absx;           /* tablet: normalized position, DI = 1 */
            dy = (s32)m_absy;
        }
        if (!abs && !dx && !dy && !dz && !dw && b == m_last_buttons) {
            m_pending = 0;
            irq_restore(f);
            break;
        }
        irq_restore(f);
        if (!mcb_sel) { m_pending = 0; break; }
        if (!abs) {
            if (dx > 32767) dx = 32767;
            if (dx < -32767) dx = -32767;
            if (dy > 32767) dy = 32767;
            if (dy < -32767) dy = -32767;
        }
        vmm_save_client(saved_client, c);
        vmm_begin_nest(c);
        c->eax = (u16)dx;
        c->ebx = (u16)dy;
        c->ecx = (u16)b | ((u32)m_last_buttons << 8);
        c->edx = (u16)dz;
        c->esi = (u16)dw;
        c->edi = abs;
        vmm_far_call(mcb_sel, mcb_off, c);
        vmm_resume_exec(c);
        vmm_end_nest(c);
        vmm_restore_client(saved_client, c);
        m_last_buttons = b;
    }
}

/* V86 API: called by WINDISK.SYS / WINUSB.SYS. AX = 0 DOS request (ES:BX), 1 API request (ES:BX), 2 poll */
void __cdecl vxd_v86api(u32 vm, struct crs *c)
{
    u32 fn = c->eax & 0xFFFF;
    u32 ptr = ((u32)c->es << 16) | (c->ebx & 0xFFFF);
    if (!started) {
        c->eax = 0xFFFF;
        return;
    }
    cur_high = *(u32 *)(vm + 4);
    vmm_begin_crit();
    switch (fn) {
    case 0:
        core_dos(plat_rm(ptr));
        c->eax = 0;
        break;
    case 1:
    {
        struct winapireq *r = plat_rm(ptr);
        r->status = (u16)core_api(r);
        c->eax = r->status;
        break;
    }
    case 2:
        core_poll();
        c->eax = 0;
        break;
    default:
        c->eax = 0xFFFF;
    }
    vmm_end_crit();
}

/* PM API: AX=0 version, AX=1 register CX:DX mouse callback (WINMOUSE.DRV), AX=2 unregister,
   AX=3 API request at CX:DX (real-mode address), AX=4 PS/2 mouse state -> BX (bit 0: driven
   by the VxD, bit 1 wheel, bit 2 buttons 4 and 5) */
void __cdecl vxd_pmapi(u32 vm, struct crs *c)
{
    switch (c->eax & 0xFFFF) {
    case 0:
        c->eax = WIN_VERSION;
        c->ebx = started;
        break;
    case 1:
        vxd_input_init();
#ifdef WIN_USB
        /* the PS/2 mouse is taken when WINMOUSE.DRV is the mouse driver: Windows 3.x, and
           Windows 95/98/ME with Windows' driver on its devnode (VMOUSE with MSMOUSE.VXD);
           with another mouse driver it stays with that driver */
        if (!ps2_status()) ps2_start(12);
        ps2_reclaim();
#endif
        mcb_sel = c->ecx & 0xFFFF;
        mcb_off = c->edx & 0xFFFF;
        m_dx = m_dy = m_dz = m_dw = 0;
        m_bq_tail = m_bq_head;
        m_bq_last = m_last_buttons = m_buttons;
        kprintf("vxd: mouse client %04lx:%04lx\n", mcb_sel, mcb_off);
        c->eax = 0;
        break;
    case 2:
        mcb_sel = 0;
        c->eax = 0;
        break;
#ifdef WIN_USB
    case 4:
        c->ebx = (c->ebx & 0xFFFF0000UL) | ps2_status();
        c->eax = 0;
        break;
#endif
    case 3:
    {
        /* API request of a protected-mode client (WINSOUND.DRV, WINMIX): CX:DX = real-mode
           address of the winapireq in the system VM's conventional memory */
        struct winapireq *r;
        if (!started) {
            c->eax = 0xFFFF;
            break;
        }
        cur_high = *(u32 *)(vm + 4);
        r = plat_rm(((c->ecx & 0xFFFF) << 16) | (c->edx & 0xFFFF));
        vmm_begin_crit();
        r->status = (u16)core_api(r);
        vmm_end_crit();
        c->eax = r->status;
        break;
    }
    default:
        c->eax = 0xFFFF;
    }
}

static void calibrate_tsc(void)
{
    u32 t0, t1, c0, c1;
    t0 = vmm_system_time();
    while ((t1 = vmm_system_time()) == t0) ;
    c0 = cpu_rdtsc_lo();
    while (vmm_system_time() - t1 < 50) ;
    c1 = cpu_rdtsc_lo();
    tsc_per_us = (c1 - c0) / 50000UL;
    if (!tsc_per_us) tsc_per_us = 1;
    ms_base = vmm_system_time();
    tsc_base = cpu_rdtsc64();
    tsc_per_ms = (c1 - c0) / 50UL;
    if (!tsc_per_ms) tsc_per_ms = 1;
}

static int find_cb(struct crs *c)
{
    u32 sv[0x6C / 4];
    if (vxd_refdata) {
        cb_far = vxd_refdata;
        return 0;
    }
    /* loaded from SYSTEM.INI: ask WINDISK.SYS / WINUSB.SYS through INT 2Fh */
    vmm_save_client(sv, c);
    vmm_begin_nest(c);
    c->eax = (WIN_MPLEX << 8) | 0x00;
    vmm_exec_int(0x2F, c);
    if ((c->eax & 0xFF) == 0xFF && (c->ebx & 0xFFFF) == 0x3357)
        cb_far = ((u32)c->es << 16) | (c->edi & 0xFFFF);
    vmm_end_nest(c);
    vmm_restore_client(sv, c);
    return cb_far ? 0 : -1;
}

#if defined(WIN_DISK) || defined(WIN_USB)
#ifdef WIN_USB
#define I13NAME "WINUSB"
#else
#define I13NAME "WINDISK"
#endif
static u8 *v86p(u32 vm, u16 seg, u32 off)
{
    return (u8 *)(*(u32 *)(vm + 4) + ((u32)seg << 4) + (off & 0xFFFF));
}

/* ---- BIOS INT 13h drives served by the driver ----
   Windows 3.x enhanced mode: the BIOS code for SD host controllers often needs 32-bit or SMM
   help that does not work from V86 mode (SeaBIOS: data written through INT 13h under Windows
   is garbage); the disk is the one the BIOS shares with us (borrow mode).
   Windows 95/98/ME: every BIOS drive that is one of our disks (AHCI, NVMe, SD / eMMC, USB):
   the BIOS code of these controllers may not work from V86 mode, and NVMe and USB
   controllers are reset when we take them over. The I/O Supervisor checks a real-mode drive
   with INT 25h before it hands the drive to the 32-bit port driver; with the transfers done
   here the check works and the drive leaves real mode (no MS-DOS compatibility mode).
   The disk transfers of these drives are done here; the calls that only report (08h, 15h,
   41h, 48h ...) still go to the BIOS. */
extern void __cdecl thk_int13(void);
static struct blkdev *i13disk[8];       /* BIOS drive 80h + k -> our disk, NULL: the BIOS's */
static u32 i13calls, i13errs;

/* ---- Windows 95/98/ME: the BIOS drives, read before we take the controllers over ----
   INT 13h AH=08h geometry, AH=48h size, and the DOS drives on each (block device list of
   IO.SYS, INT 2Fh AX=0803h) with the BPB of their boot sectors. A BIOS drive is one of our
   disks when every DOS drive on it is a FAT volume of that disk with the same BPB, the size
   (when the BIOS reports it) is the same, and no other disk qualifies. */
#define I13_MAXBPB 8
static struct biosunit {
    u8 nbpb, edd;
    u16 tried;                  /* blk_count() + 1 of the last failed match */
    u32 letters;                /* its DOS drives, bit 0 A: */
    u64 sectors;
    u8 bpb[I13_MAXBPB][25];
} bunit[8];
static int bios_scanned, i13_hooked;

static struct blkdev *i13_map(int k)
{
    struct biosunit *b = &bunit[k];
    struct blkdev *hit = NULL;
    int i, j, n = blk_count(), hits = 0;
    if ((!b->nbpb && !b->edd) || b->tried == (u16)(n + 1)) return NULL;
    b->tried = (u16)(n + 1);
    /* a drive without DOS drives: by its size alone, and only for the controllers whose BIOS
       code stops working once we run them (NVMe, USB), when no other BIOS drive has it */
    if (!b->nbpb)
        for (j = 0; j < 8; j++)
            if (j != k && bunit[j].edd && bunit[j].sectors == b->sectors) return NULL;
    for (i = 0; i < n; i++) {
        struct blkdev *d = blk_get(i);
        int ok = 1;
        if (!d || !d->present) continue;
        for (j = 0; j < 8; j++) if (j != k && i13disk[j] == d) ok = 0;
        if (ok && b->edd && b->sectors != d->sectors * (d->ssize / 512)) ok = 0;
        if (ok && !b->nbpb && d->kind != DK_NVME && d->kind != DK_USB) ok = 0;
        for (j = 0; ok && j < b->nbpb; j++) if (!blk_has_bpb(d, b->bpb[j])) ok = 0;
        if (ok) {
            hit = d;
            hits++;
        }
    }
    if (hits != 1) {
        if (hits) kprintf("vxd: BIOS drive %02x: %d disks match, left to the BIOS\n", 0x80 + k, hits);
        else if (n) kprintf("vxd: BIOS drive %02x: none of %d disk(s) matches\n", 0x80 + k, n);
        return NULL;
    }
    i13disk[k] = hit;
    kprintf("vxd: BIOS drive %02x is %s: its transfers are done by " I13NAME "\n", 0x80 + k, hit->model);
    return hit;
}

extern void __cdecl vmm_begin_nest_v86(struct crs *c);

static void bios_scan(void)
{
    u32 vm = vmm_sys_vm(), sv[0x6C / 4];
    struct crs *c = (struct crs *)*(u32 *)(vm + 8);
    int n = *v86p(vm, 0x40, 0x75), k, guard;
    u16 seg, off;
    if (bios_scanned || vmm_version() < 0x400) return;
    bios_scanned = 1;
    if (n > 8) n = 8;
    vmm_save_client(sv, c);
    vmm_begin_nest_v86(c);
    for (k = 0; k < n; k++) {
        struct wingeom *g = &cb->biosgeom[k];
        c->eax = 0x0800;
        c->edx = 0x80 + k;
        c->es = 0;
        c->edi = 0;
        c->eflags &= ~1UL;
        vmm_exec_int(0x13, c);
        if (!(c->eflags & 1)) {
            g->heads = (u16)(((c->edx >> 8) & 0xFF) + 1);
            g->spt = (u16)(c->ecx & 0x3F);
            g->cyls = (u16)((((c->ecx >> 8) & 0xFF) | ((c->ecx & 0xC0) << 2)) + 1);
        }
        c->eax = 0x4100;
        c->ebx = 0x55AA;
        c->edx = 0x80 + k;
        c->eflags &= ~1UL;
        vmm_exec_int(0x13, c);
        if (!(c->eflags & 1) && (u16)c->ebx == 0xAA55 && (c->ecx & 1)) {
            u8 *p;
            c->esp = (c->esp & 0xFFFF0000UL) | ((c->esp - 0x40) & 0xFFFF);
            p = v86p(vm, c->ss, c->esp);
            memset(p, 0, 0x40);
            *(u16 *)p = 0x1E;
            c->ds = c->ss;
            c->esi = c->esp & 0xFFFF;
            c->eax = 0x4800;
            c->edx = 0x80 + k;
            c->eflags &= ~1UL;
            vmm_exec_int(0x13, c);
            if (!(c->eflags & 1) && *(u32 *)(p + 0x10)) {
                bunit[k].sectors = *(u32 *)(p + 0x10) | ((u64)*(u32 *)(p + 0x14) << 32);
                bunit[k].edd = 1;
            }
            c->esp = (c->esp & 0xFFFF0000UL) | ((c->esp + 0x40) & 0xFFFF);
        }
    }
    cb->nbios = (u8)n;
    /* the DOS block devices (IO.SYS): BIOS unit, drive, BPB */
    c->eax = 0x0803;
    c->eflags &= ~1UL;
    vmm_exec_int(0x2F, c);
    seg = c->ds;
    off = (u16)c->edi;
    for (guard = 0; guard < 26 && off != 0xFFFF && (seg || off); guard++) {
        u8 *bds = v86p(vm, seg, off);
        u8 unit = bds[4];
        if (unit >= 0x80 && unit < 0x80 + n && bunit[unit - 0x80].nbpb < I13_MAXBPB &&
            *(u16 *)(bds + 6) == 512) {
            struct biosunit *b = &bunit[unit - 0x80];
            memcpy(b->bpb[b->nbpb++], bds + 6, 25);
            if (bds[5] < 26) b->letters |= 1UL << bds[5];
        }
        off = *(u16 *)bds;
        seg = *(u16 *)(bds + 2);
    }
    vmm_end_nest(c);
    vmm_restore_client(sv, c);
    for (k = 0; k < n; k++)
        kprintf("vxd: BIOS drive %02x: %u/%u/%u, %lu sectors%s, %d DOS drive(s)\n", 0x80 + k,
                cb->biosgeom[k].cyls, cb->biosgeom[k].heads, cb->biosgeom[k].spt, (u32)bunit[k].sectors,
                bunit[k].edd ? "" : " (no EDD)", bunit[k].nbpb);
}

/* the BIOS drive (80h ..) that is disk d, 0 if none */
int vxd_bios_unit(struct blkdev *d)
{
    int k;
    for (k = 0; k < 8 && bios_scanned; k++) {
        if (!i13disk[k]) i13_map(k);
        if (i13disk[k] == d) return 0x80 + k;
    }
    return 0;
}

u32 vxd_bios_letters(int unit)
{
    return unit >= 0x80 && unit < 0x88 ? bunit[unit - 0x80].letters : 0;
}

/* Windows 95/98/ME: the BIOS drives that are our disks get their transfers from us */
static void i13_pnp_hook(void)
{
    if (i13_hooked || !bios_scanned || !cb->nbios) return;
    i13_hooked = 1;
    if (vmm_hook_v86_int(0x13, (void *)thk_int13))
        kprintf("vxd: INT 13h hook refused, the BIOS keeps its drives\n");
}

static void i13_ret(struct crs *c, u8 st)
{
    c->eax = (c->eax & 0xFFFF00FFUL) | ((u32)st << 8);
    if (st) c->eflags |= 1;
    else c->eflags &= ~1UL;
}

static int i13_rw(u32 vm, struct blkdev *d, u64 lba, u32 n, u16 seg, u16 off, int wr)
{
    u64 total = d->sectors * (d->ssize / 512);
    if (!n) return 0;
    if (lba >= total || n > total - lba) return 0x04;                   /* sector not found */
    if ((u32)off + n * 512 > 0x10000UL) return 0x09;                    /* DMA boundary */
    if (wr ? blk_write_abs(d, lba, n, v86p(vm, seg, off)) : blk_read_abs(d, lba, n, v86p(vm, seg, off)))
        return wr ? 0xCC : 0x10;                                        /* write fault / read error */
    return 0;
}

u32 __cdecl vxd_int13(u32 vm, struct crs *c)
{
    u8 drv = (u8)c->edx, ah = (u8)(c->eax >> 8), st;
    struct blkdev *d;
    if (drv < 0x80 || drv >= 0x88) return 1;
    d = i13disk[drv - 0x80];
    if (!d && bios_scanned) d = i13_map(drv - 0x80);
    if (!d || !d->present) return 1;
    if (!i13calls++) kprintf("vxd: first INT 13h call for BIOS drive %02x served by " I13NAME "\n", drv);
    switch (ah) {
    case 0x00: case 0x0D: case 0x10: case 0x11: case 0x0C: case 0x47:
        st = 0;                                             /* reset, ready, recalibrate, seek */
        break;
    case 0x01:
        st = 0;                                             /* status of the last operation */
        break;
    case 0x02: case 0x03: case 0x04: {
        struct wingeom *g = &cb->biosgeom[drv - 0x80];
        u32 cyl = ((c->ecx >> 8) & 0xFF) | ((c->ecx & 0xC0) << 2), sec = c->ecx & 0x3F, head = (c->edx >> 8) & 0xFF;
        u32 n = c->eax & 0xFF;
        if (!sec || sec > g->spt || head >= g->heads || !n) {
            st = 0x01;
            break;
        }
        if (ah == 0x04) {
            st = 0;
            break;
        }
        st = (u8)i13_rw(vm, d, (cyl * g->heads + head) * g->spt + sec - 1, n, c->es, (u16)c->ebx, ah == 0x03);
        if (st) c->eax &= ~0xFFUL;                          /* nothing transferred */
        break;
    }
    case 0x42: case 0x43: case 0x44: {
        u8 *p = v86p(vm, c->ds, c->esi);
        u32 n = *(u16 *)(p + 2);
        u64 lba = *(u32 *)(p + 8) | ((u64)*(u32 *)(p + 12) << 32);
        if (p[0] < 16 || *(u32 *)(p + 4) == 0xFFFFFFFFUL) {
            st = 0x01;                                      /* flat 64-bit buffer addresses: not here */
            break;
        }
        if (ah == 0x44) {
            st = 0;
            break;
        }
        st = (u8)i13_rw(vm, d, lba, n, *(u16 *)(p + 6), *(u16 *)(p + 4), ah == 0x43);
        if (st) *(u16 *)(p + 2) = 0;
        break;
    }
    default:
        if (i13errs++ < 16) kprintf("vxd: INT 13h %02x drive %02x: to the BIOS\n", ah, drv);
        return 1;
    }
    *v86p(vm, 0x40, 0x74) = st;                             /* BIOS data area: last status */
    if (st && i13errs++ < 16)
        kprintf("vxd: INT 13h %02x drive %02x: error %02x (cx %04lx dx %04lx es:bx %04x:%04lx)\n", ah, drv, st,
                c->ecx & 0xFFFF, c->edx & 0xFFFF, c->es, c->ebx & 0xFFFF);
    i13_ret(c, st);
    return 0;
}

#endif

#ifdef WIN_DISK
/* ---- Intel AHCI option ROM bugs with VDS in enhanced mode (what AHCIFIX.386 fixes) ----
   The ROM's INT 13h code locks DMA regions with the descriptor in ES:DI but unlocks
   with DS:DI, which makes VDMAD read garbage; and it may use the first bytes of the
   EBDA as a DMA area, which destroys the pointing device data there. Only VDS calls
   from ROM code (segment C000h and up) are touched. */
extern void __cdecl thk_int4b(void);
extern void __cdecl thk_vdsret(void);
static u32 vds_cb;
static u32 vds_ret_ip;
static u16 vds_ret_cs;
static u8 vds_swapped, vds_restore, ebda_saved;
static u8 ebda_buf[0x6C];

static void vds_fix_init(void)
{
    if (vds_cb) return;
    vds_cb = vmm_alloc_v86_cb((void *)thk_vdsret, 0);
    if (!vds_cb || vmm_hook_v86_int(0x4B, (void *)thk_int4b)) vds_cb = 0;
}

u32 __cdecl vxd_int4b(u32 vm, struct crs *c)
{
    u16 ax = (u16)c->eax, ebda;
    u8 *d;
    if ((ax >> 8) != 0x81 || c->cs < 0xC000 || !vds_cb) return 1;
    ebda = *(u16 *)v86p(vm, 0x40, 0x0E);
    d = v86p(vm, c->es, c->edi);
    if (ax == 0x8103 || ax == 0x8105) {
        /* the region starts at the beginning of the EBDA: keep its contents */
        if (!ebda_saved && *(u16 *)(d + 8) == ebda && *(u32 *)(d + 4) < sizeof(ebda_buf)) {
            memcpy(ebda_buf, v86p(vm, ebda, 0), sizeof(ebda_buf));
            ebda_saved = 1;
        }
    } else if (ax == 0x8104 || ax == 0x8106) {
        u8 *alt = v86p(vm, c->ds, c->edi), *r;
        int swap = c->es != c->ds && *(u32 *)(d + 12) == 0 && *(u32 *)(alt + 12) != 0;
        int restore;
        r = swap ? alt : d;
        restore = ebda_saved && *(u16 *)(r + 8) == ebda && *(u32 *)(r + 4) < sizeof(ebda_buf);
        if (swap || restore) {
            if (swap) {
                u16 t = c->es;
                c->es = c->ds;
                c->ds = t;
            }
            vds_swapped = (u8)swap;
            vds_restore = (u8)restore;
            vds_ret_cs = c->cs;
            vds_ret_ip = c->eip;
            c->cs = (u16)(vds_cb >> 16);
            c->eip = vds_cb & 0xFFFF;
        }
    }
    return 1;
}

void __cdecl vxd_vdsret(u32 vm, struct crs *c)
{
    if (vds_swapped) {
        u16 t = c->es;
        c->es = c->ds;
        c->ds = t;
    }
    if (vds_restore && ebda_saved) {
        memcpy(v86p(vm, *(u16 *)v86p(vm, 0x40, 0x0E), 0), ebda_buf, sizeof(ebda_buf));
        ebda_saved = 0;
    }
    vds_swapped = vds_restore = 0;
    c->cs = vds_ret_cs;
    c->eip = vds_ret_ip;
}

/* ---- FastDisk: BIOS drives 80h/81h served through BlockDev (32-bit disk access) ----
   With device=*int13 and 32BitDiskAccess=on, Windows sends V86 INT 13h calls for these
   drives (and its swap file I/O) here instead of to the BIOS. */
struct bdd {                    /* BlockDev_Device_Descriptor */
    u32 next;
    u8 major, minor, type, int13;
    u32 flags, name;
    u32 maxsec_lo, maxsec_hi;
    u32 secsize, heads, cyls, spt;
    u32 sync_proc, cmd_proc, hwint_proc;
    u8 priv[0x30];
    struct blkdev *d;           /* ours */
};

struct bcb {                    /* BlockDev_Command_Block */
    u32 next;
    u16 cmd, status;
    u32 flags, cplt;
    u32 sec_lo, sec_hi;
    u32 count, buf;
    u32 rsv[3];
};

extern void __cdecl thk_bdcmd(void);
extern void __cdecl thk_bdsync(void);
extern u32 __cdecl blockdev_register(void *b);
extern void __cdecl blockdev_complete(void *b, void *c);
static struct bdd fds[2];
static char fdname[2][8] = { "WINDISK0", "WINDISK1" };

static int fd_rw(struct blkdev *d, u64 sec, u32 n, u8 *p, int wr)
{
    if (sec >= d->sectors || n > d->sectors - sec) return -1;
    return wr ? blk_write_abs(d, sec, n, p) : blk_read_abs(d, sec, n, p);
}

static u32 fd_cmds;

void __cdecl vxd_bdcmd(struct bdd *b, struct bcb *c)
{
    int r = 0;
    if (!fd_cmds++) kprintf("vxd: first FastDisk command (drive %02x)\n", b->int13);
    if (c->cmd == 2) {
        c->status = 0;                          /* verify */
    } else if (c->cmd > 1) {
        c->status = 0x16;                       /* BDS_Invalid_Command */
    } else {
        u64 sec = c->sec_lo | ((u64)c->sec_hi << 32);
        int wr = c->cmd == 1;
        if (c->flags & 2) {              /* scatter/gather: (sectors, address) pairs */
            u32 *sg = (u32 *)c->buf;
            for (; !r && sg[0]; sg += 2) {
                r = fd_rw(b->d, sec, sg[0], (u8 *)sg[1], wr);
                sec += sg[0];
            }
        } else {
            r = fd_rw(b->d, sec, c->count, (u8 *)c->buf, wr);
        }
        c->status = r ? 0x14 : 0;               /* BDS_Media_Error */
        if (r) kprintf("vxd: FastDisk %s error at %lu\n", wr ? "write" : "read", c->sec_lo);
    }
    blockdev_complete(b, c);
}

static void fastdisk_init(void)
{
    u8 *mbr;
    int k, j, i, n = blk_count();
    if (vmm_version() < 0x030A) {
        kprintf("vxd: Windows 3.0 has no BlockDev, BIOS disks stay with the BIOS\n");
        return;
    }
    if (!profile_ok() || !vmm_profile_bool("32BitDiskAccess", 0)) {
        kprintf("vxd: 32BitDiskAccess is off, BIOS disks stay with the BIOS\n");
        return;
    }
    mbr = kmalloc_align(512, 512);
    for (k = 0; k < 2 && k < cb->nbios && k < 8; k++) {
        struct wingeom *g = &cb->biosgeom[k];
        struct blkdev *hit = NULL;
        struct bdd *b = &fds[k];
        int hits = 0, dup = 0;
        if (!g->heads || !g->spt || !g->cyls) continue;
        /* the disk with the same MBR as BIOS drive 80h+k; any doubt and it stays with the BIOS */
        for (j = 0; j < cb->nbios && j < 8; j++)
            if (j != k && cb->biosmbr[j] == cb->biosmbr[k]) dup = 1;
        for (i = 0; i < n && !dup; i++) {
            struct blkdev *d = blk_get(i);
            if (d->removable || d->ssize != 512) continue;
            if (blk_read_abs(d, 0, 1, mbr)) continue;
            if (crc32(0, mbr, 512) == cb->biosmbr[k]) {
                hit = d;
                hits++;
            }
        }
        if (dup || hits != 1) {
            if (dup || hits) kprintf("vxd: BIOS drive %02x: no unique disk, left to the BIOS\n", 0x80 + k);
            continue;
        }
        if ((u64)g->cyls * g->heads * g->spt > hit->sectors) {
            kprintf("vxd: BIOS drive %02x: geometry larger than the disk, left to the BIOS\n", 0x80 + k);
            continue;
        }
        memset(b, 0, sizeof(*b));
        b->major = 3;
        b->minor = 0x0A;
        b->type = 5;                            /* BDT_Fixed_Disk */
        b->int13 = (u8)(0x80 + k);
        b->flags = 1 | 2 | 0x10;                /* Int 13h drive, writeable, serial commands */
        b->name = (u32)fdname[k];
        b->maxsec_lo = (u32)(hit->sectors - 1);
        b->maxsec_hi = (u32)((hit->sectors - 1) >> 32);
        b->secsize = 512;
        b->heads = g->heads;
        b->cyls = g->cyls;
        b->spt = g->spt;
        b->sync_proc = (u32)thk_bdsync;
        b->cmd_proc = (u32)thk_bdcmd;
        b->d = hit;
        if (blockdev_register(b))
            kprintf("vxd: BlockDev refused BIOS drive %02x (another 32-bit disk driver?)\n", 0x80 + k);
        else
            kprintf("vxd: 32-bit disk access for BIOS drive %02x: %s, %u/%u/%u\n", 0x80 + k, hit->model,
                    g->cyls, g->heads, g->spt);
    }
    kfree(mbr);
}
static void int13_init(void)
{
    struct sdinfo si;
    u8 *mbr;
    int i, k, any = 0;
    if (!sd_slots()) return;
    mbr = kmalloc_align(512, 512);
    if (!mbr) return;
    /* the cards the BIOS uses too, matched to its drives by the MBR (as FastDisk does) */
    for (i = 0; sd_slot_info(i, &si) == 0; i++) {
        struct blkdev *d = si.disk;
        u32 crc;
        int hit = -1, hits = 0;
        if (!si.borrow || !d || d->ssize != 512 || blk_read_abs(d, 0, 1, mbr)) continue;
        crc = crc32(0, mbr, 512);
        for (k = 0; k < cb->nbios && k < 8; k++)
            if (cb->biosmbr[k] == crc) {
                hit = k;
                hits++;
            }
        if (hits != 1 || (hit < 2 && fds[hit].d)) continue;   /* doubt, or 32-bit disk access has it */
        if (!cb->biosgeom[hit].heads || !cb->biosgeom[hit].spt) continue;
        i13disk[hit] = d;
        any = 1;
        kprintf("vxd: BIOS drive %02x (%s): disk transfers by WINDISK\n", 0x80 + hit, d->model);
    }
    kfree(mbr);
    if (any && vmm_hook_v86_int(0x13, (void *)thk_int13)) {
        kprintf("vxd: INT 13h hook refused, the BIOS keeps its drives\n");
        memset(i13disk, 0, sizeof(i13disk));
    }
}

#endif

#ifdef WIN_AUDIO
#include "dsp.h"
extern const char * __cdecl vmm_exec_path(u32 *dirlen);
extern u32 __cdecl ifs_r0io(u32 func, u32 rbx, u32 rcx, u32 rdx, void *rsi, u32 *err);
extern u32 __cdecl vmm_copy_page_table(u32 linpg, u32 npages, u32 *buf, u32 flags);

/* ---- firmware files of the Intel audio DSP ----
   Windows 3.x: loaded into XMS by WINAUDIO.SYS (control block table). Windows 95/98/ME:
   read from the SYSTEM directory (where the INF copies them) with IFSMgr ring 0 file I/O
   into fixed pages below 4 GB. */
static int cb_from_dos;

void plat_fw_request(const char *name)
{
    (void)name;
}

static int fw_from_dos(const char *name, struct fwimg *img)
{
    struct winfw *t = (struct winfw *)((u8 *)cb + CB_FWTAB);
    int i;
    for (i = 0; i < WIN_MAXFW; i++)
        if (t[i].state == WFW_OK && !strncmp(t[i].name, name, 13)) {
            img->v = plat_mmio(t[i].phys, t[i].size);
            if (!img->v) return -1;
            img->size = t[i].size;
            img->nseg = 1;
            img->seg[0].phys = t[i].phys;
            img->seg[0].len = t[i].size;
            return 0;
        }
    return -1;
}

static int fw_from_file(const char *name, struct fwimg *img)
{
    char path[160];
    u32 dl = 0, err, h, size, pages, phys = 0, lin = 0, got, i;
    const char *ep = vmm_exec_path(&dl);
    if (!ep || dl == 0 || dl > 120) return -1;
    /* SYSTEM\WINDRVFW (where WINAUDIO.INF puts the firmware), then SYSTEM */
    memcpy(path, ep, dl);
    path[dl] = 0;
    strlcat(path, "WINDRVFW\\", sizeof(path));
    strlcat(path, name, sizeof(path));
    h = ifs_r0io(0xD500, 0x0040, 0, 0x01, path, &err);     /* open: read only, deny none, existing */
    if (err) {
        path[dl] = 0;
        strlcat(path, name, sizeof(path));
        h = ifs_r0io(0xD500, 0x0040, 0, 0x01, path, &err);
    }
    if (err) {
        kprintf("vxd: %s not found\n", path);
        return -1;
    }
    size = ifs_r0io(0xD800, h, 0, 0, NULL, &err);           /* file size */
    if (err || size < 16 || size > 0x1000000UL) goto fail;
    pages = (size + 4095) >> 12;
    /* fixed, below 4 GB; contiguous when possible */
    if (!vmm_page_alloc(pages, 0, 0xFFFFF, 0x02 | 0x04 | 0x08, &phys, &lin) || !lin) {
        if (!vmm_page_alloc(pages, 0, 0xFFFFF, 0x08, &phys, &lin) || !lin) goto fail;
        phys = 0;
    }
    got = ifs_r0io(0xD600, h, size, 0, (void *)lin, &err);  /* read */
    ifs_r0io(0xD700, h, 0, 0, NULL, &err);                  /* close */
    if (got != size) return -1;
    img->v = (u8 *)lin;
    img->size = size;
    img->nseg = 0;
    if (phys) {
        img->nseg = 1;
        img->seg[0].phys = phys;
        img->seg[0].len = size;
        return 0;
    }
    /* scattered pages: physically adjacent ones joined */
    {
        static u32 pte[1024];
        u32 done = 0;
        while (done < pages) {
            u32 n = MIN(pages - done, 1024UL);
            vmm_copy_page_table((lin >> 12) + done, n, pte, 0);
            for (i = 0; i < n; i++) {
                u32 p = pte[i] & 0xFFFFF000UL;
                if (img->nseg && img->seg[img->nseg - 1].phys + img->seg[img->nseg - 1].len == p)
                    img->seg[img->nseg - 1].len += 4096;
                else if (img->nseg < FW_MAXSEG) {
                    img->seg[img->nseg].phys = p;
                    img->seg[img->nseg++].len = 4096;
                } else {
                    kprintf("vxd: %s: memory too fragmented\n", name);
                    return -1;
                }
            }
            done += n;
        }
    }
    return 0;
fail:
    ifs_r0io(0xD700, h, 0, 0, NULL, &err);
    return -1;
}

int plat_fw_load(const char *name, struct fwimg *img)
{
    memset(img, 0, sizeof(*img));
    if (cb_from_dos && !fw_from_dos(name, img)) return 0;
    if (vmm_version() >= 0x400 && !fw_from_file(name, img)) return 0;
    return -1;
}
#endif

static int core_up;
/* initialized data (a zero would land in the BSS, which the loader does not clear) */
#define DYN_NEW  0x4E455731UL
#define DYN_INIT 0x494E4931UL
static u32 dyn_state = DYN_NEW;

/* Starts the core: Sys_Critical_Init for WINDISK (BlockDev drives must be registered in
   that phase; interrupts are disabled, the TSC rate comes from the DOS core), otherwise
   Device_Init */
/* ---- standalone start (Windows 95/98/ME without the DOS driver): the control block lives
   in the VxD, the options come from SYSTEM.INI [WINUSB] / [WINAUDIO] Options= ---- */
struct optname { const char *name; u32 bit; u8 word; };
static const struct optname optnames[] = {
    { "D", OPT_DEBUG, 0 }, { "V", OPT_VERBOSE, 0 }, { "COM1", OPT_COM1, 0 }, { "S", OPT_SCREEN, 0 },
    { "POLL", OPT_POLL, 0 }, { "NORELOC", OPT2_NORELOC, 1 }, { "ALL4G", OPT2_ALL4G, 1 },
#ifdef WIN_USB
    { "NOHID", OPT_NOHID, 0 }, { "NOMSC", OPT_NOMSC, 0 }, { "NOXHCI", OPT_NOXHCI, 0 }, { "NOEHCI", OPT_NOEHCI, 0 },
    { "NOOHCI", OPT_NOOHCI, 0 }, { "NOUHCI", OPT_NOUHCI, 0 }, { "NOROUTE", OPT_NOROUTE, 0 },
    { "NOI2C", OPT_NOI2C, 0 }, { "NVME", OPT_NVME, 0 }, { "NONVME", OPT_NONVME, 0 },
    { "NOTB", OPT2_NOTB, 1 }, { "NOTBPCIE", OPT2_NOTBPCIE, 1 }, { "NOTBUSB", OPT2_NOTBUSB, 1 },
    { "PCIHP", OPT2_PCIHP, 1 }, { "NOPS2", OPT2_NOPS2, 1 }, { "NOTP", OPT2_NOTP, 1 },
    { "NOSMB", OPT2_NOSMB, 1 },
#endif
#ifdef WIN_DISK
    { "NOIDE", OPT_NOIDE, 0 }, { "NOAHCI", OPT_NOAHCI, 0 }, { "NONVME", OPT_NONVME, 0 }, { "PIO", OPT_PIO, 0 },
#endif
#ifdef WIN_AUDIO
    { "NOHDA", OPT_NOHDA, 0 }, { "NOAC97", OPT_NOAC97, 0 }, { "EAPD", OPT_INVEAPD, 0 }, { "NOGEN", OPT_NOGENERIC, 0 },
    { "IMM", OPT_HDAIMM, 0 }, { "NOSYNTH", OPT_NOSYNTH, 0 }, { "POSBUF", OPT_POSBUF, 0 }, { "GPIO", OPT_HDAGPIO, 0 },
    { "DSP", OPT2_DSP, 1 }, { "DMIC", OPT2_DMIC, 1 }, { "NODSP", OPT2_NODSP, 1 },
    { "NOTPVOL", OPT2_NOTPVOL, 1 },
    { "USBSB", OPT2_USBSB, 1 },
#endif
    { NULL, 0, 0 }
};

static int standalone;

static void parse_options(const char *s)
{
    char tok[16];
    while (*s) {
        int n = 0, i;
        while (*s == ' ' || *s == '/' || *s == '-' || *s == '\t') s++;
#ifdef WIN_AUDIO
        if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'B' || s[1] == 'b') && s[2] == ':') {
            /* /SB:A220,I5,D1,H5,P330 */
            char *d = ((struct winsb *)((u8 *)cb + WIN_SBOFF))->spec;
            int k = 0;
            s += 3;
            while (*s && *s != ' ' && *s != '/' && *s != '\t') {
                if (k < 23) d[k++] = *s;
                s++;
            }
            d[k] = 0;
            continue;
        }
#endif
        while (*s && *s != ' ' && *s != '/' && *s != '\t') {
            char ch = *s++;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            if (n < 15) tok[n++] = ch;
        }
        tok[n] = 0;
        if (!n) continue;
        if (tok[0] == 'R' && tok[1] == ':') {
            int v = 0;
            for (i = 2; tok[i] >= '0' && tok[i] <= '9'; i++) v = v * 10 + (tok[i] - '0');
            cb->slotreq = (u8)(v > 20 ? 20 : v);
            continue;
        }
#ifdef WIN_AUDIO
        {
            struct winsb *sc = (struct winsb *)((u8 *)cb + WIN_SBOFF);
            if (!strncmp(tok, "NOSB", 16)) { sc->flags |= SBF_OFF; continue; }
            if (!strncmp(tok, "NOOPL", 16)) { sc->flags |= SBF_NOOPL; continue; }
            if (!strncmp(tok, "NOMPU", 16)) { sc->flags |= SBF_NOMPU; continue; }
        }
#endif
        for (i = 0; optnames[i].name; i++) {
            if (strncmp(tok, optnames[i].name, 16)) continue;
            if (optnames[i].word) cb->opts2 |= (u16)optnames[i].bit;
            else cb->opts |= optnames[i].bit;
            break;
        }
        if (!optnames[i].name) kprintf("vxd: unknown option %s\n", tok);
    }
}

/* SYSTEM.INI is readable (VMM profile services) only while Windows initializes the
   VxDs: always for the static VxDs of 3.x, for a Plug and Play driver of 95/98/ME only when
   Configuration Manager loads it at the start (VMM_GetSystemInitState before
   SYSSTATE_VXDINITCOMPLETED). Later the services are gone and calling them stops Windows. */
static int profile_ok(void)
{
    return vmm_version() < 0x400 || vmm_init_state() < 0x30000000UL;
}

/* options: given (registry, Plug and Play) or from SYSTEM.INI [WINUSB] / [WINAUDIO] Options= */
static int ini_read;            /* SYSTEM.INI options read (static load at the start) */

int vxd_standalone_cb(const char *o)
{
    if (!cb) {
        cb = vmm_heap_alloc(WIN_BPBXOFF + WIN_MAXUNITS * WIN_BPBXSIZE);
        if (!cb) return -1;
        memset(cb, 0, WIN_BPBXOFF + WIN_MAXUNITS * WIN_BPBXSIZE);
        cb->magic = WIN_MAGIC;
        cb->version = WIN_VERSION;
        cb->size = sizeof(struct wincb);
        cb->opts = OPT_USB;
        cb->slotreq = 4 | 0x80;
    }
    if (!o && ini_read) return 0;
    if (!o && !profile_ok()) {
        /* loaded after the start of Windows (new hardware, driver update): SYSTEM.INI is
           read at the next start */
        kprintf("vxd: loaded after the start of Windows, SYSTEM.INI options not read\n");
        o = "";
    }
    if (!o) {
#ifdef WIN_USB
        o = vmm_profile_string("WINUSB", "Options", "");
#elif defined(WIN_AUDIO)
        o = vmm_profile_string("WINAUDIO", "Options", "");
#else
        o = vmm_profile_string("WINDISK", "Options", "");
#endif
        ini_read = 1;
    }
    if (o && *o) parse_options(o);
    if (cb->opts & OPT_COM1) com_port = 0x3F8;
    standalone = 1;
    return 0;
}

#ifdef WIN_USB
extern int usb_wdm_on;
extern u16 usb_wdm_ids[8][2];
extern int usb_wdm_nids;

static int hexval(const char **ps, u16 *v)
{
    const char *s = *ps;
    int n = 0;
    u16 x = 0;
    for (;; s++, n++) {
        char c = *s;
        if (c >= '0' && c <= '9') x = (u16)(x * 16 + (c - '0'));
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') x = (u16)(x * 16 + ((c | 0x20) - 'a' + 10));
        else break;
    }
    *ps = s;
    *v = x;
    return n;
}

/* the USB WDM bridge (Windows 98 / ME, inc/wdmbr.h): on unless SYSTEM.INI [WINUSB] says
   WDM=OFF; WDM=vvvv:pppp,... gives these devices to their Windows drivers even though WINUSB
   knows their class (vvvv:* every device of the vendor) */
static void wdm_setup(void)
{
    const char *s;
    usb_wdm_on = vmm_version() >= 0x40A;
    if (!usb_wdm_on || !profile_ok()) return;
    s = vmm_profile_string("WINUSB", "WDM", "");
    if (!s) return;
    if ((s[0] | 0x20) == 'o' && (s[1] | 0x20) == 'f') {
        usb_wdm_on = 0;
        kprintf("vxd: WDM bridge off (SYSTEM.INI)\n");
        return;
    }
    while (*s && usb_wdm_nids < 8) {
        u16 vid, pid = 0xFFFF;
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!hexval(&s, &vid)) break;
        if (*s == ':') {
            s++;
            if (*s == '*') s++;
            else if (!hexval(&s, &pid)) break;
        }
        usb_wdm_ids[usb_wdm_nids][0] = vid;
        usb_wdm_ids[usb_wdm_nids][1] = pid;
        usb_wdm_nids++;
        kprintf("vxd: USB device %04x:%04x goes to its Windows driver (WDM bridge)\n", vid, pid);
    }
}
#endif

/* ---- Windows 95/98/ME registry (VMM): the driver installed for a PCI function ---- */
extern u32 __cdecl vmm_reg_open(u32 key, const char *sub, u32 *out);
extern u32 __cdecl vmm_reg_close(u32 key);
extern u32 __cdecl vmm_reg_enum(u32 key, u32 i, char *name, u32 len);
extern u32 __cdecl vmm_reg_query(u32 key, const char *name, u32 *rsv, u32 *type, void *data, u32 *len);
#define HKLM 0x80000002UL

static int reg_string(const char *path, const char *name, char *out, u32 max)
{
    u32 k, len = max - 1;
    if (vmm_reg_open(HKLM, path, &k)) return -1;
    memset(out, 0, max);
    if (vmm_reg_query(k, name, NULL, NULL, out, &len)) out[0] = 0;
    vmm_reg_close(k);
    return out[0] ? 0 : -1;
}

/* 1 when the PCI function's devnode has this VxD as its device loader, or when that cannot
   be told (no registry yet, no devnode yet: Windows installs the driver later) */
int plat_pnp_ours(u32 bdf)
{
    static char path[160], name[96], drv[64], loader[32];
    u32 k, i;
    u16 ven = (u16)pci_rd32((int)(bdf >> 8), (int)((bdf >> 3) & 0x1F), (int)(bdf & 7), 0),
        dev = (u16)(pci_rd32((int)(bdf >> 8), (int)((bdf >> 3) & 0x1F), (int)(bdf & 7), 0) >> 16);
    char pre[24], inst[32];
    int found = 0;
    if (vmm_version() < 0x400) return 1;
    ksnprintf(pre, sizeof(pre), "VEN_%04X&DEV_%04X", ven, dev);
    ksnprintf(inst, sizeof(inst), "BUS_%02lX&DEV_%02lX&FUNC_%02lX", bdf >> 8, (bdf >> 3) & 0x1F, bdf & 7);
    if (vmm_reg_open(HKLM, "Enum\\PCI", &k)) return 1;
    for (i = 0; i < 256 && !found; i++) {
        memset(name, 0, sizeof(name));
        if (vmm_reg_enum(k, i, name, sizeof(name) - 1)) break;
        if (strncmp(name, pre, 17)) continue;
        ksnprintf(path, sizeof(path), "Enum\\PCI\\%s\\%s", name, inst);
        if (reg_string(path, "Driver", drv, sizeof(drv)) == 0) found = 1;
    }
    vmm_reg_close(k);
    if (!found) return 1;
    ksnprintf(path, sizeof(path), "System\\CurrentControlSet\\Services\\Class\\%s", drv);
    if (reg_string(path, "DevLoader", loader, sizeof(loader))) return 1;
    for (i = 0; loader[i]; i++)
        if (loader[i] >= 'a' && loader[i] <= 'z') loader[i] = (char)(loader[i] - 32);
#ifdef WIN_USB
    return !strncmp(loader, "WINUSB.VXD", 11);
#else
    return !strncmp(loader, "WINDISK.VXD", 12);
#endif
}

static int standalone_cb(void)
{
    return vxd_standalone_cb(NULL);
}

static void core_start(u32 vm, struct crs *c, int pnp)
{
    struct core_params p;
    u32 phys = 0, lin = 0, h, pages;
    sysvm = vm;
    cur_high = *(u32 *)(vm + 4);
    if (pnp) {
        /* Windows 95/98/ME Plug and Play: our own control block, options from the registry */
        if (!cb && standalone_cb()) return;
        kprintf("vxd: Plug and Play start (options %08lx/%04x)\n", cb->opts, cb->opts2);
    } else if (find_cb(c)) {
        /* no WINDISK.SYS / WINUSB.SYS loaded (Windows ME has no CONFIG.SYS): run on our own */
        if (standalone_cb()) return;
        kprintf("vxd: no DOS driver, standalone start (SYSTEM.INI options %08lx/%04x)\n", cb->opts, cb->opts2);
    } else {
        cb = (struct wincb *)(cur_high + ((cb_far >> 16) << 4) + (cb_far & 0xFFFF));
#ifdef WIN_AUDIO
        cb_from_dos = 1;
#endif
    }
    if (cb->magic != WIN_MAGIC) return;
    if (cb->opts & OPT_COM1) com_port = 0x3F8;
    if (cb->tsc_khz) {
        tsc_per_ms = cb->tsc_khz;
        tsc_per_us = tsc_per_ms / 1000;
        if (!tsc_per_us) tsc_per_us = 1;
        ms_base = vmm_system_time();
        tsc_base = cpu_rdtsc64();
    } else {
        calibrate_tsc();
    }
    /* 2 MB (WINUSB: 4 MB for camera frames) physically contiguous, fixed, below 4 GB, 64K aligned */
#ifdef WIN_USB
    for (pages = 1024; pages >= 128; pages /= 2) {
#else
    for (pages = 512; pages >= 128; pages /= 2) {
#endif
        /* PageUseAlign | PageContig | PageFixed */
        h = vmm_page_alloc(pages, 0x0F, 0xFFFFF, 0x02 | 0x04 | 0x08, &phys, &lin);
        if (h && lin) break;
    }
    if (!lin) {
        kprintf("vxd: cannot allocate DMA pool\n");
        return;
    }
    memset(&p, 0, sizeof(p));
    p.pool_lin = lin;
    p.pool_phys = phys;
    p.pool_size = pages * 4096UL;
    p.cb_lin = (u32)cb;
    p.mode = PM_VXD;
    if (pnp) p.flags = CPF_PNP;
#ifdef WIN_USB
    wdm_setup();
#endif
#if defined(WIN_DISK) || defined(WIN_USB)
    /* the BIOS drives, while the BIOS still runs its controllers */
    if (pnp) bios_scan();
#endif
    core_init(&p);
    core_up = 1;
#if defined(WIN_DISK) || defined(WIN_USB)
    if (pnp) i13_pnp_hook();
#endif
#ifdef WIN_DISK
    if (pnp) vds_fix_init();
#endif
#ifdef WIN_DISK
    if (!pnp) {
        fastdisk_init();
        int13_init();
        kprintf("vxd: VDS fix for BIOS disk code %s\n", vds_cb ? "active" : "not available");
    }
#endif
}

int vxd_core_up(void)
{
    return core_up && started;
}

void vxd_core_start_pnp(void)
{
    u32 vm = vmm_sys_vm();
    if (!core_up) core_start(vm, NULL, 1);
    if (!core_up) return;
    if (!tsc_per_us || !cb->tsc_khz) calibrate_tsc();
    started = 1;
    vmm_set_timeout(timer_ms, (void *)thk_timer, 0);
}

static int device_init(u32 vm, struct crs *c)
{
    if (!core_up) core_start(vm, c, 0);
#ifdef WIN_USB
    /* Windows 3.x: the PS/2 mouse becomes ours when WINMOUSE.DRV registers (Windows
       95/98/ME: through its Plug and Play devnode too) */
    if (vmm_version() < 0x400) sysvm = vm;
#endif
    if (!core_up) return 0;
    if (!tsc_per_us || !cb->tsc_khz) calibrate_tsc();
    started = 1;
    if (!standalone) {
        cb->vxd_entry = vmm_alloc_v86_cb((void *)thk_v86api, 0);
        if (cb->vxd_entry) cb->flags |= CBF_VXD;
        kprintf("vxd: V86 entry %08lx\n", cb->vxd_entry);
    }
    vmm_set_timeout(timer_ms, (void *)thk_timer, 0);
    return 0;
}

#ifdef WIN_AUDIO
/* ---- Sound Blaster emulation in MS-DOS boxes (and for Windows programs that program the
   card directly): the ports are trapped in every virtual machine, VDMAD hands over the
   emulated DMA channels, VPICD the interrupt. The card belongs to the machine that used it
   last. */
#include "sbemu.h"
extern void __cdecl thk_sbio(void);
extern void __cdecl thk_sbdma(void);
extern void __cdecl thk_sbeoi(void);
extern void __cdecl thk_sbhw(void);
extern int __cdecl vmm_install_io(u32 port, void *h);
extern void __cdecl vmm_untrap_io(u32 port);
extern int __cdecl vmm_remove_io(u32 port);
extern u32 __cdecl vdmad_phys_status(void);
extern u32 __cdecl vdmad_virtualize(u32 ch, void *cbf);
extern void __cdecl vdmad_get_state(u32 h, u32 vm, u32 *out);
extern void __cdecl vdmad_set_state(u32 h, u32 vm, u32 addr, u32 count, u32 modeflags);
extern void __cdecl vpicd_request(u32 h, u32 vm, u32 set);

static u32 sb_vm, sb_irqh, sb_dmah[2];
static struct vid sbvid;
static u32 sb_dlin[2], sb_dcnt[2], sb_dmode[2], sb_dunit[2];
static u16 sb_port[24];
static int sb_nport, sb_installed;
static u8 sb_line;

u32 plat_us(void)
{
    if (!tsc_per_us) return vmm_system_time() * 1000UL;
    return (u32)((cpu_rdtsc64() - tsc_base) / tsc_per_us);
}

u8 *plat_sb_mem(u32 lin, u32 len)
{
    (void)len;
    return lin ? (u8 *)lin : NULL;
}

void plat_sb_irq(int level)
{
    sb_line = (u8)level;
    if (sb_irqh && sb_vm) vpicd_request(sb_irqh, sb_vm, (u32)level);
}

/* The program reads the DMA counters through VDMAD, and VDMAD answers them from the 8237
   itself: the count register of the emulated channel follows the transfer (the channel
   stays masked, no device asks for it); the virtual state moves along. */
void plat_sb_dma(int which, u32 left, int tc)
{
    struct winsb *c = sb_cfg();
    int ch = which ? c->dma16 : c->dma8;
    u32 done, flags = 0;
    u16 cnt = (u16)(left ? left / sb_dunit[which] - 1 : 0xFFFF);
    if (!sb_dmah[which] || !sb_vm || !sb_dcnt[which]) return;
    done = sb_dcnt[which] - left;
    if (tc && !(sb_dmode[which] & 0x10) && !left) flags = 1;     /* DMA_masked */
    if (ch < 4) {
        outb(0x0C, 0);
        outb((u16)(ch * 2 + 1), (u8)cnt);
        outb((u16)(ch * 2 + 1), (u8)(cnt >> 8));
    } else {
        outb(0xD8, 0);
        outb((u16)(0xC2 + (ch - 4) * 4), (u8)cnt);
        outb((u16)(0xC2 + (ch - 4) * 4), (u8)(cnt >> 8));
    }
    vdmad_set_state(sb_dmah[which], sb_vm, sb_dlin[which] + done, left, (sb_dmode[which] & 0xFF) | (flags << 8));
}

/* Windows 95/98/ME: the status registers of the two 8237s (ports 08h, D0h) come from here
   instead of VDMAD, so the terminal count of an emulated channel shows: VDMAD's view of the
   hardware (its status bytes, read without taking them from VDMAD) plus the emulated
   channels. A terminal count bit is read once, as from the 8237. Commands are ignored. */
static u8 dst_prev[2];

static u32 dma_status(u32 port, u32 out)
{
    int k = port == 0xD0;
    u32 st;
    u8 hw, nw;
    if (out) return 0;
    st = vdmad_phys_status();
    hw = (u8)(k ? st >> 8 : st);
    nw = (u8)(hw & 0x0F & ~dst_prev[k]);
    dst_prev[k] = (u8)(hw & 0x0F);
    return (u8)((hw & 0xF0) | nw | sb_dma_status(k));
}

/* Windows 3.x (no Remove_IO_Handler): an IN AL from port 08h / D0h faults into VDMAD's
   trap; the fault hooks see it first, take VDMAD's answer through Simulate_IO and add the
   bits of the emulated channels */
extern u32 v86gp_prev, pmgp_prev;
extern void __cdecl thk_v86gp(void);
extern void __cdecl thk_pmgp(void);
extern int __cdecl vmm_hook_fault(u32 pm, u32 vec, void *handler, u32 *prev);
extern u32 __cdecl vmm_simulate_in(u32 vm, u32 port, void *client);
extern u32 __cdecl vmm_sel_base(u32 vm, u32 sel);

int __cdecl vxd_iofault(u32 vm, struct crs *c, u32 pm)
{
    u32 lin, port, len;
    u8 op;
    if (pm) {
        u32 base = vmm_sel_base(vm, c->cs);
        if (base == 0xFFFFFFFFUL) return 0;
        lin = base + c->eip;
    } else {
        lin = *(u32 *)(vm + 4) + ((u32)c->cs << 4) + (c->eip & 0xFFFF);
    }
    op = *(u8 *)lin;
    if (op == 0xE4) {
        port = *(u8 *)(lin + 1);
        len = 2;
    } else if (op == 0xEC) {
        port = c->edx & 0xFFFF;
        len = 1;
    } else {
        return 0;
    }
    if (port != 0x08 && port != 0xD0) return 0;
    c->eax = (c->eax & ~0xFFUL) | ((vmm_simulate_in(vm, port, c) | sb_dma_status(port == 0xD0)) & 0xFF);
    if (pm) c->eip += len;
    else c->eip = (c->eip & ~0xFFFFUL) | ((c->eip + len) & 0xFFFF);
    return 1;
}

u32 __cdecl vxd_sbio(u32 port, u32 val, u32 out, u32 vm)
{
    if (port == 0x08 || port == 0xD0) return dma_status(port, out);
    if (port < 0x388 || port > 0x38B) sb_vm = vm;
    return sb_io(port, val, (int)out);
}

/* VDMAD: the program changed the mask of an emulated channel. Address (linear) and count
   come in bytes: the count is the length of the transfer, not the 8237 register. */
void __cdecl vxd_sbdma(u32 h, u32 vm)
{
    u32 st[3];
    int w = h == sb_dmah[1];
    vdmad_get_state(h, vm, st);
    sb_vm = vm;
    sb_dmode[w] = st[2] & 0xFF;
    if (!(st[2] & 0x100)) {         /* unmasked: a new transfer */
        sb_dlin[w] = st[0];
        /* Windows 3.1 leaves garbage in the upper half */
        sb_dcnt[w] = st[1] > 0x20000UL ? st[1] & 0xFFFF : st[1];
        if (!sb_dcnt[w]) sb_dcnt[w] = 0x10000UL;
    }
    sb_vdma(w, sb_dlin[w], sb_dcnt[w], (sb_dmode[w] >> 4) & 1, (st[2] >> 8) & 1);
    kprintf("sb: DMA %d %s %08lx count %lx mode %02lx\n", w, (st[2] & 0x100) ? "masked" : "unmasked",
            st[0], st[1], st[2] & 0xFF);
}

/* edge-triggered like the ISA card: the request ends with the EOI */
void __cdecl vxd_sbeoi(u32 h, u32 vm)
{
    vpicd_request(h, vm, 0);
}

static void sb_port_add(u32 port)
{
    if (sb_nport >= (int)ARRAY_SIZE(sb_port)) return;
    if (vmm_install_io(port, (void *)thk_sbio)) {
        kprintf("sb: port %lx is taken\n", port);
        return;
    }
    sb_port[sb_nport++] = (u16)port;
}

int plat_sb_start(void)
{
    struct winsb *c = sb_cfg();
    u32 i;
    if (sb_installed) return 0;
    for (i = 0; i < 16; i++) sb_port_add(c->base + i);
    if (sb_nport < 16) {
        while (sb_nport) vmm_untrap_io(sb_port[--sb_nport]);
        kprintf("sb: ports %x-%x not available, no emulation\n", c->base, c->base + 15);
        return -1;
    }
    if (!(c->flags & SBF_NOOPL))
        for (i = 0; i < 4; i++) sb_port_add(0x388 + i);
    if (c->mpu) {
        sb_port_add(c->mpu);
        sb_port_add(c->mpu + 1);
    }
    if (vmm_version() >= 0x400) {
        /* the DMA status registers (terminal count of the emulated channels) */
        static const u16 stp[2] = { 0x08, 0xD0 };
        for (i = 0; i < 2; i++)
            if (vmm_remove_io(stp[i]) || vmm_install_io(stp[i], (void *)thk_sbio))
                kprintf("sb: DMA status port %x not taken over\n", stp[i]);
    } else if (vmm_hook_fault(0, 13, (void *)thk_v86gp, &v86gp_prev) ||
               vmm_hook_fault(1, 13, (void *)thk_pmgp, &pmgp_prev)) {
        kprintf("sb: general protection fault hooks refused\n");
    }
    sb_dunit[0] = 1;
    sb_dunit[1] = 2;
    sb_dmah[0] = vdmad_virtualize(c->dma8, (void *)thk_sbdma);
    if (c->dma16 >= 5) sb_dmah[1] = vdmad_virtualize(c->dma16, (void *)thk_sbdma);
    if (!sb_dmah[0]) kprintf("sb: DMA %d not available\n", c->dma8);
    memset(&sbvid, 0, sizeof(sbvid));
    sbvid.irq = c->irq == 2 ? 9 : c->irq;
    sbvid.options = 2;              /* VPICD_Opt_Can_Share */
    sbvid.hw_int = (u32)thk_sbhw;
    sbvid.eoi = (u32)thk_sbeoi;
    sbvid.iret_timeout = 500;
    sb_irqh = vpicd_virtualize(&sbvid);
    if (!sb_irqh) kprintf("sb: IRQ %d not available\n", c->irq);
    c->flags |= SBF_VXD;
    c->flags &= (u8)~(SBF_DOSTRAP | SBF_RTC);
    sb_installed = 1;
    return 0;
}

static void sb_vm_gone(u32 vm)
{
    if (sb_installed && vm == sb_vm) {
        sb_reset_owner();
        sb_vm = 0;
    }
}
#endif

extern char bss_begin[], bss_end[];
extern u32 __cdecl vmm_hook_cm_regdrv(void);
static int static_boot;         /* Windows 95/98/ME: loaded by SYSTEM.INI device= at the start */

u32 __cdecl vxd_control(u32 msg, u32 vm, u32 edx, u32 esi, u32 edi)
{
    (void)esi; (void)edi;
    /* /D: the life of the MS-DOS boxes (VM_Not_Executeable: EDX = the reason) */
    if (cb && (cb->opts & OPT_DEBUG) && msg >= 0x07 && msg <= 0x0C && vm != sysvm)
        kprintf("vxd: VM %08lx %s %08lx\n", vm, msg == 0x07 ? "create" : msg == 0x08 ? "critical init" :
                msg == 0x09 ? "init" : msg == 0x0A ? "terminate" : msg == 0x0B ? "not executeable" : "destroy", edx);
    switch (msg) {
    case 0x00:                  /* Sys_Critical_Init */
        memset(bss_begin, 0, (u32)(bss_end - bss_begin));
        vxd_refdata = edx;
#ifdef WIN_DISK
        vds_fix_init();         /* before any V86 disk I/O of Device_Init handlers */
        if (edx) core_start(vm, (struct crs *)*(u32 *)(vm + 8), 0);
#endif
        return 0;
    case 0x01:                  /* Device_Init */
#if defined(WIN_DISK) || defined(WIN_USB)
        if (vmm_version() >= 0x400 && !vxd_refdata) {
            /* Windows 95/98/ME, loaded by SYSTEM.INI before the I/O Supervisor: the disk
               (WINUSB: USB host) controllers that may carry BIOS drives are ours from now on,
               the INT 13h transfers of those drives too; the Configuration Manager hands the
               controllers over later as usual */
            dyn_state = DYN_INIT;
            static_boot = 1;
            kprintf("vxd: loaded at the start of Windows, VMM %lx\n", vmm_version());
            /* never unloaded: the hook stays for good */
            if (vmm_hook_cm_regdrv()) kprintf("vxd: CONFIGMG hook refused\n");
            pnp_init();
            vxd_core_start_pnp();
            if (core_up) kprintf("vxd: %d controller(s) of BIOS drives started\n", core_pnp_start_boot());
            return 0;
        }
#endif
        device_init(vm, (struct crs *)*(u32 *)(vm + 8));
        return 0;
    case 0x05:                  /* System_Exit */
#ifdef WIN_USB
        ps2_stop();
#endif
        /* loaded at the start: the I/O Supervisor writes the disk caches back after our
           System_Exit (and real-mode drives go through our INT 13h transfers until then):
           the controllers stop at Sys_Critical_Exit */
        if (static_boot) return 0;
        /* fall through */
    case 0x06:                  /* Sys_Critical_Exit */
        if (started) {
            started = 0;
            core_shutdown(0);
            cb->vxd_entry = 0;
            cb->flags &= ~CBF_VXD;
            cb->flags |= CBF_REINIT;
        }
        return 0;
    case 0x1B:                  /* Sys_Dynamic_Device_Init: loaded by the Configuration Manager */
        if (dyn_state == DYN_INIT) {
            /* loaded again for another devnode: everything stays as it is */
            kprintf("vxd: dynamic init again\n");
            return 0;
        }
        dyn_state = DYN_INIT;
        memset(bss_begin, 0, (u32)(bss_end - bss_begin));
        kprintf("vxd: dynamic load, VMM %lx\n", vmm_version());
        pnp_init();
        return 0;
    case 0x1C:                  /* Sys_Dynamic_Device_Exit */
        pnp_exit();
        if (started) {
            started = 0;
            core_shutdown(0);
        }
#ifdef WIN_AUDIO
        /* the port traps, DMA and interrupt hooks of the Sound Blaster emulation stay:
           the code must stay loaded */
        if (sb_installed) return 1;
#endif
        return 0;
    case 0x22:                  /* PNP_New_DevNode: EBX = devnode, EDX = load type */
        return pnp_new_devnode(vm, edx);
#ifdef WIN_AUDIO
    case 0x0C:                  /* Destroy_VM */
        sb_vm_gone(vm);
        return 0;
#endif
    }
    return 0;
}

/* lock key state of the keyboard focus VM (BIOS data area) -> USB LED bits */
u8 plat_kbd_leds(void)
{
    u32 vm = vmm_version() < 0x400 || vmm_init_state() >= 0x20000000UL ? vkd_kbd_owner() : 0;
    u8 f, leds;
    /* a VM still being created has no BIOS data area to read yet */
    if (!vm || (*(volatile u32 *)vm & (VMSTAT_CREATING | VMSTAT_NOT_EXECUTEABLE))) vm = sysvm;
    f = *(volatile u8 *)(*(u32 *)(vm + 4) + 0x417);
    leds = (u8)(((f >> 5) & 1) | (((f >> 6) & 1) << 1) | (((f >> 4) & 1) << 2));
#ifdef WIN_USB
    /* VKD sets the PS/2 keyboard's LEDs for its own keys only: for a while after a lock key
       of a USB keyboard, the PS/2 keyboard follows the lock state */
    if (lock_on && (kbd_ps2 || vmm_version() < 0x400)) {
        if (leds != ps2_leds &&
            ps2_kbd_leds((u8)(((leds >> 2) & 1) | ((leds & 1) << 1) | (((leds >> 1) & 1) << 2))) == 0)
            ps2_leds = leds;
        if ((s32)(plat_ms() - lock_ms) > 1500) lock_on = 0;
    }
#endif
    return leds;
}
