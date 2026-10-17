#!/bin/bash
# build.sh - builds the WINDRV packages whose sources are present, with Open Watcom v2 and NASM
#   WINDISK: WINDISK.SYS WINDCORE.BIN WINDISK.386 WINDISK.VXD WINDISK.PDR  (IDE, AHCI, NVMe, SD)
#   WINUSB : WINUSB.SYS  WINUCORE.BIN WINUSB.386 WINMOUSE.DRV WINWHEEL.EXE USBTREE.EXE WINCAM.EXE (+ sound programs)
#   WINAUDIO: WINAUDIO.SYS WINACORE.BIN WINAUDIO.386 WINSOUND.DRV WINMIX.EXE  (HD Audio, AC'97)
#   WINDISK and WINUSB: WININFO.EXE DSKBENCH.EXE; each: its installer and uninstaller
# Output goes to ../bin, or to ../bin/disk, ../bin/usb and ../bin/audio when more than one
# package is built. The sound programs (WINSOUND.DRV, WINMIX.EXE, AUDTEST.EXE) go to WINAUDIO
# and to WINUSB (USB Audio devices).
# In a package repository (src/bin-layout.txt present) the files are built into obj/out and
# then sorted into bin/DOS, bin/WIN3X and bin/WIN9X by the systems they are for.
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OBJ=${OBJ:-$SRC/../obj}
LAYOUT=$SRC/bin-layout.txt
if [ -f $LAYOUT ]; then
  BINDIR=${OUT:-$SRC/../bin}
  OUT=$OBJ/out
fi
OUT=${OUT:-$SRC/../bin}
export WATCOM=${WATCOM:-$SRC/../ow}
export PATH=$WATCOM/binl64:$PATH
export INCLUDE=$WATCOM/h
TOOLS=$SRC/buildtools
[ -d $TOOLS ] || TOOLS=$SRC/../tools
FIXVXD=$TOOLS/fixvxd.py
FWSRC=$SRC/firmware
[ -d $FWSRC ] || FWSRC=$SRC/../firmware

COMMON="winlib pci acpi blk corent"
DISKSRC="ide ahci nvme cdrom sdhci"
USBSRC="usb uhci ohci ehci xhci hub hid ptp ps2tp kbc smbtp i2c dwi2c gpio msc uaudio uvc aeng synth nvme nhi tbt icm pcihp cdrom"
AUDSRC="aeng synth hda afunc ac97 dsp dspipc4 dspipc3 dspdev dspcdc dspsst nhlt dwi2c sdw sdwlnk sdwstm sdwadv sdwrt71 sdwrt7l sdwamp sdwcs sdwmisc gpio cdces16 cdces26 cdcrt82 cdcrt10 cdcrt56 cdcrt63 cdcmax cdcda72 cdcnau cdccs42 cdctas cdcssm cdccx72 cdcm90 cdcrt86"

# build_pkg <name> <core files> <SYS name> <BIN name> <386 name> <C define> <NASM define> <Plug and Play files>
HAVE_DISK=0; HAVE_USB=0; HAVE_AUDIO=0
[ -f $SRC/core/ide.c ] && HAVE_DISK=1
[ -f $SRC/core/usb.c ] && HAVE_USB=1
[ -f $SRC/core/hda.c ] && HAVE_AUDIO=1
HAVE_UDF=0
[ -f $SRC/udf/udf.h ] && HAVE_UDF=1
# PKGS="usb udf": only those packages
if [ -n "$PKGS" ]; then
  for p in DISK USB AUDIO UDF; do
    case " ${PKGS^^} " in *" $p "*) ;; *) eval HAVE_$p=0 ;; esac
  done
fi
NPKG=$((HAVE_DISK + HAVE_USB + HAVE_AUDIO + HAVE_UDF))
dest() { if [ $NPKG -gt 1 ]; then echo $OUT/$1; else echo $OUT; fi; }

