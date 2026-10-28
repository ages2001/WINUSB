; winstor.asm - WINDISK.SYS / WINUSB.SYS: DOS block device driver front end of the
; 32-bit driver core
;
; Resident part: device header, strategy/interrupt, control block (CB),
; protected-mode thunk (raw switch or VCPI) into the 32-bit core image that
; lives in an XMS block, INT 2Fh API / Windows startup handover, timer and
; IRQ hooks used by the DOS-mode USB stack, 8042 injection queue drain.
;
; Build: nasm -f bin -o WINDISK.SYS winstor.asm
;        nasm -f bin -DPKG_USB -o WINUSB.SYS winstor.asm
;        nasm -f bin -DPKG_AUDIO -o WINAUDIO.SYS winstor.asm (character device WINAUD$)

        bits 16
        org 0

%ifdef PKG_USB
%define DRVNAME         'WINUSB'
%define HDRNAME         'WINUSB '
%define BANNER          'WINUSB 1.1 - USB driver for DOS and Windows 1.0-3.11'
%define COREFILE        'WINUCORE.BIN'
%define VXDFILE         'WINUSB.386'
%define DOSACTIVE       'USB active in DOS / standard mode'
MPLEXID         equ 0D4h
DEFSLOTS        equ 2
%elifdef PKG_AUDIO
%define DRVNAME         'WINAUDIO'
%define BANNER          'WINAUDIO 1.1 - HD Audio / AC97 sound driver for Windows 3.0-3.11'
%define COREFILE        'WINACORE.BIN'
%define VXDFILE         'WINAUDIO.386'
%define DOSACTIVE       'sound active in DOS / standard mode'
MPLEXID         equ 0D5h
DEFSLOTS        equ 0
%else
%define PKG_DISK
%define DRVNAME         'WINDISK'
%define HDRNAME         'WINDISK'
%define BANNER          'WINDISK 1.1 - IDE/AHCI/NVMe disk driver for DOS and Windows 3.0-3.11'
%define COREFILE        'WINDCORE.BIN'
%define VXDFILE         'WINDISK.386'
%define DOSACTIVE       'active'
MPLEXID         equ 0D3h
DEFSLOTS        equ 0
%endif

CBF_VXD         equ 0001h
CBF_DOSCORE     equ 0002h
CBF_USB         equ 0008h
CBF_VCPI        equ 0020h
CBF_NOCORE      equ 0040h
CBF_WHEEL4      equ 0100h
CBF_AUXWIN      equ 0400h
CBF_AUXBIOS     equ 0800h
CBF_MBOX        equ 1000h
CBF_HAVEDISK    equ 2000h
CBF_CHARDEV     equ 4000h

OPT_USB         equ 0001h
OPT_NOAHCI      equ 0002h
OPT_NONVME      equ 0004h
OPT_ALL         equ 0008h
OPT_VERBOSE     equ 0010h
OPT_DEBUG       equ 0020h
OPT_NOVXD       equ 0040h
OPT_NOHID       equ 0080h
OPT_NOMSC       equ 0100h
OPT_COM1        equ 0200h
OPT_NOKBC       equ 0400h
OPT_NVME        equ 0800h
OPT_NOIDE       equ 1000h
OPT_PIO         equ 2000h
OPT_WINONLY     equ 4000h
OPT_ISA         equ 8000h
OPT_SCREEN      equ 00010000h
OPT_POLL        equ 00020000h
OPT_NOXHCI      equ 00040000h
OPT_NOEHCI      equ 00080000h
OPT_NOOHCI      equ 00100000h
OPT_NOUHCI      equ 00200000h
OPT_NOROUTE     equ 00400000h
OPT_NOHDA       equ 00800000h
OPT_NOAC97      equ 01000000h
OPT_INVEAPD     equ 02000000h
OPT_NOGENERIC   equ 04000000h
OPT_HDAIMM      equ 08000000h
OPT_NOSYNTH     equ 10000000h
OPT_POSBUF      equ 20000000h
OPT_HDAGPIO     equ 40000000h
OPT_NOI2C       equ 80000000h
OPT2_NOTB       equ 0001h
OPT2_NOTBPCIE   equ 0002h
OPT2_NOTBUSB    equ 0004h
OPT2_PCIHP      equ 0008h
OPT2_DOSDISK    equ 0010h
OPT2_NODISK     equ 0020h
OPT2_NORELOC    equ 0040h
OPT2_NOPS2      equ 0400h
OPT2_NOTP       equ 1000h
OPT2_NOSMB      equ 2000h
OPT2_ALL4G      equ 0800h
OPT2_DSP        equ 0080h
OPT2_DMIC       equ 0100h
OPT2_NODSP      equ 0200h
OPT2_NOTPVOL    equ 4000h
OPT2_USBSB      equ 8000h

FN_INIT         equ 0
FN_DOS          equ 1
FN_POLL         equ 2
FN_IRQ          equ 3
FN_API          equ 4
FN_SHUTDOWN     equ 5
FN_RESUME       equ 6
FN_FILEDONE     equ 7
FN_SBIO         equ 8
FN_SBTICK       equ 9
WIN_SBOFF       equ 2192
WIN_DOSOFF      equ 2192            ; struct windos (WINUSB, WINDISK)
CB_TSCKHZ       equ 352
SBF_ON          equ 01h
SBF_NOOPL       equ 02h
SBF_NOMPU       equ 04h
SBF_DOSTRAP     equ 08h
SBF_RTC         equ 10h
SBF_OFF         equ 80h
CBF_FWREQ       equ 8000h
CB_FWTAB        equ 448
WIN_MAXFW       equ 16

SEL_CODE32      equ 08h
SEL_DATA32      equ 10h
SEL_CODE16      equ 18h
SEL_DATA16      equ 20h
SEL_TSS         equ 28h
SEL_VCPI        equ 30h

%ifdef PKG_AUDIO
; the built-in V86 monitor of the core (vmon.c, dpmi.c)
FN_VMSTART      equ 10
HC_CORE         equ 0
HC_RET          equ 1
HC_DPMI         equ 2
HC_RAWPM        equ 3
HC_TOREAL       equ 4
HC_CB           equ 16
VM_NCB          equ 32
DPMI_PARAS      equ 100h
VMF_NODPMI      equ 0002h
VMF_NOVCPI      equ 0004h
VMF_NOVME       equ 0008h
VMF_NOV86       equ 0040h
VMF_ON          equ 0001h
VMF_WIN         equ 0020h
VMF_WINSTD      equ 0100h
VMF_VCPI        equ 0200h
VMF_USEVME      equ 0400h
SBF_V86         equ 40h
VMB             equ cb + 2304           ; struct winvm (WIN_VMOFF)
VM_FLAGS        equ VMB + 0
VM_DRVSEG       equ VMB + 2
VM_XMS          equ VMB + 4
VM_ORET         equ VMB + 8
VM_ORAWPM       equ VMB + 10
VM_OCBS         equ VMB + 12
VM_ORMNULL      equ VMB + 14
VM_ORM16        equ VMB + 16
VM_OTERM        equ VMB + 18
VM_OCALLRET     equ VMB + 20
VM_OVRET        equ VMB + 22
VM_OSAVESS      equ VMB + 24
VM_OSAVEESP     equ VMB + 26
VM_OGW          equ VMB + 28
VM_OGWPM        equ VMB + 30
VM_OGWVC        equ VMB + 32
VM_ORMSS        equ VMB + 34
VM_CPU          equ VMB + 36
VM_NCBF         equ VMB + 37
VM_OIRET        equ VMB + 38
VM_BIOS70       equ VMB + 40
VM_ORMTGT       equ VMB + 56
VM_OVMON        equ VMB + 60
; VCPI gateway data (VMGW_* in winapi.h)
VMGW_CR3        equ 0
VMGW_CR4        equ 4
VMGW_GDTR       equ 8
VMGW_IDTR       equ 14
VMGW_TSSD       equ 20
VMGW_ESP0P      equ 24
VMGW_EXIT       equ 28
VMGW_LDT        equ 34
VMGW_TR         equ 36
VMGW_DS         equ 38
VMGW_CCR3       equ 40
VMGW_CGDTR      equ 44
VMGW_CIDTR      equ 50
VMGW_CLDT       equ 56
VMGW_CTR        equ 58
VMGW_CEIP       equ 60
VMGW_CCS        equ 64
VMGW_REGS       equ 68
VMGW_FRAME      equ 100
VMGW_STK        equ 136
VMGW_SIZE       equ 152
%macro HCALL 1
        db 63h, 0C0h + %1               ; ARPL: invalid in V86 mode, the monitor takes it
%endmacro
%endif

; control block field offsets (see winapi.h)
CB_MAGIC        equ 0
CB_VXDENTRY     equ 8
CB_FLAGS        equ 12
CB_NUNITS       equ 16
CB_FIRSTDRIVE   equ 17
CB_SLOTREQ      equ 106
CB_OPTS         equ 20
CB_BIOSGEOM     equ 72
CB_SHIRQ        equ 302
CB_HCBDF        equ 328
CB_HCIRQ        equ 344
CB_COREPHYS     equ 24
CB_CORESIZE     equ 28
CB_IRQMASK      equ 32
CB_DRVSEG       equ 34
CB_MPLEX        equ 36
CB_DOSVER       equ 38
CB_NBIOS        equ 104
CB_NMOUNTED     equ 105
CB_MOUNTED      equ 108
CB_BPBPTR       equ 236
CB_CBLIN        equ 284
CB_TICKS        equ 288
CB_VXDPATH      equ 356
CB_UNITS        equ 448
CB_OPTS2        equ 2046
CB_KQHEAD       equ 2000
CB_KQTAIL       equ 2001
CB_KQ           equ 2002
KQLEN           equ 20

; ---------------------------------------------------------------------------
; device header
hdr:    dd -1
%ifdef PKG_AUDIO
        dw 8000h                ; character device
        dw strategy
        dw interrupt
hdr_units:
        db 'WINAUD$ '
%else
        dw 08C2h                ; block, 32-bit sectors, generic IOCTL, IOCTL query, removable
        dw strategy
        dw interrupt
hdr_units:
        db 1
        db HDRNAME
%endif

; ---------------------------------------------------------------------------
; resident data
reqptr          dd 0
old2f           dd 0
old08           dd 0
old09           dd 0
old74           dd 0
old15           dd 0
old13           dd 0
old28           dd 0
old16           dd 0
tick16          dw 0FFFFh
oldirq          times 16 dd 0
xmsentry        dd 0
emb_handle      dw 0
emb_phys        dd 0
pd_phys         dd 0
drv_lin         dd 0
corefn          times 12 dd 0
core_stack      dd 0
call_fn         dd 0
call_arg        dd 0
call_ret        dd 0
save_cr0        dd 0
save_cr3        dd 0
save_ss         dw 0
save_sp         dw 0
save_esp        dd 0
rm_gdtr         dw 0
                dd 0
rm_idtr         dw 03FFh
                dd 0
busy            db 0
mode_vcpi       db 0
core_ok         db 0
handover        db 0            ; Windows enhanced mode owns the hardware
dos_usb         db 0
a20_local       db 0            ; 1: XMS local enable, 2: port 92h, to be undone
a20_p92         db 0
mplex           db MPLEXID
tickdiv         db 0
irqchain        dd 0

; INT 33h mouse driver state (the standard DOS mouse API, used by DOS programs and by the
; "Microsoft Mouse" driver of Windows 1.0x / 2.x). Position is tracked from the USB mouse
; mailbox the core fills; the resident provider needs no hook inside Windows.
m33_on          db 0            ; INT 33h provider installed
old33           dd 0
m33_x           dw 320
m33_y           dw 100
m33_btn         db 0
m33_lastbtn     db 0
m33_show        dw 0FFFFh       ; cursor visibility count, < 0 = hidden
m33_xmin        dw 0
m33_xmax        dw 639
m33_ymin        dw 0
m33_ymax        dw 199
m33_mickx       dw 0            ; motion counters for function 0Bh
m33_micky       dw 0
m33_pcnt        dw 0, 0, 0      ; button press counts
m33_rcnt        dw 0, 0, 0      ; button release counts
m33_px          dw 0, 0, 0      ; cursor x/y at the last press / release of each button
m33_py          dw 0, 0, 0
m33_rx          dw 0, 0, 0
m33_ry          dw 0, 0, 0
m33_hand        dd 0            ; function 0Ch event handler
m33_mask        dw 0
m33_wheel       db 0            ; accumulated wheel counter (CuteMouse API)
m33_wmode       db 0            ; a client enabled the wheel API (function 11h)

pm_target       dd 0            ; linear of pm32
                dw SEL_CODE32
rm_target       dw rm_real
rm_seg          dw 0
vcpi_pmentry    dd 0
                dw SEL_VCPI
vcpi_sw_lin     dd 0

; VCPI switch structure
                align 4
vcpi_sw:
vsw_cr3         dd 0
vsw_gdtr        dd 0
vsw_idtr        dd 0
vsw_ldtr        dw 0
vsw_tr          dw SEL_TSS
vsw_eip         dd 0
vsw_cs          dw SEL_CODE32

                align 8
gdt:            times 9 dq 0
gdt_end:
gdtr            dw gdt_end - gdt - 1
                dd 0
                align 8
pm_idt:         times 32 dq 0
pm_idtr         dw 32 * 8 - 1
                dd 0
                align 4
tss:            times 104 db 0

; Win386 startup information structure
sis:            db 3, 0
sis_next        dd 0
sis_path        dd 0
sis_ref         dd 0
sis_inst        dd 0

                align 16
cb:             times 2048 db 0
; wincbx, right after the control block: the PS/2 touchpad's bytes caught on IRQ 12 for the
; core, and the packets the core makes for the pointing device BIOS callback of the DOS
; mouse driver (no 8042 round trip while the touchpad speaks its own protocol)
cbx_mode        db 0            ; bit 0: aux bytes go to cbx_q
cbx_bios        db 0            ; inside a pointing device BIOS call: aux bytes to the BIOS
cbx_head        db 0
cbx_tail        db 0
cbx_bioscnt     db 0            ; pointing device BIOS calls so far
cbx_ph          db 0            ; packets: written by the core
cbx_pt          db 0            ;          delivered here
cbx_psize       db 0            ; packet size the DOS driver asked for (C205h BH)
cbx_fn          dd 0            ; its callback (C207h ES:BX)
cbx_q           times 64 db 0
cbx_pk          times 16*4 db 0 ; status, X, Y, Z
auxdeliv        db 0
; the full BPBs of the units (WIN_BPBXOFF: FAT32 needs 53 bytes), 64 bytes each
                times 2304 - ($ - cb) db 0
%ifdef PKG_AUDIO
bpbx:           times 64 db 0           ; struct winvm (no units, no BPBs)
%else
bpbx:           times 24 * 64 db 0
%endif

; ---------------------------------------------------------------------------
strategy:
        mov [cs:reqptr], bx
        mov [cs:reqptr+2], es
        retf

interrupt:
        pushf
        cli
        cmp byte [cs:rq_onstk], 0
        jne .ownstk
        mov byte [cs:rq_onstk], 1
        mov [cs:rq_ss], ss
        mov [cs:rq_sp], sp
        push cs
        pop ss
        mov sp, rq_stack_top
        call .body
        cli
        mov ss, [cs:rq_ss]
        mov sp, [cs:rq_sp]
        mov byte [cs:rq_onstk], 0
        popf
        retf
.ownstk:
        call .body
        popf
        retf
.body:
        sti
        push ds
        push es
        pushad
        les bx, [cs:reqptr]
        cmp byte [es:bx+2], 0
        jne .notinit
        jmp do_init
.notinit:
        test word [cs:cb+CB_FLAGS], CBF_CHARDEV
        jz .blockreq
        mov word [es:bx+3], 8103h       ; character device without functions
        jmp .done
.blockreq:
        test word [cs:cb+CB_FLAGS], CBF_VXD
        jnz .vxd
        cmp byte [cs:core_ok], 1
        jne .fail
        cmp byte [cs:handover], 0
        jne .fail
        movzx eax, word [cs:reqptr+2]
        shl eax, 4
        movzx ebx, word [cs:reqptr]
        add ebx, eax
        mov eax, FN_DOS
        call callcore
        cmp eax, -1
        je .fail
        jmp .done
.vxd:
        xor ax, ax
        call far [cs:cb+CB_VXDENTRY]
        cmp ax, 0FFFFh
        je .fail
        jmp .done
.fail:
        les bx, [cs:reqptr]
        mov word [es:bx+3], 810Ch       ; error, done, general failure
.done:
        call kq_drain
        popad
        pop es
        pop ds
        ret

; ---------------------------------------------------------------------------
; callcore: EAX = function index, EBX = argument. Returns EAX (-1 if busy/unavailable).
callcore:
        pushf
        cli
        cmp byte [cs:busy], 0
        jne .busy
        cmp byte [cs:core_ok], 1
        jne .busy
        mov byte [cs:busy], 1
        push ds
        push es
        push fs
        push gs
        pushad
        mov [cs:call_fn], eax
        mov [cs:call_arg], ebx
        mov [cs:save_ss], ss
        mov [cs:save_sp], sp
        mov [cs:save_esp], esp
        smsw ax
        test al, 1
        jnz .vcpi
        ; ---- real mode: raw switch with paging ----
        call a20_enable
        jnc .a20ok
        mov dword [cs:call_ret], -1
        jmp common_ret
.a20ok:
        sgdt [cs:rm_gdtr]
        sidt [cs:rm_idtr]
        mov eax, cr3
        mov [cs:save_cr3], eax
        lgdt [cs:gdtr]
        lidt [cs:pm_idtr]
        mov eax, [cs:pd_phys]
        mov cr3, eax
        mov eax, cr0
        mov [cs:save_cr0], eax
        or eax, 80000001h
        mov cr0, eax
        jmp far dword [cs:pm_target]
.vcpi:
%ifdef PKG_AUDIO
        cmp byte [cs:vm_on], 0          ; the built-in V86 monitor: straight into the core
        jne .vmc
%endif
        mov byte [cs:mode_vcpi], 1
        and byte [cs:gdt+SEL_TSS+5], 0FDh       ; TSS not busy
        mov esi, [cs:vcpi_sw_lin]
        mov ax, 0DE0Ch
        int 67h
        ; only reached if the switch failed
        mov byte [cs:mode_vcpi], 0
        mov dword [cs:call_ret], -1
        jmp common_ret
.busy:
        popf
        mov eax, -1
        ret
%ifdef PKG_AUDIO
.vmc:   mov eax, [cs:call_fn]
        mov ebx, [cs:call_arg]
        HCALL HC_CORE
        mov [cs:call_ret], eax
        jmp common_ret
%endif

; 16-bit protected-mode return path (CS = SEL_CODE16, base = driver)
rm16:
        mov ax, SEL_DATA16
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov eax, cr0
        and eax, 7FFFFFFEh
        mov cr0, eax
        jmp far [cs:rm_target]
rm_real:
        mov ax, cs
        mov ds, ax
        mov eax, [save_cr3]
        mov cr3, eax
        lidt [rm_idtr]
        lgdt [rm_gdtr]
        mov ss, [save_ss]
        mov esp, [save_esp]
        call a20_restore
        jmp common_ret

vcpi_ret:                       ; back in V86 mode
        mov ax, cs
        mov ds, ax
        mov ss, [save_ss]
        mov esp, [save_esp]
common_ret:
        mov byte [cs:busy], 0
        popad
        pop gs
        pop fs
        pop es
        pop ds
        mov eax, [cs:call_ret]
        popf
        ret

