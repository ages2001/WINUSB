# WINUSB 1.1 - USB driver for DOS and Windows 1.0-3.11 / 95 / 98 / ME

WINUSB is a USB stack for old systems: **UHCI, OHCI, EHCI, xHCI** and **USB4 / Thunderbolt**
host controllers, USB **keyboards, mice, tablets**, **disks and sticks** (drive letters in
DOS), **CD/DVD drives**, **sound cards** and **cameras**. It also drives the **PS/2 mouse**
(wheel, 5 buttons) and **notebook touchpads** (USB, I2C, PS/2 and SMBus: two-finger
scrolling, taps, three-finger swipe back / forward, TrackPoint scrolling).

WINUSB is one of the four WINDRV packages, each in its own repository and usable alone or
together with the others:

| Package | What it drives |
|---------|----------------|
| **WINDISK**  | IDE, SATA AHCI and NVMe disks, IDE/AHCI CD/DVD drives, SD card readers and eMMC |
| **WINUSB**   | USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 / Thunderbolt |
| **WINAUDIO** | HD Audio, AC'97 and the Intel audio DSP (SST / SOF), General MIDI synthesizer, Sound Blaster 16 emulation for DOS programs |
| **WINUDF**   | the UDF file system (DVD, Blu-ray, packet-written CDs, UDF disks), read and write |

---

## Supported systems

| System | WINUSB |
|--------|------|
| MS-DOS 5.0 / 6.x | disks, keyboard, mouse (PS/2 path and INT 33h), touchpads, USB CD/DVD (MSCDEX), sound (`AUDTEST /USB`), USB4 / Thunderbolt (NVMe, docks) |
| MS-DOS 7.0 / 7.1 / 8.0, FreeDOS | the same; FAT32 partitions on 7.1, 8.0 and FreeDOS; with Windows 95/98/ME installed too (MS-DOS mode, DOS boxes) |
| Windows 1.0x | USB keyboard, disks (as letters), mouse (INT 33h) + wheel (cursor-key scrolling) |
| Windows 2.x (real mode) | USB keyboard, disks (as letters), mouse (INT 33h / "Microsoft Mouse") + wheel (cursor-key scrolling) |
| Windows/386 2.x | **does not work** (see "Known limitations") |
| Windows 3.0 Real Mode | keyboard, mouse, touchpads, disks, camera, beeps (`WINBEEP.DRV`) |
| Windows 3.0 Standard / 386 Enhanced Mode | everything (sound with the Multimedia Extensions) |
| Windows 3.1, WfW 3.11 | everything |
| Windows 95 / 98 / ME | everything, as Plug and Play devices: `WINUSB.VXD` + the IOS port driver `WINUSB.PDR` (32-bit drive letters), PS/2 keyboard, mouse and touchpads (section 3b); Windows 98 / ME: the other USB devices with their own Windows drivers through the WDM bridge `WINUSBW.SYS` (section 3c) |

## Installation

From DOS (not inside Windows 3.x), in `bin\DOS` of this package:

```
INSTUSB                      into the Windows directory, CONFIG.SYS, SYSTEM.INI, WIN.INI updated
INSTUSB C:\DRV               another directory
INSTUSB /NOWIN               the DOS driver only
INSTUSB /O /R:4              with driver options (everything after /O goes to WINUSB.SYS)
```

The installer finds every Windows of the machine (1.0x to 3.11 and 95 / 98 / ME), copies
the files, adds the driver to CONFIG.SYS after HIMEM.SYS (also on plain MS-DOS 7.x / 8.0 and
FreeDOS), installs the mouse driver `WINMOUSE.DRV` and the wheel helper `WINWHEEL.EXE`; the
originals are saved as `*.WNU`. On Windows 95 / 98 / ME it prepares the Plug and Play
drivers: then pick the USB controller in **Device Manager** (Windows 98 / ME: Update Driver
-> Have Disk -> `C:\WINDOWS\INF`; Windows 95: Remove + Refresh). Uninstall: `UNINUSB`.

### By hand