build_pkg() {
  local pkg=$1 files=$2 sys=$3 bin=$4 vxd=$5 cdef=$6 ndef=$7 pnpf=$8
  local o=$OBJ/$pkg d=$(dest $pkg)
  mkdir -p $o/dos $o/vxd $d
  local CF="-q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -I$SRC/core -I$SRC/inc $cdef"
  # DOS core (flat binary at 80004000h)
  for f in $files; do wcc386 $CF -fo=$o/dos/$f.obj $SRC/core/$f.c; done
  wcc386 $CF -fo=$o/dos/platdos.obj $SRC/dos/platdos.c
  nasm -f obj -o $o/dos/dosent.obj $SRC/dos/dosent.asm
  {
    echo "format raw bin"
    echo "name $d/$bin"
    echo "option offset=0x80004000"
    echo "option nodefaultlibs, quiet, map=$o/dos/core.map"
    echo "library $WATCOM/lib386/dos/clib3r.lib"
    echo "order clname CODE segment _HDR clname DATA clname BSS segment _BSSHEAD clname STACK clname ZZEND"
    echo "file $o/dos/dosent.obj"
    for f in $files platdos; do echo "file $o/dos/$f.obj"; done
  } > $o/dos/core.lnk
  wlink @$o/dos/core.lnk 2>&1 | grep -v "W1023" || true
  # VxD
  for f in $files; do wcc386 $CF -nt=_LTEXT -nc=CODE -fo=$o/vxd/$f.obj $SRC/core/$f.c; done
  wcc386 $CF -nt=_LTEXT -nc=CODE -fo=$o/vxd/platvxd.obj $SRC/vxd/platvxd.c
  for f in $pnpf; do wcc386 $CF -nt=_LTEXT -nc=CODE -I$SRC/vxd -fo=$o/vxd/$f.obj $SRC/vxd/$f.c; done
  nasm -f obj -I$SRC/inc/ $ndef -o $o/vxd/vxdent.obj $SRC/vxd/vxdent.asm
  nasm -f obj -I$SRC/inc/ $ndef -DWIN9X -o $o/vxd/vxdent9x.obj $SRC/vxd/vxdent.asm
  # vxd_link <output> <entry object> <format>
  vxd_link() {
  {
    echo "format windows vxd $3"
    echo "name $1"
    echo "option nodefaultlibs, quiet, offset=0, map=$o/vxd/vxd.map"
    echo "library $WATCOM/lib386/dos/clib3r.lib"
    echo "option description 'WINDRV $pkg driver core'"
    echo "segment _LTEXT preload nondiscardable"
    echo "segment _LDATA preload nondiscardable"
    echo "export WINCORE_DDB.1"
    echo "file $o/vxd/$2.obj"
    for f in $files platvxd; do echo "file $o/vxd/$f.obj"; done
    for f in $pnpf; do echo "file $o/vxd/$f.obj"; done
  } > $o/vxd/vxd.lnk
  wlink @$o/vxd/vxd.lnk
  }
  vxd_link $d/$vxd vxdent ""
  python3 $FIXVXD $d/$vxd
  # Windows 95/98/ME: dynamically loaded Plug and Play VxD (same code, Windows 95 DDB)
  if [ -n "$pnpf" ]; then vxd_link $d/${vxd%.386}.VXD vxdent9x dynamic; python3 $FIXVXD $d/${vxd%.386}.VXD; fi
  # DOS driver
  nasm -f bin -I$SRC/dos/ $ndef -o $d/$sys $SRC/dos/winstor.asm
  nasm -f bin -I$SRC/dos/ -o $d/WIN30FIX.COM $SRC/dos/win30fix.asm
  if [ $pkg = disk ]; then nasm -f bin -o $d/WINCD.SYS $SRC/dos/wincd.asm; fi
  if [ $pkg = usb ]; then nasm -f bin -DPKG_USB -o $d/WINUCD.SYS $SRC/dos/wincd.asm; fi
}

