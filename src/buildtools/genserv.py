import re, glob, os, sys
inc = sys.argv[1]
ids = {}
for f in glob.glob(inc + "/*.INC"):
    for line in open(f, errors="replace"):
        m = re.match(r"\s*(\w+)_Device_ID\s+EQU\s+([0-9A-Fa-f]+)h", line)
        if m: ids[m.group(1)] = int(m.group(2), 16)
out = ["/* Generated from Windows 3.1 DDK *.INC service tables */", "#ifndef VXDSVC_H", "#define VXDSVC_H"]
for f in sorted(glob.glob(inc + "/*.INC")):
    lines = open(f, errors="replace").read().splitlines()
    dev = None; n = 0
    for line in lines:
        m = re.match(r"\s*Begin_Service_Table\s+(\w+)", line)
        if m and m.group(1) != "VxD":
            dev = m.group(1); n = 0; continue
        if dev and re.match(r"\s*End_Service_Table", line):
            dev = None; continue
        if dev:
            m = re.match(r"\s*" + dev + r"_Service\s+(\w+)", line, re.I)
            if m:
                did = ids.get(dev)
                if did is None:
                    for k in ids:
                        if k.lower() == dev.lower(): did = ids[k]
                out.append("#define SVC_%s 0x%08X" % (m.group(1), (did << 16) | n))
                n += 1
for k, v in sorted(ids.items(), key=lambda x: x[1]):
    out.append("#define %s_DEVICE_ID 0x%04X" % (k.upper(), v))
out.append("#endif")
open(sys.argv[2], "w").write("\r\n".join(out) + "\r\n")
if len(sys.argv) > 3:
    inc = ["; Generated from Windows 3.1 DDK *.INC service tables"]
    for l in out:
        if l.startswith("#define") and len(l.split()) == 3:
            _, k, v = l.split()
            inc.append("%%define %s %s" % (k, v))
    open(sys.argv[3], "w").write("\r\n".join(inc) + "\r\n")
print(len(out))
