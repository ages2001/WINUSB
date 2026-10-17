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
static u8 keyq[64];
static volatile u32 keyq_head, keyq_tail;
static u16 com_port;

/* mouse delivery to WINMOUSE.DRV */
static u32 mcb_sel, mcb_off;    /* PM callback in the system VM */
static volatile s32 m_dx, m_dy, m_dz, m_dw;
static volatile u32 m_buttons, m_last_buttons;
static volatile u32 m_usb_buttons, m_ps2_buttons;
static volatile u32 m_latch;    /* buttons pressed since the last delivery (short clicks) */
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
    while (n--) {
        u32 nh = (keyq_head + 1) % sizeof(keyq);
        if (nh == keyq_tail) break;
        keyq[keyq_head] = *sc++;
        keyq_head = nh;
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
    m_latch |= nb & ~m_buttons;
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

void vxd_input_init(void)
{
    if (!sysvm) sysvm = vmm_sys_vm();
}

static int kbd_ps2;

void vxd_kbd_start(void)
{
    kbd_ps2 = 1;
}

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
        /* a press released again before it was delivered is delivered first, the release
           follows in the next round */
        b = m_buttons | (m_latch & ~m_last_buttons);
        m_latch = 0;
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
        /* Windows 95/98/ME: a PS/2 mouse whose devnode has Windows' driver (VMOUSE with
           MSMOUSE.VXD) is taken over too when WINMOUSE.DRV is the mouse driver */
        if (vmm_version() >= 0x400 && !ps2_status()) ps2_start(12);
        ps2_reclaim();
#endif
        mcb_sel = c->ecx & 0xFFFF;
        mcb_off = c->edx & 0xFFFF;
        m_dx = m_dy = m_dz = m_dw = 0;
        m_last_buttons = m_buttons;
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
        cb = vmm_heap_alloc(sizeof(struct wincb));
        if (!cb) return -1;
        memset(cb, 0, sizeof(struct wincb));
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
    /* Windows 3.x: the PS/2 mouse is ours (Windows 95/98/ME: through its Plug and Play devnode) */
    if (vmm_version() < 0x400) {
        sysvm = vm;
        ps2_start(12);
    }
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

extern char bss_begin[], bss_end[];
extern u32 __cdecl vmm_hook_cm_regdrv(void);
static int static_boot;         /* Windows 95/98/ME: loaded by SYSTEM.INI device= at the start */

u32 __cdecl vxd_control(u32 msg, u32 vm, u32 edx, u32 esi, u32 edi)
{
    (void)esi; (void)edi;
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
        return 0;
    case 0x22:                  /* PNP_New_DevNode: EBX = devnode, EDX = load type */
        return pnp_new_devnode(vm, edx);
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
