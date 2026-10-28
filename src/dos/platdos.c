/* platdos.c - driver core platform layer for DOS real/V86 mode and Windows standard mode.
   The core runs in 32-bit ring-0 protected mode entered by WINDISK.SYS / WINUSB.SYS (raw or VCPI),
   with interrupts disabled for the duration of each call.
   Linear layout: 0-4MB conventional memory (as seen by DOS), 80000000h core window
   (page directory, page tables, image, pool), C0000000h MMIO windows. */
#include "core.h"

u32 plat_mode = PM_DOS;

#define CORE_WIN    0x80000000UL
#define PT_MMIO     ((u32 *)(CORE_WIN + 0x3000))
#define MMIO_BASE   0xC0000000UL

void core_resume(void);

static u32 tsc_per_ms, tsc_per_us;
static u64 tsc_base;
static u32 ms_base;
static int mmio_next;
static u16 com_port;
static s32 m_dx, m_dy, m_dz;
static u32 m_buttons, m_last_buttons;
static int m_dirty;

u32 rdtsc_lo(void);
#pragma aux rdtsc_lo = 0x0F 0x31 value [eax] modify exact [eax edx];
u64 rdtsc64(void);
#pragma aux rdtsc64 = 0x0F 0x31 value [edx eax] modify exact [eax edx];
void reload_cr3(void);
#pragma aux reload_cr3 = "mov eax,cr3" "mov cr3,eax" modify exact [eax];

static int scr_on;

/* /S: the log also goes to the text screen (BIOS data area cursor, scrolling), so a hang
   on a machine without a serial port shows its last step */
static void scr_putc(char c)
{
    volatile u8 *bda = (volatile u8 *)0x400;
    volatile u16 *vram;
    u8 mode = bda[0x49], page = bda[0x62];
    u32 cols = *(volatile u16 *)(bda + 0x4A), rows = (u32)bda[0x84] + 1, col, row, i, pos;
    u16 crtc;
    if (mode != 2 && mode != 3 && mode != 7) return;
    if (cols < 40 || cols > 132 || page > 7) return;
    if (rows < 25 || rows > 60) rows = 25;
    vram = (volatile u16 *)((mode == 7 ? 0xB0000UL : 0xB8000UL) + *(volatile u16 *)(bda + 0x4E));
    col = bda[0x50 + page * 2];
    row = bda[0x51 + page * 2];
    if (row >= rows) row = rows - 1;
    if (c == '\r') {
        col = 0;
    } else if (c == '\n') {
        row++;
    } else {
        vram[row * cols + col] = (u16)(0x0700 | (u8)c);
        if (++col >= cols) {
            col = 0;
            row++;
        }
    }
    if (row >= rows) {
        for (i = 0; i < (rows - 1) * cols; i++) vram[i] = vram[i + cols];
        for (i = 0; i < cols; i++) vram[(rows - 1) * cols + i] = 0x0720;
        row = rows - 1;
    }
    bda[0x50 + page * 2] = (u8)col;
    bda[0x51 + page * 2] = (u8)row;
    crtc = *(volatile u16 *)(bda + 0x63);
    if (crtc != 0x3D4 && crtc != 0x3B4) return;
    pos = row * cols + col + (*(volatile u16 *)(bda + 0x4E) >> 1);
    outb(crtc, 0x0E);
    outb((u16)(crtc + 1), (u8)(pos >> 8));
    outb(crtc, 0x0F);
    outb((u16)(crtc + 1), (u8)pos);
}

void plat_screen(int on)
{
    scr_on = on;
}

void plat_putc(char c)
{
    if (c == '\n') plat_putc('\r');
    if (scr_on) scr_putc(c);
    if (com_port) {
        int i;
        for (i = 0; i < 20000 && !(inb((u16)(com_port + 5)) & 0x20); i++) ;
        outb(com_port, (u8)c);
    } else {
        outb(0xE9, (u8)c);
    }
}

static u32 bios_ticks(void)
{
    return *(volatile u32 *)0x46C;
}