; A20 must be on for the raw switch (paging maps physical addresses 1:1 for the core).
; This runs from interrupt handlers too, where HIMEM may be in the middle of its own
; A20 or block move code and must not be re-entered: the state is tested directly,
; the fast A20 gate (port 92h) is used and restored, and XMS is only a fallback
; outside of hardware interrupts. CF set: A20 could not be enabled.
a20_check:                      ; CF clear when A20 is enabled
        push ds
        push es
        push ax
        xor ax, ax
        mov ds, ax
        not ax
        mov es, ax
        mov ax, [ds:0500h]
        cmp ax, [es:0510h]
        jne .on
        not word [ds:0500h]
        mov ax, [ds:0500h]
        cmp ax, [es:0510h]
        not word [ds:0500h]
        jne .on
        stc
        jmp .x
.on:    clc
.x:     pop ax
        pop es
        pop ds
        ret

a20_enable:
        mov byte [cs:a20_local], 0
        call a20_check
        jnc .ok
        in al, 92h
        cmp al, 0FFh
        je .xms
        mov [cs:a20_p92], al
        or al, 02h
        and al, 0FEh                    ; never pulse the reset bit
        out 92h, al
        call a20_check
        jc .p92fail
        mov byte [cs:a20_local], 2
        jmp .ok
.p92fail:
        mov al, [cs:a20_p92]
        out 92h, al
.xms:   cmp byte [cs:irq_onstk], 0
        jne .fail
        cmp dword [cs:xmsentry], 0
        je .fail
        mov ah, 05h
        call far [cs:xmsentry]
        cmp ax, 1
        jne .fail
        mov byte [cs:a20_local], 1
.ok:    clc
        ret
.fail:  stc
        ret

a20_restore:
        cmp byte [cs:a20_local], 1
        jne .p92
        mov ah, 06h
        call far [cs:xmsentry]
        ret
.p92:   cmp byte [cs:a20_local], 2
        jne .x
        mov al, [cs:a20_p92]
        out 92h, al
.x:     ret

; ---------------------------------------------------------------------------
; 32-bit protected mode part (CS = flat, EIP = driver linear + offset)
        bits 32
pm32:
        cld                     ; the interrupted code may run with DF = 1
        mov ax, SEL_DATA32
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        ; Nothing may be pushed before ESP points to the core stack: ESP still holds the
        ; real-mode SP, and with the flat stack segment a push would land in DOS memory.
        ; The driver's linear address is patched into this instruction at init.
pm32_base:
        mov ebp, strict dword 0
        mov esp, [ebp+core_stack]
        push ebp
        push dword [ebp+call_arg]
        mov eax, [ebp+call_fn]
        call [ebp+corefn+eax*4]
        add esp, 4
        pop ebp
        mov [ebp+call_ret], eax
        cmp byte [ebp+mode_vcpi], 0
        jne .vback
        jmp SEL_CODE16:rm16
.vback:
        mov byte [ebp+mode_vcpi], 0
        ; the VCPI server reloads its own CR3 before reading the frame:
        ; the frame must live in conventional memory
        lea esp, [ebp+vstack_top]
        movzx eax, word [ebp+rm_seg]
        movzx ecx, word [ebp+save_ss]
        mov edx, [ebp+save_esp]
        push eax                ; GS
        push eax                ; FS
        push eax                ; DS
        push eax                ; ES
        push ecx                ; SS
        push edx                ; ESP
        push dword 00023002h    ; EFLAGS (VM, IOPL3)
        push eax                ; CS
        push dword vcpi_ret     ; EIP
        mov ax, 0DE0Ch
        call far dword [ebp+vcpi_pmentry]

; exception reporting while in protected mode: print and halt
%macro EXCSTUB 1
exc%1:  push strict dword %1
        jmp strict near exc_common
%endmacro
%assign i 0
%rep 32
EXCSTUB i
%assign i i+1
%endrep

exc_common:
        mov ax, SEL_DATA32
        mov ds, ax
        mov esi, [esp]          ; vector
        mov ebx, [esp+4]        ; error code or EIP
        mov ecx, [esp+8]
        mov edx, [esp+12]
        ; also on the first text line (white on red), for machines without port E9h/COM
        mov edi, 0B8000h
        push esi
        call pm_base
        lea esi, [eax+exc_text]
.t:     mov al, [esi]
        test al, al
        jz .td
        mov [edi], al
        mov byte [edi+1], 4Fh
        add edi, 2
        inc esi
        jmp .t
.td:    pop esi
        mov al, 'X'
        out 0E9h, al
        mov eax, esi
        call hex32
        mov eax, ebx
        call hex32
        mov eax, ecx
        call hex32
        mov eax, edx
        call hex32
        mov eax, cr2
        call hex32
.halt:  cli
        hlt
        jmp .halt

hex32:  push ecx
        mov ecx, 8
.l:     rol eax, 4
        push eax
        and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     out 0E9h, al
        mov [edi], al
        mov byte [edi+1], 4Fh
        add edi, 2
        pop eax
        loop .l
        mov al, ' '
        out 0E9h, al
        mov [edi], al
        mov byte [edi+1], 4Fh
        add edi, 2
        pop ecx
        ret

; EAX = linear address of the driver (from the patched instruction at pm32_base)
pm_base:
        call .here
.here:  pop eax
        sub eax, .here - pm_base
        sub eax, pm_base
        ret

exc_text        db DRVNAME, ' CORE EXCEPTION (vector, error/EIP, ..., CR2): ', 0

        bits 16

; ---------------------------------------------------------------------------
; 8042 injection queue drain (USB keyboard/mouse in DOS and standard mode)
kq_drain:
        push ax
        push bx
        push cx
        mov bl, [cs:cb+CB_KQTAIL]
        cmp bl, [cs:cb+CB_KQHEAD]
        je .x
        in al, 64h
        test al, 03h
        jnz .x
        xor bh, bh
        shl bx, 1
        mov ax, [cs:cb+CB_KQ+bx]
        xchg al, ah
        out 64h, al
        mov cx, 4000
.w:     in al, 64h
        test al, 02h
        loopnz .w
        mov al, ah
        out 60h, al
        mov al, [cs:cb+CB_KQTAIL]
        inc al
        cmp al, KQLEN
        jb .s
        xor al, al
.s:     mov [cs:cb+CB_KQTAIL], al
.x:     pop cx
        pop bx
        pop ax
        ret

; ---------------------------------------------------------------------------
; INT 15h: track the BIOS pointing device state, so mouse packets are injected into
; the aux port only while a DOS mouse driver consumes them
int15:
        cmp ah, 0C2h
        jne .chain
%ifdef PKG_USB
        cmp al, 07h
        jne .n7
        mov [cs:cbx_fn], bx
        mov [cs:cbx_fn+2], es
.n7:    cmp al, 05h
        jne .n5
        mov [cs:cbx_psize], bh
.n5:
%endif
        cmp al, 00h
        je .enab
        cmp al, 01h                     ; reset and initialize leave the device disabled
        je .off
        cmp al, 05h
        jne .dev
.off:   and word [cs:cb+CB_FLAGS], ~CBF_AUXBIOS
        jmp .dev
.enab:  and word [cs:cb+CB_FLAGS], ~CBF_AUXBIOS
        test bh, bh
        jz .dev
        or word [cs:cb+CB_FLAGS], CBF_AUXBIOS
.dev:
%ifdef PKG_USB
        ; a touchpad of ours: the BIOS talks to it itself (its answers go to the BIOS), the
        ; core programs it again afterwards
        test byte [cs:cbx_mode], 1
        jz .chain
        inc byte [cs:cbx_bios]
        pushf
        call far [cs:old15]
        dec byte [cs:cbx_bios]
        inc byte [cs:cbx_bioscnt]
        retf 2
%endif
.chain: jmp far [cs:old15]

; ---------------------------------------------------------------------------
; INT 2Fh: API, Windows enhanced mode startup/exit
%include "win30x.inc"
win30on           db 0            ; this driver hooks XMS for Windows 3.0 standard mode

int2f:
        cmp byte [cs:win30on], 0
        je .now30
        WIN30_INT2F old2f
.now30: cmp ah, [cs:mplex]
        je .ours
        cmp ax, 1605h
        je .winstart
        cmp ax, 1606h
        je .winexit
%ifdef PKG_AUDIO
        cmp ax, 1687h
        je .dpmi
%endif
        jmp far [cs:old2f]
%ifdef PKG_AUDIO
.dpmi:  ; DPMI host of the built-in V86 monitor
        cmp byte [cs:vm_on], 0
        je .chain
        cmp byte [cs:handover], 0
        jne .chain
        test word [cs:VM_FLAGS], VMF_NODPMI
        jnz .chain
        xor ax, ax
        mov bx, 1                       ; 32-bit programs too
        mov cl, [cs:VM_CPU]
        mov dx, 005Ah                   ; version 0.90
        mov si, DPMI_PARAS
        push cs
        pop es
        mov di, vm_dpmi
        iret
%endif
.ours:
        cmp al, 00h
        jne .n1
        mov al, 0FFh
        mov bx, 3357h
        push cs
        pop es
        mov di, cb
        iret
.n1:    cmp al, 01h
        jne .n3
        push ds
        push ebx
        push ecx
        push edx
        push esi
        push edi
        test word [cs:cb+CB_FLAGS], CBF_VXD
        jz .apicore
        mov ax, 1
        call far [cs:cb+CB_VXDENTRY]
        jmp .apiret
.apicore:
        mov ax, es
        movzx eax, ax
        shl eax, 4
        movzx ebx, bx
        add ebx, eax
        mov eax, FN_API
        call callcore
.apiret:
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        pop ds
        iret
.n3:    cmp al, 03h                     ; BL bit 0: 4-byte wheel packets, bit 1: WINMOUSE reads the aux
        jne .n4                         ; port, bit 2: WINMOUSE reads the mouse mailbox
        and word [cs:cb+CB_FLAGS], ~(CBF_WHEEL4 | CBF_AUXWIN | CBF_MBOX)
        test bl, 4
        jz .r3a
        or word [cs:cb+CB_FLAGS], CBF_MBOX
.r3a:
        test bl, 1
        jz .r3
        or word [cs:cb+CB_FLAGS], CBF_WHEEL4
.r3:    test bl, 2
        jz .r3b
        or word [cs:cb+CB_FLAGS], CBF_AUXWIN
.r3b:   xor ax, ax
        iret
.n4:    cmp al, 04h                     ; WINAUDIO's V86 monitor starts: become its VCPI client
        jne .n5                         ; (ES:DI three descriptors, EBX protected-mode entry)
        push ds
        push es
        push si
        push di
        push cx
        push es
        pop ds
        mov si, di
        push cs
        pop es
        mov di, gdt + SEL_VCPI
        mov cx, 24
        cld
        rep movsb
        pop cx
        pop di
        pop si
        pop es
        pop ds
        mov [cs:vcpi_pmentry], ebx
        xor ax, ax
        iret
.n5:    mov ax, 0FFFFh
        iret

.winstart:
        test dx, 1                      ; standard mode (DOSX): keep the DOS core
%ifdef PKG_AUDIO
        jz .enh
        or word [cs:VM_FLAGS], VMF_WINSTD       ; it needs VCPI from the monitor
        jmp .chain
.enh:
%else
        jnz .chain
%endif
%ifdef PKG_AUDIO
        call sb_rtc_off
        call vm_emmoff
%endif
        pushf
        call far [cs:old2f]
        test cx, cx
        jnz .wsret
%ifdef PKG_AUDIO
        cmp byte [cs:vm_on], 0
        je .novm
        push cs                         ; DS:SI: the V86 mode switch routine of the monitor
        pop ds
        mov si, vm_switch
.novm:
%endif
        cmp byte [cs:core_ok], 1
        jne .wsret
        pushad
        push ds
        push es
        mov eax, FN_SHUTDOWN
        xor ebx, ebx
%ifdef PKG_USB
        cmp byte [cs:cb+CB_DOSVER+1], 7 ; Windows 95/98/ME: the I/O Supervisor gives the USB
        jb .wsh                         ; disks their letters, the DOS drives are hidden
        mov byte [cs:cb+WIN_DOSOFF+4], 1
        call cds_sync
.wsh:
%endif
        call callcore
        mov byte [cs:handover], 1
        pop es
        pop ds
        popad
        test word [cs:cb+CB_OPTS], OPT_NOVXD
        jnz .wsret
%ifndef PKG_DISK
        cmp byte [cs:cb+CB_DOSVER+1], 7 ; Windows 95/98 load their Plug and Play VxD themselves
        jae .wsret
%endif
        mov [cs:sis_next], bx
        mov [cs:sis_next+2], es
        push cs
        pop es
        mov bx, sis
.wsret: iret
.chain: jmp far [cs:old2f]

.winexit:
        test dx, 1
%ifdef PKG_AUDIO
        jz .enx
        and word [cs:VM_FLAGS], ~VMF_WINSTD
        jmp .chain
.enx:
        test word [cs:VM_FLAGS], VMF_ON
        jz .enx2
        call vm_emmon
.enx2:
%else
        jnz .chain
%endif
        cmp byte [cs:handover], 0
        je .chain
        mov byte [cs:handover], 0
%ifdef PKG_USB
        mov byte [cs:cb+WIN_DOSOFF+4], 0
%endif
        pushad
        push ds
        push es
        mov eax, FN_RESUME
        xor ebx, ebx
        call callcore
%ifdef PKG_AUDIO
        ; Windows left the CPU in real mode without calling the switch routine: the V86
        ; monitor starts again
        test word [cs:VM_FLAGS], VMF_WIN
        jz .vmok
        smsw ax
        test al, 1
        jnz .vmok
        mov eax, FN_VMSTART
        mov ebx, 1
        call callcore
.vmok:
        test word [cs:VM_FLAGS], VMF_ON
        jz .vmoff
        call vm_emmon
.vmoff:
%endif
        pop es
        pop ds
        popad
%ifdef PKG_AUDIO
        call sb_rtc_on
%endif
        jmp far [cs:old2f]

; ---------------------------------------------------------------------------
; INT 33h: the standard DOS mouse API, backed by the USB mouse. Position is kept up to date
; from the mouse mailbox the core fills (CBF_MBOX), so the same provider serves DOS programs
; and the Microsoft Mouse driver of Windows 1.0x / 2.x without any hook inside Windows.

; fold the accumulated mailbox motion into the absolute position and button state. CS = resident.
m33_pull:
        push ax
        push bx
        push cx
        push dx
        push si
        mov ax, [cs:cb+304]             ; mb_dx
        mov bx, [cs:cb+306]             ; mb_dy
        mov word [cs:cb+304], 0
        mov word [cs:cb+306], 0
        add [cs:m33_mickx], ax
        add [cs:m33_micky], bx
        mov dl, [cs:cb+308]             ; mb_dz (wheel)
        mov byte [cs:cb+308], 0
        add [cs:m33_wheel], dl
        mov cx, [cs:m33_x]
        add cx, ax
        mov ax, [cs:m33_xmin]
        cmp cx, ax
        jge .xh
        mov cx, ax
.xh:    mov ax, [cs:m33_xmax]
        cmp cx, ax
        jle .xo
        mov cx, ax
.xo:    mov [cs:m33_x], cx
        mov cx, [cs:m33_y]
        add cx, bx
        mov ax, [cs:m33_ymin]
        cmp cx, ax
        jge .yh
        mov cx, ax
.yh:    mov ax, [cs:m33_ymax]
        cmp cx, ax
        jle .yo
        mov cx, ax
.yo:    mov [cs:m33_y], cx
        ; buttons: bit0 left, bit1 right, bit2 middle
        mov al, [cs:cb+309]             ; mb_btn
        and al, 7
        mov ah, [cs:m33_lastbtn]
        mov [cs:m33_btn], al
        mov [cs:m33_lastbtn], al
        mov dl, al
        xor dl, ah                      ; changed buttons
        xor si, si                      ; button index
.bl:    mov bx, si
        mov cl, bl
        mov ch, 1
        shl ch, cl
        test dl, ch
        jz .bn
        shl bx, 1
        test al, ch
        jz .brel
        inc word [cs:m33_pcnt+bx]       ; press
        mov cx, [cs:m33_x]
        mov [cs:m33_px+bx], cx
        mov cx, [cs:m33_y]
        mov [cs:m33_py+bx], cx
        jmp .bn
.brel:  inc word [cs:m33_rcnt+bx]       ; release
        mov cx, [cs:m33_x]
        mov [cs:m33_rx+bx], cx
        mov cx, [cs:m33_y]
        mov [cs:m33_ry+bx], cx
.bn:    inc si
        cmp si, 3
        jb .bl
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        ret

int33:
        cmp byte [cs:m33_on], 0
        je .chain
        push ds
        push si
        push bp
        push cs
        pop ds
        cmp ax, 0
        je .reset
        cmp ax, 1
        je .show
        cmp ax, 2
        je .hide
        cmp ax, 3
        je .getpos
        cmp ax, 4
        je .setpos
        cmp ax, 5
        je .press
        cmp ax, 6
        je .release
        cmp ax, 7
        je .xrange
        cmp ax, 8
        je .yrange
        cmp ax, 0Bh
        je .motion
        cmp ax, 0Ch
        je .sethand
        cmp ax, 11h
        je .wheel
        cmp ax, 24h
        je .version
        ; unhandled: return harmless
        pop bp
        pop si
        pop ds
        iret
.reset:
        or word [cs:cb+CB_FLAGS], CBF_MBOX
        mov word [m33_x], 320
        mov word [m33_y], 100
        mov byte [m33_btn], 0
        mov byte [m33_lastbtn], 0
        mov word [m33_show], 0FFFFh
        mov word [m33_xmin], 0
        mov word [m33_xmax], 639
        mov word [m33_ymin], 0
        mov word [m33_ymax], 199
        mov dword [m33_hand], 0
        mov ax, 0FFFFh                  ; mouse installed
        mov bx, 2                       ; two buttons
        jmp .out
.show:  inc word [m33_show]
        jmp .ok
.hide:  dec word [m33_show]
        jmp .ok
.getpos:
        call m33_pull
        mov cx, [m33_x]
        mov dx, [m33_y]
        xor bx, bx
        mov bl, [m33_btn]
        cmp byte [m33_wmode], 0
        je .out
        mov bh, [m33_wheel]             ; CuteMouse wheel delta in BH, then cleared
        mov byte [m33_wheel], 0
        jmp .out
.setpos:
        mov [m33_x], cx
        mov [m33_y], dx
        jmp .ok
.press:
        call m33_pull
        and bx, 1                       ; button number (0 left, 1 right)
        mov si, bx
        shl si, 1
        mov cx, [m33_px+si]             ; x at the last press
        mov dx, [m33_py+si]             ; y at the last press
        mov bx, [m33_pcnt+si]           ; press count since the last call
        mov word [m33_pcnt+si], 0
        xor ax, ax
        mov al, [m33_btn]               ; current button state
        jmp .out
.release:
        call m33_pull
        and bx, 1
        mov si, bx
        shl si, 1
        mov cx, [m33_rx+si]
        mov dx, [m33_ry+si]
        mov bx, [m33_rcnt+si]
        mov word [m33_rcnt+si], 0
        mov ax, [m33_btn]
        and ax, 7
        jmp .out
.xrange:
        mov [m33_xmin], cx
        mov [m33_xmax], dx
        jmp .ok
.yrange:
        mov [m33_ymin], cx
        mov [m33_ymax], dx
        jmp .ok
.motion:
        call m33_pull
        mov cx, [m33_mickx]
        mov dx, [m33_micky]
        mov word [m33_mickx], 0
        mov word [m33_micky], 0
        jmp .out
.sethand:
        mov [m33_hand], dx
        mov [m33_hand+2], es
        mov [m33_mask], cx
        jmp .ok
.wheel:
        ; CuteMouse wheel API: declare wheel support and let the client read it itself,
        ; so the wheel-to-keys fallback in the core stops for this client.
        mov byte [m33_wmode], 1
        or word [cs:cb+300], 1          ; CB_WHEELCTL bit 0
        mov ax, 574Dh                   ; 'WM'
        xor bx, bx
        mov cx, 1                        ; wheel present
        jmp .out