# build_pdr <destination> <file name> <module name> <NASM/C define> <description>: I/O Supervisor
# port driver of a package (Windows 95/98/ME), from the same sources for WINUSB and WINDISK
build_pdr() {
  local d=$1 name=$2 mod=$3 def=$4 desc=$5 o=$OBJ/pdr/$3
  mkdir -p $o
  wcc386 -q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -nt=_LTEXT -nc=CODE $def -fo=$o/winpdr.obj $SRC/pdr/winpdr.c
  nasm -f obj -I$SRC/inc/ $def -o $o/pdrent.obj $SRC/pdr/pdrent.asm
  {
    echo "format windows vxd dynamic"
    echo "name $d/$name"
    echo "option nodefaultlibs, quiet, offset=0, map=$o/pdr.map"
    echo "option modname=$mod"
    echo "option description '$desc'"
    echo "segment _LTEXT preload nondiscardable"
    echo "segment _LDATA preload nondiscardable"
    echo "export WINPDR_DDB.1"
    echo "file $o/pdrent.obj, $o/winpdr.obj"
  } > $o/pdr.lnk
  wlink @$o/pdr.lnk
  python3 $FIXVXD $d/$name
}

if [ $HAVE_DISK = 1 ]; then
  build_pkg disk "$COMMON $DISKSRC" WINDISK.SYS WINDCORE.BIN WINDISK.386 "" "" "pnp pnpdisk pnpblk"
  # WINDISK.PDR: Windows 95/98/ME I/O Supervisor port driver (disks of WINDISK.VXD)
  build_pdr $(dest disk) WINDISK.PDR WINDSKPD -DPDR_DISK "WINDISK port driver for the Windows 95 I/O Supervisor"
  cp $SRC/inf/WINDISK.INF $SRC/inf/WINDISKD.INF $SRC/inf/WINDISKC.INF $(dest disk)/
