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
%define BANNER          'WINUSB 1.0 - USB driver for DOS and Windows 1.0-3.11'
%define COREFILE        'WINUCORE.BIN'
%define VXDFILE         'WINUSB.386'
%define DOSACTIVE       'USB active in DOS / standard mode'
MPLEXID         equ 0D4h
DEFSLOTS        equ 4
%elifdef PKG_AUDIO
%define DRVNAME         'WINAUDIO'
%define BANNER          'WINAUDIO 1.0 - HD Audio / AC97 sound driver for Windows 3.0-3.11'
%define COREFILE        'WINACORE.BIN'
%define VXDFILE         'WINAUDIO.386'
%define DOSACTIVE       'sound active in DOS / standard mode'
MPLEXID         equ 0D5h
DEFSLOTS        equ 0
%else
%define PKG_DISK
%define DRVNAME         'WINDISK'
%define HDRNAME         'WINDISK'
%define BANNER          'WINDISK 1.0 - IDE/AHCI/NVMe disk driver for DOS and Windows 3.0-3.11'
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

FN_INIT         equ 0
FN_DOS          equ 1
FN_POLL         equ 2
FN_IRQ          equ 3
FN_API          equ 4
FN_SHUTDOWN     equ 5
FN_RESUME       equ 6
FN_FILEDONE     equ 7
CBF_FWREQ       equ 8000h
CB_FWTAB        equ 448
WIN_MAXFW       equ 16

SEL_CODE32      equ 08h
SEL_DATA32      equ 10h
SEL_CODE16      equ 18h
SEL_DATA16      equ 20h
SEL_TSS         equ 28h
SEL_VCPI        equ 30h

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
oldirq          times 16 dd 0
xmsentry        dd 0
emb_handle      dw 0
emb_phys        dd 0
pd_phys         dd 0
drv_lin         dd 0
corefn          times 8 dd 0
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
        jmp far [cs:old2f]
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
.n4:    mov ax, 0FFFFh
        iret

.winstart:
        test dx, 1                      ; standard mode (DOSX): keep the DOS core
        jnz .chain
        pushf
        call far [cs:old2f]
        test cx, cx
        jnz .wsret
        cmp byte [cs:core_ok], 1
        jne .wsret
        pushad
        push ds
        push es
        mov eax, FN_SHUTDOWN
        xor ebx, ebx
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
        jnz .chain
        cmp byte [cs:handover], 0
        je .chain
        mov byte [cs:handover], 0
        pushad
        push ds
        push es
        mov eax, FN_RESUME
        xor ebx, ebx
        call callcore
        pop es
        pop ds
        popad
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
        call dos_safe
        jc .x
        call do_poll
.x:     iret

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
        pop ax
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
fhandle         dw 0
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
msg_notsc       db DRVNAME, ': CPU without time stamp counter (Pentium or later required)', 13, 10, '$'
msg_noxms       db DRVNAME, ': XMS driver (HIMEM.SYS) not loaded', 13, 10, '$'
msg_noemb       db DRVNAME, ': cannot allocate extended memory', 13, 10, '$'
msg_nofile      db DRVNAME, ': cannot read ', COREFILE, 13, 10, '$'
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
        mov word [cb+4], 0100h
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
        movzx eax, word [rm_seg]
        shl eax, 4
        mov [drv_lin], eax
        mov [pm32_base+1], eax          ; mov ebp, imm32 in pm32
        add eax, cb
        mov [cb+CB_CBLIN], eax

        call parse_cmdline
%ifdef PKG_USB
        ; MS-DOS 7 (Windows 95/98): the Plug and Play VxD and its port driver give the disks
        ; their letters; DOS letters only with /DOSDISK
        cmp byte [cb+CB_DOSVER+1], 7
        jb .dosdisk
        test word [cb+CB_OPTS2], OPT2_DOSDISK
        jnz .dosdisk
        or word [cb+CB_OPTS2], OPT2_NODISK
        mov byte [cb+CB_SLOTREQ], 0
.dosdisk:
%endif
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

        ; core_init(params)
        mov eax, FN_INIT
        mov ebx, [drv_lin]
        add ebx, params
        call callcore
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
        ; sound devices found: resident as the character device WINAUD$
        test word [cb+CB_FLAGS], CBF_HAVEDISK
        jz .unload
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
        mov ax, cb+CB_UNITS+28
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

; core_init parameters (struct core_params)
params:
p_pool_lin      dd 0
p_pool_phys     dd 0
p_pool_size     dd 0
p_cb_lin        dd 0
p_mode          dd 1
p_flags         dd 0
p_mbr_lin       dd 0
                dd 0, 0

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
.tu:    cmp di, 15
        jae .tn
        mov [tok+di], al
        inc di
.tn:    inc si
        jmp .tk
.te:    mov byte [tok+di], 0
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

tok     times 16 db 0
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
        jc .nofile
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
        mov si, initbuf+16
        mov di, corefn
        mov cx, 8
.cf:    mov eax, [si]
        mov [di], eax
        add si, 4
        add di, 4
        loop .cf
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
.eof:   mov bx, [fhandle]
        mov ah, 3Eh
        int 21h
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
        mov dx, msg_nofile
        call print
        stc
        ret
.bad:   mov dx, msg_badfile
        call print
        stc
        ret
.noemb: mov dx, msg_noemb
        call print
        stc
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

; MBR of every BIOS hard disk (for detecting disks DOS already uses)
read_bios_mbrs:
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
        jcxz .none
        mov dl, 80h
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
        call bios_geom
        add bx, 512
        inc dl
        loop .l
        mov eax, [drv_lin]
        add eax, mbrbuf
        mov [p_mbr_lin], eax
.none:  ret

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
        push ds
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
