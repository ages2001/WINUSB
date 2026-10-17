/* tp.h - host harness stubs for the touchpad code (ptp.c, ps2tp.c, smbtp.c, i2c.c, acpi.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define RD32(p) (*(volatile u32 *)(p))
#define WR32(p, v) (*(volatile u32 *)(p) = (u32)(v))
#define kprintf(...) do { if (verbose) printf(__VA_ARGS__); } while (0)
#define DBG(...) do { if (verbose > 1) printf(__VA_ARGS__); } while (0)
#define DBG2(...) do { if (verbose > 2) printf(__VA_ARGS__); } while (0)
#define ksnprintf snprintf
#define OPT_NOI2C 0x80000000UL
#define OPT_DEBUG 0x20
#define CORE_H
#define USB_H
#define ACPI_H
#define I2CBUS_H
static int verbose;
static u32 now_us;
static u32 plat_ms(void) { now_us += 2; return now_us / 1000; }
static void plat_udelay(u32 us) { now_us += us; }
static void udelay(u32 us) { now_us += us; }
static void mdelay(u32 ms) { now_us += ms * 1000; }
static void *kmalloc(u32 n) { return calloc(1, n); }
static void kfree(void *p) { free(p); }
static u32 irq_save(void) { return 0; }
static void irq_restore(u32 f) { (void)f; }
#define strlcpy(d, s, n) do { u32 n_ = (n); if (n_) { strncpy((d), (s), n_ - 1); (d)[n_ - 1] = 0; } } while (0)
struct pcidev { u8 bus, dev, fn, irq; u16 vendor, device; u8 cls, sub, prog, rev; u32 bar[6]; };
#define PCI_BDF(p) (((u32)(p)->bus << 8) | ((p)->dev << 3) | (p)->fn)
struct wincb { u32 opts; u16 opts2; };
static struct wincb cbx_, *cb = &cbx_;
/* mouse events collected by the tests */
static int ev_n, ev_dx, ev_dy, ev_dz, ev_dw, ev_btn, ev_clicks[8], ev_lastbtn;
static void plat_mouse(int dx, int dy, int dz, int dw, int b)
{
    int i;
    ev_n++; ev_dx += dx; ev_dy += dy; ev_dz += dz; ev_dw += dw;
    for (i = 0; i < 3; i++) if ((b & (1 << i)) && !(ev_lastbtn & (1 << i))) ev_clicks[i]++;
    ev_lastbtn = b; ev_btn = b;
    if (verbose > 1) printf("  mouse %d %d wheel %d %d btn %d\n", dx, dy, dz, dw, b);
}
static void ev_reset(void) { ev_n = ev_dx = ev_dy = ev_dz = ev_dw = ev_btn = ev_lastbtn = 0; memset(ev_clicks, 0, sizeof(ev_clicks)); }
/* usb.h parts */
struct ptp;
struct ptp_contact { u8 id; s32 x, y; };
struct ps2io { int (*cmd)(u8 c); int (*read)(u32 ms); };
struct hiddev;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
