; win4g.asm - WIN4G.VXD entry: a small static VxD for Windows 95/98/ME (and 3.1) that
; moves every PCI resource above 4G below 4G in Sys_Critical_Init, before the Plug and Play
; PCI enumerator and the device drivers see the machine. Loaded by SYSTEM.INI
; [386Enh] device=WIN4G.VXD (the WINDRV INF files and installers add the line).
        bits 32

%include "vxdsvc.inc"

%macro VMMCALL 1
        int 20h
        dd %1
%endmacro

segment _LTEXT public class=CODE use32 align=16
segment _LDATA public class=CODE use32 align=16

extern _w4g_init

segment _LDATA
global WIN4G_DDB
WIN4G_DDB:
        dd 0                    ; next
        dw 030Ah                ; SDK version (Windows 3.1 layout, loads on 95/98/ME too)
        dw 0                    ; Undefined_Device_ID: no services
        db 1, 1                 ; version 1.1
        dw 0                    ; flags
        db 'WIN4G   '           ; name
        dd 80000000h            ; Undefined_Init_Order
        dd Control
        dd 0, 0                 ; V86 / PM API
        dd 0, 0
        dd 0                    ; reference data
        dd 0, 0                 ; services

segment CONST public class=DATA use32 align=4
segment CONST2 public class=DATA use32 align=4
segment _DATA public class=DATA use32 align=4
segment _BSS public class=BSS use32 align=16
global _bss_begin
_bss_begin:
segment ZZBSS public class=BSS use32 align=16
global _bss_end
_bss_end:
group DGROUP CONST CONST2 _DATA _BSS ZZBSS

segment _LTEXT

Control:
        cmp eax, 0              ; Sys_Critical_Init
        jne .ok
        pushad
        cld
        call _w4g_init
        popad
.ok:    clc
        ret

; u32 w4g_map(u32 phys, u32 len): linear address, 0xFFFFFFFF on failure
global _w4g_map
_w4g_map:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword 0
        push dword [ebp+12]
        push dword [ebp+8]
        VMMCALL SVC__MapPhysToLinear
        add esp, 12
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; const char *w4g_profile(const char *section, const char *key, const char *def)
global _w4g_profile
_w4g_profile:
        push esi
        push edi
        mov esi, [esp+12]
        mov edi, [esp+16]
        mov edx, [esp+20]
        VMMCALL SVC_Get_Profile_String
        mov eax, edx
        pop edi
        pop esi
        ret
