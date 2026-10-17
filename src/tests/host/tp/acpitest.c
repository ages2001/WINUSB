/* acpitest.c - host harness: acpi_hid_i2c() on an AML blob (aml.bin) with two HID over I2C
   devices: _DSM returning a constant / a named object, GpioInt, alternative addresses */
#include "tp.h"
#undef ACPI_H
static void *plat_mmio(u32 p, u32 l) { (void)p; (void)l; return NULL; }
#include "../../../core/acpi.h"
#include "../../../core/acpi.c"
int main(int argc, char **argv)
{
    static u8 a[8192];
    FILE *f = fopen("aml.bin", "rb");
    struct acpi_hidi2c o[4];
    int n, i;
    if (argc > 1) verbose = atoi(argv[1]);
    aml_tab[0] = a;
    aml_len[0] = (u32)fread(a, 1, sizeof a, f);
    naml = 1;
    n = acpi_hid_i2c(o, 4);
    for (i = 0; i < n; i++)
        printf("acpi: %s %s addr %02x/%02x (%u) bus %s %u Hz desc %04x gpio %s %u flags %02x\n", o[i].seg, o[i].hid,
               o[i].addr[0], o[i].addr[1], o[i].naddr, o[i].bus, o[i].speed, o[i].descreg, o[i].gctl, o[i].gpin,
               o[i].gflags);
    CHECK(n == 2, "devices %d", n);
    CHECK(n > 0 && o[0].addr[0] == 0x2C && o[0].descreg == 0x20 && o[0].gpin == 0x35 && o[0].gflags == 2 &&
          !strcmp(o[0].gctl, "\\_SB.GPI0") && !strcmp(o[0].bus, "\\_SB.PCI0.I2C1"), "TPD0");
    CHECK(n > 1 && o[1].naddr == 2 && o[1].addr[1] == 0x2C && o[1].descreg == 0x0001, "TPD1");
    printf("acpi: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
