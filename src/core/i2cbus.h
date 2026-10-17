/* i2cbus.h - I2C masters (dwi2c.c: Synopsys DesignWare controllers of the Intel LPSS and the
   AMD FCH) and GPIO pads (gpio.c) for the codec drivers and the HID over I2C touchpads */
#ifndef I2CBUS_H
#define I2CBUS_H
#ifndef CORE_H
#include "core.h"
#endif
#include "acpi.h"

struct i2cbus;
struct i2cbus *i2c_bus_get(const char *acpipath);       /* \_SB.PCI0.I2C1 -> its controller */
struct i2cbus *i2c_bus_pci(struct pcidev *p);           /* an LPSS I2C function -> its controller */
int i2c_xfer(struct i2cbus *b, int addr, const u8 *w, int wn, u8 *r, int rn);
u32 i2c_bus_id(struct i2cbus *b);                       /* PCI bdf, else 0x10000 | ACPI index */
void i2c_bus_off(void);                                 /* controllers disabled, forgotten */

/* GPIO pads: level of a pad (0 / 1, -1 when unknown), output level */
int gpio_set(const char *ctlpath, u16 pin, int val);
int gpio_get(const char *ctlpath, u16 pin);
#endif