.version:
        mov bx, 0631h                   ; report version 6.31
        mov cx, 0400h                   ; CH = type 4 (PS/2), CL = 0
        jmp .out
.ok:    pop bp
        pop si
        pop ds
        iret
.out:   pop bp
        pop si
        pop ds
        iret
.chain: jmp far [cs:old33]

; ---------------------------------------------------------------------------
; timer: drive the DOS-mode USB stack
int08:
        pushf
        call far [cs:old08]
%ifdef PKG_USB
        call cds_sync
%endif
        call dos_safe
        jc .x
        call do_poll
.x:
%ifdef PKG_AUDIO
        call sb_deliver
%endif
        iret

%ifdef PKG_USB
; Empty drive letters are hidden from DOS: the Current Directory Structure entry of a slot
; without a medium is marked invalid, so DOS and programs that look at every drive find no
; drive there ("Invalid drive specification" instead of "Not ready reading drive" or
; "General failure", which a program cannot always get out of). The entry is valid again
; when a medium comes. Only entries whose DPB names this driver are touched. Runs on every
; timer tick in the virtual machine that gets it (Windows 3.x: each MS-DOS box its own CDS).
cds_esz         db 58h
cds_last        db 0
cds_l           db 0
cds_dpbdev      dw 13h
first_ok        db 0            ; the drive of unit 0 checked in the DPB chain
cds_sync:
        push ds
        push es
        pusha
        push cs
        pop ds
        mov ax, [cb+WIN_DOSOFF]
        or ax, [cb+WIN_DOSOFF+2]
        jz .x
        les bx, [cb+WIN_DOSOFF]
        mov al, [es:bx+21h]             ; LASTDRIVE
        mov [cds_last], al
        les di, [es:bx+16h]             ; CDS array
        mov ax, es
        or ax, di
        jz .x
        cmp di, 0FFFFh
        je .x
        mov byte [cds_esz], 58h
        mov word [cds_dpbdev], 13h
        cmp byte [cb+CB_DOSVER+1], 4
        jae .sz
        mov byte [cds_esz], 51h
        mov word [cds_dpbdev], 12h
.sz:    cmp byte [first_ok], 0
        jne .f1
        les bx, [cb+WIN_DOSOFF]
        call first_drive
        les di, [es:bx+16h]
.f1:    xor si, si                      ; unit
.l:     mov al, [cb+CB_NUNITS]
        xor ah, ah
        cmp si, ax
        jae .x
        mov ax, si
        add al, [cb+CB_FIRSTDRIVE]
        cmp al, [cds_last]
        jae .x
        cmp al, 26
        jae .x
        mov [cds_l], al
        mul byte [cds_esz]
        mov bx, di
        add bx, ax                      ; ES:BX: the entry of the letter
        mov bp, si
        shl bp, 6
        test word [es:bx+43h], 0B000h   ; network (MSCDEX took the letter), JOIN, SUBST
        jnz .taken
        push es
        push bx
        les bx, [es:bx+45h]             ; its DPB
        mov ax, es
        or ax, bx
        jz .np
        add bx, [cds_dpbdev]
        cmp word [es:bx], 0             ; device header: this driver
        jne .np
        mov ax, cs
        cmp [es:bx+2], ax
        jne .np
        pop bx
        pop es
        and byte [ds:bp+cb+CB_UNITS+1], ~20h    ; US_TAKEN
        xor cx, cx                      ; hidden
        cmp byte [cb+WIN_DOSOFF+4], 0   ; Windows 95/98/ME runs: all of them
        jne .w
        cmp byte [ds:bp+cb+CB_UNITS], 1 ; UT_FIXED
        je .v
        test byte [ds:bp+cb+CB_UNITS+1], 1      ; US_PRESENT
        jz .w
.v:     mov cx, 4000h
.w:     mov ax, [es:bx+43h]
        mov dx, ax
        and dx, 4000h
        cmp dx, cx
        je .n
        and ax, 0BFFFh
        or ax, cx
        jcxz .set
        ; visible again: at the root directory of the medium
        mov dl, [cds_l]
        add dl, 'A'
        mov [es:bx], dl
        mov word [es:bx+1], 5C3Ah       ; ':\'
        mov byte [es:bx+3], 0
        mov word [es:bx+49h], 0
        mov word [es:bx+4Fh], 2
.set:   mov [es:bx+43h], ax
.n:     inc si
        jmp .l
.np:    pop bx
        pop es
        test word [es:bx+43h], 0C000h   ; a drive of another driver
        jz .n
.taken: cmp byte [ds:bp+cb+CB_UNITS], 2 ; UT_SLOT: no medium goes there
        jne .n
        or byte [ds:bp+cb+CB_UNITS+1], 20h
        jmp .n
.x:     popa
        pop es
        pop ds
        ret
%endif

%ifdef PKG_USB
; The drive of unit 0 from the DPB chain, once DOS has built the DPBs: the number in the INIT
; request is not to be trusted (MS-DOS 7 DEVICEHIGH gives the next letter after the ones the
; driver will get). ES:BX = List of Lists.
first_drive:
        push es
        push bx
        mov cx, 64                      ; guard
        les bx, [es:bx]                 ; first DPB
.d:     cmp bx, 0FFFFh
        je .x
        mov ax, es
        or ax, bx
        jz .x
        push bx
        add bx, [cds_dpbdev]
        cmp word [es:bx], 0
        jne .nx
        mov ax, cs
        cmp [es:bx+2], ax
        jne .nx
        pop bx
        cmp byte [es:bx+1], 0           ; unit 0
        jne .nx2
        mov al, [es:bx]
        mov [cb+CB_FIRSTDRIVE], al
        mov byte [first_ok], 1
        jmp .x
.nx:    pop bx
.nx2:   cmp byte [cb+CB_DOSVER+1], 4
        jb .d3
        les bx, [es:bx+19h]
        jmp .c
.d3:    les bx, [es:bx+18h]
.c:     loop .d
        mov byte [first_ok], 1          ; not found: keep the number of the INIT request
.x:     pop bx
        pop es
        ret
%endif

; DOS idle: DOS is waiting for console input and may be interrupted safely
int28:
        pushf
        call far [cs:old28]
        cmp byte [cs:in13], 0
        jne .x
        pushf
        cli
        call do_poll
        popf
.x:     iret

%ifdef PKG_USB
; keyboard BIOS: a program (or DOS itself, inside a function: COPY CON, handle reads of CON)
; waiting for a key gets the USB keyboard polled; the timer does not poll while DOS is
; inside a function, so without this a USB keyboard would never answer there
int16:
        push ax
        and ah, 0EFh
        cmp ah, 01h
        je .stat
        test ah, ah
        jnz .chain
.lp:    call poll16
        pop ax
        push ax
        inc ah                          ; 00h -> 01h, 10h -> 11h: a key waiting?
        pushf
        call far [cs:old16]
        jnz .chain
        sti
        hlt
        jmp .lp
.stat:  call poll16
.chain: pop ax
        jmp far [cs:old16]

; at most once per timer tick (programs ask for keys in tight loops)
poll16:
        cmp byte [cs:in13], 0
        jne .x
        push ds
        push ax
        xor ax, ax
        mov ds, ax
        mov ax, [046Ch]
        cmp ax, [cs:tick16]
        mov [cs:tick16], ax
        pop ax
        pop ds
        je .x
        pushf
        cli
        call do_poll
        popf
.x:     ret
%endif


