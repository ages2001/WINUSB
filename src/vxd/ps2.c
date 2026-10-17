/* ps2.c - PS/2 mouse on the 8042 auxiliary port, driven by the VxD (Windows 3.x 386 enhanced
   mode and Windows 95/98/ME). The mouse is switched to IntelliMouse Explorer mode (wheel and
   buttons 4 and 5, 4-byte packets) or IntelliMouse mode (wheel) when it answers to the knock
   sequences, and its packets take the path of the USB mice: vxd_ps2_mouse() -> WINMOUSE.DRV.
   Synaptics and ELAN touchpads are switched to their absolute protocols first (ps2tp.c:
   two-finger scrolling, taps, clickpads); one that has an SMBus companion (Synaptics
   InterTouch, ELAN) may run on SMBus instead (smbtp.c), its PS/2 side then stays silent.
   The 8042 itself is driven by kbc.c. The keyboard stays with VKD; ps2_kbd_leds() sets the
   LEDs of the PS/2 keyboard. */
#include "vxd.h"
#include "usb.h"

#define KBC_DATA    0x60
#define KBC_STAT    0x64
#define ST_OBF      0x01            /* output buffer full */
#define ST_IBF      0x02            /* input buffer full */
#define ST_AUX      0x20            /* the output buffer holds mouse data */
#define CCB_KINT    0x01            /* command byte: keyboard interrupt */
#define CCB_AINT    0x02            /* aux interrupt */
#define CCB_ADIS    0x20            /* aux clock disabled */

static struct vid ps2vid;
static u32 ps2_irqh;
static int ps2_irqno;
static volatile int ps2_on;
static u8 ps2_id;                   /* 0 standard, 3 IntelliMouse, 4 IntelliMouse Explorer,
                                       5 touchpad (ps2tp.c), 6 touchpad on SMBus (smbtp.c) */
static u8 pkt[4];
static u8 pkt_pos, pkt_len = 3;
static u32 pkt_ms;
static u32 n_pkts, n_bad;


static void ps2_byte(u8 b);
extern void __cdecl thk_int15(void);
extern u32 __cdecl vmm_hook_v86_int(u32 n, void *f);
static int int15_hooked;
static u32 n_bioscalls;

/* aux bytes that are no answer while the mouse runs: packet bytes */
static void stray(u8 b)
{
    if (ps2_on) ps2_byte(b);
}

static int aux_cmd2(u8 c, u8 arg)
{
    return aux_cmd(c) || aux_cmd(arg) ? -1 : 0;
}

static int aux_id(void)
{
    if (aux_cmd(0xF2)) return -1;
    return kbc_read(1, 30);
}

static int ps2_begin(u8 *ccb)
{
    kbc_stray = stray;
    return kbc_begin(ccb);
}

static void ps2_end(u8 ccb, int aux_int)
{
    kbc_end(ccb, aux_int);
}

static int set_rate(u8 r)
{
    return aux_cmd2(0xF3, r);
}

/* probe and program the mouse: -1 none, else the device id in use */
static int ps2_program(void)
{
    int id;
    if (aux_cmd(0xF5) && aux_cmd(0xF5)) return -1;      /* streaming off */
    if (aux_cmd(0xF6)) return -1;                       /* defaults */
    if (!(cb && (cb->opts2 & OPT2_NOTP)) && ps2tp_probe(&kbc_io, vxd_ps2_mouse)) {
        u8 sa;
        int k = ps2tp_kind(&sa);
        if (sa && !(cb && (cb->opts2 & OPT2_NOSMB)) && smbtp_start(k, sa, vxd_ps2_mouse) == 0) {
            ps2tp_off();                                /* the PS/2 side stays silent */
            return 6;
        }
        if (aux_cmd(0xF4)) return -1;                   /* streaming on */
        return 5;
    }
    /* IntelliMouse knock (wheel), then IntelliMouse Explorer knock (wheel, buttons 4 and 5) */
    set_rate(200); set_rate(100); set_rate(80);
    id = aux_id();
    if (id == 3) {
        set_rate(200); set_rate(200); set_rate(80);
        id = aux_id();
    }
    if (id != 3 && id != 4) id = 0;
    set_rate(100);
    aux_cmd2(0xE8, 3);                                  /* 8 counts per mm */
    aux_cmd(0xE6);                                      /* scaling 1:1 */
    if (aux_cmd(0xF4)) return -1;                       /* streaming on */
    return id;
}

