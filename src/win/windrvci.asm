; windrvci.asm - WINDRVCI.DLL: icons of the device classes Windows 95 does not know (Universal
; Serial Bus controllers, Human Interface Devices, Imaging devices). The WINDRV INF files
; create these classes on Windows 95 with Installer = this DLL and Icon = the resource id of
; the class icon in it: SETUPX takes the icon of a class from its class installer. The class
; installer leaves every request to SETUPX. Windows 98 and ME have the classes and their
; icons; the DLL is not used there.
        bits 16

segment _TEXT public class=CODE align=16

; library entry: DI = instance, DS = data segment, CX = heap size. AX <> 0: loaded.
..start:
global LIBENTRY
LIBENTRY:
        mov ax, 1
        retf

global WEP
WEP:
        mov ax, 1
        retf 2

; RETERR FAR PASCAL ClassInstall(DI_FUNCTION, LPDEVICE_INFO): ERR_DI_DO_DEFAULT (506), SETUPX
; does the default processing of every request
global CLASSINSTALL
CLASSINSTALL:
        mov ax, 506
        retf 6

; BOOL FAR PASCAL EnumPropPages(LPDEVICE_INFO, LPFNADDPROPSHEETPAGE, LPARAM): no pages of
; its own, Device Manager shows the standard ones
global ENUMPROPPAGES
ENUMPROPPAGES:
        mov ax, 1
        retf 12