1. Copy `bin\DOS` to a directory, e.g. `C:\WINDRV`.
2. CONFIG.SYS:
   ```
   DEVICE=C:\DOS\HIMEM.SYS
   DEVICE=C:\WINDRV\WINUSB.SYS
   DEVICE=C:\WINDRV\WINUCD.SYS           (USB CD/DVD drives)
   LASTDRIVE=Z
   ```
   AUTOEXEC.BAT for USB CD/DVD drives: `C:\DOS\MSCDEX.EXE /D:WINUCD01`
3. Windows 3.x: copy `bin\WIN3X\WINUSB.386` next to WINUSB.SYS (loaded by itself in 386
   Enhanced Mode), `WINMOUSE.DRV` to `WINDOWS\SYSTEM` and set `mouse.drv=winmouse.drv` in
   `[boot]` of SYSTEM.INI; for the wheel add `WINWHEEL.EXE` to `load=` in WIN.INI.
4. Windows 95 / 98 / ME: Device Manager -> the USB controller -> Update Driver -> Have Disk ->
   `bin\WIN9X` of this package.

### Most used options (WINUSB.SYS)

| Option | Meaning |
|--------|---------|
| `/R:n` | drive letters reserved for USB disks (default 2, more with the partitions found at boot) |
| `/WIN` | USB only in Windows 386 Enhanced Mode |
| `/NOHID`, `/NOMSC` | no keyboard / mouse, no disks |
| `/NOPS2`, `/NOTP` | leave the PS/2 mouse to the BIOS / Windows; touchpads in mouse mode |
| `/POLL` | no interrupts, polling only |
| `/DOSDISK` | drive letters in DOS also when Windows 95/98/ME is installed |
| `/S` | show the driver log at boot (diagnosis) |

## Repository layout

```
bin/DOS     DOS: the driver, its 32-bit core, tools, installer and uninstaller (also all
            Windows 1.0x / 2.x need)
bin/WIN3X   Windows 3.0, 3.1 and Windows for Workgroups 3.11
bin/WIN9X   Windows 95, 98 and ME (Plug and Play drivers and their INF files: the folder
            to give to "Have Disk" / "Update Driver")
src         the source code (see "Building")
```

Files used by several systems are in each of their folders. The installer is run from
`bin\DOS`; it takes the files of the other systems from the folders next to it, so the
`bin` tree has to stay together (copy all of it to a floppy or a directory).

| Folder | Files |
|--------|-------|
| `bin/DOS` | `AUDTEST.EXE`, `DSKBENCH.EXE`, `INSTUSB.EXE`, `MANUAL.TXT`, `README.TXT`, `UNINUSB.EXE`, `WININFO.EXE`, `WINUCD.SYS`, `WINUCORE.BIN`, `WINUSB.SYS` |
| `bin/WIN3X` | `OEMSETUP.INF`, `USBTREE.EXE`, `WIN30FIX.COM`, `WINBEEP.DRV`, `WINCAM.EXE`, `WINGRP.EXE`, `WINMIX.EXE`, `WINMOUSE.DRV`, `WINSOUND.DRV`, `WINUSB.386`, `WINUSB.TXT`, `WINWHEEL.EXE` |
| `bin/WIN9X` | `USBD.SYS`, `USBTREE.EXE`, `WIN4G.VXD`, `WINCAM.EXE`, `WINDRVCI.DLL`, `WINGRP.EXE`, `WINKBD.INF`, `WINMIX.EXE`, `WINMOU.INF`, `WINMOUSE.DRV`, `WINSOUND.DRV`, `WINUSB.INF`, `WINUSB.PDR`, `WINUSB.TXT`, `WINUSB.VXD`, `WINUSBA.INF`, `WINUSBD.INF`, `WINUSBH.INF`, `WINUSBS.INF`, `WINUSBV.INF`, `WINUSBW.SYS`, `WINWHEEL.EXE` |

## More

**MANUAL.md** (`MANUAL.TXT` in `bin\DOS`, `WINUSB.TXT` for Windows) describes everything
else: all options, how the driver works, hardware and chipset notes, the programming
interface, building from source, tests and known limitations.