/* milliseconds from the TSC. The BIOS tick count is not usable: timer interrupts are
   lost while the core runs with interrupts disabled. */
u32 plat_ms(void)
{
    return ms_base + (u32)((rdtsc64() - tsc_base) / tsc_per_ms);
}

void plat_udelay(u32 us)
{
    u32 t0 = rdtsc_lo(), want = us * tsc_per_us;
    while (rdtsc_lo() - t0 < want) cpu_pause();
}

/* mappings already made are reused (the core probes the hardware again after Windows) */
static struct { u32 first, pages, lin; } mmio_map[64];
static int mmio_nmap;
static u32 *mmio_pt[4];
static int mmio_npt = 1;

void *plat_mmio(u32 phys, u32 len)
{
    u32 first = phys & ~4095UL, pages, i, lin;
    pages = ((phys + len + 4095) & ~4095UL) - first;
    pages >>= 12;
    for (i = 0; i < (u32)mmio_nmap; i++)
        if (first >= mmio_map[i].first && first + pages * 4096UL <= mmio_map[i].first + mmio_map[i].pages * 4096UL)
            return (void *)(mmio_map[i].lin + (first - mmio_map[i].first) + (phys & 4095));
    /* the window grows by page tables from the pool (4 MB each, 16 MB in all) */
    if (!mmio_pt[0]) mmio_pt[0] = PT_MMIO;
    while ((u32)mmio_next + pages > (u32)mmio_npt * 1024UL) {
        u32 *pt;
        if (mmio_npt >= (int)ARRAY_SIZE(mmio_pt) || !(pt = kmalloc_align(4096, 4096))) {
            kprintf("dos: MMIO window full\n");
            return NULL;
        }
        memset(pt, 0, 4096);
        ((u32 *)CORE_WIN)[768 + mmio_npt] = vphys(pt) | 3;
        mmio_pt[mmio_npt++] = pt;
    }
    lin = MMIO_BASE + mmio_next * 4096UL;
    for (i = 0; i < pages; i++) {
        u32 k = (u32)mmio_next + i;
        mmio_pt[k >> 10][k & 1023] = (first + i * 4096UL) | 0x1B;    /* present, rw, PWT, PCD */
    }
    mmio_next += pages;
    reload_cr3();
    if (mmio_nmap < (int)ARRAY_SIZE(mmio_map)) {
        mmio_map[mmio_nmap].first = first;
        mmio_map[mmio_nmap].pages = pages;
        mmio_map[mmio_nmap++].lin = lin;
    }
    return (void *)(lin + (phys & 4095));
}

void *plat_rm(u32 segoff)
{
    return (void *)(((segoff >> 16) << 4) + (segoff & 0xFFFF));
}

void *plat_sysrm(u32 segoff)
{
    return plat_rm(segoff);
}

/* the sound bridge: the drivers linked their control blocks at their start */
struct winbridge *plat_bridge_peer(void)
{
    struct winbridge *own = (struct winbridge *)((u8 *)cb + WIN_BROFF);
    if (!own->peer) return NULL;
    return (struct winbridge *)((u8 *)plat_rm(own->peer) + WIN_BROFF);
}

void *plat_bridge_ring(struct winbridge *p)
{
    return plat_mmio(p->phys, p->size + 32);
}

void plat_bridge_publish(struct winbridge *own)
{
    (void)own;
}

/* ---- 8042 injection queue, drained by WINDISK.SYS / WINUSB.SYS in real mode ----
   The control block holds 20 entries; more wait here (a burst of keys, a mouse packet
   stream) and move up whenever the driver has drained some: no byte is dropped, so no key
   release is lost. */
#define KQ2LEN 256
static u16 kq2[KQ2LEN];
static u16 kq2_head, kq2_tail;

static int kq_hw_space(void)
{
    int used = (cb->kq_head - cb->kq_tail + WIN_KQLEN) % WIN_KQLEN;
    return WIN_KQLEN - 1 - used;
}

