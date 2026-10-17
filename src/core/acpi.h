/* acpi.h - I2C devices from the ACPI tables (acpi.c) */
#ifndef ACPI_H
#define ACPI_H

struct acpi_i2c {
    u16 addr;                   /* 7-bit slave address */
    u8 hid;                     /* a HID over I2C device (PNP0C50) */
    u8 rsv;
    u32 speed;                  /* Hz */
    char path[32];              /* controller, e.g. \_SB.PCI0.I2C1 */
};

/* a device found by its _HID / _CID (acpi_find_devices) */
struct acpi_dev {
    char hid[9];
    char path[5];                   /* the device's name segment */
    u16 i2caddr[2];
    u32 i2cspeed;
    char i2cbus[32];                /* controller path of the first I2cSerialBus */
    u8 ni2c;
    u8 ngpio;
    struct { u16 pin; u8 io; u8 pull; char ctl[32]; } gpio[4];  /* GpioIo / GpioInt resources */
    u32 adr;                        /* _ADR */
};
int acpi_find_devices(const char *const *hids, struct acpi_dev *out, int max);

int acpi_scan_aml(const u8 *aml, u32 len, struct acpi_i2c *out, int n, int max);
int acpi_i2c_devices(struct acpi_i2c *out, int max, u16 *fmcn, u16 *sscn);
int acpi_path_adr(const char *path, u32 *adr);
int acpi_dev_info(const char *path, char *hid, u32 *uid, u32 *mem, int maxmem);
int acpi_name_int(const char *name, u32 *v);
/* device properties (_DSD): the integer / string after the key string, searched in the
   package of Name (scope, ...) when scope is not NULL; 0 when found */
int acpi_prop_int(const char *scope, const char *key, u32 *v);
int acpi_prop_str(const char *scope, const char *key, char *out, int n);
/* a Device () with this _ADR; up[] receives the _ADR of its parent, grandparent ... */
int acpi_find_adr(u32 adr, u32 *up, int n);

/* Atom / Broadwell devices in ACPI mode: values their methods fill in at run time.
   acpi_dev_mem: the Memory32Fixed ranges of the device's resource template with the bases and
   lengths _CRS stores into it (fields of the BIOS NVS area) resolved; the count, -1 when there
   is no such device. acpi_dev_irqs: the Interrupt () lines of the device. acpi_dev_field: the
   physical address of a field unit of the device's own SystemMemory region (e.g. PSAT, the
   power state register of its PCI configuration mirror). acpi_dev_pkg: the integers of
   Name (name, Package ()) inside the device; the count, -1 when there is none. */
int acpi_dev_mem(const char *path, u32 *base, u32 *len, int max);
int acpi_dev_irqs(const char *path);
int acpi_dev_field(const char *path, const char *name, u32 *phys);
int acpi_dev_pkg(const char *path, const char *name, u32 *v, int n);
/* acpi_dev_const: Name (name, integer) of the device, or Method (name) whose body is a single
   Return (integer) (e.g. _STA, _RMV); 0 when found, -1 when absent or computed at run time.
   acpi_dev_irq: the first line of the device's first Interrupt () descriptor; 0 when found.
   acpi_dev_prop: an integer device property (_DSD) inside the device; 0 when found. */
int acpi_dev_const(const char *path, const char *name, u32 *v);
int acpi_dev_irq(const char *path, u32 *irq);
int acpi_dev_prop(const char *path, const char *key, u32 *v);

/* HID over I2C devices (PNP0C50 / ACPI0C50) */
struct acpi_hidi2c {
    char seg[5];                    /* the device's name segment */
    char hid[9];                    /* its _HID when it is a Name */
    u16 addr[4];                    /* I2C addresses (alternative resource templates) */
    u32 naddr;
    u32 speed;                      /* Hz */
    char bus[32];                   /* controller of the first I2cSerialBus */
    u16 descreg;                    /* HID descriptor register from _DSM, FFFFh unknown */
    u16 gpin;                       /* GpioInt pin of gctl ("" without one) */
    char gctl[32];
    u8 gflags;                      /* interrupt flags: bit 0 edge, bits 1-2 polarity (1 low) */
};
int acpi_hid_i2c(struct acpi_hidi2c *out, int max);
#endif
