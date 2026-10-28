; wbsvc.asm - WINUSBW.SYS: the services of WINUSB.VXD for the WDM bridge (inc/wdmbr.h), called
; as VxD services (INT 20h dynamic links) from the WDM driver; cdecl, the arguments copied
        bits 32

%include "vxdsvc.inc"

%define COREID 0x3D34

segment _TEXT public class=CODE use32 align=16

; VxD service with n dword arguments, cdecl
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

CORESVC _wb_version, 0, 0
CORESVC _wb_attach, 6, 1
CORESVC _wb_list, 7, 2
CORESVC _wb_desc, 8, 5
CORESVC _wb_queue, 9, 1
CORESVC _wb_cancel, 10, 1
CORESVC _wb_frame, 11, 1
CORESVC _wb_log, 12, 1

; unsigned long wb_present(void): the DDB of WINUSB.VXD (Get_DDB), 0 when it is not loaded
global _wb_present
_wb_present:
        push ebx
        push esi
        push edi
        mov eax, COREID
        xor edi, edi
        int 20h
        dd SVC_Get_DDB
        mov eax, ecx
        pop edi
        pop esi
        pop ebx
        ret

; unsigned long irq_off(void) / void irq_on(unsigned long f): the flags, interrupts off / back
global _irq_off
_irq_off:
        pushfd
        pop eax
        cli
        ret

global _irq_on
_irq_on:
        push dword [esp+4]
        popfd
        ret
