; dosent.asm - header of WINCORE.BIN (flat binary linked at 80004000h)
; WINDISK.SYS / WINUSB.SYS reads this header to find the entry points.
        bits 32

segment _HDR public class=CODE use32 align=16
segment _BSSHEAD public class=BSS use32 align=16
segment _STKSEG public class=STACK use32 align=16
segment _ZZEND public class=ZZEND use32 align=16

extern _ent_init
extern _ent_dos
extern _ent_poll
extern _ent_irq
extern _ent_api
extern _ent_shutdown
extern _ent_resume
extern _ent_filedone
extern _ent_sbio
extern _ent_sbtick
extern _ent_vmstart

segment _HDR
global _wincore_header
_wincore_header:
        db 'W3CI'           ; +0  magic
        dd 80004000h            ; +4  link base
        dd bss_start            ; +8  end of file image (linear)
        dd image_end            ; +12 end of image incl. bss/stack (linear)
        dd _ent_init            ; +16
        dd _ent_dos             ; +20
        dd _ent_poll            ; +24
        dd _ent_irq             ; +28
        dd _ent_api             ; +32
        dd _ent_shutdown        ; +36
        dd _ent_resume          ; +40
        dd stack_top            ; +44
        dd _ent_filedone        ; +48 firmware files loaded (WINAUDIO)
        dd _ent_sbio            ; +52 Sound Blaster emulation: port access
        dd _ent_sbtick          ; +56 Sound Blaster emulation: RTC tick
        dd _ent_vmstart         ; +60 WINAUDIO: start the V86 monitor

segment _BSSHEAD
bss_start:

segment _STKSEG
        resb 24576
stack_top:

segment _ZZEND
image_end:
