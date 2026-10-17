#!/bin/sh
# run.sh: host tests of the touchpad code (about a second): PS/2 Synaptics and ELAN models,
# RMI4 and ELAN on an i801 SMBus model, HID over I2C with _DSM / GpioInt, the ACPI scan
cd "$(dirname "$0")"
T=${TMPDIR:-/tmp}/tptest.$$
mkdir -p $T
python3 - <<'PY'
import importlib.util, struct
spec = importlib.util.spec_from_file_location('u', '../../qemu/uredir.py'); u = importlib.util.module_from_spec(spec); spec.loader.exec_module(u)
open('rd.bin', 'wb').write(u.PTP_RD)
d = u.PTP(); out = open('rep.bin', 'wb')
for t, cs, b in d.make_script()[:200]:
    r = d.report(cs, b) if cs else bytes([1] + [0] * 18) + struct.pack('<HBB', 0, 1, 0)
    out.write(struct.pack('<IB', int(t * 1000), len(r)) + r)
def pl(n):                                      # PkgLength of a body of n bytes (itself included)
    if n + 1 < 64: return bytes([n + 1])
    n += 2
    return bytes([0x40 | (n & 15), n >> 4])
def i2c(addr, speed, path):
    body = bytes([2, 0, 1, 2]) + struct.pack("<HBH", 0, 1, 6) + struct.pack("<IH", speed, addr) + path.encode() + b"\0"
    return bytes([0x8E]) + struct.pack("<H", len(body)) + body
def gpioint(pin, ctl, flags):
    name = ctl.encode() + b"\0"
    body = bytes([1, 0]) + struct.pack("<HHBHH", 0, flags, 0, 0, 0) + struct.pack("<HBHHH", 23, 0, 25, 0, 0) + struct.pack("<H", pin) + name
    return bytes([0x8C]) + struct.pack("<H", len(body)) + body
def rt(d):
    data = d + bytes([0x79, 0]); buf = bytes([0x0A, len(data)]) + data
    return bytes([0x11]) + pl(len(buf)) + buf
def name(n, v): return b"\x08" + n.encode() + v
def dev(n, body):
    b = n.encode() + body
    return bytes([0x5B, 0x82]) + pl(len(b)) + b
def method(n, args, body):
    b = n.encode() + bytes([args]) + body
    return bytes([0x14]) + pl(len(b)) + b
def iff(pred, body):
    b = pred + body
    return bytes([0xA0]) + pl(len(b)) + b
uuid = bytes([0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE])
touuid = bytes([0x11]) + pl(18) + bytes([0x0A, 0x10]) + uuid
dsm1 = method("_DSM", 4, iff(b"\x93\x68" + touuid, iff(b"\x93\x6A\x00", b"\xA4\x11\x03\x0A\x01\x03") + iff(b"\x93\x6A\x01", b"\xA4\x0A\x20")))
dsm2 = method("_DSM", 4, iff(b"\x93\x68" + touuid, b"\x08_T_0\x99\x6A\x00" + iff(b"\x93_T_0\x01", b"\xA4HIDA")))
aml = dev("TPD0", name("_HID", b"\x0dSYNA1234\x00") + name("_CID", b"\x0dPNP0C50\x00") +
          name("SBFB", rt(i2c(0x2C, 400000, "\\_SB.PCI0.I2C1"))) + name("SBFG", rt(gpioint(0x35, "\\_SB.GPI0", 2))) + dsm1)
aml += dev("TPD1", name("_HID", b"\x0dELAN0001\x00") + method("_CID", 0, b"\xA4\x0c\x41\xd0\x0c\x50") + name("HIDA", b"\x01") +
           name("SBF1", rt(i2c(0x15, 400000, "\\_SB.PCI0.I2C0"))) + name("SBF2", rt(i2c(0x2C, 400000, "\\_SB.PCI0.I2C0"))) + dsm2)
aml += dev("ALS0", name("_HID", b"\x0dACPI0008\x00") + name("_CRS", rt(i2c(0x1E, 100000, "\\_SB.PCI0.I2C0"))))
open('aml.bin', 'wb').write(aml)
PY
r=0
for t in ps2test smbtest i2ctest acpitest; do
  gcc -w -O1 -o $T/$t $t.c || r=1
  $T/$t || r=1
done
rm -rf $T; [ -n "$KEEP" ] || rm -f rd.bin rep.bin aml.bin
exit $r
