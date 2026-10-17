/* w4gplat.c - platform services of WIN4G.VXD: the PCI relocation of pci.c running in
   Sys_Critical_Init of Windows 95/98/ME. SYSTEM.INI [WIN4G] Debug=1 writes the log to
   port E9h (Bochs/QEMU debug console), Debug=COM1 to the first serial port. All4G=1 moves the
   resources of every device, not only of the ones WINDRV drives. */
#include "core.h"

u32 plat_mode = PM_VXD;
struct wincb *cb;
extern char bss_begin[], bss_end[];
u32 __cdecl w4g_map(u32 phys, u32 len);
const char *__cdecl w4g_profile(const char *section, const char *key, const char *def);
static u16 com;
static u8 e9;

void plat_putc(char c)
{
    if (e9) outb(0xE9, (u8)c);
    if (com) {
        int i;
        for (i = 0; i < 10000 && !(inb((u16)(com + 5)) & 0x20); i++) ;
        outb(com, (u8)c);
    }
}

void plat_udelay(u32 us)
{
    /* port 80h reads take about 1 us on every PC chipset */
    while (us--) inb(0x80);
}

void *plat_mmio(u32 phys, u32 len)
{
    u32 lin = w4g_map(phys, len);
    return lin == 0xFFFFFFFFUL ? NULL : (void *)lin;
}

void __cdecl w4g_init(void)
{
    const char *d;
    memset(bss_begin, 0, (u32)(bss_end - bss_begin));
    d = w4g_profile("WIN4G", "Debug", "0");
    if (d && d[0] == '1') e9 = 1;
    if (d && (d[0] == 'C' || d[0] == 'c')) {
        com = 0x3F8;
        outb(0x3FB, 0x80);
        outb(0x3F8, 1);             /* 115200 baud */
        outb(0x3F9, 0);
        outb(0x3FB, 3);
        outb(0x3FA, 0);
    }
    dbg_level = (e9 || com) ? 1 : 0;
    log_init();
    d = w4g_profile("WIN4G", "All4G", "0");
    kprintf("WIN4G: PCI resources above 4G\n");
    pci_relocate_all(d && d[0] == '1');
}