%ifndef PKG_AUDIO
; ---------------------------------------------------------------------------
; INT 13h of the BIOS disks on controllers this driver took over from the BIOS (a USB boot
; disk; NVMe with /NVME): the BIOS code of those controllers no longer works, so the calls
; are served from the core (WINF_RAWIO). Without this DOS loses the boot drive ("Bad or
; missing ..." for every file it reads afterwards). Windows 95/98/ME do the same in the VxD.
WINF_RAWIO      equ 0024h
WINF_BIOSMAP    equ 0025h
i13disk         times 8 db 0            ; struct winbiosmap
i13secs         times 8 dd 0
i13secshi       times 8 dd 0
old13s          dd 0
i13req          times 32 db 0
i13stat         times 8 db 0
i13lba          dd 0, 0
i13left         dw 0
i13wr           db 0
i13k            db 0

int13s:
        cmp dl, 80h
        jb .pass
        cmp dl, 88h
        jae .pass
        push bx
        movzx bx, dl
        cmp byte [cs:i13disk+bx-80h], 0
        pop bx
        jne .ours
.pass:  jmp far [cs:old13s]
        ; frame: [bp+2] IP [bp+4] CS [bp+6] flags, [bp-2] DS [bp-4] ES, PUSHAD below:
        ; EAX bp-8 (AL bp-8, AH bp-7), ECX bp-12, EDX bp-16, EBX bp-20, ESI bp-32, EDI bp-36
.ours:  push bp
        mov bp, sp
        push ds
        push es
        pushad
        sti
        mov [cs:i13k], dl
        movzx si, dl
        sub si, 80h                     ; SI = k
        cmp ah, 02h
        je .chs
        cmp ah, 03h
        je .chs
        cmp ah, 42h
        je .ext
        cmp ah, 43h
        je .ext
        cmp ah, 08h
        je .geom
        cmp ah, 15h
        je .type
        cmp ah, 41h
        je .edd
        cmp ah, 48h
        je .params
        cmp ah, 01h
        je .status
        mov di, okfns
        mov cx, okfns_n
.f:     cmp ah, [cs:di]
        je .ok
        inc di
        loop .f
        mov al, 01h                     ; invalid function
        jmp .err
.status:
        mov al, [cs:i13stat+si]
        test al, al
        jnz .err
.ok:    xor al, al
.err:   ; AL = status
        mov [cs:i13stat+si], al
        mov [bp-7], al                  ; AH of the caller
        and word [bp+6], 0FFFEh
        test al, al
        jz .ret
        or word [bp+6], 1
.ret:   popad
        pop es
        pop ds
        pop bp
        iret

        ; CHS: AL sectors, CH cylinder low, CL bits 7-6 cylinder high and 5-0 sector, DH head
.chs:   mov byte [cs:i13wr], 0
        cmp ah, 03h
        jne .chs1
        mov byte [cs:i13wr], 1
.chs1:  movzx ax, al
        mov [cs:i13left], ax
        mov di, si
        shl di, 2
        movzx eax, cl
        shl eax, 2
        mov al, ch                      ; EAX = cylinder (bits 9-8 from CL 7-6)
        and eax, 3FFh
        movzx ebx, byte [cs:cb+CB_BIOSGEOM+di+2]
        mul ebx                         ; * heads
        movzx ebx, byte [bp-15]         ; DH of the caller
        add eax, ebx
        movzx ebx, byte [cs:cb+CB_BIOSGEOM+di+3]
        mul ebx                         ; * sectors per track
        movzx ebx, byte [bp-12]         ; CL
        and bl, 3Fh
        jz .badsec
        dec ebx
        add eax, ebx
        mov [cs:i13lba], eax
        mov dword [cs:i13lba+4], 0
        mov es, [bp-4]
        mov di, [bp-20]                 ; BX of the caller
        call i13xfer
        mov [bp-8], cl                  ; AL: sectors done
        mov al, ah
        jmp .err
.badsec:
        mov al, 04h
        jmp .err

        ; extended: DS:SI packet (count word +2, buffer +4, LBA qword +8)
.ext:   mov byte [cs:i13wr], 0
        cmp ah, 43h
        jne .ext1
        mov byte [cs:i13wr], 1
.ext1:  mov ds, [bp-2]
        mov bx, [bp-32]                 ; SI of the caller
        cmp dword [bx+4], 0FFFFFFFFh    ; 64-bit flat buffer: not supported
        je .inval
        mov ax, [bx+2]
        mov [cs:i13left], ax
        mov eax, [bx+8]
        mov [cs:i13lba], eax
        mov eax, [bx+12]
        mov [cs:i13lba+4], eax
        les di, [bx+4]
        push bx
        call i13xfer
        pop bx
        mov ds, [bp-2]
        mov [bx+2], cx
        mov al, ah
        jmp .err
.inval: mov al, 01h
        jmp .err

        ; the BIOS geometry recorded at the start (AH=08h)
.geom:  mov di, si
        shl di, 2
        mov cx, [cs:cb+CB_BIOSGEOM+di]  ; cylinders
        dec cx
        xchg cl, ch
        ror cl, 2
        and cl, 0C0h
        or cl, [cs:cb+CB_BIOSGEOM+di+3]
        mov [bp-12], cx                 ; CX
        mov dh, [cs:cb+CB_BIOSGEOM+di+2]
        dec dh
        mov dl, [cs:cb+CB_NBIOS]
        mov [bp-16], dx                 ; DX
        mov byte [bp-20], 0             ; BL
        mov byte [bp-8], 0              ; AL
        jmp .ok

.type:  mov di, si
        shl di, 2
        mov ax, [cs:i13secs+di+2]
        mov [bp-12], ax                 ; CX:DX sectors
        mov ax, [cs:i13secs+di]
        mov [bp-16], ax
        mov byte [cs:i13stat+si], 0
        mov byte [bp-7], 03h            ; AH = 3: fixed disk
        and word [bp+6], 0FFFEh
        jmp .ret

.edd:   cmp word [bp-20], 55AAh
        jne .inval
        mov word [bp-20], 0AA55h        ; BX
        mov word [bp-12], 0001h         ; CX: packet functions
        mov byte [cs:i13stat+si], 0
        mov byte [bp-7], 21h            ; AH: EDD 1.1
        and word [bp+6], 0FFFEh
        jmp .ret

.params:
        mov ds, [bp-2]
        mov bx, [bp-32]                 ; DS:SI result buffer
        cmp word [bx], 26
        jb .inval
        mov word [bx], 26
        mov word [bx+2], 0
        mov di, si
        shl di, 2
        movzx eax, word [cs:cb+CB_BIOSGEOM+di]
        mov [bx+4], eax
        movzx eax, byte [cs:cb+CB_BIOSGEOM+di+2]
        mov [bx+8], eax
        movzx eax, byte [cs:cb+CB_BIOSGEOM+di+3]
        mov [bx+12], eax
        mov eax, [cs:i13secs+di]
        mov [bx+16], eax
        mov eax, [cs:i13secshi+di]
        mov [bx+20], eax
        mov word [bx+24], 512
        jmp .ok

okfns           db 00h, 04h, 09h, 0Ch, 0Dh, 10h, 11h, 14h, 44h, 47h
okfns_n         equ $ - okfns

; transfer i13left sectors at i13lba to/from ES:DI (disk of BIOS drive i13k):
; AH = 0 or a BIOS error code, CX = sectors done
i13xfer:
        push ebx
        push edx
        push esi
        push edi
        push es
        xor cx, cx
.lp:    cmp word [cs:i13left], 0
        je .done
        ; normalize ES:DI (the core reads at most 64K - 16 at a time)
        mov ax, di
        shr ax, 4
        mov bx, es
        add bx, ax
        mov es, bx
        and di, 0Fh
        movzx bx, byte [cs:i13k]
        movzx eax, byte [cs:i13disk+bx-80h]
        dec eax
        cmp byte [cs:i13wr], 0
        je .rd
        or eax, 80000000h
.rd:    mov word [cs:i13req], WINF_RAWIO
        mov word [cs:i13req+2], 0FFFFh
        mov [cs:i13req+4], eax
        mov eax, [cs:i13lba]
        mov [cs:i13req+8], eax
        mov eax, [cs:i13lba+4]
        mov [cs:i13req+12], eax
        movzx eax, word [cs:i13left]
        cmp eax, 127
        jbe .n
        mov eax, 127
.n:     shl eax, 9
        mov [cs:i13req+16], eax         ; buflen
        mov dword [cs:i13req+20], 0
        mov ax, es
        shl eax, 16
        mov ax, di
        mov [cs:i13req+24], eax         ; buf seg:off
        push cx
        push es
        push di
        call i13api
        pop di
        pop es
        pop cx
        cmp word [cs:i13req+2], 0
        jne .ioerr
        mov eax, [cs:i13req+20]         ; bytes done
        shr eax, 9
        jz .ioerr
        add cx, ax
        sub [cs:i13left], ax
        add [cs:i13lba], eax
        adc dword [cs:i13lba+4], 0
        shl ax, 5                       ; paragraphs
        mov bx, es
        add bx, ax
        mov es, bx
        jmp .lp
.done:  xor ah, ah
        jmp .x
.ioerr: mov ah, 04h                     ; sector not found / read error
        cmp byte [cs:i13wr], 0
        je .x
        mov ah, 0CCh                    ; write fault
.x:     pop es
        pop edi
        pop esi
        pop edx
        pop ebx
        ret

; the request i13req to the core: the DOS core, or the VxD while Windows runs
i13api:
        pushad
        push ds
        push es
        push cs
        pop es
        mov bx, i13req
        test word [cs:cb+CB_FLAGS], CBF_VXD
        jz .core
        mov ax, 1
        call far [cs:cb+CB_VXDENTRY]
        jmp .x
.core:  mov ax, cs
        movzx eax, ax
        shl eax, 4
        movzx ebx, bx
        add ebx, eax
        mov eax, FN_API
        call callcore
.x:     pop es
        pop ds
        popad
        ret
%endif

; BIOS disk calls must not be interrupted by a mode switch: count them
int13:
        inc byte [cs:in13]
        pushf
        call far [cs:old13]
        pushf
        dec byte [cs:in13]
        popf
        retf 2

; CF clear when the interrupted code may be suspended for a poll: DOS is not inside
; a function (InDOS, critical error) and no BIOS disk call is in progress. DOS calls
; HIMEM and the BIOS from inside its functions; a mode switch in the middle of those
; (A20 handling, block moves, BIOS 32-bit transitions) is not safe.
dos_safe:
        cmp byte [cs:in13], 0
        jne .no
        push ds
        push bx
        lds bx, [cs:indos_ptr]
        cmp word [bx-1], 0
        pop bx
        pop ds
        jne .no
        clc
        ret
.no:    stc
        ret

; poll the core on the private stack; interrupts disabled
do_poll:
        cmp byte [cs:dos_usb], 0
        je .x
        cmp byte [cs:handover], 0
        jne .x
        cmp byte [cs:busy], 0
        jne .x
        test word [cs:cb+CB_FLAGS], CBF_VXD
        jnz .x
        cmp byte [cs:irq_onstk], 0
        jne .x
        mov byte [cs:irq_onstk], 1
        mov [cs:irq_ss], ss
        mov [cs:irq_sp], sp
        push cs
        pop ss
        mov sp, irq_stack_top
        pushad
        push ds
        push es
        ; interrupts that arrived at an unsafe moment first
        mov cx, [cs:irq_deferred]
        test cx, cx
        jz .poll
        mov word [cs:irq_deferred], 0
        xor ebx, ebx
.dirq:  shr cx, 1
        jnc .dnext
        push cx
        push ebx
        mov eax, FN_IRQ
        call callcore
        pop ebx
        pop cx
        call irq_unmask
.dnext: inc bx
        test cx, cx
        jnz .dirq
.poll:  mov eax, FN_POLL
        xor ebx, ebx
        call callcore
        call kq_drain
        pop es
        pop ds
        popad
        mov ss, [cs:irq_ss]
        mov sp, [cs:irq_sp]
        mov byte [cs:irq_onstk], 0
%ifdef PKG_USB
        call aux_deliver
%endif
.x:     ret

; BL = IRQ: unmask it in the PIC
irq_unmask:
        push ax
        push cx
        mov cl, bl
        mov ah, 1
        cmp cl, 8
        jae .hi
        shl ah, cl
        not ah
        in al, 21h
        and al, ah
        out 21h, al
        jmp .x
.hi:    sub cl, 8
        shl ah, cl
        not ah
        in al, 0A1h
        and al, ah
        out 0A1h, al
.x:     pop cx
        pop ax
        ret

; keyboard / aux interrupts: after the owner read port 60h, push the next queued byte
int09:
        pushf
        call far [cs:old09]
        call kq_drain
        iret

%ifdef PKG_USB
; IRQ 12: while WINUSB drives a PS/2 touchpad its bytes are queued for the core (decoded at
; once when the moment is safe, else at the next poll); the BIOS gets them during its own
; pointing device calls
int74:
        test byte [cs:cbx_mode], 1
        jz .bios
        cmp byte [cs:cbx_bios], 0
        jne .bios
        push ax
        in al, 64h
        and al, 21h
        cmp al, 21h
        jne .notaux
        in al, 60h
        push bx
        xor bh, bh
        mov bl, [cs:cbx_head]
        mov [cs:cbx_q+bx], al
        inc bl
        and bl, 63
        cmp bl, [cs:cbx_tail]
        je .full
        mov [cs:cbx_head], bl
.full:  pop bx
        mov al, 20h
        out 0A0h, al
        out 20h, al
        pop ax
        call aux_core
        iret
.notaux:
        ; no mouse byte (an IRQ 12 left over): a keyboard byte may wait, and the BIOS
        ; handler would read port 60h and take it (a lost key release); IRQ 1 delivers it
        mov al, 20h
        out 0A0h, al
        out 20h, al
        pop ax
        iret
.bios:  pushf
        call far [cs:old74]
        call kq_drain
        iret

; the core decodes the queued aux bytes now if DOS may be interrupted, then the packets go
; to the DOS mouse driver; all registers kept
aux_core:
        cmp byte [cs:handover], 0
        jne .x
        cmp byte [cs:busy], 0
        jne .x
        cmp byte [cs:irq_onstk], 0
        jne .x
        call dos_safe
        jc .x
        mov byte [cs:irq_onstk], 1
        mov [cs:irq_ss], ss
        mov [cs:irq_sp], sp
        push cs
        pop ss
        mov sp, irq_stack_top
        pushad
        push ds
        push es
        mov eax, FN_IRQ
        mov ebx, 12
        call callcore
        pop es
        pop ds
        popad
        mov ss, [cs:irq_ss]
        mov sp, [cs:irq_sp]
        mov byte [cs:irq_onstk], 0
.x:     call aux_deliver
        ret

; packets of the core to the pointing device BIOS callback, as the BIOS's IRQ 12 handler
; would call it: status, X, Y, Z pushed as words, far call
aux_deliver:
        cmp byte [cs:auxdeliv], 0
        jne .r
        mov byte [cs:auxdeliv], 1
        pushad
        push ds
        push es
.l:     xor bh, bh
        mov bl, [cs:cbx_pt]
        cmp bl, [cs:cbx_ph]
        je .done
        inc byte [cs:cbx_pt]
        and byte [cs:cbx_pt], 15
        test word [cs:cb+CB_FLAGS], CBF_AUXBIOS
        jz .l
        cmp word [cs:cbx_fn+2], 0
        je .l
        shl bx, 2
        xor ah, ah
        mov al, [cs:cbx_pk+bx]
        push ax
        mov al, [cs:cbx_pk+bx+1]
        push ax
        mov al, [cs:cbx_pk+bx+2]
        push ax
        mov al, [cs:cbx_pk+bx+3]
        push ax
        call far [cs:cbx_fn]
        add sp, 8
        jmp .l
.done:  pop es
        pop ds
        popad
        mov byte [cs:auxdeliv], 0
.r:     ret
%else
int74:
        pushf
        call far [cs:old74]
        call kq_drain
        iret
%endif

; USB host controller IRQs
%macro IRQSTUB 1
irqs%1: push ax
        mov al, %1
        jmp irq_common
%endmacro
%assign i 0
%rep 16
IRQSTUB i
%assign i i+1
%endrep

irq_common:
        push bx
        cmp byte [cs:handover], 0
        jne .chain
        cmp byte [cs:busy], 0
        jne .chain
        cmp byte [cs:irq_onstk], 0
        jne .chain
        call dos_safe
        jnc .now
        ; A line shared with another device is never masked: its driver (e.g. the BIOS disk
        ; code in the middle of INT 13h) may wait for that interrupt. Our controllers stop
        ; asserting it (PCI command register: INTx disable) and the interrupt is passed on;
        ; the core serves them and switches INTx back on at the next safe poll.
        push cx
        mov cl, al
        mov bx, 1
        shl bx, cl
        test [cs:cb+CB_SHIRQ], bx
        pop cx
        jz .mask
        or [cs:irq_deferred], bx
        call intx_off
        jmp .chain
.mask:
        ; unsafe moment: mask the line, acknowledge it and service it at the next poll
        push cx
        mov cl, al
        mov bx, 1
        shl bx, cl
        or [cs:irq_deferred], bx
        mov ah, bl
        cmp al, 8
        jae .mhi
        in al, 21h
        or al, ah
        out 21h, al
        mov al, 20h
        out 20h, al
        jmp .mdone
.mhi:   in al, 0A1h
        or al, bh
        out 0A1h, al
        mov al, 20h
        out 0A0h, al
        out 20h, al
.mdone: pop cx
        pop bx
        pop ax
        iret
.now:
        mov byte [cs:irq_onstk], 1
        mov [cs:irq_ss], ss
        mov [cs:irq_sp], sp
        push cs
        pop ss
        mov sp, irq_stack_top
        pushad
        push ds
        push es
        movzx ebx, al
        mov eax, FN_IRQ
        call callcore
        mov [cs:irqchain], eax
        call kq_drain
        pop es
        pop ds
        popad
        mov ss, [cs:irq_ss]
        mov sp, [cs:irq_sp]
        mov byte [cs:irq_onstk], 0
%ifdef PKG_USB
        call aux_deliver
%endif
        cmp dword [cs:irqchain], 1
        jne .chain
        cmp al, 8
        jb .m
        push ax
        mov al, 20h
        out 0A0h, al
        pop ax
.m:     mov al, 20h
        out 20h, al
        pop bx
        pop ax
        iret
.chain:
        xor bh, bh
        mov bl, al
        shl bx, 2
        push word [cs:oldirq+bx]
        pop word [cs:chainvec]
        push word [cs:oldirq+bx+2]
        pop word [cs:chainvec+2]
        pop bx
        pop ax
        jmp far [cs:chainvec]

; AL = IRQ: set INTx disable in the PCI command register of our controllers on that line
intx_off:
        push eax
        push ecx
        push edx
        push si
        mov cl, al
        mov dx, 0CF8h
        in eax, dx
        push eax                        ; the interrupted code may be between CF8h and CFCh
        xor si, si
.n:     cmp [cs:cb+CB_HCIRQ+si], cl
        jne .nx
        push si
        shl si, 1
        movzx eax, word [cs:cb+CB_HCBDF+si]
        pop si
        shl eax, 8
        or eax, 80000004h
        mov dx, 0CF8h
        out dx, eax
        mov dx, 0CFCh
        in ax, dx
        or ax, 0400h
        out dx, ax
.nx:    inc si
        cmp si, 8
        jb .n
        pop eax
        mov dx, 0CF8h
        out dx, eax
        pop si
        pop edx
        pop ecx
        pop eax
        ret

%ifdef PKG_DISK
; ---------------------------------------------------------------------------
; Intel AHCI option ROM bugs with VDS (EMM386 and other DOS memory managers), the DOS
; part of what AHCIFIX.386/AHCIFIXD.SYS do: the ROM unlocks DMA regions with the
; descriptor in DS:DI instead of ES:DI, and uses the first bytes of the EBDA (pointing
; device data) as a DMA area. While a BIOS disk call runs, INT 4Bh goes through
; vds_filter. In Windows enhanced mode WINDISK.386 does the same in protected mode.
int13d:
        cmp byte [cs:handover], 0
        jne .pass
        cmp byte [cs:vds_active], 0
        jne .pass
        push ax
        push es
        mov ax, 40h
        mov es, ax
        test byte [es:7Bh], 20h         ; VDS services present?
        pop es
        pop ax
        jz .pass
        mov byte [cs:vds_active], 1
        push eax
        push es
        xor ax, ax
        mov es, ax
        mov eax, [es:4Bh*4]
        mov [cs:old4b], eax
        mov word [es:4Bh*4], vds_filter
        mov [es:4Bh*4+2], cs
        pop es
        pop eax
        pushf
        call far [cs:old13d]
        push eax
        push es
        pushf
        xor ax, ax
        mov es, ax
        mov eax, [cs:old4b]
        mov [es:4Bh*4], eax
        popf
        pop es
        pop eax
        mov byte [cs:vds_active], 0
        retf 2
.pass:  jmp far [cs:old13d]

vds_filter:
        cmp ah, 81h
        jne .pass
        cmp al, 03h                     ; lock DMA region
        je .lock
        cmp al, 05h                     ; scatter/gather lock
        je .lock
        cmp al, 04h                     ; unlock DMA region
        je .unlock
        cmp al, 06h                     ; scatter/gather unlock
        jne .pass
.unlock:
        cmp dword [es:di+12], 0         ; no physical address: not the locked descriptor
        jne .unl2
        push ax                         ; swap ES and DS for the memory manager ..
        mov ax, es
        push ds
        pop es
        mov ds, ax
        pop ax
        pop word [cs:vds_ret]           ; .. and back on the way out
        pop word [cs:vds_ret+2]
        push cs
        push word vds_unswap
.unl2:  call vds_in_ebda
        jne .pass
        cmp byte [cs:ebda_saved], 0
        je .pass
        push si                         ; the region is free again: restore the EBDA
        push di
        push cx
        push ds
        push es
        push cs
        pop ds
        mov si, ebda_buf
        push ax
        mov ax, 40h
        mov es, ax
        mov es, [es:0Eh]
        pop ax
        xor di, di
        mov cx, 6Ch / 4
        cld
        rep movsd
        pop es
        pop ds
        pop cx
        pop di
        pop si
        mov byte [cs:ebda_saved], 0
        jmp .pass
.lock:  call vds_in_ebda
        jne .pass
        push si
        push di
        push cx
        push ds
        push es
        push ax
        mov ax, 40h
        mov ds, ax
        mov ds, [ds:0Eh]
        pop ax
        push cs
        pop es
        xor si, si
        mov di, ebda_buf
        mov cx, 6Ch / 4
        cld
        rep movsd
        pop es
        pop ds
        pop cx
        pop di
        pop si
        mov byte [cs:ebda_saved], 1
.pass:  jmp far [cs:old4b]

vds_unswap:                             ; the memory manager returned here (flags restored)
        push ax
        mov ax, es
        push ds
        pop es
        mov ds, ax
        pop ax
        jmp far [cs:vds_ret]

; ZF set when the DMA region of the descriptor at ES:DI starts in the first 6Ch bytes
; of the EBDA
vds_in_ebda:
        push ax
        push ds
        mov ax, 40h
        mov ds, ax
        mov ax, [ds:0Eh]
        pop ds
        cmp [es:di+8], ax
        jne .x
        cmp dword [es:di+4], 6Ch
        jae .no
        cmp ax, ax
        jmp .x
.no:    or al, 1                        ; ZF clear (AL restored below)
.x:     pop ax
        ret

old13d          dd 0
old4b           dd 0
vds_ret         dd 0
vds_active      db 0
ebda_saved      db 0
ebda_buf        times 6Ch db 0
%endif

%ifdef PKG_AUDIO
; ---------------------------------------------------------------------------
; Sound Blaster 16 emulation for DOS programs: the card's ports, the FM and MPU-401 ports
; and the registers of the emulated DMA channels are trapped through the QPI of QEMM or of
; JEMM386 / JEMMEX with QPIEMU. The core emulates the card; the frequent status reads (FM
; timers, DSP and MPU status) are answered here. The RTC periodic interrupt services the
; sound between timer ticks and delivers the card's interrupt to the program.
SB_BASE         equ cb + WIN_SBOFF + 0
SB_MPU          equ cb + WIN_SBOFF + 2
SB_IRQ          equ cb + WIN_SBOFF + 4
SB_DMA8         equ cb + WIN_SBOFF + 5
SB_DMA16        equ cb + WIN_SBOFF + 6
SB_FLAGS        equ cb + WIN_SBOFF + 7
SB_EDGE         equ cb + WIN_SBOFF + 8
SB_LINE         equ cb + WIN_SBOFF + 9
SB_RDST         equ cb + WIN_SBOFF + 10
SB_MPUST        equ cb + WIN_SBOFF + 11
SB_OPLIDX       equ cb + WIN_SBOFF + 12
SB_T1           equ cb + WIN_SBOFF + 14
SB_T2           equ cb + WIN_SBOFF + 15
SB_TCTL         equ cb + WIN_SBOFF + 16
SB_TSTAT        equ cb + WIN_SBOFF + 17
SB_PIC          equ cb + WIN_SBOFF + 18
SB_INIRQ        equ cb + WIN_SBOFF + 19
SB_T1LO         equ cb + WIN_SBOFF + 20
SB_T1HI         equ cb + WIN_SBOFF + 24
SB_T2LO         equ cb + WIN_SBOFF + 28
SB_T2HI         equ cb + WIN_SBOFF + 32
SB_TSC80        equ cb + WIN_SBOFF + 36
SB_QPI          equ cb + WIN_SBOFF + 40
SB_NTRAPS       equ cb + WIN_SBOFF + 44
SB_IRQS         equ cb + WIN_SBOFF + 46
SB_TTICKS       equ cb + WIN_SBOFF + 48
SB_SPEC         equ cb + WIN_SBOFF + 52

old70           dd 0
sb_rtcon        db 0            ; the RTC periodic interrupt drives the emulation
sb_inrtc        db 0
sb_tdiv         db 0
sb_oldb         db 0            ; RTC register B before
sb_vec          db 0            ; interrupt vector of the emulated IRQ
sb_irqbit       db 0            ; its bit in the master PIC (0 on the slave)
sb_tmp          db 0
sb_out          db 0
sb_ss           dw 0
sb_sp           dw 0
rtc_ss          dw 0
rtc_sp          dw 0
                align 4
sb_stack        times 640 db 0
sb_stack_top:
rtc_stack       times 640 db 0
rtc_stack_top:

; QPI port trap callback: DX = port, CL bit 2 = output, AL = data. Returns AL for an input;
; every other register is preserved. Interrupts are disabled.
sb_trap:
        push eax
        push bx
        cmp dx, 20h
        je .pic
        mov bx, dx
        sub bx, [cs:SB_BASE]
        cmp bx, 16
        jb .card
        cmp dx, 388h
        jb .other
        cmp dx, 38Bh
        ja .other
        mov bx, dx                      ; 388h/389h: bank 0 (as base+8/9), 38Ah/38Bh: bank 1
        and bx, 3
        cmp bl, 2
        jae .opl
        add bl, 8
        jmp .opl
.card:  cmp bl, 4
        jb .opl
        cmp bl, 8
        je .opl
        cmp bl, 9
        je .opl
        test cl, 4
        jnz .core
        cmp bl, 0Ch                     ; write buffer status: always ready
        je .rdc
        cmp bl, 0Eh                     ; read buffer status (acknowledges the 8-bit IRQ)
        je .rde
        cmp bl, 0Fh                     ; 16-bit IRQ acknowledge
        je .rdf
        jmp .core
.rdc:   mov al, 7Fh
        jmp .x
.rde:   cmp byte [cs:SB_LINE], 0
        jne .core
        mov al, [cs:SB_RDST]
        jmp .x
.rdf:   cmp byte [cs:SB_LINE], 0
        jne .core
        mov al, 0FFh
        jmp .x
.other: mov bx, [cs:SB_MPU]
        test bx, bx
        jz .core
        inc bx
        cmp dx, bx
        jne .core
        test cl, 4
        jnz .core
        mov al, [cs:SB_MPUST]           ; MPU-401 status
        jmp .x
        ; FM: BL = 0/1 bank 0 index/data, 2/3 bank 1, 8/9 bank 0
.opl:   test cl, 4
        jnz .oplw
        test bl, 1
        jnz .ff
        call opl_status
        jmp .x
.ff:    mov al, 0FFh
        jmp .x
.oplw:  test bl, 1
        jnz .opld
        test bl, 2
        jnz .ix1
        mov [cs:SB_OPLIDX], al
        jmp .x
.ix1:   mov [cs:SB_OPLIDX+1], al
        jmp .x
.opld:  test bl, 2
        jnz .bank1
        mov bl, [cs:SB_OPLIDX]
        cmp bl, 2
        jb .bank0
        cmp bl, 4
        ja .bank0
        call opl_timer                  ; timers: here only
        jmp .x
.bank0: mov bh, 0F0h                    ; register write F0rrh / F1rrh to the core
        jmp .reg
.bank1: mov bl, [cs:SB_OPLIDX+1]
        mov bh, 0F1h
.reg:   push dx
        mov dx, bx
        call sb_core
        pop dx
        jmp .x
.core:  call sb_core
        jmp .x
        ; PIC command port while the program's handler of the emulated IRQ runs: its EOI is
        ; for an interrupt the PIC never saw, a read of the in-service register shows it
.pic:   test cl, 4
        jnz .picw
        push ax
        mov ax, 1A00h
        call far [cs:SB_QPI]
        pop ax
        mov al, bl
        cmp byte [cs:SB_PIC], 0Bh
        jne .x
        or al, [cs:sb_irqbit]
        jmp .x
.picw:  cmp al, 20h
        je .x
        mov bl, [cs:SB_IRQ]
        and bl, 7
        or bl, 60h
        cmp al, bl
        je .x
        cmp al, 0Ah
        je .ocw3
        cmp al, 0Bh
        jne .fwd
.ocw3:  mov [cs:SB_PIC], al
.fwd:   mov bl, al
        push ax
        mov ax, 1A01h
        call far [cs:SB_QPI]
        pop ax
.x:     mov [cs:sb_tmp], al
        pop bx
        pop eax
        mov al, [cs:sb_tmp]
        retf

; the core: DX = port, AL = data, CL bit 2 = output; AL = result
sb_core:
        push ebx
        push ecx
        push edx
        push esi
        push edi
        push ebp
        push ds
        push es
        push fs
        push gs
        mov [cs:sb_out], al
        mov [cs:sb_ss], ss
        mov [cs:sb_sp], sp
        push cs
        pop ss
        mov sp, sb_stack_top
        movzx ebx, dx
        movzx eax, al
        shl eax, 16
        or ebx, eax
        test cl, 4
        jz .in
        or ebx, 1000000h
.in:    push cx
        push dx
        mov eax, FN_SBIO
        call callcore
        pop dx
        pop cx
        cmp eax, -1
        je .done
        test ah, 1                      ; SBIO_FWD: the real port (another DMA channel)
        jz .done
        test cl, 4
        jnz .fout
        push ax
        mov ax, 1A00h
        call far [cs:SB_QPI]
        pop ax
        or al, bl
        jmp .done
.fout:  mov bl, [cs:sb_out]
        mov ax, 1A01h
        call far [cs:SB_QPI]
.done:  mov ss, [cs:sb_ss]
        mov sp, [cs:sb_sp]
        pop gs
        pop fs
        pop es
        pop ds
        pop ebp
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret

; FM status: timer flags from the TSC. AL = status, EAX upper part destroyed.
opl_status:
        push edx
        push ecx
        test byte [cs:SB_TCTL], 1
        jz .t2
        test byte [cs:SB_TCTL], 40h
        jnz .t2
        rdtsc
        sub eax, [cs:SB_T1LO]
        sbb edx, [cs:SB_T1HI]
        jnz .e1
        movzx ecx, byte [cs:SB_T1]
        neg ecx
        add ecx, 256
        imul ecx, [cs:SB_TSC80]
        cmp eax, ecx
        jb .t2
.e1:    or byte [cs:SB_TSTAT], 0C0h
.t2:    test byte [cs:SB_TCTL], 2
        jz .done
        test byte [cs:SB_TCTL], 20h
        jnz .done
        rdtsc
        sub eax, [cs:SB_T2LO]
        sbb edx, [cs:SB_T2HI]
        jnz .e2
        movzx ecx, byte [cs:SB_T2]
        neg ecx
        add ecx, 256
        imul ecx, [cs:SB_TSC80]
        shl ecx, 2
        cmp eax, ecx
        jb .done
.e2:    or byte [cs:SB_TSTAT], 0A0h
.done:  pop ecx
        pop edx
        mov al, [cs:SB_TSTAT]
        ret

; FM timer registers: BL = 2, 3 or 4, AL = value
opl_timer:
        cmp bl, 2
        jne .n2
        mov [cs:SB_T1], al
        ret
.n2:    cmp bl, 3
        jne .n4
        mov [cs:SB_T2], al
        ret
.n4:    test al, 80h
        jz .ctl
        mov byte [cs:SB_TSTAT], 0       ; IRQ reset
        ret
.ctl:   push eax
        push edx
        push cx
        mov cl, al
        test cl, 1
        jz .s2
        test byte [cs:SB_TCTL], 1
        jnz .s2
        rdtsc
        mov [cs:SB_T1LO], eax
        mov [cs:SB_T1HI], edx
.s2:    test cl, 2
        jz .s3
        test byte [cs:SB_TCTL], 2
        jnz .s3
        rdtsc
        mov [cs:SB_T2LO], eax
        mov [cs:SB_T2HI], edx
.s3:    mov [cs:SB_TCTL], cl
        mov al, cl
        and al, 60h
        not al
        and [cs:SB_TSTAT], al
        test byte [cs:SB_TSTAT], 60h
        jnz .k
        mov byte [cs:SB_TSTAT], 0
.k:     pop cx
        pop edx
        pop eax
        ret

; RTC periodic interrupt (1024 Hz): the sound every other tick, then the card's interrupt
int70:
        pushf
        call far [cs:old70]
        cmp byte [cs:sb_rtcon], 0
        je .x
        push ax
        mov al, 0Bh                     ; the BIOS may turn the periodic interrupt off
        out 70h, al                     ; when its own wait (INT 15h 86h) ends
        in al, 71h
        test al, 40h
        jnz .pie
        or al, 40h
        mov ah, al
        mov al, 0Bh
        out 70h, al
        mov al, ah
        out 71h, al
.pie:   pop ax
        cmp byte [cs:sb_inrtc], 0
        jne .x
        xor byte [cs:sb_tdiv], 1
        jz .x
        mov byte [cs:sb_inrtc], 1
        inc dword [cs:SB_TTICKS]
        call sb_tick
        call sb_deliver
        mov byte [cs:sb_inrtc], 0
.x:     iret

sb_tick:
        cmp byte [cs:handover], 0
        jne .x
        cmp byte [cs:busy], 0
        jne .x
        test word [cs:cb+CB_FLAGS], CBF_VXD
        jnz .x
        mov [cs:rtc_ss], ss
        mov [cs:rtc_sp], sp
        push cs
        pop ss
        mov sp, rtc_stack_top
        pushad
        push ds
        push es
        mov eax, FN_SBTICK
        xor ebx, ebx
        call callcore
        pop es
        pop ds
        popad
        mov ss, [cs:rtc_ss]
        mov sp, [cs:rtc_sp]
.x:     ret

; the card raised its interrupt: call the program's handler like the PIC would, when the
; IRQ is unmasked and no other interrupt is in service (else at a later tick)
sb_deliver:
        cmp byte [cs:SB_EDGE], 0
        je .x
        cmp byte [cs:SB_INIRQ], 0
        jne .x
        test byte [cs:SB_FLAGS], SBF_DOSTRAP
        jz .x
        push ax
        push bx
        push dx
        push ds
        mov al, 0Bh
        out 20h, al
        in al, 20h
        mov ah, al
        mov al, 0Ah
        out 20h, al
        test ah, ah
        jnz .later
        cmp byte [cs:sb_irqbit], 0
        je .slave
        in al, 21h
        test al, [cs:sb_irqbit]
        jnz .later
        jmp .go
.slave: in al, 0A1h                     ; IRQ 10
        test al, 4
        jnz .later
.go:    mov byte [cs:SB_EDGE], 0
        mov byte [cs:SB_INIRQ], 1
        mov byte [cs:SB_PIC], 0
        mov dx, 20h
        mov ax, 1A09h
        call far [cs:SB_QPI]
        xor bx, bx
        mov ds, bx
        mov bl, [cs:sb_vec]
        shl bx, 2
        pushf
        call far [bx]
        mov dx, 20h
        mov ax, 1A0Ah
        call far [cs:SB_QPI]
        mov byte [cs:SB_INIRQ], 0
.later: pop ds
        pop dx
        pop bx
        pop ax
.x:     ret

; RTC periodic interrupt on / off (Windows in 386 enhanced mode owns the machine)
sb_rtc_on:
        cmp byte [cs:sb_rtcon], 0
        jne .x
        test byte [cs:SB_FLAGS], SBF_DOSTRAP
        jz .x
        cmp dword [cs:old70], 0
        je .x
        push ax
        pushf
        cli
        mov al, 0Ah                     ; rate: the standard 1024 Hz if none is set
        out 70h, al
        in al, 71h
        test al, 0Fh
        jnz .rate
        or al, 06h
        mov ah, al
        mov al, 0Ah
        out 70h, al
        mov al, ah
        out 71h, al
.rate:  mov al, 0Bh
        out 70h, al
        in al, 71h
        mov [cs:sb_oldb], al
        or al, 40h
        mov ah, al
        mov al, 0Bh
        out 70h, al
        mov al, ah
        out 71h, al
        mov al, 0Ch
        out 70h, al
        in al, 71h
        in al, 0A1h
        and al, 0FEh
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
        mov byte [cs:sb_rtcon], 1
        or byte [cs:SB_FLAGS], SBF_RTC
        popf
        pop ax
.x:     ret

sb_rtc_off:
        cmp byte [cs:sb_rtcon], 0
        je .x
        push ax
        pushf
        cli
        mov byte [cs:sb_rtcon], 0
        and byte [cs:SB_FLAGS], ~SBF_RTC
        mov al, 0Bh
        out 70h, al
        in al, 71h
        and al, 0BFh
        mov ah, [cs:sb_oldb]
        and ah, 40h
        or ah, al
        mov al, 0Bh
        out 70h, al
        mov al, ah
        out 71h, al
        popf
        pop ax
.x:     ret

; ---------------------------------------------------------------------------
; The built-in V86 monitor (vmon.c, dpmi.c in the core): without EMM386 or JEMM the core runs
; DOS in virtual 8086 mode and traps the Sound Blaster ports itself; it is a VCPI server and a
; DPMI host too. Its real-mode pieces: hypercalls (ARPL, invalid in V86 mode), the DPMI entry,
; the way back to real mode for Windows 386 enhanced mode and the VCPI gateway.
vm_on           db 0
vm_ret:         HCALL HC_RET            ; real-mode calls of the monitor end here
vm_rawpm:       HCALL HC_RAWPM          ; DPMI raw switch to protected mode
vm_rmnull:      retf                    ; DPMI state save / restore
vm_iret:        iret                    ; INT 67h without an EMS driver
vm_term:        int 21h                 ; a program ends (AX = 4Cxxh)
                jmp short vm_term
vm_dpmi:        HCALL HC_DPMI           ; protected mode, or back here with CF set
                retf
vm_cbs:                                 ; DPMI real-mode callbacks
%assign i 0
%rep VM_NCB
                HCALL HC_CB + i
%assign i i+1
%endrep

; character device EMMQXXX0 (VCPI without EMS, like EMM386 NOEMS): DOS extenders and Windows
; standard mode look for it before they ask INT 67h for VCPI. Put into the DOS device chain
; while the monitor runs.
vm_emmhdr       dd -1
                dw 0C000h               ; character device, IOCTL
                dw vm_emmstrat
                dw vm_emmint
                db 'EMMQXXX0'
vm_emmreq       dd 0
vm_nulptr       dd 0                    ; the NUL device header
vm_emmin        db 0                    ; EMMQXXX0 is in the device chain
vm_i67          db 0                    ; INT 67h is our IRET
vm_emmstrat:
        mov [cs:vm_emmreq], bx
        mov [cs:vm_emmreq+2], es
        retf
vm_emmint:
        push bx
        push es
        les bx, [cs:vm_emmreq]
        mov word [es:bx+3], 0100h       ; done
        cmp byte [es:bx+2], 3           ; IOCTL read: nothing
        jne .x
        mov word [es:bx+12h], 0
.x:     pop es
        pop bx
        retf

; EMMQXXX0 and the INT 67h IRET only while the monitor runs: Windows 386 enhanced mode would
; take them for an EMS driver it has to import from
vm_emmoff:
        cmp byte [cs:vm_emmin], 0
        je .x
        push es
        push bx
        push ax
        push dx
        les bx, [cs:vm_nulptr]
.l:     mov ax, [es:bx]
        mov dx, [es:bx+2]
        cmp ax, 0FFFFh
        je .d
        cmp ax, vm_emmhdr
        jne .n
        push cx
        mov cx, cs
        cmp dx, cx
        pop cx
        jne .n
        mov ax, [cs:vm_emmhdr]
        mov [es:bx], ax
        mov ax, [cs:vm_emmhdr+2]
        mov [es:bx+2], ax
        jmp .d
.n:     mov bx, ax
        mov es, dx
        jmp .l
.d:     mov byte [cs:vm_emmin], 0
        cmp byte [cs:vm_i67], 0
        je .p
        xor ax, ax
        mov es, ax
        mov dword [es:67h*4], 0
.p:     pop dx
        pop ax
        pop bx
        pop es
.x:     ret

vm_emmon:
        cmp byte [cs:vm_emmin], 0
        jne .x
        push es
        push bx
        push eax
        les bx, [cs:vm_nulptr]
        mov eax, [es:bx]
        mov [cs:vm_emmhdr], eax
        mov word [es:bx], vm_emmhdr
        mov [es:bx+2], cs
        mov byte [cs:vm_emmin], 1
        cmp byte [cs:vm_i67], 0
        je .p
        xor ax, ax
        mov es, ax
        mov word [es:67h*4], vm_iret
        mov [es:67h*4+2], cs
.p:     pop eax
        pop bx
        pop es
.x:     ret

; Windows 386 enhanced mode (INT 2Fh 1605h): AX = 0 real mode, AX = 1 V86 mode again.
; Interrupts are disabled.
vm_switch:
        test ax, ax
        jnz .on
        HCALL HC_TOREAL                 ; back here in real mode
        clc
        retf
.on:    push eax
        push ebx
        mov eax, FN_VMSTART
        mov ebx, 1
        call callcore
        test eax, eax
        pop ebx
        pop eax
        jnz .err
        clc
        retf
.err:   stc
        retf

; 16-bit protected mode (CS = SEL_CODE16, base = driver), from the monitor: real mode, then the
; registers the monitor left on the V86 stack
vm_rm16:
        mov ax, SEL_DATA16
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov eax, cr0
        and eax, 7FFFFFFEh
        mov cr0, eax
        jmp far [cs:vm_rmtgt]
vm_rmreal:
        lidt [cs:vm_ivt]
        mov ss, [cs:vm_rss]
        mov sp, [cs:vm_rsp]
        popad
        pop ds
        pop es
        pop fs
        pop gs
        iret
vm_rmtgt        dw vm_rmreal, 0
vm_ivt          dw 03FFh
                dd 0
vm_rss          dw 0
vm_rsp          dw 0

        bits 32
; VCPI gateway: identity-mapped in the address space of every VCPI client (conventional
; memory), so the switch between the client's and the monitor's page tables happens here
                align 4
vm_gw:          times VMGW_SIZE db 0    ; filled by the monitor

; the VCPI protected-mode entry: far call from a client (CPL 0, its page tables), AX = function
vm_gwpm:
        cmp ax, 0DE0Ch
        je .tov86
        cmp ax, 0DE03h
        jne .n3
        xor edx, edx                    ; no free pages (memory comes from XMS)
        mov ah, 0
        retf
.n3:    cmp ax, 0DE04h
        jne .n4
        mov ah, 88h
        retf
.n4:    cmp ax, 0DE05h
        jne .n5
        mov ah, 0
        retf
.n5:    mov ah, 8Fh
        retf
.tov86: ; back to V86 mode: the frame on the client's stack, after the return address
        push ebx
        mov bx, cs
        add bx, 8                       ; the server's data descriptor follows its code
        mov ds, bx
        call .h
.h:     pop ebx
        add ebx, vm_gw - .h
        mov [ebx+VMGW_REGS+24], ecx
        mov [ebx+VMGW_REGS+20], edx
        mov [ebx+VMGW_REGS+8], ebp
        mov [ebx+VMGW_REGS+4], esi
        mov [ebx+VMGW_REGS], edi
        mov [ebx+VMGW_REGS+28], eax
        pop ecx
        mov [ebx+VMGW_REGS+16], ecx
        add esp, 8
        lea edi, [ebx+VMGW_FRAME]
        mov ecx, 9
.cp:    pop eax
        mov [edi], eax
        add edi, 4
        loop .cp
        mov eax, [ebx+VMGW_CR3]
        mov cr3, eax
        mov eax, [ebx+VMGW_CR4]
        test eax, eax
        jz .c4
        mov cr4, eax
.c4:    lgdt [ebx+VMGW_GDTR]
        lidt [ebx+VMGW_IDTR]
        mov eax, [ebx+VMGW_TSSD]
        and byte [eax+5], 0FDh
        ltr [ebx+VMGW_TR]
        lldt [ebx+VMGW_LDT]
        mov ax, [ebx+VMGW_DS]
        mov ss, ax
        mov es, ax
        mov ds, ax
        mov eax, [ebx+VMGW_ESP0P]
        mov esp, [eax]
        push dword [ebx+VMGW_FRAME+32]  ; GS
        push dword [ebx+VMGW_FRAME+28]  ; FS
        push dword [ebx+VMGW_FRAME+24]  ; DS
        push dword [ebx+VMGW_FRAME+20]  ; ES
        push dword [ebx+VMGW_FRAME+16]  ; SS
        push dword [ebx+VMGW_FRAME+12]  ; ESP
        mov eax, [ebx+VMGW_FRAME+8]
        and eax, 0FFFEBFFFh             ; NT, RF
        or eax, 00023002h               ; VM, IOPL 3
        push eax
        push dword [ebx+VMGW_FRAME+4]   ; CS
        push dword [ebx+VMGW_FRAME]     ; EIP
        push dword 0                    ; error code
        push dword 0FFh                 ; vector
        push dword [ebx+VMGW_REGS+28]
        push dword [ebx+VMGW_REGS+24]
        push dword [ebx+VMGW_REGS+20]
        push dword [ebx+VMGW_REGS+16]
        push dword 0
        push dword [ebx+VMGW_REGS+8]
        push dword [ebx+VMGW_REGS+4]
        push dword [ebx+VMGW_REGS]
        push dword 2                    ; no NT (IRET would switch tasks)
        popfd
        jmp far [ebx+VMGW_EXIT]

; the monitor -> a VCPI client's protected mode (V86 AX=DE0Ch), interrupts disabled
vm_gwvc:
        call .h
.h:     pop ebx
        add ebx, vm_gw - .h
        lea esp, [ebx+VMGW_SIZE]
        push dword [ebx+VMGW_CCS]
        push dword [ebx+VMGW_CEIP]
        mov eax, [ebx+VMGW_CCR3]
        mov cr3, eax
        lgdt [ebx+VMGW_CGDTR]
        lidt [ebx+VMGW_CIDTR]
        movzx eax, word [ebx+VMGW_CTR]
        and al, 0F8h
        add eax, [ebx+VMGW_CGDTR+2]
        and byte [eax+5], 0FDh          ; TSS not busy
        ltr [ebx+VMGW_CTR]
        lldt [ebx+VMGW_CLDT]
        mov eax, [ebx+VMGW_REGS+28]
        mov ecx, [ebx+VMGW_REGS+24]
        mov edx, [ebx+VMGW_REGS+20]
        mov ebp, [ebx+VMGW_REGS+8]
        mov esi, [ebx+VMGW_REGS+4]
        mov edi, [ebx+VMGW_REGS]
        mov ebx, [ebx+VMGW_REGS+16]
        retf
        bits 16
%endif

chainvec        dd 0
indos_ptr       dd 0            ; DOS InDOS flag (critical error flag at -1)
in13            db 0            ; nesting of BIOS disk calls
irq_deferred    dw 0            ; IRQs masked until the next safe poll
                align 4
vstack:         times 64 db 0
vstack_top:
                align 4
rq_stack:       times 1024 db 0
rq_stack_top:
irq_stack:      times 1024 db 0
irq_stack_top:
rq_ss           dw 0
rq_sp           dw 0
irq_ss          dw 0
irq_sp          dw 0
irq_onstk       db 0
rq_onstk        db 0

irqtab:
%assign i 0
%rep 16
        dw irqs %+ i
%assign i i+1
%endrep

resident_end:

; ===========================================================================
; initialization (discarded after INIT)
; ===========================================================================
init_ok         db 0
cmdline         dd 0
coresize        dd 0
imgtotal        dd 0
filesize        dd 0
fhandle         dw 0FFFFh
emb_kb          dw 0
emb_off         dd 0            ; offset of the 4K-aligned window start inside the EMB
vcpi_ok         db 0
pathbuf         times 96 db 0
xmove:
xm_len          dd 0
xm_sh           dw 0
xm_so           dd 0
xm_dh           dw 0
xm_do           dd 0

msg_banner      db BANNER, 13, 10, '$'
msg_no386       db DRVNAME, ': 386 or later required', 13, 10, '$'
msg_notsc       db DRVNAME, ': CPU without CPUID / time stamp counter (Pentium class or later required)', 13, 10, '$'
msg_noxms       db DRVNAME, ': XMS driver (HIMEM.SYS) not loaded', 13, 10, '$'
msg_noemb       db DRVNAME, ': cannot allocate extended memory', 13, 10, '$'
msg_nofile      db DRVNAME, ': cannot read $'
msg_badfile     db DRVNAME, ': ', COREFILE, ' is invalid', 13, 10, '$'
msg_v86         db DRVNAME, ': V86 mode without VCPI server, core disabled', 13, 10, '$'
msg_units1      db DRVNAME, ': drives $'
msg_units2      db ':$'
msg_nounits     db DRVNAME, ': no drive letters assigned', 13, 10, '$'
msg_crlf        db 13, 10, '$'
msg_nodisk      db DRVNAME, ': no disk found, not installed (/S shows the full probe log)', 13, 10, '$'
msg_chrdev      db DRVNAME, ': no drive letters; resident for Windows', 13, 10, '$'
chrname         db 'WINDISK$'
msg_vcpi        db DRVNAME, ': entering protected mode through VCPI', 13, 10, '$'
msg_usb         db DRVNAME, ': ', DOSACTIVE, 13, 10, '$'
%ifdef PKG_AUDIO
msg_nosnd       db DRVNAME, ': no sound hardware found, not installed (/S shows the probe log)', 13, 10, '$'
%endif
corename        db COREFILE, 0
vxdname         db VXDFILE, 0

%ifdef PKG_AUDIO
; Sound Blaster emulation for DOS programs: QPI port traps, RTC, summary
qemmname        db 'QEMM386$', 0
qpibuf          dd 0
sb_ports        times 48 dw 0
msg_sb1         db DRVNAME, ': Sound Blaster 16 emulation, SET BLASTER=A$'
msg_sbi         db ' I$'
msg_sbd         db ' D$'
msg_sbh         db ' H$'
msg_sbp         db ' P$'
msg_sbt         db ' T6', 13, 10, '$'
msg_sbok1       db DRVNAME, ': DOS programs: $'
msg_sbok2       db ' ports trapped (QPI), RTC interrupt 1024 Hz', 13, 10, '$'
msg_sbemm       db DRVNAME, ': DOS programs: a memory manager without port trapping runs (EMM386?);', 13, 10
                db DRVNAME, ':   without it this driver traps the ports itself, or use QEMM, or JEMM386 /', 13, 10
                db DRVNAME, ':   JEMMEX with DEVICE=JLOAD.EXE QPIEMU.DLL (Windows MS-DOS boxes always', 13, 10
                db DRVNAME, ':   have the emulation)', 13, 10, '$'
msg_sbnov86     db DRVNAME, ': DOS programs: no emulation (/NOV86)', 13, 10, '$'
msg_vmfail      db DRVNAME, ': DOS programs: the V86 monitor could not start', 13, 10, '$'
msg_vmon        db DRVNAME, ': DOS programs: ports trapped by the built-in V86 monitor, VCPI and DPMI', 13, 10
                db DRVNAME, ':   available, RTC interrupt 1024 Hz', 13, 10, '$'
msg_sbbusy      db DRVNAME, ': DOS programs: another program traps ports through the QPI (SBEMU?),', 13, 10
                db DRVNAME, ':   no emulation in DOS', 13, 10, '$'
msg_sboff       db DRVNAME, ': Sound Blaster emulation off (/NOSB)', 13, 10, '$'
pagetab8        db 87h, 83h, 81h, 82h
pagetab16       db 8Bh, 89h, 8Ah

; AX -> hex, DX not changed
print_hex:
        push ax
        push bx
        push cx
        push dx
        mov bx, ax
        mov cx, 4
        xor dh, dh                      ; leading zeros off
.l:     rol bx, 4
        mov dl, bl
        and dl, 0Fh
        cmp cx, 1
        je .p
        test dh, dh
        jnz .p
        test dl, dl
        jz .n
.p:     mov dh, 1
        add dl, '0'
        cmp dl, '9'
        jbe .o
        add dl, 7
.o:     mov ah, 02h
        int 21h
.n:     loop .l
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; AX -> decimal
print_dec:
        push ax
        push bx
        push cx
        push dx
        mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        test ax, ax
        jnz .d
.o:     pop dx
        add dl, '0'
        mov ah, 02h
        int 21h
        loop .o
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; the QPI entry of QEMM (QEMM386$ IOCTL) or of JEMM's QPIEMU (INT 2Fh 1684h, 4354h)
qpi_find:
        mov dx, qemmname
        mov ax, 3D00h
        int 21h
        jc .jemm
        mov bx, ax
        mov ax, 4402h
        mov cx, 4
        mov dx, qpibuf
        int 21h
        pushf
        push ax
        mov ah, 3Eh
        int 21h
        pop ax
        popf
        jc .jemm
        cmp ax, 4
        jne .jemm
        mov eax, [qpibuf]
        test eax, eax
        jz .jemm
        mov [SB_QPI], eax
        jmp .ver
.jemm:  push es
        xor di, di
        mov es, di
        mov ax, 1684h
        mov bx, 4354h
        int 2Fh
        mov ax, es
        pop es
        test ax, ax
        jz .none
        mov [SB_QPI], di
        mov [SB_QPI+2], ax
.ver:   mov ah, 03h
        xor bx, bx
        call far [SB_QPI]
        jc .none
        cmp bx, 0703h
        jb .none
        clc
        ret
.none:  mov dword [SB_QPI], 0
        stc
        ret

; SI -> next free entry of sb_ports: AX = port
sb_addport:
        mov [si], ax
        add si, 2
        ret

sb_init:
        test byte [SB_FLAGS], SBF_OFF
        jz .on
        mov dx, msg_sboff
        call print
        ret
.on:    test byte [SB_FLAGS], SBF_ON
        jz .x
        ; summary: the BLASTER variable for the programs
        mov dx, msg_sb1
        call print
        mov ax, [SB_BASE]
        call print_hex
        mov dx, msg_sbi
        call print
        movzx ax, byte [SB_IRQ]
        call print_dec
        mov dx, msg_sbd
        call print
        movzx ax, byte [SB_DMA8]
        call print_dec
        mov dx, msg_sbh
        call print
        movzx ax, byte [SB_DMA16]
        test ax, ax
        jnz .h
        mov al, [SB_DMA8]
.h:     call print_dec
        cmp word [SB_MPU], 0
        je .nop
        mov dx, msg_sbp
        call print
        mov ax, [SB_MPU]
        call print_hex
.nop:   mov dx, msg_sbt
        call print
        ; TSC ticks in 80 us (FM timers)
        mov eax, [cb+CB_TSCKHZ]
        imul eax, eax, 80
        xor edx, edx
        mov ecx, 1000
        div ecx
        mov [SB_TSC80], eax
        call qpi_find
        jnc .qpi
        ; no QPI: the built-in V86 monitor, when DOS runs in real mode
        mov dx, msg_sbemm
        cmp byte [vcpi_ok], 0
        jne .noqpi
        mov dx, msg_sbnov86
        test word [VM_FLAGS], VMF_NOV86
        jnz .noqpi
        call vm_start
        jnc .x
        mov dx, msg_vmfail
.noqpi: call print
        ret
.qpi:   push es
        mov ax, 1A06h                   ; someone else's trap callback?
        call far [SB_QPI]
        mov ax, es
        pop es
        or ax, di
        jz .free
        mov dx, msg_sbbusy
        call print
        ret
.free:  push es
        push cs
        pop es
        mov di, sb_trap
        mov ax, 1A07h
        call far [SB_QPI]
        pop es
        ; the ports
        mov si, sb_ports
        mov ax, [SB_BASE]
        mov cx, 16
.cp:    call sb_addport
        inc ax
        loop .cp
        test byte [SB_FLAGS], SBF_NOOPL
        jnz .nopl
        mov ax, 388h
        mov cx, 4
.op:    call sb_addport
        inc ax
        loop .op
.nopl:  mov ax, [SB_MPU]
        test ax, ax
        jz .nompu
        call sb_addport
        inc ax
        call sb_addport
.nompu: movzx bx, byte [SB_DMA8]        ; 8-bit DMA channel: address, count, page
        mov ax, bx
        shl ax, 1
        call sb_addport
        inc ax
        call sb_addport
        movzx ax, byte [pagetab8+bx]
        call sb_addport
        mov ax, 08h
        call sb_addport
        mov al, 0Ah
.c1:    call sb_addport
        inc ax
        cmp al, 10h
        jb .c1
        movzx bx, byte [SB_DMA16]       ; 16-bit DMA channel
        cmp bl, 5
        jb .nodma16
        mov ax, bx
        sub ax, 4
        shl ax, 2
        add ax, 0C0h
        call sb_addport
        add ax, 2
        call sb_addport
        movzx ax, byte [pagetab16+bx-5]
        call sb_addport
        mov ax, 0D0h
        call sb_addport
        mov al, 0D4h
.c2:    call sb_addport
        add al, 2
        cmp al, 0E0h
        jb .c2
.nodma16:
        mov word [si], 0
        mov si, sb_ports
.trap:  mov dx, [si]
        test dx, dx
        jz .trapped
        mov ax, 1A09h
        call far [SB_QPI]
        jc .tn
        inc word [SB_NTRAPS]
.tn:    add si, 2
        jmp .trap
.trapped:
        or byte [SB_FLAGS], SBF_DOSTRAP
        ; the emulated IRQ
        mov al, [SB_IRQ]
        cmp al, 8
        jae .hi
        mov cl, al
        mov ah, 1
        shl ah, cl
        mov [sb_irqbit], ah
        add al, 8
        mov [sb_vec], al
        jmp .rtc
.hi:    add al, 70h - 8
        mov [sb_vec], al
.rtc:   mov al, 70h
        mov di, old70
        mov dx, int70
        call hook
        call sb_rtc_on
        mov dx, msg_sbok1
        call print
        mov ax, [SB_NTRAPS]
        call print_dec
        mov dx, msg_sbok2
        call print
.x:     ret

; the built-in V86 monitor: fills in struct winvm, then FN_VMSTART returns in V86 mode
vm_start:
        mov [VM_DRVSEG], cs
        mov [vm_rmtgt+2], cs
        mov eax, [xmsentry]
        mov [VM_XMS], eax
        mov word [VM_ORET], vm_ret
        mov word [VM_ORAWPM], vm_rawpm
        mov word [VM_OCBS], vm_cbs
        mov word [VM_ORMNULL], vm_rmnull
        mov word [VM_ORM16], vm_rm16
        mov word [VM_OTERM], vm_term
        mov word [VM_OCALLRET], call_ret
        mov word [VM_OVRET], vcpi_ret
        mov word [VM_OSAVESS], save_ss
        mov word [VM_OSAVEESP], save_esp
        mov word [VM_OGW], vm_gw
        mov word [VM_OGWPM], vm_gwpm
        mov word [VM_OGWVC], vm_gwvc
        mov word [VM_ORMSS], vm_rss
        mov word [VM_ORMTGT], vm_rmtgt
        mov word [VM_OIRET], vm_iret
        mov word [VM_OVMON], vm_on
        mov byte [VM_NCBF], VM_NCB
        mov eax, 1                      ; CPU family for DPMI
        cpuid
        shr eax, 8
        and al, 0Fh
        cmp al, 6
        jbe .fam
        mov al, 6
.fam:   mov [VM_CPU], al
        push es
        xor ax, ax
        mov es, ax
        cmp dword [es:67h*4], 0         ; VCPI programs look at the INT 67h vector
        jne .h67
        mov word [es:67h*4], vm_iret
        mov [es:67h*4+2], cs
        mov byte [vm_i67], 1
.h67:   mov eax, [es:70h*4]
        mov [VM_BIOS70], eax
        pop es
        call vm_sisters
        mov eax, FN_VMSTART
        xor ebx, ebx
        call callcore
        test eax, eax
        jnz .fail
        or byte [SB_FLAGS], SBF_V86
        push es                         ; EMMQXXX0 after NUL in the device chain
        mov ah, 52h
        int 21h
        add bx, 22h
        mov [vm_nulptr], bx
        mov [vm_nulptr+2], es
        pop es
        call vm_emmon
        mov dx, msg_vmon
        call print
        clc
        ret
.fail:  stc
        ret

; WINDISK.SYS and WINUSB.SYS loaded before this driver use raw switches: they become VCPI
; clients of the monitor (INT 2Fh AL=04h)
vm_sisters:
        push es
        mov si, vm_mplex
.l:     mov ah, [si]
        test ah, ah
        jz .x
        inc si
        push si
        push ax
        xor al, al
        xor bx, bx
        int 2Fh
        cmp al, 0FFh
        pop ax
        jne .n
        cmp bx, 3357h
        jne .n
        mov al, 04h
        push cs
        pop es
        mov di, vm_vdesc
        movzx ebx, word [rm_seg]
        shl ebx, 4
        add ebx, vm_gwpm
        int 2Fh
.n:     pop si
        jmp .l
.x:     pop es
        ret
vm_mplex        db 0D3h, 0D4h, 0
vm_vdesc        dw 0FFFFh, 0, 9A00h, 00CFh      ; code: base 0, 4 GB, 32-bit, ring 0
                dw 0FFFFh, 0, 9200h, 00CFh      ; data
                dw 0FFFFh, 0, 9200h, 00CFh
%endif

print:  push ax
        push ds
        push cs
        pop ds
        mov ah, 09h
        int 21h
        pop ds
        pop ax
        ret


do_init:
        push cs
        pop ds
        mov [rm_seg], cs
        mov [cb+CB_DRVSEG], cs
        mov dx, msg_banner
        call print
        ; CB defaults
        mov dword [cb+CB_MAGIC], 42433357h
        mov word [cb+4], 0110h
        mov word [cb+6], 2048
        mov al, [mplex]
        mov [cb+CB_MPLEX], al
        mov byte [cb+CB_SLOTREQ], DEFSLOTS | 80h  ; default: grows with the partitions found
        les bx, [reqptr]
        mov al, [es:bx+16h]
        mov [cb+CB_FIRSTDRIVE], al
        mov eax, [es:bx+12h]
        mov [cmdline], eax
        mov ah, 30h
        int 21h
        xchg al, ah
        mov [cb+CB_DOSVER], ax
%ifndef PKG_AUDIO
        push es                         ; DOS List of Lists: the core finds the CDS there
        mov ah, 52h
        int 21h
        mov [cb+WIN_DOSOFF], bx
        mov [cb+WIN_DOSOFF+2], es
        pop es
%endif
        movzx eax, word [rm_seg]
        shl eax, 4
        mov [drv_lin], eax
        mov [pm32_base+1], eax          ; mov ebp, imm32 in pm32
        add eax, cb
        mov [cb+CB_CBLIN], eax

        call parse_cmdline
%ifndef PKG_DISK
        or word [cb+CB_OPTS], OPT_USB   ; core in DOS and standard mode unless /WIN
        test word [cb+CB_OPTS], OPT_WINONLY
        jz .optdone
        and word [cb+CB_OPTS], ~OPT_USB
.optdone:
%else
        and word [cb+CB_OPTS], ~OPT_USB
%endif

        ; 386 check: FLAGS bits 12-15 behave differently on 8086/286
        pushf
        pop ax
        or ax, 7000h
        push ax
        popf
        pushf
        pop ax
        test ax, 7000h
        jnz .is386
        mov dx, msg_no386
        call print
        jmp init_fail
.is386:
        ; CPUID / TSC
        pushfd
        pop eax
        mov ecx, eax
        xor eax, 200000h
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        test eax, 200000h
        jz .notsc
        mov eax, 1
        cpuid
        test dl, 10h
        jnz .tscok
        call tsc_probe                  ; CPUs that have a TSC without the CPUID flag (Vortex86)
        jnc .tscok
.notsc: mov dx, msg_notsc
        call print
        jmp init_fail
.tscok:
        ; XMS
        mov ax, 4300h
        int 2Fh
        cmp al, 80h
        je .xms
        mov dx, msg_noxms
        call print
        jmp init_fail
.xms:   mov ax, 4310h
        int 2Fh
        mov [xmsentry], bx
        mov [xmsentry+2], es
        ; Windows 3.0 standard mode fix, once (WIN30FIX.COM or another WIN driver may have it)
        push es
        mov ax, 0D6FFh
        int 2Fh
        pop es
        cmp ax, 0FFFFh
        je .win30have
        mov byte [win30on], 1
        mov ax, [xmsentry]
        mov [win30xms], ax
        mov ax, [xmsentry+2]
        mov [win30xms+2], ax
.win30have:
        push cs
        pop es

        ; V86 mode needs VCPI
        smsw ax
        test al, 1
        jz .realmode
        call vcpi_detect
        jnc .vcpiok
        mov dx, msg_v86
        call print
        jmp init_fail
.vcpiok:
        mov byte [vcpi_ok], 1
        test word [cb+CB_OPTS], OPT_VERBOSE
        jz .realmode
        mov dx, msg_vcpi
        call print
.realmode:
        call load_core
        jc init_fail
        call setup_pm
        jc init_fail
        call read_bios_mbrs
        call read_bds
        mov byte [core_ok], 1
        or word [cb+CB_FLAGS], CBF_DOSCORE

%ifndef PKG_DISK
        call br_link
%endif
%ifdef PKG_AUDIO
        call br_wait
%endif
        ; core_init(params)
        mov eax, FN_INIT
        mov ebx, [drv_lin]
        add ebx, params
        call callcore
        mov dword [p_mbr2_lin], 0       ; HMA: the core is done with it
%ifdef PKG_AUDIO
        ; firmware files the core asked for (Intel audio DSP): loaded into XMS, then the
        ; core goes on with the sound devices
.fwloop:
        test word [cb+CB_FLAGS], CBF_FWREQ
        jz .fwdone
        and word [cb+CB_FLAGS], ~CBF_FWREQ
        call load_fw
        mov eax, FN_FILEDONE
        xor ebx, ebx
        call callcore
        jmp .fwloop
.fwdone:
%endif
%ifndef PKG_AUDIO
        ; the BIOS disks on controllers the core took over: their INT 13h is served here
        mov word [i13req], WINF_BIOSMAP
        mov word [i13req+2], 0FFFFh
        mov dword [i13req+16], 72
        mov ax, cs
        shl eax, 16
        mov ax, i13disk
        mov [i13req+24], eax
        movzx ebx, word [rm_seg]
        shl ebx, 4
        add ebx, i13req
        mov eax, FN_API
        call callcore
        cmp word [i13req+2], 0
        jne .noi13
        xor si, si
.ci:    cmp byte [i13disk+si], 0
        jne .hki13
        inc si
        cmp si, 8
        jb .ci
        jmp .noi13
.hki13: mov al, 13h
        mov di, old13s
        mov dx, int13s
        call hook
.noi13:
%endif

%ifndef PKG_USB
%ifdef PKG_DISK
        ; No drive letters to serve. With disks found the driver stays for Windows (VxD:
        ; 32-bit disk access, AHCI BIOS fix) as a character device: DOS reads the device
        ; attributes after INIT, so the header is turned into a character device header.
        cmp byte [cb+CB_NUNITS], 0
        jne .serve
        test word [cb+CB_FLAGS], CBF_HAVEDISK
        jz .unload
        mov word [hdr+4], 8000h
        mov si, chrname
        mov di, hdr_units
        push cs
        pop es
        mov cx, 8
        rep movsb
        or word [cb+CB_FLAGS], CBF_CHARDEV
        mov dx, msg_chrdev
        call print
        jmp .serve
%else
        ; sound devices found: resident as the character device WINAUD$; also without any
        ; when WINUSB.SYS runs USB in DOS (its USB Audio device plays through the bridge)
        test word [cb+CB_FLAGS], CBF_HAVEDISK
        jnz .snd
        call br_usbdos
        jc .unload
.snd:
        or word [cb+CB_FLAGS], CBF_CHARDEV
        jmp .serve
%endif
.unload:
        mov eax, FN_SHUTDOWN
        xor ebx, ebx
        call callcore
        mov byte [core_ok], 0
%ifdef PKG_AUDIO
        call free_fw
%endif
        mov dx, [emb_handle]
        mov ah, 0Dh
        call far [xmsentry]
        mov dx, [emb_handle]
        mov ah, 0Ah
        call far [xmsentry]
%ifdef PKG_DISK
        mov dx, msg_nodisk
%else
        mov dx, msg_nosnd
%endif
        call print
        jmp init_fail
.serve:
%endif

        ; DOS-mode USB hooks
        test word [cb+CB_OPTS], OPT_USB
        jz .nousb
        mov byte [dos_usb], 1
        or word [cb+CB_FLAGS], CBF_USB
        push es
        push bx
        mov ah, 34h                     ; InDOS flag address
        int 21h
        mov [indos_ptr], bx
        mov [indos_ptr+2], es
        pop bx
        pop es
        call hook_irqs                  ; before INT 08h can poll (INTx goes on at the first poll)
        mov dx, msg_usb
        call print
        mov al, 08h
        mov di, old08
        mov dx, int08
        call hook
%ifdef PKG_USB
        mov al, 09h
        mov di, old09
        mov dx, int09
        call hook
        mov al, 74h
        mov di, old74
        mov dx, int74
        call hook
        mov al, 15h
        mov di, old15
        mov dx, int15
        call hook
        ; INT 33h (DOS mouse API): provide the USB mouse to INT 33h clients (DOS programs and
        ; the Microsoft Mouse driver of Windows 1.0x / 2.x). A real DOS mouse driver loaded
        ; afterwards hooks INT 33h ahead of this and keeps priority.
        mov al, 33h
        mov di, old33
        mov dx, int33
        call hook
        mov byte [m33_on], 1
%endif
        mov al, 13h
        mov di, old13
        mov dx, int13
        call hook
        mov al, 28h
        mov di, old28
        mov dx, int28
        call hook
%ifdef PKG_USB
        mov al, 16h
        mov di, old16
        mov dx, int16
        call hook
%endif
.nousb:
%ifdef PKG_DISK
        mov al, 13h
        mov di, old13d
        mov dx, int13d
        call hook
%endif
        mov al, 2Fh
        mov di, old2f
        mov dx, int2f
        call hook

%ifdef PKG_AUDIO
        call sb_init
%endif
        ; SIS for Windows enhanced mode
        mov word [sis_path], cb+CB_VXDPATH
        mov [sis_path+2], cs
        mov word [sis_ref], cb
        mov [sis_ref+2], cs

        test word [cb+CB_FLAGS], CBF_CHARDEV
        jz .block
        les bx, [reqptr]
        mov byte [es:bx+0Dh], 0
        mov word [es:bx+0Eh], resident_end
        mov [es:bx+10h], cs
        mov word [es:bx+3], 0100h
        jmp init_ret
.block:
        ; at least one unit, otherwise DOS discards the driver
        cmp byte [cb+CB_NUNITS], 0
        jne .haveunits
        mov byte [cb+CB_NUNITS], 1
        mov byte [cb+CB_UNITS], 2       ; UT_SLOT
        mov dx, msg_nounits
        call print
.haveunits:
        ; BPB pointer array
        movzx cx, byte [cb+CB_NUNITS]
        xor si, si
        mov ax, bpbx
.bpb:   mov [cb+CB_BPBPTR+si], ax
        add ax, 64
        add si, 2
        loop .bpb
        call show_units
        les bx, [reqptr]
        mov al, [cb+CB_NUNITS]
        mov [es:bx+0Dh], al
        mov [hdr_units], al
        mov word [es:bx+0Eh], resident_end
        mov [es:bx+10h], cs
        mov word [es:bx+12h], cb+CB_BPBPTR
        mov [es:bx+14h], cs
        mov word [es:bx+3], 0100h
        jmp init_ret

init_fail:
%ifndef PKG_DISK
        call br_unlink
%endif
        les bx, [reqptr]
        mov byte [es:bx+0Dh], 0
        mov word [es:bx+0Eh], 0
        mov [es:bx+10h], cs
        mov word [es:bx+3], 810Ch
init_ret:
        popad
        pop es
        pop ds
        ret

%ifndef PKG_DISK
; Sound bridge (struct winbridge at WIN_BROFF of both control blocks): WINAUDIO.SYS and
; WINUSB.SYS link their control blocks, whichever starts second, so that the Sound Blaster
; emulation of WINAUDIO can play on a USB Audio device of WINUSB
WIN_BROFF       equ 2272
%ifdef PKG_AUDIO
BR_SISTER       equ 0D4h                ; WINUSB.SYS
%else
BR_SISTER       equ 0D5h                ; WINAUDIO.SYS
%endif
br_peer         dd 0
br_link:
        push es
        pushad
        mov ah, BR_SISTER
        xor al, al
        xor bx, bx
        xor di, di
        mov es, di
        int 2Fh
        cmp al, 0FFh
        jne .x
        cmp bx, 3357h
        jne .x
        mov ax, es
        test ax, ax
        jz .x
        mov [br_peer], di
        mov [br_peer+2], es
        mov [cb+WIN_BROFF+4], di
        mov [cb+WIN_BROFF+6], es
        mov word [es:di+WIN_BROFF+4], cb
        mov [es:di+WIN_BROFF+6], cs
.x:     popad
        pop es
        ret

%ifdef PKG_AUDIO
; CF clear: WINUSB.SYS is linked and runs USB in DOS
br_usbdos:
        push es
        push di
        les di, [br_peer]
        mov ax, es
        test ax, ax
        jz .no
        test word [es:di+CB_OPTS], OPT_USB
        jz .no
        clc
        jmp .x
.no:    stc
.x:     pop di
        pop es
        ret

; WINUSB.SYS may still be enumerating its devices (hubs): up to 3 s for a USB Audio output
br_wait:
        pushad
        push es
        push fs
        call br_usbdos
        jc .x
        xor ax, ax
        mov fs, ax
        mov ebx, [fs:46Ch]
        les di, [br_peer]
        sti
.w:     cmp byte [es:di+WIN_BROFF+27], 0        ; struct winbridge outs
        jne .x
        mov eax, [fs:46Ch]
        sub eax, ebx
        cmp eax, 55
        jb .w
.x:     pop fs
        pop es
        popad
        ret
%endif

; the driver does not stay: the other one forgets it
br_unlink:
        push es
        push di
        push ax
        les di, [br_peer]
        mov ax, es
        test ax, ax
        jz .x
        mov dword [es:di+WIN_BROFF+4], 0
        mov dword [br_peer], 0
.x:     mov dword [cb+WIN_BROFF+4], 0
        pop ax
        pop di
        pop es
        ret
%endif

; core_init parameters (struct core_params)
params:
p_pool_lin      dd 0
p_pool_phys     dd 0
p_pool_size     dd 0
p_cb_lin        dd 0
p_mode          dd 1
p_flags         dd 0
p_mbr_lin       dd 0
p_mbr2_lin      dd 0            ; loaded high: the second copy, in the HMA
                dd 0

; AL = vector, DI = old vector slot, DX = new handler
hook:
        push es
        push bx
        push ax
        mov ah, 35h
        int 21h
        mov [di], bx
        mov [di+2], es
        pop ax
        push ax
        mov ah, 25h
        int 21h
        pop ax
        pop bx
        pop es
        ret

hook_irqs:
        mov cx, 0
.l:     mov ax, 1
        shl ax, cl
        test [cb+CB_IRQMASK], ax
        jz .n
        mov al, cl
        add al, 08h
        cmp cl, 8
        jb .lo
        add al, 70h - 10h
.lo:    mov di, cx
        shl di, 2
        add di, oldirq
        mov bx, cx
        shl bx, 1
        mov dx, [irqtab+bx]
        call hook
        ; unmask in the PIC
        cmp cl, 8
        jb .m1
        in al, 0A1h
        mov ah, 1
        push cx
        sub cl, 8
        shl ah, cl
        pop cx
        not ah
        and al, ah
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
        jmp .n
.m1:    in al, 21h
        mov ah, 1
        shl ah, cl
        not ah
        and al, ah
        out 21h, al
.n:     inc cx
        cmp cx, 16
        jb .l
        ret

; ---------------------------------------------------------------------------
parse_cmdline:
        push es
        les si, [cmdline]
        ; copy the driver path to pathbuf
        xor di, di
.p:     mov al, [es:si]
        cmp al, ' '
        jbe .pe
        cmp al, '/'
        je .pe
        mov [pathbuf+di], al
        inc si
        inc di
        cmp di, 80
        jb .p
.pe:    mov byte [pathbuf+di], 0
        ; options
.o:     mov al, [es:si]
        cmp al, 13
        je .done
        cmp al, 10
        je .done
        test al, al
        jz .done
        cmp al, '/'
        je .opt
        cmp al, '-'
        je .opt
        inc si
        jmp .o
.opt:   inc si
        ; token -> tok (upper case)
        xor di, di
.tk:    mov al, [es:si]
        cmp al, ' '
        jbe .te
        cmp al, '/'
        je .te
        cmp al, 'a'
        jb .tu
        cmp al, 'z'
        ja .tu
        sub al, 20h
.tu:    cmp di, 31
        jae .tn
        mov [tok+di], al
        inc di
.tn:    inc si
        jmp .tk
.te:    mov byte [tok+di], 0
%ifdef PKG_AUDIO
        cmp word [tok], 'SB'
        jne .nsb
        cmp byte [tok+2], ':'
        jne .nsb
        push si                         ; /SB:A220,I5,D1,H5,P330
        mov si, tok+3
        mov di, SB_SPEC
        mov cx, 23
.sbc:   lodsb
        mov [di], al
        inc di
        test al, al
        loopnz .sbc
        mov byte [SB_SPEC+23], 0
        pop si
        jmp .o
.nsb:   cmp dword [tok], 'NOSB'
        jne .nsb1
        cmp byte [tok+4], 0
        jne .nsb1
        or byte [SB_FLAGS], SBF_OFF
        jmp .o
.nsb1:  cmp dword [tok], 'NOOP'
        jne .nsb2
        cmp word [tok+4], 'L'
        jne .nsb2
        or byte [SB_FLAGS], SBF_NOOPL
        jmp .o
.nsb2:  cmp dword [tok], 'NOMP'
        jne .nsb3
        cmp word [tok+4], 'U'
        jne .nsb3
        or byte [SB_FLAGS], SBF_NOMPU
        jmp .o
.nsb3:  mov bx, vmoptab                 ; /NOV86 /NODPMI /NOVCPI /NOVME
.vo:    mov dx, [bx]
        test dx, dx
        jz .nvm
        push si
        lea si, [bx+2]
        xor di, di
.vc:    mov al, [si]
        cmp al, [tok+di]
        jne .vne
        test al, al
        jz .veq
        inc si
        inc di
        jmp .vc
.veq:   pop si
        or [VM_FLAGS], dx
        jmp .o
.vne:   pop si
        add bx, 2
.vs:    cmp byte [bx], 0
        je .vs2
        inc bx
        jmp .vs
.vs2:   inc bx
        jmp .vo
.nvm:
%endif
        cmp word [tok], 'R:'
        jne .tab
        mov al, [tok+2]
        sub al, '0'
        cmp al, 9
        ja .o
        mov ah, [tok+3]
        sub ah, '0'
        cmp ah, 9
        ja .one
        mov bl, 10
        mul bl
        add al, [tok+3]
        sub al, '0'
.one:   cmp al, 20
        jbe .st
        mov al, 20
.st:    mov [cb+CB_SLOTREQ], al
        jmp .o
.tab:   mov bx, optab
        mov byte [opt2pass], 0
.t:     mov edx, [bx]
        test edx, edx
        jnz .t1
        cmp byte [opt2pass], 0
        jne .o
        mov byte [opt2pass], 1          ; second option word (OPT2_*)
        mov bx, optab2
        jmp .t
.t1:    push si
        lea si, [bx+4]
        xor di, di
.cmp:   mov al, [si]
        cmp al, [tok+di]
        jne .ne
        test al, al
        jz .eq
        inc si
        inc di
        jmp .cmp
.eq:    pop si
        cmp byte [opt2pass], 0
        jne .eq2
        or [cb+CB_OPTS], edx
        jmp .o
.eq2:   or [cb+CB_OPTS2], dx
        jmp .o
.ne:    pop si
        add bx, 4
.sk:    cmp byte [bx], 0
        je .sk2
        inc bx
        jmp .sk
.sk2:   inc bx
        jmp .t
.done:
        ; directory part of the path
        mov di, 0
        mov bx, 0
.d:     mov al, [pathbuf+di]
        test al, al
        jz .de
        cmp al, '\'
        jne .dn
        lea bx, [di+1]
.dn:    inc di
        jmp .d
.de:    ; core path in pathbuf, vxd path in CB
        push cs
        pop es
        mov si, corename
        lea di, [pathbuf+bx]
        push bx
.c1:    lodsb
        stosb
        test al, al
        jnz .c1
        pop bx
        mov cx, bx
        mov si, pathbuf
        mov di, cb+CB_VXDPATH
        rep movsb
        mov si, vxdname
.c2:    lodsb
        stosb
        test al, al
        jnz .c2
        pop es
        ret

tok     times 32 db 0
%ifdef PKG_AUDIO
vmoptab dw VMF_NOV86
        db 'NOV86', 0
        dw VMF_NODPMI
        db 'NODPMI', 0
        dw VMF_NOVCPI
        db 'NOVCPI', 0
        dw VMF_NOVME
        db 'NOVME', 0
        dw VMF_VCPI
        db 'VCPI', 0
        dw VMF_USEVME
        db 'VME', 0
        dw 0
%endif
opt2pass db 0
; option bit, name (second table: bits of the second option word)
optab2:
%ifdef PKG_USB
        dd OPT2_NOTB
        db 'NOTB', 0
        dd OPT2_NOTBPCIE
        db 'NOTBPCIE', 0
        dd OPT2_NOTBUSB
        db 'NOTBUSB', 0
        dd OPT2_PCIHP
        db 'PCIHP', 0
        dd OPT2_DOSDISK
        db 'DOSDISK', 0
        dd OPT2_NOPS2
        db 'NOPS2', 0
        dd OPT2_NOTP
        db 'NOTP', 0
        dd OPT2_NOSMB
        db 'NOSMB', 0
%endif
%ifdef PKG_AUDIO
        dd OPT2_DSP
        db 'DSP', 0
        dd OPT2_DMIC
        db 'DMIC', 0
        dd OPT2_NODSP
        db 'NODSP', 0
        dd OPT2_NOTPVOL
        db 'NOTPVOL', 0
        dd OPT2_USBSB
        db 'USBSB', 0
%endif
        dd OPT2_NORELOC
        db 'NORELOC', 0
        dd OPT2_ALL4G
        db 'ALL4G', 0
        dd 0
optab:
%ifdef PKG_USB
        dd OPT_WINONLY
        db 'WIN', 0
        dd OPT_NOHID
        db 'NOHID', 0
        dd OPT_NOMSC
        db 'NOMSC', 0
        dd OPT_NOKBC
        db 'NOKBC', 0
        dd OPT_POLL
        db 'POLL', 0
        dd OPT_NOXHCI
        db 'NOXHCI', 0
        dd OPT_NOEHCI
        db 'NOEHCI', 0
        dd OPT_NOOHCI
        db 'NOOHCI', 0
        dd OPT_NOUHCI
        db 'NOUHCI', 0
        dd OPT_NOROUTE
        db 'NOROUTE', 0
        dd OPT_NOI2C
        db 'NOI2C', 0
        dd OPT_NONVME
        db 'NONVME', 0
        dd OPT_NVME
        db 'NVME', 0
%elifdef PKG_AUDIO
        dd OPT_WINONLY
        db 'WIN', 0
        dd OPT_NOHDA
        db 'NOHDA', 0
        dd OPT_NOAC97
        db 'NOAC97', 0
        dd OPT_INVEAPD
        db 'EAPD', 0
        dd OPT_NOGENERIC
        db 'NOGEN', 0
        dd OPT_HDAIMM
        db 'IMM', 0
        dd OPT_NOSYNTH
        db 'NOSYNTH', 0
        dd OPT_POSBUF
        db 'POSBUF', 0
        dd OPT_HDAGPIO
        db 'GPIO', 0
%else
        dd OPT_NOIDE
        db 'NOIDE', 0
        dd OPT_PIO
        db 'PIO', 0
        dd OPT_ISA
        db 'ISA', 0
        dd OPT_NOAHCI
        db 'NOAHCI', 0
        dd OPT_NONVME
        db 'NONVME', 0
        dd OPT_ALL
        db 'ALL', 0
        dd OPT_NVME
        db 'NVME', 0
%endif
        dd OPT_NOVXD
        db 'NOVXD', 0
        dd OPT_COM1
        db 'COM1', 0
        dd OPT_VERBOSE
        db 'V', 0
        dd OPT_DEBUG
        db 'D', 0
        dd OPT_SCREEN
        db 'S', 0
        dd 0

; ---------------------------------------------------------------------------
vcpi_detect:
        ; INT 67h must be installed
        push es
        xor ax, ax
        mov es, ax
        mov ax, [es:67h*4+2]
        pop es
        test ax, ax
        jz .no
        mov ax, 0DE00h
        mov bx, 5733h                   ; WINAUDIO's V86 monitor: a WINDRV driver asks
        int 67h
        test ah, ah
        jnz .no
        clc
        ret
.no:    stc
        ret

; XMS move of xm_len bytes from conventional ESI(seg:off) to EMB offset EDI
xms_to_emb:
        mov [xm_len], ecx
        mov word [xm_sh], 0
        mov [xm_so], esi
        mov ax, [emb_handle]
        mov [xm_dh], ax
        mov [xm_do], edi
        mov si, xmove
        mov ah, 0Bh
        call far [xmsentry]
        cmp ax, 1
        jne .e
        clc
        ret
.e:     stc
        ret

; clear the init buffer (BX = bytes)
clrbuf:
        push es
        push cs
        pop es
        mov di, initbuf
        mov cx, bx
        xor al, al
        rep stosb
        pop es
        ret

; buffer far pointer for XMS moves
bufptr: mov esi, cs
        shl esi, 16
        mov si, initbuf
        ret

%ifdef PKG_USB
CORE_POOL       equ 3584 * 1024             ; camera frames and their isochronous buffers (core window: 4 MB)
%else
CORE_POOL       equ 1536 * 1024
%endif

;---------------------------------------------------------------------------
%ifdef PKG_AUDIO
; load_fw: every firmware entry of the control block in state WFW_REQ is read from the
; driver directory into its own XMS block (locked, 4K aligned start)
fwpath          times 128 db 0
fwsub           db 'WINDRVFW\', 0
fwname          dw 0
fwent           dw 0
fwhandle        dw 0
fwemb           dw 0
fwphys          dd 0
fwoff           dd 0
fwsize          dd 0

load_fw:
        pushad
        mov word [fwent], cb+CB_FWTAB
.next:  cmp word [fwent], cb+CB_FWTAB+WIN_MAXFW*24
        jae .x
        mov bx, [fwent]
        cmp byte [bx+13], 1
        jne .skip
        ; path: directory of the driver (as in the VxD path) + file name
        mov si, cb+CB_VXDPATH
        mov di, fwpath
        mov dx, fwpath
.c:     lodsb
        mov [di], al
        inc di
        cmp al, '\'
        jne .c1
        mov dx, di
.c1:    test al, al
        jnz .c
        ; first the WINDRVFW subdirectory (where the installer puts the firmware)
        mov di, dx
        mov [fwname], dx
        mov si, fwsub
.c2:    lodsb
        mov [di], al
        inc di
        test al, al
        jnz .c2
        dec di
        call .name
        mov dx, fwpath
        mov ax, 3D00h
        int 21h
        jnc .opened
        ; then the driver directory itself
        mov di, [fwname]
        call .name
        mov dx, fwpath
        mov ax, 3D00h
        int 21h
        jc .miss
.opened:
        mov [fhandle], ax
        mov bx, ax
        mov ax, 4202h
        xor cx, cx
        xor dx, dx
        int 21h
        jc .close
        mov [fwsize], ax
        mov [fwsize+2], dx
        mov bx, [fhandle]
        mov ax, 4200h
        xor cx, cx
        xor dx, dx
        int 21h
        mov eax, [fwsize]
        cmp eax, 10h
        jb .close
        cmp eax, 1000000h
        jae .close
        ; XMS block: size + 4K for the alignment, in KB
        add eax, 2000h - 1
        shr eax, 10
        mov dx, ax
        mov ah, 09h
        call far [xmsentry]
        cmp ax, 1
        jne .close
        mov [fwemb], dx
        mov ah, 0Ch
        call far [xmsentry]
        cmp ax, 1
        jne .freeemb
        mov [fwphys], bx
        mov [fwphys+2], dx
        mov eax, [fwphys]
        add eax, 0FFFh
        and eax, 0FFFFF000h
        mov ecx, eax
        sub ecx, [fwphys]
        mov [fwoff], ecx
        mov [fwphys], eax
        ; the file in 16K pieces
        mov edi, [fwoff]
.rd:    mov bx, [fhandle]
        mov ah, 3Fh
        mov cx, 4000h
        mov dx, initbuf
        int 21h
        jc .unlock
        test ax, ax
        jz .eof
        movzx ecx, ax
        inc ecx
        and ecx, 0FFFFFFFEh
        push edi
        push ecx
        call bufptr
        mov ax, [emb_handle]
        push ax
        mov ax, [fwemb]
        mov [emb_handle], ax
        call xms_to_emb
        pop ax
        mov [emb_handle], ax
        pop ecx
        pop edi
        jc .unlock
        add edi, ecx
        jmp .rd
.eof:   mov bx, [fhandle]
        mov ah, 3Eh
        int 21h
        mov bx, [fwent]
        mov eax, [fwphys]
        mov [bx+16], eax
        mov eax, [fwsize]
        mov [bx+20], eax
        mov ax, [fwemb]
        mov [bx+14], ax
        mov byte [bx+13], 2
        jmp .skip
.unlock:
        mov dx, [fwemb]
        mov ah, 0Dh
        call far [xmsentry]
.freeemb:
        mov dx, [fwemb]
        mov ah, 0Ah
        call far [xmsentry]
.close: mov bx, [fhandle]
        mov ah, 3Eh
        int 21h
.miss:  mov bx, [fwent]
        mov byte [bx+13], 3
.skip:  add word [fwent], 24
        jmp .next
.x:     popad
        ret
; the file name of the entry [fwent] to [di] (NUL included)
.name:  mov si, [fwent]
.n1:    lodsb
        mov [di], al
        inc di
        test al, al
        jnz .n1
        ret

; free_fw: the XMS blocks of the firmware files (driver not staying resident)
free_fw:
        pushad
        mov bx, cb+CB_FWTAB
.f:     cmp bx, cb+CB_FWTAB+WIN_MAXFW*24
        jae .fx
        cmp byte [bx+13], 2
        jne .fn
        push bx
        mov dx, [bx+14]
        mov ah, 0Dh
        call far [xmsentry]
        pop bx
        push bx
        mov dx, [bx+14]
        mov ah, 0Ah
        call far [xmsentry]
        pop bx
        mov byte [bx+13], 3
.fn:    add bx, 24
        jmp .f
.fx:    popad
        ret
%endif

load_core:
        mov dx, pathbuf
        mov ax, 3D00h
        int 21h
        jnc .opened
        call psp_path
        jc .nofile
        mov dx, pathbuf
        mov ax, 3D00h
        int 21h
        jc .nofile
.opened:
        mov [fhandle], ax
        mov bx, ax
        mov ah, 3Fh
        mov cx, 64
        mov dx, initbuf
        int 21h
        jc .nofile
        cmp ax, 64
        jne .bad
        cmp dword [initbuf], 49433357h          ; 'W3CI'
        jne .bad
        cmp dword [initbuf+4], 80004000h
        jne .bad
        mov eax, [initbuf+8]
        sub eax, 80004000h
        mov [filesize], eax
        mov eax, [initbuf+12]
        sub eax, 80004000h
        mov [imgtotal], eax
        ; core entry points and stack
        mov si, initbuf+16              ; +16..+40: functions 0-6
        mov di, corefn
        mov cx, 7
.cf:    mov eax, [si]
        mov [di], eax
        add si, 4
        add di, 4
        loop .cf
        mov si, initbuf+48              ; +48..+60: functions 7-10 (+44 is the stack)
        mov cx, 4
.cf2:   mov eax, [si]
        mov [di], eax
        add si, 4
        add di, 4
        loop .cf2
        mov eax, [initbuf+44]
        mov [core_stack], eax
        ; EMB: tables (16K) + image + pool + 4K alignment slack
        mov eax, [imgtotal]
        add eax, 4000h + 0FFFh
        and eax, 0FFFFF000h
        mov [coresize], eax
        add eax, CORE_POOL + 4096
        shr eax, 10
        mov [emb_kb], ax
        mov dx, ax
        mov ah, 09h
        call far [xmsentry]
        cmp ax, 1
        jne .noemb
        mov [emb_handle], dx
        mov ah, 0Ch
        call far [xmsentry]
        cmp ax, 1
        jne .noemb
        mov [emb_phys], bx
        mov [emb_phys+2], dx
        mov eax, [emb_phys]
        add eax, 0FFFh
        and eax, 0FFFFF000h
        mov [pd_phys], eax
        sub eax, [emb_phys]
        mov [emb_off], eax
        ; image: rewind and copy the file in 16K chunks to EMB offset 4000h
        mov bx, [fhandle]
        mov ax, 4200h
        xor cx, cx
        xor dx, dx
        int 21h
        mov edi, [emb_off]
        add edi, 4000h
.rd:    mov bx, [fhandle]
        mov ah, 3Fh
        mov cx, 4000h
        mov dx, initbuf
        int 21h
        jc .nofile
        test ax, ax
        jz .eof
        movzx ecx, ax
        inc ecx
        and ecx, 0FFFFFFFEh
        push edi
        push ecx
        call bufptr
        call xms_to_emb
        pop ecx
        pop edi
        jc .noemb
        add edi, ecx
        jmp .rd
.eof:   call close_core
        ; zero bss and stack
        mov bx, 4000h
        call clrbuf
        mov edi, [emb_off]
        add edi, 4000h
        add edi, [filesize]
        and edi, 0FFFFFFFEh
        mov edx, [emb_off]
        add edx, 4000h
        add edx, [imgtotal]
.z:     cmp edi, edx
        jae .zd
        mov ecx, edx
        sub ecx, edi
        cmp ecx, 4000h
        jbe .zl
        mov ecx, 4000h
.zl:    inc ecx
        and ecx, 0FFFFFFFEh
        push edi
        push edx
        push ecx
        call bufptr
        call xms_to_emb
        pop ecx
        pop edx
        pop edi
        add edi, ecx
        jmp .z
.zd:    mov eax, [pd_phys]
        mov [cb+CB_COREPHYS], eax
        mov eax, [coresize]
        mov [cb+CB_CORESIZE], eax
        ; pool parameters (core window linear / physical)
        mov eax, [coresize]
        add eax, 80000000h
        mov [p_pool_lin], eax
        mov eax, [pd_phys]
        add eax, [coresize]
        mov [p_pool_phys], eax
        mov dword [p_pool_size], CORE_POOL
        mov eax, [cb+CB_CBLIN]
        mov [p_cb_lin], eax
        clc
        ret
.nofile:
        call close_core
        mov dx, msg_nofile
        call print
        mov si, pathbuf
        call printz
        mov dx, msg_crlf
        call print
        stc
        ret
.bad:   call close_core
        mov dx, msg_badfile
        call print
        stc
        ret
.noemb: call close_core
        mov dx, msg_noemb
        call print
        stc
        ret

; DEVLOAD and other loaders may give the driver name without its directory: look for the
; driver's path in the command tail of the running program (PSP:81h) and put the core file
; name next to it. CF clear: pathbuf holds the new path.
drvname         db DRVNAME, 0
psp_path:
        pushad
        push es
        mov ah, 62h
        int 21h
        mov es, bx
        movzx cx, byte [es:80h]
        cmp cx, 7Eh
        ja .fail
        mov si, 81h
        add cx, si                      ; end of the tail
.tok:   cmp si, cx
        jae .fail
        cmp byte [es:si], ' '
        ja .t0
        inc si
        jmp .tok
.t0:    mov bx, si                      ; token start
        mov dx, si                      ; name start (after the last '\' or ':')
.t1:    cmp si, cx
        jae .t2
        mov al, [es:si]
        cmp al, ' '
        jbe .t2
        inc si
        cmp al, '\'
        je .tn
        cmp al, ':'
        jne .t1
.tn:    mov dx, si
        jmp .t1
.t2:    cmp dx, bx                      ; a directory part is needed
        je .tok
        push si
        mov si, dx
        mov di, drvname
.cmp:   mov al, [cs:di]
        test al, al
        jz .match
        mov ah, [es:si]
        cmp ah, 'a'
        jb .up
        cmp ah, 'z'
        ja .up
        sub ah, 20h
.up:    cmp ah, al
        jne .no
        inc si
        inc di
        jmp .cmp
.no:    pop si
        jmp .tok
.match: pop si
        mov si, bx                      ; directory part to pathbuf
        xor di, di
.cp:    cmp si, dx
        jae .cpe
        cmp di, 80
        jae .fail
        mov al, [es:si]
        mov [pathbuf+di], al
        inc si
        inc di
        jmp .cp
.cpe:   mov si, corename
.cn:    mov al, [cs:si]
        mov [pathbuf+di], al
        inc si
        inc di
        test al, al
        jnz .cn
        pop es
        popad
        clc
        ret
.fail:  pop es
        popad
        stc
        ret

; RDTSC works on this CPU (some x86 SoCs, e.g. Vortex86, have a time stamp counter but do
; not report it in CPUID): CF clear when it runs and counts. An invalid opcode exception is
; caught through INT 06h (V86 monitors reflect it there).
tsc_ud          db 0
tsc_old06       dd 0
tsc_trap:
        push bp
        mov bp, sp
        add word [bp+2], 2              ; skip RDTSC
        mov byte [cs:tsc_ud], 1
        pop bp
        iret
tsc_probe:
        pushad
        push es
        xor ax, ax
        mov es, ax
        cli
        mov eax, [es:6*4]
        mov [tsc_old06], eax
        mov word [es:6*4], tsc_trap
        mov [es:6*4+2], cs
        sti
        mov byte [tsc_ud], 0
        xor eax, eax
        rdtsc
        mov ebx, eax
        mov cx, 1000
.dl:    in al, 80h
        loop .dl
        xor eax, eax
        rdtsc
        cli
        mov edx, [tsc_old06]
        mov [es:6*4], edx
        sti
        pop es
        cmp byte [tsc_ud], 0
        jne .no
        cmp eax, ebx
        je .no
        popad
        clc
        ret
.no:    popad
        stc
        ret

; close the core file if it is open (a TSR must not keep handles of its own)
close_core:
        cmp word [fhandle], 0FFFFh
        je .r
        push ax
        push bx
        mov bx, [fhandle]
        mov ah, 3Eh
        int 21h
        mov word [fhandle], 0FFFFh
        pop bx
        pop ax
.r:     ret

; print the zero-terminated string at CS:SI
printz: push ax
        push dx
        push si
.l:     mov dl, [cs:si]
        test dl, dl
        jz .e
        mov ah, 02h
        int 21h
        inc si
        jmp .l
.e:     pop si
        pop dx
        pop ax
        ret

; build descriptors, IDT, page tables and the VCPI switch structure
; set_desc: BX = selector, EAX = base, ECX = limit (20 bit), DL = access, DH = flags (G/D in bits 7/6)
set_desc:
        mov [gdt+bx], cx
        mov [gdt+bx+2], ax
        shr eax, 16
        mov [gdt+bx+4], al
        mov [gdt+bx+5], dl
        shr ecx, 16
        and cl, 0Fh
        or cl, dh
        mov [gdt+bx+6], cl
        mov [gdt+bx+7], ah
        ret

setup_pm:
        mov bx, SEL_CODE32
        xor eax, eax
        mov ecx, 0FFFFFh
        mov dx, 0C09Ah
        call set_desc
        mov bx, SEL_DATA32
        xor eax, eax
        mov ecx, 0FFFFFh
        mov dx, 0C092h
        call set_desc
        mov bx, SEL_CODE16
        mov eax, [drv_lin]
        mov ecx, 0FFFFh
        mov dx, 009Ah
        call set_desc
        mov bx, SEL_DATA16
        mov eax, [drv_lin]
        mov ecx, 0FFFFFh
        mov dx, 8092h
        call set_desc
        mov bx, SEL_TSS
        mov eax, [drv_lin]
        add eax, tss
        mov ecx, 67h
        mov dx, 0089h
        call set_desc
        ; gdtr / idtr / far pointers
        mov eax, [drv_lin]
        add eax, gdt
        mov [gdtr+2], eax
        mov eax, [drv_lin]
        add eax, pm_idt
        mov [pm_idtr+2], eax
        mov eax, [drv_lin]
        add eax, pm32
        mov [pm_target], eax
        mov [vsw_eip], eax
        mov eax, [drv_lin]
        add eax, gdtr
        mov [vsw_gdtr], eax
        mov eax, [drv_lin]
        add eax, pm_idtr
        mov [vsw_idtr], eax
        mov eax, [drv_lin]
        add eax, vcpi_sw
        mov [vcpi_sw_lin], eax
        mov eax, [pd_phys]
        mov [vsw_cr3], eax
        ; exception gates
        mov cx, 0
.g:     mov bx, cx
        shl bx, 3
        mov si, cx
        imul si, si, exc1 - exc0
        add si, exc0
        movzx eax, si
        add eax, [drv_lin]
        mov [pm_idt+bx], ax
        mov word [pm_idt+bx+2], SEL_CODE32
        mov word [pm_idt+bx+4], 8E00h
        shr eax, 16
        mov [pm_idt+bx+6], ax
        inc cx
        cmp cx, 32
        jb .g

        ; page table 0: identity (real mode) or the VCPI server's first table
        mov bx, 4096
        call clrbuf
        cmp byte [vcpi_ok], 0
        je .ident
        push es
        push cs
        pop es
        mov di, initbuf
        mov si, gdt+SEL_VCPI
        mov ax, 0DE01h
        int 67h
        pop es
        test ah, ah
        jnz .fail
        mov [vcpi_pmentry], ebx
        or word [cb+CB_FLAGS], CBF_VCPI
        ; the HMA (100000h-10FFFFh) as it is: a server emulating A20 by paging may have it
        ; wrapped to 0-FFFFh at this moment, and DOS=HIGH keeps its buffers there
        mov di, initbuf + 256*4
        mov eax, 100000h
        mov cx, 16
.hma:   mov edx, [di]
        and edx, 0FFFh
        or edx, eax
        or edx, 7
        mov [di], edx
        add eax, 1000h
        add di, 4
        loop .hma
        jmp .pt0
.ident: xor eax, eax
        mov di, initbuf
        mov cx, 1024
.i:     mov edx, eax
        or edx, 7
        mov [di], edx
        add eax, 1000h
        add di, 4
        loop .i
.pt0:   mov edi, [emb_off]
        add edi, 1000h
        mov ecx, 4096
        call bufptr
        call xms_to_emb
        jc .fail
        ; core window page table: 80000000h -> EMB
        mov bx, 4096
        call clrbuf
        mov eax, [pd_phys]
        mov di, initbuf
        mov edx, [coresize]
        add edx, CORE_POOL
        shr edx, 12
        cmp edx, 1024
        jbe .wl
        mov edx, 1024
.wl:    mov cx, dx
.w:     mov ebx, eax
        or ebx, 3
        mov [di], ebx
        add eax, 1000h
        add di, 4
        loop .w
        mov edi, [emb_off]
        add edi, 2000h
        mov ecx, 4096
        call bufptr
        call xms_to_emb
        jc .fail
        ; MMIO page table: empty
        mov bx, 4096
        call clrbuf
        mov edi, [emb_off]
        add edi, 3000h
        mov ecx, 4096
        call bufptr
        call xms_to_emb
        jc .fail
        ; page directory
        mov eax, [pd_phys]
        add eax, 1007h
        mov [initbuf], eax
        mov eax, [pd_phys]
        add eax, 2003h
        mov [initbuf+512*4], eax
        mov eax, [pd_phys]
        add eax, 3003h
        mov [initbuf+768*4], eax
        mov edi, [emb_off]
        mov ecx, 4096
        call bufptr
        call xms_to_emb
        jc .fail
        clc
        ret
.fail:  mov dx, msg_noemb
        call print
        stc
        ret

; MBR of every BIOS hard disk (for detecting disks DOS already uses). Loaded high
; (DEVICEHIGH) the driver's buffer is in an upper memory block the memory manager maps from
; elsewhere, where BIOS code that writes to physical memory (32-bit code, SMM, bus master USB)
; misses it; BIOS code in V86 mode finds it. Then every sector is read a second time into the
; free part of the HMA (DOS=HIGH; where it really is), and the core takes the copy with a boot
; signature.
read_bios_mbrs:
        mov word [mbr_seg], 0
        mov ah, 08h
        mov dl, 80h
        push es
        int 13h
        pop es
        jc .none
        cmp dl, 8
        jbe .n
        mov dl, 8
.n:     mov [cb+CB_NBIOS], dl
        movzx cx, dl
        test cx, cx
        jz near .none
        mov ax, cs
        cmp ax, 0A000h
        jb .buf
        push cx
        push es
        push di
        mov ax, 4A01h                   ; free HMA space: BX bytes at ES:DI
        xor bx, bx
        int 2Fh
        mov ax, es
        mov dx, di
        pop di
        pop es
        pop cx
        cmp ax, 0FFFFh
        jne .buf
        shr bx, 9                       ; sectors that fit
        cmp bx, cx
        jb .buf
        mov [mbr_seg], ax
        mov [mbr_off], dx
        call a20_enable                 ; the HMA as itself
.buf:   mov dl, 80h
        mov bx, mbrbuf
.l:     push cx
        push dx
        push bx
        push cs
        pop es
        mov ax, 0201h
        mov cx, 0001h
        mov dh, 0
        int 13h
        pop bx
        pop dx
        pop cx
        cmp word [mbr_seg], 0
        je .g
        push cx
        push dx
        push bx
        sub bx, mbrbuf
        add bx, [mbr_off]
        mov es, [mbr_seg]
        mov ax, 0201h
        mov cx, 0001h
        mov dh, 0
        int 13h
        pop bx
        pop dx
        pop cx
.g:     call bios_geom
        add bx, 512
        inc dl
        loop .l
        mov eax, [drv_lin]
        add eax, mbrbuf
        mov [p_mbr_lin], eax
        cmp word [mbr_seg], 0
        je .none
        call a20_restore
        mov eax, 0FFFF0h
        movzx edx, word [mbr_off]
        add eax, edx
        mov [p_mbr2_lin], eax
.none:  ret

mbr_seg         dw 0
mbr_off         dw 0

; DL = BIOS drive: INT 13h AH=08h geometry into cb.biosgeom (for the FastDisk of the VxD)
bios_geom:
        pushad
        push es
        mov [cs:geom_drv], dl
        mov ah, 08h
        int 13h
        jc .x
        movzx bx, byte [cs:geom_drv]
        sub bl, 80h
        cmp bl, 8
        jae .x
        shl bx, 2
        mov al, cl
        and al, 3Fh
        mov [cb+CB_BIOSGEOM+bx+3], al   ; sectors per track
        inc dh
        mov [cb+CB_BIOSGEOM+bx+2], dh   ; heads
        mov al, cl
        shr al, 6
        xchg al, ah
        mov al, ch
        inc ax
        mov [cb+CB_BIOSGEOM+bx], ax     ; cylinders
.x:     pop es
        popad
        ret
geom_drv        db 0

; partitions DOS already mounted from BIOS disks (INT 2Fh AX=0803h drive data tables)
read_bds:
        ; FreeDOS has no drive data table list (INT 2Fh AX=0803h): every FAT partition of
        ; a BIOS disk is taken as one of its drives (0FFh: unknown)
        mov ax, 3000h
        int 21h
        cmp bh, 0FDh
        jne .ms
        mov byte [cs:cb+CB_NMOUNTED], 0FFh
        ret
.ms:    push ds
        mov ax, 0803h
        int 2Fh
        ; DS:DI = first table
        xor bx, bx
.l:     cmp di, 0FFFFh
        je .x
        mov al, [di+4]
        cmp al, 80h
        jb .nx
        cmp bx, 16 * 8
        jae .x
        mov [cs:cb+CB_MOUNTED+bx], al
        mov al, [di+5]
        mov [cs:cb+CB_MOUNTED+bx+1], al
        mov eax, [di+17h]
        mov [cs:cb+CB_MOUNTED+bx+4], eax
        add bx, 8
        inc byte [cs:cb+CB_NMOUNTED]
.nx:    lds di, [di]
        jmp .l
.x:     pop ds
        ret

show_units:
        mov dx, msg_units1
        call print
        movzx cx, byte [cb+CB_NUNITS]
        mov bl, [cb+CB_FIRSTDRIVE]
        add bl, 'A'
.l:     mov dl, bl
        mov ah, 02h
        int 21h
        mov dx, msg_units2
        call print
        mov dl, ' '
        mov ah, 02h
        int 21h
        inc bl
        loop .l
        mov dx, msg_crlf
        call print
        ret

        align 16
mbrbuf:         times 8 * 512 db 0
        align 16
initbuf:
