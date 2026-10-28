/* kbc.c - the 8042 keyboard controller seen from the mouse side: commands to the auxiliary
   (PS/2 mouse / touchpad) device with the aux interrupt off, polled answers. Keyboard bytes
   that arrive meanwhile are kept and handed back afterwards: to kbc_kbd_sink (the platform's
   keyboard queue: one byte at a time once the owner has read the previous one, or VKD), else
   through the controller (D2h, as if the keyboard had sent them); aux bytes that are no answer go to kbc_stray (the packet decoder
   while the mouse runs). Used by the VxD (ps2.c) and by the DOS core (platdos.c). */
#include "usb.h"

#define KBC_DATA    0x60
#define KBC_STAT    0x64
#define ST_OBF      0x01
#define ST_IBF      0x02
#define ST_AUX      0x20
#define CCB_AINT    0x02
#define CCB_ADIS    0x20

void (*kbc_stray)(u8 b);
void (*kbc_kbd_sink)(const u8 *b, int n);
static u8 kbd_save[32];
static int kbd_nsave;

int kbc_wait_in(void)
{
    u32 i;
    for (i = 0; i < 50000; i++) {
        if (!(inb(KBC_STAT) & ST_IBF)) return 0;
        plat_udelay(2);
    }
    return -1;
}

int kbc_cmd(u8 c)
{
    if (kbc_wait_in()) return -1;
    outb(KBC_STAT, c);
    return 0;
}

int kbc_write(u8 c, u8 d)
{
    if (kbc_cmd(c) || kbc_wait_in()) return -1;
    outb(KBC_DATA, d);
    return 0;
}

static void other(u8 st, u8 b)
{
    if (!(st & ST_AUX)) {
        if (kbd_nsave < (int)sizeof(kbd_save)) kbd_save[kbd_nsave++] = b;
    } else if (kbc_stray) {
        kbc_stray(b);
    }
}

/* next byte of the wanted source (aux != 0: the aux device, else keyboard / controller) */
int kbc_read(int aux, u32 ms)
{
    u32 i;
    for (i = 0; i < ms * 20; i++) {
        u8 st = inb(KBC_STAT);
        if (st & ST_OBF) {
            u8 b = inb(KBC_DATA);
            if (!(st & ST_AUX) == !aux) return b;
            other(st, b);
            continue;
        }
        plat_udelay(50);
    }
    return -1;
}

void kbc_flush(void)
{
    int n;
    for (n = 0; n < 32 && (inb(KBC_STAT) & ST_OBF); n++) {
        u8 st = inb(KBC_STAT);
        other(st, inb(KBC_DATA));
    }
}

void kbc_kbd_restore(void)
{
    int i;
    if (kbc_kbd_sink && kbd_nsave) {
        /* a key release must never get lost (a Shift that stays down): the bytes go through
           the queue, the owner reads one before the next one is injected */
        kbc_kbd_sink(kbd_save, kbd_nsave);
        kbd_nsave = 0;
        return;
    }
    for (i = 0; i < kbd_nsave; i++) {
        u32 n;
        kbc_write(0xD2, kbd_save[i]);
        for (n = 0; n < 2000 && (inb(KBC_STAT) & ST_OBF); n++) plat_udelay(10);
    }
    kbd_nsave = 0;
}

void kbc_kbd_forget(void)
{
    kbd_nsave = 0;
}

/* a command to the aux device; 0 when acknowledged (FAh), resent on FEh */
int aux_cmd(u8 c)
{
    int r, tries;
    for (tries = 0; tries < 3; tries++) {
        if (kbc_write(0xD4, c)) return -1;
        r = kbc_read(1, 30);
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

int aux_read(u32 ms)
{
    return kbc_read(1, ms);
}

/* keyboard interface off, the command byte read, aux interrupt off and aux clock on;
   -1: no controller */
int kbc_begin(u8 *ccb)
{
    int c;
    kbd_nsave = 0;
    if (kbc_cmd(0xAD)) return -1;
    kbc_flush();
    if (kbc_cmd(0x20)) return -1;
    c = kbc_read(0, 30);
    if (c < 0) return -1;
    *ccb = (u8)c;
    kbc_write(0x60, (u8)(c & ~(CCB_AINT | CCB_ADIS)));
    kbc_cmd(0xA8);
    return 0;
}

void kbc_end(u8 ccb, int aux_int)
{
    ccb &= (u8)~(CCB_AINT | CCB_ADIS);
    if (aux_int) ccb |= CCB_AINT;
    kbc_write(0x60, ccb);
    kbc_cmd(0xAE);
    kbc_kbd_restore();
}

const struct ps2io kbc_io = { aux_cmd, aux_read };