static void kq_move(void)
{
    while (kq2_tail != kq2_head && kq_hw_space() > 0) {
        cb->kq[cb->kq_head] = kq2[kq2_tail];
        cb->kq_head = (u8)((cb->kq_head + 1) % WIN_KQLEN);
        kq2_tail = (u16)((kq2_tail + 1) % KQ2LEN);
    }
}

static int kq_space(void)
{
    return KQ2LEN - 1 - (int)((kq2_head - kq2_tail + KQ2LEN) % KQ2LEN) + kq_hw_space();
}

static void kq_put(u8 cmd, u8 data)
{
    u16 nh;
    kq_move();
    if (kq2_head == kq2_tail && kq_hw_space() > 0) {
        cb->kq[cb->kq_head] = (u16)(((u16)cmd << 8) | data);
        cb->kq_head = (u8)((cb->kq_head + 1) % WIN_KQLEN);
        return;
    }
    nh = (u16)((kq2_head + 1) % KQ2LEN);
    if (nh == kq2_tail) return;
    kq2[kq2_head] = (u16)(((u16)cmd << 8) | data);
    kq2_head = nh;
}

/* push one queued byte into the controller if its output buffer is free */
static void kq_drain(void)
{
    static u32 aux_since;
    u16 e;
    int i;
    u8 st;
    kq_move();
    if (cb->kq_head == cb->kq_tail) return;
    st = inb(0x64);
    if ((st & 0x21) == 0x21) {
        /* mouse data nobody reads (aux interrupt off) would block the keyboard forever */
        if (!aux_since) aux_since = plat_ms() | 1;
        else if (plat_ms() - aux_since > 300) {
            (void)inb(0x60);
            aux_since = 0;
        }
        return;
    }
    aux_since = 0;
    if (st & 0x03) return;
    e = cb->kq[cb->kq_tail];
    outb(0x64, (u8)(e >> 8));
    for (i = 0; i < 10000 && (inb(0x64) & 0x02); i++) ;
    outb(0x60, (u8)e);
    cb->kq_tail = (u8)((cb->kq_tail + 1) % WIN_KQLEN);
}

void plat_key(const u8 *sc, int n)
{
    if (cb->opts & OPT_NOKBC) return;
    if (kq_space() < n) {
        kprintf("dos: key queue full\n");
        return;
    }
    while (n--) kq_put(0xD2, *sc++);
    kq_drain();
}

static int clamp(s32 v, s32 lim)
{
    if (v > lim) return (int)lim;
    if (v < -lim) return (int)-lim;
    return (int)v;
}

/* Mouse wheel -> arrow keys. Windows 1.0x / 2.x and their programs (and plain DOS software)
   predate the wheel and have no scroll message, but they do respond to the cursor keys, so a
   wheel notch is injected as a few Up / Down key strokes into the 8042 the same way the USB
   keyboard is. A DOS program that understands the INT 33h wheel API turns this off for itself
   (cb->wheel_ctl bit 0) and reads the wheel directly instead. */
static u8 wheel_mode  = 1;      /* 0 off, 1 Up/Down arrows, 2 PgUp/PgDn */
static u8 wheel_lines = 3;      /* key strokes per notch */
static u8 wheel_inv;            /* reverse the scroll direction */

static void wheel_to_keys(int dz)
{
    u8 key, b[4];
    int up, n;
    if (!wheel_mode || !dz)
        return;
    up = dz > 0;
    if (wheel_inv) up = !up;
    if (dz < 0) dz = -dz;
    if (dz > 4) dz = 4;                         /* a fast spin should not flood the queue */
    if (wheel_mode == 2) key = up ? 0x49 : 0x51;        /* PgUp / PgDn */
    else                 key = up ? 0x48 : 0x50;        /* Up / Down */
    for (n = dz * wheel_lines; n > 0; n--) {
        if (kq_space() < 4)
            break;
        b[0] = 0xE0; b[1] = key;                /* extended make */
        b[2] = 0xE0; b[3] = (u8)(key | 0x80);   /* extended break */
        plat_key(b, 4);
    }
}

int plat_pnp_ours(u32 bdf)
{
    (void)bdf;
    return 1;
}

