; pdrent.asm - WINUSB.PDR / WINDISK.PDR (-DPDR_DISK): I/O Supervisor port driver for Windows
; 95/98/ME (DDB, control dispatch, IOS calls and the services of WINUSB.VXD / WINDISK.VXD)
        bits 32

%include "vxdsvc.inc"

%macro VMMCALL 1
        int 20h
        dd %1
%endmacro

segment _LTEXT public class=CODE use32 align=16
segment _LDATA public class=CODE use32 align=16

extern _pdr_control
extern _pdr_aer
extern _pdr_request

segment _LDATA
global WINPDR_DDB
WINPDR_DDB:
        dd 0                    ; next
        dw 0400h                ; SDK version
        dw 0                    ; device id: undefined
        db 1, 0                 ; version 1.0
        dw 0                    ; flags
%ifdef PDR_DISK
        db 'WINDSKPD'           ; name
%else
        db 'WINUSBPD'           ; name
%endif
        dd 080000000h           ; init order: undefined
        dd Control
        dd 0, 0                 ; V86 / PM API
        dd 0, 0                 ; CS:IP
        dd 0                    ; reference data: the DRP (set by pdr_control: a reference
                                ; from here to the data object would get the wrong object)
        dd 0, 0                 ; services
        dd 0                    ; Win32 services
        dd 0x50726576           ; 'Prev'
        dd 80                   ; DDB size
        dd 0x52737631, 0x52737632, 0x52737633   ; 'Rsv1' 'Rsv2' 'Rsv3'

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
        pushad
        cld
        push edx
        push ebx
        push eax
        call _pdr_control
        add esp, 12
        bt eax, 0               ; 1 -> carry (fail)
        popad
        ret

; IOS asynchronous event routine: C function with the AEP pointer on the stack
global _thk_aer
_thk_aer:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        cld
        push dword [ebp+8]
        call _pdr_aer
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; IOS request routine: C function with the IOP pointer on the stack
global _thk_request
_thk_request:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        cld
        push dword [ebp+8]
        call _pdr_request
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 ios_register(void *drp)
global _ios_register
_ios_register:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword [ebp+8]
        VMMCALL SVC_IOS_Register
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; void pdr_call1(void *fn, void *arg): C-style call with one argument (ILB services, callbacks)
global _pdr_call1
_pdr_call1:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword [ebp+12]
        call [ebp+8]
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; services of the VxD (WINUSB.VXD 3D34h, WINDISK.VXD 3D33h): arguments copied through
%ifdef PDR_DISK
%define COREID 0x3D33
%else
%define COREID 0x3D34
%endif
%macro CORESVC 3
global %1
%1:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
%assign i %3
%rep %3
        push dword [ebp + 4 + 4*i]
%assign i i-1
%endrep
        int 20h
        dd (COREID << 16) | %2
        add esp, 4*%3
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
%endmacro

CORESVC _core_get_version, 0, 0
CORESVC _core_blk_lookup, 1, 3
CORESVC _core_blk_io, 2, 6
CORESVC _core_blk_status, 3, 1
CORESVC _core_cd_media, 4, 1
CORESVC _core_cd_command, 5, 5

; u32 vmm_get_ddb(u32 devid) -> DDB or 0 (Windows 95)
global _vmm_get_ddb
_vmm_get_ddb:
        push ecx
        push edi
        mov eax, [esp+12]
        xor edi, edi
        VMMCALL SVC_Get_DDB
        mov eax, ecx
        pop edi
        pop ecx
        ret

; u32 vmm_init_state(void): VMM_GetSystemInitState
global _vmm_init_state
_vmm_init_state:
        push ecx
        push edx
        VMMCALL 00010111h
        pop edx
        pop ecx
        ret

global _pdr_outb
_pdr_outb:
        mov edx, [esp+4]
        mov eax, [esp+8]
        out dx, al
        ret
