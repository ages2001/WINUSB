#!/usr/bin/env python3
"""oslayout.py <bin-layout.txt> <build output> <bin>: sorts the files a package build made
into the folders of the systems they are for (bin/DOS, bin/WIN3X, bin/WIN9X). Each line of
the layout file is a file (or directory) name followed by its folders."""
import os, shutil, sys

layout, out, bindir = sys.argv[1:4]
missing = 0
for line in open(layout):
    line = line.split('#')[0].split()
    if not line:
        continue
    name, dirs = line[0], line[1:]
    src = os.path.join(out, name)
    if not os.path.exists(src):
        print('oslayout: %s was not built' % name)
        missing += 1
        continue
    for d in dirs:
        dst = os.path.join(bindir, d, name)
        os.makedirs(os.path.join(bindir, d), exist_ok=True)
        if os.path.isdir(src):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
for d in sorted(os.listdir(bindir)):
    p = os.path.join(bindir, d)
    if os.path.isdir(p):
        print('%s: %s' % (d, ' '.join(sorted(os.listdir(p)))))
sys.exit(1 if missing else 0)