#ifdef WIN_USB
#include "usb.h"
/* ---- PS/2 touchpad (Synaptics / ELAN, ps2tp.c) in DOS mode ----
   WINUSB.SYS queues the aux bytes it catches on IRQ 12 (cbx->q) while cbx->mode is set; the
   core decodes them and hands the motion to the DOS mouse driver as packets for its pointing
   device BIOS callback (cbx->pk), to INT 33h and to WINMOUSE.DRV (standard mode) through the
   mailbox. Pointing device BIOS calls reach the touchpad itself (a reset puts it back into its
   mouse mode): it is programmed again once they are over. */
static u8 aux_state;                /* 0 not ours, 1 touchpad on PS/2 */
static u8 aux_probed, aux_wastp;    /* probed once; a touchpad was found */
static u8 aux_bioscnt, aux_dirty;
static u32 aux_bios_ms, aux_check_ms, aux_good0, aux_bad0;

static struct wincbx *cbx(void)
{
    return (struct wincbx *)((u8 *)cb + WIN_CBSIZE);
}

static void aux_packets(void)
{
    struct wincbx *x = cbx();
    int z4 = x->psize == 4;
    if (!(cb->flags & CBF_AUXBIOS) || !x->cbfn) {
        m_dx = m_dy = m_dz = 0;
        m_dirty = 0;
        m_last_buttons = m_buttons;
        return;
    }
    while (m_dirty && ((x->phead + 1) & (WIN_AUXPK - 1)) != x->ptail) {
        int dx = clamp(m_dx, 255), dy = clamp(-m_dy, 255), dz = clamp(-m_dz, 7);
        u8 *p = x->pk[x->phead];
        p[0] = (u8)(0x08 | (m_buttons & 7) | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0));
        p[1] = (u8)dx;
        p[2] = (u8)dy;
        p[3] = (u8)(z4 ? dz & 0x0F : 0);
        x->phead = (u8)((x->phead + 1) & (WIN_AUXPK - 1));
        m_dx -= dx;
        m_dy += dy;
        if (z4) m_dz += dz;
        else m_dz = 0;
        m_last_buttons = m_buttons;
        m_dirty = (m_dx || m_dy || m_dz);
    }
}

/* the touchpad's bytes from the queue */
int plat_aux_irq(void)
{
    struct wincbx *x;
    if (!aux_state) return 0;
    x = cbx();
    while (x->qtail != x->qhead) {
        u8 b = x->q[x->qtail];
        x->qtail = (u8)((x->qtail + 1) & (WIN_AUXQ - 1));
        if (aux_state == 1) ps2tp_byte(b);
    }
    return 1;
}

/* keyboard bytes that came while the touchpad was being programmed: back through the queue */
static void dos_kbd_sink(const u8 *b, int n)
{
    while (n-- > 0) kq_put(0xD2, *b++);
    kq_drain();
}

static void aux_start(void)
{
    struct wincbx *x = cbx();
    u8 ccb;
    int found = 0;
    if (aux_state || (cb->opts & OPT_NOKBC) || (cb->opts2 & OPT2_NOTP) || !(cb->opts & OPT_USB)) return;
    /* a DOS mouse driver already runs the mouse and no touchpad was found before: the probe
       would only undo the driver's settings (rate, resolution, wheel mode) */
    if ((cb->flags & CBF_AUXBIOS) && aux_probed && !aux_wastp) return;
    kbc_kbd_sink = dos_kbd_sink;
    kbc_stray = NULL;
    if (kbc_begin(&ccb)) return;
    if (!(aux_cmd(0xF5) && aux_cmd(0xF5)) && !aux_cmd(0xF6)) found = ps2tp_probe(&kbc_io, plat_mouse);
    /* PS/2 even for a pad with an SMBus side: its bytes come by interrupt, SMBus would only be
       polled at the timer rate here */
    aux_probed = 1;
    aux_wastp = (u8)(found != 0);
    if (found) {
        if (!aux_cmd(0xF4)) aux_state = 1;
    } else {
        /* the knocks may have left another device in a special mode: power-on state again */
        if (!aux_cmd(0xFF)) {
            kbc_read(1, 1000);          /* AAh */
            kbc_read(1, 30);            /* 00h */
        }
        if (cb->flags & CBF_AUXBIOS) aux_cmd(0xF4);    /* the DOS driver's mouse streams again */
    }
    kbc_end(ccb, aux_state || (ccb & 2));
    if (!aux_state) return;
    x->qhead = x->qtail = 0;
    x->phead = x->ptail = 0;
    aux_bioscnt = x->bioscnt;
    aux_dirty = 0;
    aux_check_ms = plat_ms();
    aux_good0 = ps2tp_stats(&aux_bad0);
    x->mode = 1;
    outb(0xA1, (u8)(inb(0xA1) & ~0x10));          /* IRQ 12 */
    outb(0x21, (u8)(inb(0x21) & ~0x04));          /* cascade */
    kprintf("dos: PS/2 touchpad %s\n", ps2tp_name());
}