/* ---- packets ---- */
static void ps2_packet(void)
{
    u8 b0 = pkt[0];
    int dx, dy, dz = 0, dw = 0, btn;
    if (b0 & 0xC0) {                    /* overflow: the motion is meaningless */
        n_bad++;
        return;
    }
    btn = b0 & 7;
    dx = (int)pkt[1] - ((b0 & 0x10) ? 256 : 0);
    dy = -((int)pkt[2] - ((b0 & 0x20) ? 256 : 0));     /* PS/2: up is positive */
    if (ps2_id == 3) {
        dz = -(int)(s8)pkt[3];
    } else if (ps2_id == 4) {
        u8 b3 = pkt[3];
        switch (b3 & 0xC0) {
        case 0x80:                      /* 6-bit vertical scroll (IntelliMouse Explorer 4.0) */
            dz = (int)(b3 & 0x20) - (int)(b3 & 0x1F);
            break;
        case 0x40:                      /* 6-bit horizontal scroll, positive = right */
            dw = (int)(b3 & 0x20) - (int)(b3 & 0x1F);
            break;
        default:                        /* 4-bit wheel, buttons 4 and 5 */
            dz = (int)(b3 & 0x08) - (int)(b3 & 0x07);
            if (b3 & 0x10) btn |= 8;
            if (b3 & 0x20) btn |= 16;
        }
    }
    n_pkts++;
    vxd_ps2_mouse(dx, dy, dz, dw, btn);
}

static void ps2_byte(u8 b)
{
    u32 now;
    if (ps2_id >= 5) {
        if (ps2_id == 5) ps2tp_byte(b);
        return;
    }
    now = plat_ms();
    /* a gap inside a packet: start over (bytes of one packet follow each other closely) */
    if (pkt_pos && now - pkt_ms > 40) pkt_pos = 0;
    pkt_ms = now;
    if (pkt_pos == 0 && !(b & 0x08)) {  /* the first byte always has bit 3 set */
        n_bad++;
        return;
    }
    pkt[pkt_pos++] = b;
    if (pkt_pos < pkt_len) return;
    pkt_pos = 0;
    ps2_packet();
}

/* VPICD hardware interrupt (vxd_hwint): mouse bytes in the controller are ours, keyboard
   bytes are left to VKD */
u32 ps2_hwint(u32 h)
{
    int n = 0;
    if (!ps2_on || h != ps2_irqh) return 1;
    while (n < 16) {
        u8 st = inb(KBC_STAT);
        if ((st & (ST_OBF | ST_AUX)) != (ST_OBF | ST_AUX)) break;
        ps2_byte(inb(KBC_DATA));
        n++;
    }
    if (!n) return 1;
    vpicd_phys_eoi(ps2_irqh);
    return 0;
}

/* INT 15h AH=C2h, the pointing device BIOS, while the mouse is ours: every call succeeds
   without touching the mouse, so that Windows' own mouse drivers (VMOUSE / MSMOUSE.VXD through
   the BIOS), DOS mouse drivers and the BIOS do not switch it back to 3-byte packets. They get
   no data: the mouse reaches the programs through WINMOUSE.DRV. */
u32 __cdecl vxd_int15(u32 vm, struct crs *c)
{
    u8 al = (u8)c->eax;
    (void)vm;
    if (!ps2_on || ((c->eax >> 8) & 0xFF) != 0xC2) return 1;
    if (!n_bioscalls++) kprintf("ps2: pointing device BIOS call %04lx answered by WINUSB\n", c->eax & 0xFFFF);
    switch (al) {
    case 0x01:                                  /* reset: self test passed, device id 0 */
        c->ebx = (c->ebx & 0xFFFF0000UL) | 0x00AA;
        break;
    case 0x04:                                  /* device id */
        c->ebx &= 0xFFFF00FFUL;
        break;
    case 0x06:
        if (((c->ebx >> 8) & 0xFF) == 0) {      /* status: enabled, 100 reports/s, 8 counts/mm */
            c->ebx = (c->ebx & 0xFFFFFF00UL) | 0x20;
            c->ecx = (c->ecx & 0xFFFFFF00UL) | 3;
            c->edx = (c->edx & 0xFFFFFF00UL) | 100;
        }
        break;
    case 0x00: case 0x02: case 0x03: case 0x05: case 0x07:
        break;
    default:
        c->eax = (c->eax & 0xFFFF00FFUL) | 0x0100;     /* invalid function */
        c->eflags |= 1;
        return 0;
    }
    c->eax &= 0xFFFF00FFUL;
    c->eflags &= ~1UL;
    return 0;
}

/* Windows' mouse drivers may share IRQ 12 too (MSMOUSE.VXD reads the mouse through the BIOS
   after we have started): VPICD calls the handler virtualized last first, so the IRQ is
   virtualized again once they are set up (WINMOUSE.DRV registering is that moment). */