## Version history

**1.1 (26 October 2026)**

Changelog:

- Added the USB WDM bridge for Windows 98 / ME: the USB devices (and the interfaces of composite devices) no WINDRV class driver serves are reported by the new WDM bus driver `WINUSBW.SYS` as `USB\VID_xxxx&PID_xxxx` devices, so their own Windows drivers load (vendor drivers, Windows' HID, printer and other class drivers) and their USB requests run on WINUSB's controllers; `[WINUSB] WDM=vvvv:pppp` in SYSTEM.INI gives a device to its Windows driver even when WINUSB knows its class, `WDM=OFF` turns the bridge off.
- Drive letters for every partition of a USB disk, whatever its file system: FAT12 / FAT16 / FAT32 partitions as DOS drives (FAT32 on MS-DOS 7.10 and later), NTFS, exFAT, HPFS, Linux (ext2/3/4), HFS+ partitions and FAT32 on older DOS versions as small read-only placeholder drives labelled with the file system, so that the letters of the following partitions do not shift.
- Empty drive letter slots are hidden from DOS until a disk is plugged in: no more "Not ready reading drive", "General failure reading drive", "Current drive is no longer valid" or "Cannot read from drive" in programs that walk all drives; letters of other drivers (MSCDEX, SHSUCDX, SUBST, network) are left alone.
- MS-DOS 7.x with Windows 98 / ME installed: the DOS driver gives drive letters too (`/DOSDISK` is no longer needed); the installer adds WINUSB.SYS to the `[SafeList]` of IOS.INI, so Windows 98 no longer warns that a drive uses MS-DOS compatibility mode.
- FAT32 drives answer the FAT32 generic IOCTL (category 48h) and report "FAT32" in their media ID.
- Fixed transfers into the HMA (DOS=HIGH buffers) under VCPI servers that emulate A20 by paging (JEMM): corrupt directory listings (only the first entry shown, lost clusters), frozen keyboard.
- WINUSB.SYS can be loaded high (`DEVICEHIGH`): a USB disk the BIOS also offers is recognized from an upper memory block too (its first sector is read into the HMA as well, where BIOS USB code writes), so its BIOS drive keeps working once WINUSB has the controller (the machine hung at the first access to such a drive).
- Windows 98 / ME: USB CD/DVD drives no longer lose the disc for a moment when the drive is busy (CD Player).
- Windows 95/98/ME: WINUSB.VXD loaded at the start takes only the USB controllers whose installed driver is WINUSB (Windows' own UHCI / OHCI driver on the other controllers starts them too; freezes on machines such as the ThinkPad T42p with WINUSB on one of several controllers).
- Fixed the BIOS drives of Windows 9x not being matched to their USB disks when a FAT16 drive gives its size in the 32-bit field of the BPB.
- Added the audio functions of USB CD/DVD drives for MSCDEX (play, pause, resume, stop, Q-channel, UPC, audio status) and SCSI commands with data to the drive (MODE SELECT).
- Safe removal (WININFO /E, USBTREE): sticks without a write cache, which reject SYNCHRONIZE CACHE, no longer report "Flush failed".
- Added Thunderbolt 5 / USB4 version 2 routers and the host interfaces of Lunar Lake, Panther Lake, Wildcat Lake and Barlow Ridge.
- Fixed "CPU without time stamp counter" on CPUs that have one but do not report it through CPUID (Vortex86 DX / SX / EX ...).
- The TSC rate is measured with a time limit and with the PIT counter when the timer interrupt does not come (freezes at the start under some VCPI servers).
- WINUSB.SYS loaded with DEVLOAD or another device loader finds WINUCORE.BIN in its own directory.
- The installer tells Windows 98, 98 Second Edition and ME apart by the version of USER.EXE (Windows 98 was shown as Windows 95).
- Fixed lost Shift releases (a stuck Shift key) and missing number keys with PS/2 and USB keyboards in DOS and Windows.
- Fixed the PS/2 mouse, touchpads and trackpads not working on some machines, and double clicks.
- Added touchpad gestures: double tap, tap and drag, three-finger swipe for back / forward (PS/2, SMBus, I2C and USB touchpads); TrackPoint scrolling with the middle button (up / down and left / right).
- PS/2 mice: the wheel scrolls up / down and the 4th / 5th buttons go back / forward on every Windows version (IntelliMouse Explorer mode).
- Added USB keyboards that work only in report protocol.
- The USB keyboard works inside DOS functions too (COPY CON, programs reading the console through DOS).
- Fixed USB boot disks being lost in DOS once WINUSB.SYS started ("Bad or missing ...\WINUCD.SYS", "Not ready reading drive"): their INT 13h calls are served by the driver.
- Added MS-DOS 7.0, 7.1, 8.0 and FreeDOS support: drive letters (FAT32 on 7.10 / 8.0 and FreeDOS); the installer adds the CONFIG.SYS lines there too.
- 2 drive letter slots by default instead of 4.
- WINBEEP.DRV plays the system sounds of WIN.INI [sounds] in Windows 3.0 Real Mode; WINSOUND.DRV no longer repeats the first 64 KB of long sounds.
- WINSOUND.DRV: a mixer driver for Windows 95/98/ME (`mixer=winsound.drv`, Volume Control and the volume icon of the taskbar), the volumes are kept across restarts (SYSTEM.INI `[winsound.drv]`), USB Audio volume follows the Windows scale (half volume is -6 dB), removed USB Audio devices no longer accept format queries; synthesizer instruments without the harsh noise of strong feedback, fuller basses, softer high notes, no tick at note starts.
- USB Audio devices play WINAUDIO's Sound Blaster 16 emulation (DOS, Windows 3.x MS-DOS boxes, Windows 95/98/ME) when WINAUDIO is installed too.
- The documentation is split into README.md (overview, installation) and MANUAL.md (all details).

**1.0 (5 October 2026)** - first release.

**1.0 update (14 October 2026):**

* Windows 95 / 98 / ME: a separate INF and class for every device kind (USB controllers,
  HID, sound, imaging, disks, system devices), type and speed in the device names (UHCI /
  OHCI / EHCI / xHCI / USB4, USB 1.1 / 2.0 / 3.x hubs, USB Audio 1.0 / 2.0), icons of the
  classes Windows 95 lacks (`WINDRVCI.DLL`), the IOS port driver `WINUSB.PDR`, booting from
  USB.
* PS/2 keyboard and mouse inside the driver (Windows' PS/2 mouse driver disabled, LEDs
  follow the USB lock keys), back / forward buttons, autoscroll with a wheel click; all
  partitions of USB disks, 4 letter slots by default, more with the partitions found at
  boot.
* Touchpads on every system: HID over I2C from ACPI (`PNP0C50` / `ACPI0C50`, `_DSM`
  descriptor address, `GpioInt`, Intel LPSS ACPI mode and AMD I2C controllers), PS/2
  Synaptics and ELAN touchpads (absolute protocols, clickpads, TrackPoint; in DOS too),
  Synaptics InterTouch (RMI4) and ELAN on SMBus; `/NOTP`, `/NOSMB`.
* USB CD/DVD drives in DOS (`WINUCD.SYS`, MSCDEX device `WINUCD01`); USB sticks without FAT
  partitions get a letter on Windows 98 / ME as removable drives.
* The USB controllers are handed back to the BIOS when Windows shuts down and restarts in
  MS-DOS mode (EHCI / xHCI ownership SMI, the BIOS state of UHCI / OHCI restored): the USB
  keyboard works in DOS without a new POST.
* PCI resources above 4 GB: the boot-time move applies only to WINDRV's devices (`/ALL4G`
  for all), WIN4G.VXD on Windows 9x.
* Windows 95 stability fixes: the DDB reference of the port driver (fatal exception 0E,
  Explorer / MSGSRV32 closing), the SYSTEM.INI service when installing while Windows runs.
* Installer: Start menu groups on Windows 95/98/ME, "Read Me" items, no duplicate entries
  when installing again; the package is split into `bin/DOS`, `bin/WIN3X` and `bin/WIN9X`.