/* the touchpad back in its mouse mode for the BIOS, Windows and the DOS drivers */
static void aux_stop(void)
{
    u8 ccb;
    if (!aux_state) return;
    cbx()->mode = 0;
    ps2tp_off();
    aux_state = 0;
    kbc_stray = NULL;
    if (kbc_begin(&ccb)) return;
    aux_cmd(0xF5);
    if (!aux_cmd(0xFF)) {
        kbc_read(1, 1000);              /* AAh */
        kbc_read(1, 30);                /* 00h */
    }
    if (cb->flags & CBF_AUXBIOS) aux_cmd(0xF4);
    kbc_end(ccb, 1);
}

static void aux_reprogram(void)
{
    u8 ccb;
    int ok;
    kbc_stray = NULL;
    if (kbc_begin(&ccb)) return;
    ok = ps2tp_reprogram(&kbc_io) && !aux_cmd(0xF4);
    if (!ok) {
        kprintf("dos: the PS/2 touchpad does not answer, left to the BIOS\n");
        cbx()->mode = 0;
        ps2tp_off();
        aux_state = 0;
        if (cb->flags & CBF_AUXBIOS) aux_cmd(0xF4);
    }
    kbc_end(ccb, 1);
    aux_good0 = ps2tp_stats(&aux_bad0);
    aux_check_ms = plat_ms();
}

/* after the pointing device BIOS calls have settled, and when only garbage comes (someone
   reset the pad behind our back) */
void plat_aux_poll(void)
{
    struct wincbx *x;
    u32 now, n, bad;
    if (!aux_state) return;
    x = cbx();
    plat_aux_irq();
    now = plat_ms();
    if (x->bioscnt != aux_bioscnt) {
        aux_bioscnt = x->bioscnt;
        aux_bios_ms = now;
        aux_dirty = 1;
    }
    if (aux_dirty && !x->bios && now - aux_bios_ms > 150) {
        aux_dirty = 0;
        aux_reprogram();
        return;
    }
    if (now - aux_check_ms > 1000) {
        n = ps2tp_stats(&bad);
        if (n == aux_good0 && bad - aux_bad0 >= 12) {
            kprintf("dos: the PS/2 touchpad left its protocol, programming it again\n");
            aux_reprogram();
            return;
        }
        aux_good0 = n;
        aux_bad0 = bad;
        aux_check_ms = now;
    }
}
#else
int plat_aux_irq(void)
{
    return 0;
}

void plat_aux_poll(void)
{
}
#endif