void ps2_reclaim(void)
{
    u32 f, h;
    if (!ps2_on || !ps2_irqh) return;
    f = irq_save();
    vpicd_force_default(ps2_irqh);
    h = vpicd_virtualize(&ps2vid);
    ps2_irqh = h;
    if (h) vpicd_unmask(h);
    irq_restore(f);
    if (!h) {
        ps2_on = 0;
        kprintf("ps2: IRQ %d lost, the PS/2 mouse is left to Windows\n", ps2_irqno);
    }
}

int ps2_is_irq(u32 h)
{
    return ps2_irqh && h == ps2_irqh;
}

u32 ps2_status(void)
{
    if (!ps2_on) return 0;
    return 1 | (ps2_id >= 3 ? 2 : 0) | (ps2_id == 4 ? 4 : 0) | (ps2_id >= 5 ? 8 : 0);
}

int ps2_start(int irq)
{
    u32 f;
    u8 ccb;
    int id;
    if (ps2_on) return 0;
    if (cb && (cb->opts2 & OPT2_NOPS2)) {
        kprintf("ps2: /NOPS2, the PS/2 mouse is left to Windows\n");
        return -1;
    }
    if (irq <= 0 || irq > 15) irq = 12;
    if (ps2_irqh && ps2_irqno != irq) return -1;
    vxd_input_init();
    if (!int15_hooked && vmm_hook_v86_int(0x15, (void *)thk_int15) == 0) int15_hooked = 1;
    if (!ps2_irqh) {
        /* shared: VKD (Windows 3.x) keeps its own handler on IRQ 12 for the BIOS mouse */
        memset(&ps2vid, 0, sizeof(ps2vid));
        ps2vid.irq = (u16)irq;
        ps2vid.options = 2;             /* VPICD_Opt_Can_Share */
        ps2vid.hw_int = (u32)thk_hwint;
        ps2vid.iret_timeout = 500;
        ps2_irqh = vpicd_virtualize(&ps2vid);
        if (!ps2_irqh) {
            kprintf("ps2: IRQ %d is not shareable, the PS/2 mouse is left to Windows\n", irq);
            return -1;
        }
        ps2_irqno = irq;
    }
    f = irq_save();
    if (ps2_begin(&ccb)) {
        irq_restore(f);
        kprintf("ps2: no keyboard controller\n");
        return -1;
    }
    id = ps2_program();
    pkt_pos = 0;
    ps2_id = (u8)(id < 0 ? 0 : id);
    pkt_len = (u8)(ps2_id ? 4 : 3);
    ps2_on = id >= 0;
    ps2_end(ccb, 1);
    irq_restore(f);
    if (id < 0) {
        kprintf("ps2: no PS/2 mouse\n");
        return -1;
    }
    vpicd_unmask(ps2_irqh);
    kprintf("ps2: %s%s on IRQ %d\n", id == 6 ? "touchpad on SMBus, " : id == 5 ? "touchpad " : "",
            id >= 5 ? ps2tp_name() : id == 4 ? "IntelliMouse Explorer (wheel, 5 buttons)" :
            id == 3 ? "IntelliMouse (wheel)" : "standard mouse", irq);
    return 0;
}

/* the mouse back in its power-on mode (3-byte packets) for the BIOS and DOS mouse drivers */
void ps2_stop(void)
{
    u32 f;
    u8 ccb;
    if (!ps2_on) return;
    ps2_on = 0;
    kprintf("ps2: stop (%lu packets, %lu bad bytes)\n", n_pkts, n_bad);
    if (ps2_id == 6) smbtp_stop();
    ps2tp_off();
    f = irq_save();
    if (!ps2_begin(&ccb)) {
        aux_cmd(0xF5);
        if (!aux_cmd(0xFF)) {
            kbc_read(1, 1000);          /* AAh: self test passed */
            kbc_read(1, 30);            /* 00h: device id */
        }
        ps2_end(ccb, 1);
    }
    irq_restore(f);
}

/* LEDs of the PS/2 keyboard (bit 0 Scroll, 1 Num, 2 Caps). The acknowledges are read here,
   so VKD and the BIOS never see them. */
int ps2_kbd_leds(u8 leds)
{
    u32 f = irq_save();
    int r = -1;
    kbc_kbd_forget();
    kbc_stray = stray;
    kbc_flush();
    if (!kbc_wait_in()) {
        outb(KBC_DATA, 0xED);
        if (kbc_read(0, 30) == 0xFA && !kbc_wait_in()) {
            outb(KBC_DATA, (u8)(leds & 7));
            if (kbc_read(0, 30) == 0xFA) r = 0;
        }
    }
    kbc_kbd_restore();
    irq_restore(f);
    return r;
}