fi
if [ $HAVE_AUDIO = 1 ]; then
  build_pkg audio "$COMMON $AUDSRC" WINAUDIO.SYS WINACORE.BIN WINAUDIO.386 "-DWIN_AUDIO" "-DPKG_AUDIO" "pnp pnpaud"
  cp $SRC/inf/WINAUDIO.INF $SRC/inf/WINAUDM.INF $(dest audio)/
  # firmware of the Intel audio DSPs (SOF, AVS) and of SoundWire codecs: WINDRVFW next to the driver
  if [ -d $FWSRC ]; then
    rm -rf $(dest audio)/WINDRVFW; mkdir -p $(dest audio)/WINDRVFW
    cp $FWSRC/* $(dest audio)/WINDRVFW/
  fi
fi
mkdir -p $OBJ/win $OBJ/tools
if [ $HAVE_USB = 1 ]; then
build_pkg usb "$COMMON $USBSRC" WINUSB.SYS WINUCORE.BIN WINUSB.386 "-DWIN_USB" "-DPKG_USB" "pnp pnpusb pnpblk ps2"
U=$(dest usb)

# WINUSB.PDR: Windows 95/98/ME I/O Supervisor port driver (disks of WINUSB.VXD)
build_pdr $U WINUSB.PDR WINUSBPD "" "WINUSB port driver for the Windows 95 I/O Supervisor"

# WINDRVCI.DLL: icons of the device classes Windows 95 lacks (USB, HID, imaging)
nasm -f obj -o $OBJ/win/windrvci.obj $SRC/win/windrvci.asm
(cd $SRC/win && wrc -q -r -bt=windows -I$WATCOM/h/win windrvci.rc -fo=$OBJ/win/windrvci.res)
{
  echo "format windows dll"
  echo "name $U/WINDRVCI.DLL"
  echo "option modname=WINDRVCI"
  echo "option description 'WINDRV device class icons for Windows 95'"
  echo "option quiet, map=$OBJ/win/windrvci.map"
  echo "option resource=$OBJ/win/windrvci.res"
  echo "segment type CODE preload moveable discardable"
  echo "file $OBJ/win/windrvci.obj"
  echo "export WEP.1, ENUMPROPPAGES.2, CLASSINSTALL.3"
} > $OBJ/win/windrvci.lnk
wlink @$OBJ/win/windrvci.lnk
for f in WINUSB WINUSBD WINUSBH WINUSBA WINUSBV WINUSBS WINKBD WINMOU OEMSETUP; do cp $SRC/inf/$f.INF $U/; done

# Windows mouse driver (MOUSE.DRV replacement) and applications
nasm -f obj -o $OBJ/win/winmouse.obj $SRC/win/winmouse.asm
{
  echo "format windows dll"
  echo "name $U/WINMOUSE.DRV"
  echo "option modname=MOUSE"
  echo "option description 'WINMOUSE USB and PS/2 mouse driver with wheel for Windows 3.x and 95/98/ME'"
  echo "option quiet, map=$OBJ/win/winmouse.map"
  echo "segment _TEXT preload fixed"
  echo "segment _DATA preload fixed"
  echo "file $OBJ/win/winmouse.obj"
  echo "export Inquire.1, Enable.2, Disable.3, MouseGetIntVect.4, WEP, SETWHEELWINDOW=SetWheelWindow"
  echo "export SETWHEELOPTIONS=SetWheelOptions, AUTOSCROLL=AutoScroll, MIDDLEBUTTON=MiddleButton"
  echo "import GetVersion KERNEL.3"
  echo "import GetModuleHandle KERNEL.47"
  echo "import GetProcAddress KERNEL.50"
  echo "import GetWinFlags KERNEL.132"
} > $OBJ/win/winmouse.lnk
wlink @$OBJ/win/winmouse.lnk
WF="-q -ml -zw -bt=windows -ox -s -wx -I$WATCOM/h/win -I$SRC/inc -I$SRC/tools"
wcc $WF -fo=$OBJ/win/winwheel.obj $SRC/win/winwheel.c
(cd $SRC/win && wrc -q -r -bt=windows -I$WATCOM/h/win winwheel.rc -fo=$OBJ/win/winwheel.res)
{
  echo "system windows"
  echo "name $U/WINWHEEL.EXE"
  echo "option quiet, heapsize=1024, stack=8192"
  echo "option description 'WINWHEEL mouse wheel, back / forward and autoscroll helper for Windows 3.x and 95/98/ME'"
  echo "option resource=$OBJ/win/winwheel.res"
  echo "file $OBJ/win/winwheel.obj"
} > $OBJ/win/winwheel.lnk
wlink @$OBJ/win/winwheel.lnk
wcc $WF -fo=$OBJ/win/usbtree.obj $SRC/win/usbtree.c
(cd $SRC/win && wrc -q -r -bt=windows -I$WATCOM/h/win usbtree.rc -fo=$OBJ/win/usbtree.res)
wcc $WF -fo=$OBJ/win/winfmtw.obj $SRC/tools/winfmt.c
wcc $WF -fo=$OBJ/win/winfsw.obj $SRC/tools/winfs.c
{
  echo "system windows"
  echo "name $U/USBTREE.EXE"
  echo "option quiet, heapsize=4096, stack=12288"
  echo "option description 'USBTREE USB device viewer for Windows 3.0 and 3.1'"
  echo "option resource=$OBJ/win/usbtree.res"
  echo "file $OBJ/win/usbtree.obj, $OBJ/win/winfmtw.obj, $OBJ/win/winfsw.obj"
} > $OBJ/win/usbtree.lnk
wlink @$OBJ/win/usbtree.lnk
# WINCAM.EXE: 386 instructions (WINUSB needs a PCI machine anyway)
wcc $WF -3 -fo=$OBJ/win/wincam.obj $SRC/win/wincam.c
wcc $WF -3 -fo=$OBJ/win/camjpeg.obj $SRC/win/camjpeg.c
(cd $SRC/win && wrc -q -r -bt=windows -I$WATCOM/h/win wincam.rc -fo=$OBJ/win/wincam.res)
{
  echo "system windows"
  echo "name $U/WINCAM.EXE"
  echo "option quiet, heapsize=4096, stack=12288"
  echo "option description 'WINCAM USB camera viewer for Windows 3.0 and 3.1'"
  echo "option resource=$OBJ/win/wincam.res"
  echo "option map=$OBJ/win/wincam.map"
  echo "file $OBJ/win/wincam.obj, $OBJ/win/camjpeg.obj"
} > $OBJ/win/wincam.lnk
wlink @$OBJ/win/wincam.lnk

fi

# WINUDF: UDF file system. WINUDF.EXE, the DOS redirector (tiny model .COM image, the part
# after its resident data is dropped when it stays resident)
if [ $HAVE_UDF = 1 ]; then
  F=$OBJ/udf
  mkdir -p $F $(dest udf)
  UF="-q -ms -s -os -zl -zq -wx -we -DUDF_DOS16 -I$SRC/udf"
  nasm -f obj -o $F/udfdos_a.obj $SRC/udf/udfdos.asm
  for f in udfcore udfwr udfdos; do wcc $UF -fo=$F/$f.obj $SRC/udf/$f.c; done
  wcc $UF -zc -nt=INIT_TEXT -nc=INITCODE -fo=$F/udfinit.obj $SRC/udf/udfinit.c
  {
    echo "format dos com"
    echo "name $(dest udf)/WINUDF.EXE"
    echo "option quiet, map=$F/winudf.map, nodefaultlibs"
    echo "library $WATCOM/lib286/dos/clibs.lib"
    echo "file $F/udfdos_a.obj, $F/udfdos.obj, $F/udfcore.obj, $F/udfwr.obj, $F/udfinit.obj"
  } > $F/winudf.lnk
  wlink @$F/winudf.lnk
  # WINUDF.VXD: the file system driver of Windows 95/98/ME (static VxD, SYSTEM.INI device=)
  VF="-q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -nt=_LTEXT -nc=CODE -I$SRC/udf"
  mkdir -p $F/vxd
  for f in udfcore udfwr udfvxd; do wcc386 $VF -fo=$F/vxd/$f.obj $SRC/udf/$f.c; done
  nasm -f obj -o $F/vxd/udfvxd_a.obj $SRC/udf/udfvxd.asm
  {
    echo "format windows vxd"
    echo "name $(dest udf)/WINUDF.VXD"
    echo "option nodefaultlibs, quiet, offset=0, map=$F/vxd/winudf.map"
    echo "library $WATCOM/lib386/dos/clib3r.lib"
    echo "option description 'WINUDF UDF file system driver for Windows 95/98/ME'"
    echo "segment _LTEXT preload nondiscardable"
    echo "segment _LDATA preload nondiscardable"
    echo "export WINUDF_DDB.1"
    echo "file $F/vxd/udfvxd_a.obj, $F/vxd/udfvxd.obj, $F/vxd/udfcore.obj, $F/vxd/udfwr.obj"
  } > $F/vxd/winudf.lnk
  wlink @$F/vxd/winudf.lnk
  python3 $FIXVXD $(dest udf)/WINUDF.VXD
fi

# DOS tools (shared by WINDISK and WINUSB)
DF="-q -ml -bt=dos -ox -s -wx -I$SRC/inc -I$SRC/tools"
if [ $HAVE_DISK = 1 ] || [ $HAVE_USB = 1 ]; then
  for f in wininfo winfmt winfs dskbench; do wcc $DF -fo=$OBJ/tools/$f.obj $SRC/tools/$f.c; done
  wlink system dos name $OUT/WININFO.EXE file $OBJ/tools/wininfo.obj, $OBJ/tools/winfmt.obj, $OBJ/tools/winfs.obj option quiet, stack=8192
  wlink system dos name $OUT/DSKBENCH.EXE file $OBJ/tools/dskbench.obj option quiet, stack=4096
fi
# WIN4G.VXD: static VxD moving PCI resources above 4G below 4G before Windows 95/98/ME
# enumerates the PCI bus (all packages; needed where no DOS driver runs, e.g. Windows ME)
if [ $HAVE_DISK = 1 ] || [ $HAVE_USB = 1 ] || [ $HAVE_AUDIO = 1 ]; then
G=$OBJ/win4g
mkdir -p $G
CF4="-q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -nt=_LTEXT -nc=CODE -I$SRC/core -I$SRC/inc"
for f in winlib pci acpi; do wcc386 $CF4 -fo=$G/$f.obj $SRC/core/$f.c; done
wcc386 $CF4 -fo=$G/w4gplat.obj $SRC/vxd/w4gplat.c
nasm -f obj -I$SRC/inc/ -o $G/win4g.obj $SRC/vxd/win4g.asm
{
  echo "format windows vxd"
  echo "name $G/WIN4G.VXD"
  echo "option nodefaultlibs, quiet, offset=0, map=$G/win4g.map"
  echo "option description 'WIN4G moves PCI resources above 4G below 4G'"
  echo "segment _LTEXT preload nondiscardable"
  echo "segment _LDATA preload nondiscardable"
  echo "export WIN4G_DDB.1"
  echo "file $G/win4g.obj, $G/w4gplat.obj, $G/winlib.obj, $G/pci.obj, $G/acpi.obj"
} > $G/win4g.lnk
wlink @$G/win4g.lnk
python3 $FIXVXD $G/WIN4G.VXD
[ $HAVE_DISK = 1 ] && cp $G/WIN4G.VXD $(dest disk)/
[ $HAVE_USB = 1 ] && cp $G/WIN4G.VXD $(dest usb)/
[ $HAVE_AUDIO = 1 ] && cp $G/WIN4G.VXD $(dest audio)/
fi
# WINGRP.EXE: Program Manager groups requested by the installers (all packages)
WF2="-q -ml -zw -bt=windows -ox -s -wx -I$WATCOM/h/win"
wcc $WF2 -fo=$OBJ/win/wingrp.obj $SRC/win/wingrp.c
(cd $SRC/win && wrc -q -r -bt=windows wingrp.rc -fo=$OBJ/win/wingrp.res)
wlink system windows name $OBJ/win/WINGRP.EXE file $OBJ/win/wingrp.obj option quiet, heapsize=1024, stack=4096, resource=$OBJ/win/wingrp.res, description \'WINGRP Program Manager groups for WINDRV\'
for p in disk usb audio udf; do
  eval h=\$HAVE_${p^^}
  [ $h = 1 ] && cp $OBJ/win/WINGRP.EXE $(dest $p)/
done
# installer and uninstaller of each package
setup_pkg() {
  local d=$1 def=$2 inst=$3 unin=$4
  wcc $DF $def -fo=$OBJ/tools/$inst.obj $SRC/tools/winsetup.c
  wcc $DF $def -DUNINST -fo=$OBJ/tools/$unin.obj $SRC/tools/winsetup.c
  wlink system dos name $d/${inst^^}.EXE file $OBJ/tools/$inst.obj option quiet, stack=12288
  wlink system dos name $d/${unin^^}.EXE file $OBJ/tools/$unin.obj option quiet, stack=12288
}
[ $HAVE_DISK = 1 ] && setup_pkg $(dest disk) "" instdisk unindisk
[ $HAVE_USB = 1 ] && setup_pkg $(dest usb) "-DWIN_USB" instusb uninusb
[ $HAVE_AUDIO = 1 ] && [ -f $SRC/tools/winsetup.c ] && setup_pkg $(dest audio) "-DWIN_AUDIO" instaud uninaud
[ $HAVE_UDF = 1 ] && setup_pkg $(dest udf) "-DWIN_UDF" instudf uninudf
# sound programs: WINAUDIO package, and WINUSB for USB Audio devices
if [ -f $SRC/win/winsound.c ] && { [ $HAVE_USB = 1 ] || [ $HAVE_AUDIO = 1 ]; }; then
  A=$OBJ/win
  # MMSYSTEM driver (Windows 3.1, Windows 3.0 with Multimedia Extensions)
  wcc -q -ml -zu -s -bt=windows -ox -wx -zl -I$WATCOM/h/win -I$SRC/inc -fo=$OBJ/win/winsound.obj $SRC/win/winsound.c
  nasm -f obj -o $OBJ/win/winsndlb.obj $SRC/win/winsndlb.asm
  {
    echo "format windows dll"
    echo "name $A/WINSOUND.DRV"
    echo "option modname=WINSOUND"
    echo "option description 'wave,midi,aux:WINSOUND HD Audio, AC97 and USB Audio'"
    echo "option quiet, map=$OBJ/win/winsound.map"
    echo "segment type CODE preload fixed"
    echo "segment type DATA preload fixed"
    echo "file $OBJ/win/winsndlb.obj, $OBJ/win/winsound.obj"
    echo "library $WATCOM/lib286/win/windows.lib, $WATCOM/lib286/win/mmsystem.lib, $WATCOM/lib286/win/clibl.lib"
    echo "export WEP, DRIVERPROC.1, WODMESSAGE.2, WIDMESSAGE.3, MODMESSAGE.4, MIDMESSAGE.5, AUXMESSAGE.6, TIMERPROC"
  } > $OBJ/win/winsound.lnk
  wlink @$OBJ/win/winsound.lnk
  # SOUND driver for Windows 3.0 (MessageBeep and the voice API on the synthesizer, all modes)
  wcc -q -ml -zu -s -bt=windows -ox -wx -zl -I$WATCOM/h/win -I$SRC/inc -fo=$OBJ/win/winbeep.obj $SRC/win/winbeep.c
  {
    echo "format windows dll"
    echo "name $A/WINBEEP.DRV"
    echo "option modname=SOUND"
    echo "option description 'WINBEEP sound driver for Windows 3.0'"
    echo "option quiet, map=$OBJ/win/winbeep.map"
    echo "segment type CODE preload fixed"
    echo "segment type DATA preload fixed"
    echo "file $OBJ/win/winsndlb.obj, $OBJ/win/winbeep.obj"
    echo "library $WATCOM/lib286/win/windows.lib, $WATCOM/lib286/win/clibl.lib"
    echo "import CREATESYSTEMTIMER SYSTEM.2, KILLSYSTEMTIMER SYSTEM.3, GETSYSTEMMSECCOUNT SYSTEM.6"
    echo "export OPENSOUND.1, CLOSESOUND.2, SETVOICEQUEUESIZE.3, SETVOICENOTE.4, SETVOICEACCENT.5"
    echo "export SETVOICEENVELOPE.6, SETSOUNDNOISE.7, SETVOICESOUND.8, STARTSOUND.9, STOPSOUND.10"
    echo "export WAITSOUNDSTATE.11, SYNCALLVOICES.12, COUNTVOICENOTES.13, GETTHRESHOLDEVENT.14"
    echo "export GETTHRESHOLDSTATUS.15, SETVOICETHRESHOLD.16, DOBEEP.17, MYOPENSOUND.18, WEP.19"
    echo "export ENABLE.20, DISABLE.21"
  } > $OBJ/win/winbeep.lnk
  wlink @$OBJ/win/winbeep.lnk
  wcc -q -ml -zw -bt=windows -ox -s -wx -I$WATCOM/h/win -I$SRC/inc -fo=$OBJ/win/winmix.obj $SRC/win/winmix.c
  (cd $SRC/win && wrc -q -r -bt=windows winmix.rc -fo=$OBJ/win/winmix.res)
  {
    echo "system windows"
    echo "name $A/WINMIX.EXE"
    echo "option quiet, heapsize=1024, stack=8192"
    echo "option description 'WINMIX volume control for WINAUDIO and WINUSB sound devices'"
    echo "file $OBJ/win/winmix.obj"
    echo "option resource=$OBJ/win/winmix.res"
  } > $OBJ/win/winmix.lnk
  wlink @$OBJ/win/winmix.lnk
  wcc $DF -fo=$OBJ/tools/audtest.obj $SRC/tools/audtest.c
  wlink system dos name $A/AUDTEST.EXE file $OBJ/tools/audtest.obj option quiet, stack=8192
  for p in audio usb; do
    if [ $p = audio ] && [ $HAVE_AUDIO = 0 ]; then continue; fi
    if [ $p = usb ] && [ $HAVE_USB = 0 ]; then continue; fi
    cp $A/WINSOUND.DRV $A/WINBEEP.DRV $A/WINMIX.EXE $A/AUDTEST.EXE $(dest $p)/
  done
fi
if [ $NPKG -gt 1 ]; then
  for p in disk usb; do [ -d $OUT/$p ] && [ -f $OUT/WININFO.EXE ] && cp $OUT/WININFO.EXE $OUT/DSKBENCH.EXE $OUT/$p/; done
  rm -f $OUT/WININFO.EXE $OUT/DSKBENCH.EXE
  for p in disk usb audio udf; do [ -d $OUT/$p ] && ls -l $OUT/$p; done
elif [ -f $LAYOUT ]; then
  python3 $TOOLS/oslayout.py $LAYOUT $OUT $BINDIR
else
  ls -l $OUT
fi