/* convert accumulated motion into PS/2 packets */
static void ps2_flush(void)
{
    int wheel = (cb->flags & CBF_WHEEL4) != 0;
    int need = wheel ? 4 : 3;
    if (cb->flags & CBF_MBOX) {
        /* WINMOUSE.DRV in standard mode: no 8042 round trip, the driver reads the mailbox */
        if (m_dirty) {
            cb->mb_dx = (short)clamp((s32)cb->mb_dx + m_dx, 32000);
            cb->mb_dy = (short)clamp((s32)cb->mb_dy + m_dy, 32000);
            cb->mb_dz = (signed char)clamp((s32)cb->mb_dz + m_dz, 127);
            cb->mb_btn = (u8)(m_buttons & 0x1F);
            /* presses are latched: a click shorter than a timer tick is not lost */
            cb->mb_flags |= (u8)((((m_buttons & ~m_last_buttons) & 7) << 4) |
                                 (((m_buttons & ~m_last_buttons) & 0x18) >> 2));
            cb->mb_seq++;
        }
        m_dx = m_dy = m_dz = 0;
        m_dirty = 0;
        m_last_buttons = m_buttons;
        kq_drain();
        return;
    }
#ifdef WIN_USB
    if (aux_state) {
        /* the PS/2 touchpad is ours: packets straight to the DOS driver's BIOS callback */
        aux_packets();
        return;
    }
#endif
    if (!(cb->flags & (CBF_AUXWIN | CBF_AUXBIOS))) {
        /* nobody reads the aux port: drop the motion instead of filling the 8042 */
        m_dx = m_dy = m_dz = 0;
        m_dirty = 0;
        m_last_buttons = m_buttons;
        kq_drain();
        return;
    }
    while (m_dirty && kq_space() >= need) {
        int dx = clamp(m_dx, 255), dy = clamp(-m_dy, 255), dz = clamp(-m_dz, 7);
        u8 b0 = (u8)(0x08 | (m_buttons & 7));
        if (dx < 0) b0 |= 0x10;
        if (dy < 0) b0 |= 0x20;
        kq_put(0xD3, b0);
        kq_put(0xD3, (u8)dx);
        kq_put(0xD3, (u8)dy);
        if (wheel) kq_put(0xD3, (u8)dz);
        m_dx -= dx;
        m_dy += dy;
        if (wheel) m_dz += dz;
        else m_dz = 0;
        m_last_buttons = m_buttons;
        m_dirty = (m_dx || m_dy || m_dz);
    }
    kq_drain();
}

void plat_mouse(int dx, int dy, int dz, int dw, int buttons)
{
    (void)dw;
    if (cb->opts & OPT_NOKBC) return;
    if (dz && !(cb->wheel_ctl & 1))
        wheel_to_keys(dz);              /* scroll through the cursor keys (pre-wheel software) */
    m_dx += dx; m_dy += dy; m_dz += dz;
    m_buttons = (u32)buttons;
    if (dx || dy || dz || m_buttons != m_last_buttons) m_dirty = 1;
    ps2_flush();
}

/* Tablet: WINMOUSE.DRV in standard mode takes the position as it is; DOS mouse drivers only
   know relative motion, which is made from the change of position (1024 steps across). */
static int abs_have;
static int abs_x, abs_y;

void plat_mouse_abs(int x, int y, int dz, int dw, int buttons)
{
    if (cb->opts & OPT_NOKBC) return;
    if (cb->flags & CBF_MBOX) {
        cb->mb_absx = (u16)x;
        cb->mb_absy = (u16)y;
        cb->mb_flags |= 1;
        m_dz += dz;
        m_buttons = (u32)buttons;
        m_dirty = 1;
        ps2_flush();
        return;
    }
    x >>= 6;
    y >>= 6;
    if (!abs_have) {
        abs_have = 1;
        abs_x = x;
        abs_y = y;
    }
    plat_mouse(x - abs_x, abs_y - y, dz, dw, buttons);
    abs_x = x;
    abs_y = y;
}

int plat_irq_attach(int irq)
{
    if (irq <= 0 || irq > 15 || irq == 2) return -1;
    cb->irqmask |= (u16)(1U << irq);
    return 0;
}

void plat_irq_detach(int irq)
{
    if (irq <= 0 || irq > 15) return;
    cb->irqmask &= (u16)~(1U << irq);
    if (irq < 8) outb(0x21, (u8)(inb(0x21) | (1 << irq)));
    else outb(0xA1, (u8)(inb(0xA1) | (1 << (irq - 8))));
}

void plat_kick(void)
{
    cb->flags |= CBF_KICK;
}

void plat_unit_changed(int unit)
{
    (void)unit;
}

/* PIT channel 0 count (latched) */
static u16 pit0(void)
{
    u8 lo, hi;
    outb(0x43, 0x00);
    lo = inb(0x40);
    hi = inb(0x40);
    return (u16)(lo | (hi << 8));
}

/* TSC calibration against PIT channel 2 (10 ms one-shot). Where its output never shows in
   port 61h, channel 0 (the timer tick, always running) is counted instead. */
static void calibrate(void)
{
    u32 t0, t1, n = 0;
    u8 v = inb(0x61);
    outb(0x61, (u8)((v & ~0x02) | 0x01));
    outb(0x43, 0xB0);                   /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, (u8)(11932 & 0xFF));
    outb(0x42, (u8)(11932 >> 8));
    t0 = rdtsc_lo();
    while (!(inb(0x61) & 0x20) && ++n < 500000UL) ;
    t1 = rdtsc_lo();
    outb(0x61, v);
    if (n >= 500000UL) {
        u32 clocks = 0;
        u16 prev, cur;
        int sq;
        outb(0x43, 0xE2);               /* read back the status of channel 0: its mode */
        sq = ((inb(0x40) >> 1) & 3) == 3;     /* square wave: two counts per clock */
        prev = pit0();
        t0 = rdtsc_lo();
        for (n = 0; clocks < 11932UL && n < 5000000UL; n++) {
            cur = pit0();
            clocks += (u16)(prev - cur) >> sq;
            prev = cur;
        }
        t1 = rdtsc_lo();
        if (clocks) t1 = t0 + (u32)(((u64)(t1 - t0) * 11932UL) / clocks);
    }
    tsc_per_ms = (t1 - t0) / 10;
    tsc_per_us = tsc_per_ms / 1000;
    if (!tsc_per_ms) tsc_per_ms = 1;
    if (!tsc_per_us) tsc_per_us = 1;
}

static void session(void)
{
}

static void session_end(void)
{
    if (cb) {
        kq_move();
        ps2_flush();
        cb->pollcnt++;
    }
}

#ifdef WIN_AUDIO
/* ---- Sound Blaster emulation: WINAUDIO.SYS traps the ports through the QPI of QEMM or
   JEMM + QPIEMU and passes each access here; the RTC periodic interrupt services the sound
   between timer ticks and delivers the interrupt to the program ---- */
#include "audio.h"
#include "sbemu.h"

u32 plat_us(void)
{
    return (u32)((rdtsc64() - tsc_base) / tsc_per_us);
}

/* the DMA addresses of a real-mode program are its linear addresses */
u8 *plat_sb_mem(u32 lin, u32 len)
{
    if (!lin || lin + len > 0x110000UL) return NULL;
    return (u8 *)lin;
}

void plat_sb_irq(int level)
{
    struct winsb *c = sb_cfg();
    if (level && !c->irqline) c->irqedge = 1;
    c->irqline = (u8)level;
}

void plat_sb_dma(int which, u32 left, int tc)
{
    (void)which; (void)left; (void)tc;
}

int plat_sb_start(void)
{
    return 0;
}

/* port access: port | value << 16 | out << 24 */
u32 __cdecl ent_sbio(u32 arg)
{
    return sb_io(arg & 0xFFFF, (arg >> 16) & 0xFF, (int)((arg >> 24) & 1));
}

/* RTC tick */
u32 __cdecl ent_sbtick(u32 arg)
{
    static u8 low;
    (void)arg;
    if (!low) {
        low = 1;
        aeng_lowlat(1);
    }
    aeng_tick();
    return 0;
}
#else
u32 __cdecl ent_vmstart(u32 arg)
{
    (void)arg;
    return 1;
}

u32 __cdecl ent_sbio(u32 arg)
{
    (void)arg;
    return 0xFF;
}

u32 __cdecl ent_sbtick(u32 arg)
{
    (void)arg;
    return 0;
}
#endif

/* ---- entry points listed in the WINCORE.BIN header ---- */
u32 __cdecl ent_init(struct core_params *p)
{
    struct wincb *c = (struct wincb *)p->cb_lin;
    u32 r;
    if (c->opts & OPT_COM1) {
        com_port = 0x3F8;
        outb(0x3FB, 0x80); outb(0x3F8, 1); outb(0x3F9, 0);   /* 115200 8N1 */
        outb(0x3FB, 0x03); outb(0x3FC, 0x03); outb(0x3FA, 0xC7);
    }
    scr_on = (c->opts & OPT_SCREEN) != 0;
    calibrate();
    tsc_base = rdtsc64();
    ms_base = bios_ticks() * 55UL;
    session();
    c->tsc_khz = tsc_per_ms;
    r = (u32)core_init(p);
    kprintf("dos: TSC %lu kHz\n", tsc_per_ms);
#ifdef WIN_USB
    if (!r) aux_start();
#endif
    session_end();
    return r;
}

u32 __cdecl ent_filedone(u32 arg)
{
    u32 r;
    (void)arg;
    session();
    r = (u32)core_filedone();
    session_end();
    return r;
}

#ifdef WIN_AUDIO
#include "dsp.h"
/* ---- firmware files (Intel audio DSP): WINAUDIO.SYS loads them into XMS ---- */
static struct winfw *fw_table(void)
{
    return (struct winfw *)((u8 *)cb + CB_FWTAB);
}

void plat_fw_request(const char *name)
{
    struct winfw *t = fw_table();
    int i;
    for (i = 0; i < WIN_MAXFW; i++)
        if (t[i].state && !strncmp(t[i].name, name, 13)) return;
    for (i = 0; i < WIN_MAXFW && t[i].state; i++) ;
    if (i == WIN_MAXFW) return;
    strlcpy(t[i].name, name, sizeof(t[i].name));
    t[i].state = WFW_REQ;
    cb->flags |= CBF_FWREQ;
}

int plat_fw_load(const char *name, struct fwimg *img)
{
    struct winfw *t = fw_table();
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
#endif

u32 __cdecl ent_dos(u32 pkt)
{
    session();
    core_dos((u8 *)pkt);
    session_end();
    return 0;
}

/* Timer poll. Polls are spaced by at least 40 ms of real time: a poll that ran long with
   interrupts disabled makes virtual machines replay the missed timer ticks back to back,
   and polling on every replayed tick would starve DOS. */
u32 __cdecl ent_poll(u32 arg)
{
    static u32 next_poll;
    (void)arg;
    session();
    if ((s32)(plat_ms() - next_poll) < 0 && !(cb->flags & CBF_KICK)) {
        kq_move();
        return 0;
    }
    cb->flags &= ~CBF_KICK;
    core_poll();
    next_poll = plat_ms() + 40;
    session_end();
    return 0;
}

u32 __cdecl ent_irq(u32 irq)
{
    u32 r;
    session();
    cb->irqcnt++;
    r = (u32)core_irq((int)irq);
    if (r == 1) core_poll();
    session_end();
    return r;
}

u32 __cdecl ent_api(u32 req)
{
    struct winapireq *r = (struct winapireq *)req;
    session();
    r->status = (u16)core_api(r);
    session_end();
    return r->status;
}

u32 __cdecl ent_shutdown(u32 how)
{
    session();
#ifdef WIN_USB
    aux_stop();
#endif
    core_shutdown((int)how);
    return 0;
}

u32 __cdecl ent_resume(u32 arg)
{
    (void)arg;
    session();
    core_resume();
#ifdef WIN_USB
    aux_start();
#endif
    session_end();
    return 0;
}

/* lock key state from the BIOS data area -> USB LED bits (num 1, caps 2, scroll 4) */
u8 plat_kbd_leds(void)
{
    u8 f = *(volatile u8 *)0x417;
    return (u8)(((f >> 5) & 1) | (((f >> 6) & 1) << 1) | (((f >> 4) & 1) << 2));
}
