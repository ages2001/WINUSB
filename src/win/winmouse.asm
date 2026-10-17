; winmouse.asm - WINMOUSE.DRV: Windows 3.x and 95/98/ME mouse driver for USB and PS/2 mice
; with wheel, back / forward buttons and autoscroll support
;
; 386 enhanced mode (and Windows 95/98/ME): WINUSB.386 / WINUSB.VXD calls MouseCallback through
;   nested execution with AX = delta X, BX = delta Y, CL = buttons, CH = previous buttons,
;   DX = wheel, SI = tilt. The VxD drives the PS/2 mouse itself (IntelliMouse Explorer: wheel
;   and buttons 4 and 5) unless it was told not to (/NOPS2); then a real PS/2 mouse is driven
;   through the INT 15h pointing device BIOS like the original MOUSE.DRV (no wheel on that path).
; standard and real mode: USB mice come through the mailbox of WINUSB.SYS (read on every timer
;   tick); a real PS/2 mouse is driven directly through the 8042 and switched to IntelliMouse
;   Explorer or IntelliMouse mode when possible.
; Buttons: bit 0 left, 1 right, 2 middle, 3 back (4th), 4 forward (5th). Wheel and tilt are
;   posted to the window registered with SetWheelWindow (WINWHEEL.EXE). Back and forward are
;   typed as Alt+Left / Alt+Right. The middle button goes to Windows 95/98/ME as such, or to
;   WINWHEEL when it asked for it (autoscroll); during autoscroll (AutoScroll) the motion moves
;   an offset WINWHEEL reads instead of the cursor, and a click ends it.

        bits 16

segment _TEXT public class=CODE use16
segment _DATA public class=DATA use16
group DGROUP _DATA

extern GetModuleHandle
extern GetProcAddress
extern GetWinFlags

extern GetVersion

WIN_VXD_ID       equ 3D34h                       ; WINUSB.386
WIN_MPLEX        equ 0D4h                        ; WINUSB.SYS
CBX_MODE         equ 2048                        ; wincbx.mode after its control block
WM_WINWHEEL      equ 0400h + 3D3h
WM_WINTILT       equ 0400h + 3D4h
WM_WINMBUTTON    equ 0400h + 3D5h                ; wParam 1 middle button pressed, 0 released
WM_WINASEND      equ 0400h + 3D6h                ; a click ended autoscroll

SF_MOVEMENT     equ 0001h
SF_B1_DOWN      equ 0002h
SF_B1_UP        equ 0004h
SF_B2_DOWN      equ 0008h
SF_B2_UP        equ 0010h
SF_B3_DOWN      equ 0020h                       ; Windows 95/98/ME
SF_B3_UP        equ 0040h
SF_ABSOLUTE     equ 8000h                       ; BX, CX: normalized 0..65535

WO_XBUTTONS     equ 01h                         ; SetWheelOptions: back / forward keys
WO_MIDDLE       equ 02h                         ; middle button to WINWHEEL (autoscroll)

VK_MENU         equ 12h
VK_LEFT         equ 25h
VK_RIGHT        equ 27h

segment _DATA
event_proc      dd 0
vmd_entry       dd 0
vxd_entry       dd 0
old74           dd 0
hwnd_wheel      dw 0
postmsg         dd 0
user_name       db 'USER', 0
enabled         db 0
enhanced        db 0
pkt             db 0, 0, 0, 0
pktlen          db 0
pktneed         db 3
last_buttons    db 0
absmode         db 0                    ; deliver: AX/BX are an absolute position
swap            db 0
ps2id           db 0FFh                 ; 0FFh: no real PS/2 mouse
hooked          db 0
hooked08        db 0
in_timer        db 0
latch           db 0
last_seq        db 0
cb_sel          dw 0
cb_off          dw 0
old08           dd 0
rmregs          times 50 db 0
ps2bios         db 0
bios_flag       db 0
bios_stat       db 0
bios_x          db 0
bios_y          db 0
imps_seq        db 0F3h, 200, 0F3h, 100, 0F3h, 80, 0
imex_seq        db 0F3h, 200, 0F3h, 200, 0F3h, 80, 0
imex            db 0                    ; PS/2: IntelliMouse Explorer packets (buttons 4 and 5)
ps2vxd          db 0                    ; enhanced mode: WINUSB.386 drives the PS/2 mouse
win4            db 0FFh                 ; Windows 95/98/ME (0FFh: not checked yet)
wopts           db WO_XBUTTONS          ; SetWheelOptions
keybd_ev        dd 0                    ; USER.keybd_event
mid_ww          db 0                    ; the middle button press went to WINWHEEL
as_on           db 0                    ; autoscroll running
as_x            dw 0                    ; motion since autoscroll started
as_y            dw 0
swallow         db 0                    ; buttons whose release is not delivered
ev_dx           dw 0
ev_dy           dw 0
ev_flags        dw 0
ev_btn          db 0
ev_chg          db 0

segment _TEXT

; ---------------------------------------------------------------------------
; DLL entry: DI = hInstance, DS = data segment
global LibEntry
LibEntry:
..start:
        mov ax, 1
        retf

; int FAR PASCAL WEP(int)
global WEP
WEP:
        mov ax, 1
        retf 2

; DS = DGROUP: win4 = 1 on Windows 95/98/ME (GetVersion 3.95 and later)
chk_win4:
        cmp byte [win4], 0FFh
        jne .x
        push ax
        push bx
        push cx
        push dx
        push es
        call far GetVersion
        mov byte [win4], 0
        cmp al, 3
        ja .y
        jb .r
        cmp ah, 95
        jb .r
.y:     mov byte [win4], 1
.r:     pop es
        pop dx
        pop cx
        pop bx
        pop ax
.x:     ret

; int FAR PASCAL Inquire(LPMOUSEINFO)
global Inquire
Inquire:
        push bp
        mov bp, sp
        push ds
        push di
        mov ax, DGROUP
        mov ds, ax
        call chk_win4
        cld
        les di, [bp+6]
        mov al, 1                       ; msExists
        stosb
        mov al, 0FFh                    ; msRelative
        stosb
        mov ax, 2                       ; msNumButtons: 3 where Windows knows the middle button
        add al, [win4]
        stosw
        mov ax, 125                     ; msRate
        stosw
        mov ax, 2                       ; thresholds
        stosw
        stosw
        xor ax, ax                      ; resolution
        stosw
        stosw
        mov ax, 14
        pop di
        pop ds
        pop bp
        retf 4

; void FAR PASCAL Enable(FARPROC lpEventProc)
global Enable
Enable:
        push bp
        mov bp, sp
        push ds
        push si
        push di
        mov ax, DGROUP
        mov ds, ax
        cli
        mov ax, [bp+6]
        mov [event_proc], ax
        mov ax, [bp+8]
        mov [event_proc+2], ax
        sti
        cmp byte [enabled], 0
        jne .done
        call chk_win4
        call resolve_user
        mov ax, 1600h                   ; enhanced mode?
        int 2Fh
        test al, 7Fh
        jz .standard
        mov byte [enhanced], 1
        mov byte [ps2vxd], 0
        mov ax, 1684h
        mov bx, WIN_VXD_ID
        xor di, di
        mov es, di
        int 2Fh
        mov ax, es
        or ax, di
        jz .bios
        mov [vxd_entry], di
        mov [vxd_entry+2], es
        mov ax, 4                       ; PS/2 mouse driven by the VxD?
        xor bx, bx
        call far [vxd_entry]
        test ax, ax
        jnz .bios
        test bl, 1
        jz .bios
        mov byte [ps2vxd], 1
        jmp .reg
.bios:  call bios_ps2_enable            ; a real PS/2 mouse works next to the USB one
.reg:   cmp word [vxd_entry+2], 0
        je .enh_done
        mov ax, 1                       ; register callback CX:DX
        mov cx, cs
        mov dx, MouseCallback
        call far [vxd_entry]
.enh_done:
        mov byte [enabled], 1
        jmp .done
.standard:
        mov byte [enhanced], 0
        ; USB mice: WINUSB.SYS leaves the movement in its mailbox, read on every timer tick
        call mbox_setup
        jc .ps2
        ; a PS/2 touchpad WINUSB.SYS drives in its own protocol: its motion comes through the
        ; mailbox too, the 8042 is left alone
        mov es, [cb_sel]
        mov bx, [cb_off]
        test byte [es:bx+CBX_MODE], 1
        jnz .mbox
.ps2:   call ps2_setup
        call hook74
        cmp word [cb_sel], 0
        je .nombox
.mbox:  call hook08
        mov ah, WIN_MPLEX
        mov al, 03h
        mov bl, 4
        int 2Fh
.nombox:
        mov byte [enabled], 1
.done:
        pop di
        pop si
        pop ds
        pop bp
        retf 4

; INT 15h pointing device call with retries on transmission errors. AX, BX as for INT 15h.
i15:
        push cx
        mov cx, 20
.r:     push ax
        push bx
        int 15h
        pop bx
        jnc .ok
        cmp ah, 4                       ; transmission error: retry
        pop ax
        jne .f
        loop .r
.f:     stc
        pop cx
        ret
.ok:    pop ax
        pop cx
        ret

; enhanced mode: enable a real PS/2 mouse through the BIOS and tell VMD about it
bios_ps2_enable:
        int 11h
        test al, 04h                    ; pointing device installed?
        jz .x
        mov ax, 0C205h                  ; initialize, 3-byte packets
        mov bh, 3
        call i15
        jc .x
        mov ax, 0C201h                  ; reset
        call i15
        jc .x
        mov ax, 0C203h                  ; resolution 8 counts/mm
        mov bh, 3
        call i15
        mov ax, 0C207h                  ; install our handler
        push cs
        pop es
        mov bx, BiosCallback
        call i15
        jc .x
        mov ax, 0C206h                  ; 1:1 scaling
        mov bh, 1
        call i15
        mov ax, 0C202h                  ; 100 reports per second
        mov bh, 5
        call i15
        mov ax, 0C200h                  ; enable
        mov bh, 1
        call i15
        jc .x
        mov byte [ps2bios], 1
        mov byte [bios_flag], 0
        call hook74
        ; VMD: mouse type PS/2 on vector 74h, so IRQ 12 is routed to the system VM
        xor di, di
        mov es, di
        mov ax, 1684h
        mov bx, 000Ch
        int 2Fh
        mov ax, es
        or ax, di
        jz .x
        mov [vmd_entry], di
        mov [vmd_entry+2], es
        mov ax, 100h
        mov bx, 7404h
        call far [vmd_entry]
.x:     ret

bios_ps2_disable:
        cmp byte [ps2bios], 0
        je .x
        mov ax, 0C200h
        xor bh, bh
        call i15
        mov ax, 0C207h
        xor bx, bx
        mov es, bx
        call i15
        mov byte [ps2bios], 0
.x:     ret

; called by the BIOS IRQ 12 handler with status, X, Y, Z on the stack
BiosCallback:
        push bp
        mov bp, sp
        push ax
        push ds
        mov ax, DGROUP
        mov ds, ax
        mov al, [bp+0Ch]
        mov [bios_stat], al
        mov al, [bp+0Ah]
        mov [bios_x], al
        mov al, [bp+08h]
        mov [bios_y], al
        mov byte [bios_flag], 1
        pop ds
        pop ax
        pop bp
        retf

; standard and real mode: locate the WINUSB.SYS control block (INT 2Fh D400h, in protected
; mode through DPMI) and make a selector for it. CF set when WINUSB.SYS is not loaded.
mbox_setup:
        push es
        push di
        call far GetWinFlags
        test al, 1                      ; WF_PMODE
        jnz .pm
        mov ax, WIN_MPLEX * 256          ; real mode (Windows 3.0 WIN /R): call it directly
        int 2Fh
        cmp al, 0FFh
        jne .fail
        cmp bx, 3357h
        jne .fail
        mov [cb_sel], es
        mov [cb_off], di
        jmp .ok
.pm:    push ds
        pop es
        mov di, rmregs
        mov cx, 25
        xor ax, ax
        cld
        rep stosw
        mov word [rmregs+1Ch], WIN_MPLEX * 256 ; EAX: AH = multiplex id, AL = 0
        mov di, rmregs
        mov ax, 0300h
        mov bx, 002Fh
        xor cx, cx
        int 31h
        jc .fail
        cmp byte [rmregs+1Ch], 0FFh     ; AL
        jne .fail
        cmp word [rmregs+10h], 3357h    ; BX
        jne .fail
        mov bx, [rmregs+22h]            ; ES
        mov ax, 0002h                   ; segment to descriptor
        int 31h
        jc .fail
        mov [cb_sel], ax
        mov ax, [rmregs+00h]            ; DI
        mov [cb_off], ax
.ok:    mov byte [last_seq], 0
        clc
        jmp .x
.fail:  stc
.x:     pop di
        pop es
        ret

; Interrupt vectors are set through DOS: the DOS extender of Windows 3.0 standard mode
; routes hardware interrupts to vectors set this way (not to DPMI 0205h), and real mode
; has no DPMI at all. AL = vector, DS:DX = handler; old vector -> [DI]
setvec:
        push es
        push bx
        push ax
        mov ah, 35h
        int 21h
        mov [di], bx
        mov [di+2], es
        pop ax
        push ds
        push cs
        pop ds
        mov ah, 25h
        int 21h
        pop ds
        pop bx
        pop es
        ret

; AL = vector, restore from [DI]
resetvec:
        push ds
        mov dx, [di]
        mov ds, [di+2]
        mov ah, 25h
        int 21h
        pop ds
        ret

hook08:
        cmp byte [hooked08], 0
        jne .x
        mov al, 08h
        mov di, old08
        mov dx, Timer08
        call setvec
        mov byte [hooked08], 1
.x:     ret

unhook08:
        cmp byte [hooked08], 0
        je .x
        mov al, 08h
        mov di, old08
        call resetvec
        mov byte [hooked08], 0
.x:     ret

; hook INT 74h (IRQ 12)
hook74:
        cmp byte [hooked], 0
        jne .x
        mov al, 74h
        mov di, old74
        mov dx, Irq12
        call setvec
        mov byte [pktlen], 0
        mov byte [hooked], 1
        in al, 0A1h
        and al, 0EFh
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
.x:     ret

unhook74:
        cmp byte [hooked], 0
        je .x
        mov al, 74h
        mov di, old74
        call resetvec
        mov byte [hooked], 0
.x:     ret

; read the 8042 command byte into AL
kbc_getcmd:
        call kbc_wait
        mov al, 20h
        out 64h, al
        jmp kbc_read

; write AL to the 8042 command byte
kbc_setcmd:
        mov ah, al
        call kbc_wait
        mov al, 60h
        out 64h, al
        call kbc_wait
        mov al, ah
        out 60h, al
        ret

; read one byte from the aux device; CF on timeout. Keyboard bytes are dropped.
aux_read:
        push cx
        push dx
        mov dx, 16
.o:     mov cx, 0FFFFh
.w:     in al, 64h
        test al, 01h
        jz .n
        test al, 20h
        in al, 60h
        jnz .ok
.n:     loop .w
        dec dx
        jnz .o
        stc
        jmp .x
.ok:    clc
.x:     pop dx
        pop cx
        ret

; send AL to the aux device and wait for its acknowledge; CF on failure
aux_cmd:
        mov ah, al
        call kbc_wait
        mov al, 0D4h
        out 64h, al
        call kbc_wait
        mov al, ah
        out 60h, al
        call aux_read
        jc .x
        cmp al, 0FAh
        je .x
        stc
.x:     ret

; probe a real PS/2 mouse, switch it to IntelliMouse mode if possible and enable it.
; Sets ps2id (0FFh = none) and pktneed. The aux interface and IRQ 12 are left enabled
; for the packets WINUSB.SYS injects.
ps2_setup:
        pushf
        cli
        mov byte [ps2id], 0FFh
        mov byte [pktneed], 3
        mov byte [imex], 0
        call kbc_wait
        mov al, 0A8h                    ; enable aux interface
        out 64h, al
        call kbc_getcmd
        and al, 0DDh                    ; aux clock on, aux interrupt off while probing
        call kbc_setcmd
        mov al, 0F5h                    ; disable reporting
        call aux_cmd
        jc .done
        mov al, 0F6h                    ; defaults
        call aux_cmd
        jc .done
        mov si, imps_seq                ; IntelliMouse knock: wheel
        call aux_knock
        jc .done
        mov [ps2id], al
        cmp al, 3
        jne .rate
        mov byte [pktneed], 4
        mov si, imex_seq                ; IntelliMouse Explorer knock: buttons 4 and 5
        call aux_knock
        jc .rate
        cmp al, 4
        jne .rate
        mov [ps2id], al
        mov byte [imex], 1
.rate:  mov al, 0F3h
        call aux_cmd
        mov al, 100
        call aux_cmd
        mov al, 0F4h                    ; enable reporting
        call aux_cmd
.done:  call kbc_getcmd
        or al, 02h                      ; aux interrupt on
        and al, 0DFh
        call kbc_setcmd
        popf
        ret

; send the sample rate sequence at SI, then read the device id into AL; CF on failure
aux_knock:
        lodsb
        test al, al
        jz .id
        call aux_cmd
        jc .x
        jmp aux_knock
.id:    mov al, 0F2h                    ; get device id
        call aux_cmd
        jc .x
        call aux_read
.x:     ret

; leave the mouse in plain 3-byte mode for DOS mouse drivers
ps2_reset:
        cmp byte [ps2id], 0FFh
        je .x
        pushf
        cli
        mov al, 0FFh
        call aux_cmd
        jc .r
        call aux_read                   ; AAh
        call aux_read                   ; 00h
.r:     popf
.x:     ret

kbc_wait:
        push cx
        mov cx, 0FFFFh
.w:     in al, 64h
        test al, 02h
        loopnz .w
        pop cx
        ret

kbc_read:
        push cx
        mov cx, 0FFFFh
.r:     in al, 64h
        test al, 01h
        loopz .r
        in al, 60h
        pop cx
        ret

; void FAR PASCAL Disable(void)
global Disable
Disable:
        push ds
        push si
        push di
        mov ax, DGROUP
        mov ds, ax
        cmp byte [enabled], 0
        je .done
        call unhook74
        cmp byte [enhanced], 0
        je .std
        call bios_ps2_disable
        cmp word [vxd_entry+2], 0
        je .rst
        mov ax, 2
        call far [vxd_entry]
        jmp .off
.std:   call unhook08
        mov ah, WIN_MPLEX
        mov al, 03h
        xor bl, bl
        int 2Fh
.rst:   call ps2_reset
.off:   mov byte [enabled], 0
.done:  pop di
        pop si
        pop ds
        retf

; int FAR PASCAL MouseGetIntVect(void)
global MouseGetIntVect
MouseGetIntVect:
        mov ax, -1
        retf

; void FAR PASCAL SetWheelWindow(HWND)
global SetWheelWindow
SetWheelWindow:
        push bp
        mov bp, sp
        push ds
        mov ax, DGROUP
        mov ds, ax
        push si
        push di
        call resolve_user
        mov ax, [bp+6]
        cli
        mov [hwnd_wheel], ax
        test ax, ax
        jnz .set
        ; WINWHEEL is gone: no autoscroll, the middle button goes to Windows again
        and byte [wopts], ~WO_MIDDLE
        mov byte [as_on], 0
.set:   sti
        pop di
        pop si
        pop ds
        pop bp
        retf 2

; USER.PostMessage and USER.keybd_event (DS = DGROUP)
resolve_user:
        push ax
        push bx
        push cx
        push dx
        push es
        cmp word [postmsg+2], 0
        jne .x
        push ds
        push word user_name
        call far GetModuleHandle
        test ax, ax
        jz .x
        push ax
        push ax
        push word 0
        push word 110                   ; PostMessage
        call far GetProcAddress
        mov [keybd_ev], ax              ; temporary
        mov [keybd_ev+2], dx
        pop ax
        push ax
        push word 0
        push word 289                   ; keybd_event
        call far GetProcAddress
        cli
        mov bx, [keybd_ev]
        mov cx, [keybd_ev+2]
        mov [keybd_ev], ax
        mov [keybd_ev+2], dx
        mov [postmsg], bx
        mov [postmsg+2], cx
        sti
.x:     pop es
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; void FAR PASCAL SetWheelOptions(WORD): WO_XBUTTONS, WO_MIDDLE
global SetWheelOptions
SetWheelOptions:
        push bp
        mov bp, sp
        push ds
        mov ax, DGROUP
        mov ds, ax
        mov al, [bp+6]
        mov [wopts], al
        pop ds
        pop bp
        retf 2

; DWORD FAR PASCAL AutoScroll(WORD mode): 1 start (the motion is held back and added up),
; 0 stop, 2 query; returns the motion since the start: low word X, high word Y
global AutoScroll
AutoScroll:
        push bp
        mov bp, sp
        push ds
        mov ax, DGROUP
        mov ds, ax
        mov ax, [bp+6]
        cli
        cmp ax, 1
        jne .n1
        mov word [as_x], 0
        mov word [as_y], 0
        mov byte [as_on], 1
        jmp .q
.n1:    test ax, ax
        jnz .q
        mov byte [as_on], 0
.q:     mov ax, [as_x]
        mov dx, [as_y]
        sti
        pop ds
        pop bp
        retf 2

; void FAR PASCAL MiddleButton(WORD down): a middle button event WINWHEEL hands back to
; Windows (95/98/ME)
global MiddleButton
MiddleButton:
        push bp
        mov bp, sp
        push ds
        push si
        push di
        mov ax, DGROUP
        mov ds, ax
        cmp byte [win4], 1
        jne .x
        cmp word [event_proc+2], 0
        je .x
        mov ax, SF_B3_UP
        cmp word [bp+6], 0
        je .up
        mov ax, SF_B3_DOWN
.up:    xor bx, bx
        xor cx, cx
        mov dx, 3
        xor si, si
        xor di, di
        push ds
        call far [event_proc]
        pop ds
.x:     pop di
        pop si
        pop ds
        pop bp
        retf 2

; ---------------------------------------------------------------------------
; common event path. In: AX dx, BX dy, CL buttons (bit 0 left, 1 right, 2 middle, 3 back,
; 4 forward), DX wheel, SI tilt. DS = DGROUP. The decisions are taken with interrupts off
; (the IRQ 12 handler can enter here while the timer handler is inside), the calls into
; USER come afterwards.
deliver:
        push bp
        mov bp, sp
        push dx                         ; [bp-2] wheel
        push si                         ; [bp-4] tilt
        sub sp, 10                      ; [bp-6] status, [bp-8] dX, [bp-10] dY, [bp-11] keys,
                                        ; [bp-12] middle button message, [bp-13] autoscroll end
        pushf
        cli
        mov word [bp-6], 0
        mov word [bp-12], 0
        mov byte [bp-13], 0
        ; autoscroll: the motion goes to the offset WINWHEEL reads, not to the cursor
        cmp byte [as_on], 0
        je .mv
        cmp byte [absmode], 0
        jne .asabs
        push cx
        mov cx, [as_x]
        call add_clamp
        mov [as_x], cx
        mov ax, bx
        mov cx, [as_y]
        call add_clamp
        mov [as_y], cx
        pop cx
.asabs: mov byte [absmode], 0
        xor ax, ax
        xor bx, bx
.mv:    mov [bp-8], ax
        mov [bp-10], bx
        cmp byte [absmode], 0
        je .rel
        mov word [bp-6], SF_MOVEMENT | SF_ABSOLUTE
        mov byte [absmode], 0
        jmp .btn
.rel:   or ax, bx
        jz .btn
        mov word [bp-6], SF_MOVEMENT
.btn:   and cl, 1Fh
        mov ch, [last_buttons]
        mov [last_buttons], cl
        xor ch, cl                      ; CH = changed buttons
        ; a press during autoscroll ends it; neither the press nor its release is delivered
        cmp byte [as_on], 0
        je .sw
        mov al, ch
        and al, cl
        jz .sw
        mov byte [as_on], 0
        mov byte [bp-13], 1
        or [swallow], al
        not al
        and ch, al
.sw:    mov al, cl
        not al
        and al, ch
        and al, [swallow]               ; releases of swallowed presses
        jz .b1
        not al
        and ch, al
        and [swallow], al
.b1:    test ch, 1
        jz .b2
        test cl, 1
        jz .b1up
        or word [bp-6], SF_B1_DOWN
        jmp .b2
.b1up:  or word [bp-6], SF_B1_UP
.b2:    test ch, 2
        jz .b3
        test cl, 2
        jz .b2up
        or word [bp-6], SF_B2_DOWN
        jmp .b3
.b2up:  or word [bp-6], SF_B2_UP
.b3:    test ch, 4
        jz .b45
        test cl, 4
        jz .b3up
        ; middle pressed: to WINWHEEL when it asked for it (autoscroll), else to Windows 95/98/ME
        test byte [wopts], WO_MIDDLE
        jz .b3win
        cmp word [hwnd_wheel], 0
        je .b3win
        cmp word [postmsg+2], 0
        je .b3win
        mov byte [mid_ww], 1
        mov byte [bp-12], 2
        jmp .b45
.b3win: mov byte [mid_ww], 0
        cmp byte [win4], 1
        jne .b45
        or word [bp-6], SF_B3_DOWN
        jmp .b45
.b3up:  cmp byte [mid_ww], 0            ; the release goes where the press went
        je .b3upw
        mov byte [mid_ww], 0
        mov byte [bp-12], 1
        jmp .b45
.b3upw: cmp byte [win4], 1
        jne .b45
        or word [bp-6], SF_B3_UP
.b45:   and ch, cl                      ; back / forward act on the press
        and ch, 18h
        test byte [wopts], WO_XBUTTONS
        jnz .k
        xor ch, ch
.k:     mov [bp-11], ch
        popf
        ; mouse event to USER
        mov ax, [bp-6]
        test ax, ax
        jz .ev_done
        cmp word [event_proc+2], 0
        je .ev_done
        mov bx, [bp-8]                  ; BX = delta X
        mov cx, [bp-10]                 ; CX = delta Y
        mov dx, 2                       ; number of buttons
        cmp byte [win4], 1
        jne .nb
        inc dx
.nb:    xor si, si
        xor di, di
        push ds
        sti
        call far [event_proc]
        pop ds
.ev_done:
        ; middle button and end of autoscroll for WINWHEEL
        cmp byte [bp-13], 0
        je .mid
        mov bx, WM_WINASEND
        xor dx, dx
        call post_wheel
.mid:   mov al, [bp-12]
        test al, al
        jz .keys
        dec al
        mov dl, al
        xor dh, dh
        mov bx, WM_WINMBUTTON
        call post_wheel
.keys:  test byte [bp-11], 08h          ; back: Alt+Left
        jz .fwd
        mov ax, VK_LEFT
        mov bx, 14Bh
        call alt_key
.fwd:   test byte [bp-11], 10h          ; forward: Alt+Right
        jz .wheel
        mov ax, VK_RIGHT
        mov bx, 14Dh
        call alt_key
.wheel: ; wheel and tilt go to the helper window
        mov dx, [bp-2]
        test dx, dx
        jz .tilt
        mov bx, WM_WINWHEEL
        call post_wheel
.tilt:  mov dx, [bp-4]
        test dx, dx
        jz .x
        mov bx, WM_WINTILT
        call post_wheel
.x:     mov sp, bp
        pop bp
        ret

; AX += CX into CX, limited to +-4000 (autoscroll offset)
add_clamp:
        push eax
        push edx
        movsx eax, ax
        movsx edx, cx
        add eax, edx
        cmp eax, 4000
        jle .lo
        mov eax, 4000
.lo:    cmp eax, -4000
        jge .ok
        mov eax, -4000
.ok:    mov cx, ax
        pop edx
        pop eax
        ret

; PostMessage(hwnd_wheel, BX, DX, 0)
post_wheel:
        cmp word [hwnd_wheel], 0
        je .x
        cmp word [postmsg+2], 0
        je .x
        push word [hwnd_wheel]
        push bx
        push dx
        push word 0
        push word 0
        call far [postmsg]
.x:     ret

; Alt + the key AX (virtual key) / BX (scan code, BH = 1 extended) through USER.keybd_event
alt_key:
        cmp word [keybd_ev+2], 0
        je .x
        push bx
        push ax
        mov ax, VK_MENU
        mov bx, 38h
        call key_ev
        pop ax
        pop bx
        call key_ev
        or ax, 8000h
        call key_ev
        mov ax, VK_MENU | 8000h
        mov bx, 38h
        call key_ev
.x:     ret

; AX = virtual key (AH bit 7: released), BX = scan code
key_ev:
        pusha
        push ds
        push es
        xor si, si
        xor di, di
        call far [keybd_ev]
        pop es
        pop ds
        popa
        ret

; called by WINUSB.386 (nested execution in the system VM)
global MouseCallback
MouseCallback:
        push ds
        push es
        push bp
        push di
        push si
        push dx
        push cx
        push bx
        push ax
        push ax
        mov ax, DGROUP
        mov ds, ax
        pop ax
        test di, 1                      ; WINUSB.386: tablet position
        jz .rel
        mov byte [absmode], 1
.rel:   call deliver
        pop ax
        pop bx
        pop cx
        pop dx
        pop si
        pop di
        pop bp
        pop es
        pop ds
        retf

; timer (standard mode): after WINUSB.SYS polled the USB stack, deliver the mailbox
Timer08:
        push ds
        push ax
        mov ax, DGROUP
        mov ds, ax
        pushf
        call far [old08]
        cmp byte [in_timer], 0
        jne .x
        mov byte [in_timer], 1
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push es
        mov es, [cb_sel]
        mov bx, [cb_off]
        cli
        mov al, [es:bx+310]             ; mb_seq
        cmp al, [last_seq]
        je .none
        mov [last_seq], al
        mov ch, [es:bx+311]             ; bits 4-6 / 1-2: buttons 1-3 / 4-5 pressed since the last read
        and byte [es:bx+311], 01h
        mov cl, ch
        shr ch, 4
        and ch, 7
        shl cl, 2
        and cl, 18h
        or ch, cl
        mov [latch], ch
        mov ax, [es:bx+304]             ; mb_dx
        mov si, [es:bx+306]             ; mb_dy
        movsx dx, byte [es:bx+308]      ; mb_dz
        mov cl, [es:bx+309]             ; mb_btn
        mov word [es:bx+304], 0
        mov word [es:bx+306], 0
        mov byte [es:bx+308], 0
        test byte [es:bx+311], 1        ; mb_flags: tablet position
        jz .relm
        and byte [es:bx+311], 0FEh
        mov ax, [es:bx+2042]            ; mb_absx
        mov si, [es:bx+2044]            ; mb_absy
        mov byte [absmode], 1
.relm:  mov bx, si
        xor si, si
        ; a button pressed and released between two ticks: deliver the press first
        mov ch, cl
        or ch, [last_buttons]
        not ch
        and ch, [latch]                 ; pressed, but neither down now nor before
        jz .one
        push cx
        or cl, ch
        call deliver                    ; movement and the press
        pop cx
        xor ax, ax
        xor bx, bx
        xor dx, dx
        xor si, si
.one:   call deliver
.none:  pop es
        pop bp
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        mov byte [in_timer], 0
.x:     pop ax
        pop ds
        iret

; IRQ 12 (standard mode): assemble PS/2 packets coming from the 8042 aux port
Irq12:
        push ax
        push ds
        mov ax, DGROUP
        mov ds, ax
        cmp byte [ps2bios], 0
        je .direct
        pushf
        call far [old74]                ; the BIOS reads the packet and calls BiosCallback
        mov ax, DGROUP
        mov ds, ax
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push es
        cmp byte [bios_flag], 0
        je .x
        mov byte [bios_flag], 0
        mov cl, [bios_stat]
        movzx ax, byte [bios_x]
        test cl, 10h
        jz .xp
        or ah, 0FFh
.xp:    movzx bx, byte [bios_y]
        test cl, 20h
        jz .yp
        or bh, 0FFh
.yp:    neg bx
        and cl, 07h
        xor dx, dx
        xor si, si
        call deliver
.x:     pop es
        pop bp
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ds
        pop ax
        iret
.direct:
        pop ds
        pop ax

Irq12Direct:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push ds
        push es
        mov ax, DGROUP
        mov ds, ax
.more:  in al, 64h
        test al, 01h
        jz .eoi
        test al, 20h
        jz .eoi                         ; keyboard byte: leave it to the keyboard handler
        in al, 60h
        movzx bx, byte [pktlen]
        cmp bx, 0
        jne .store
        test al, 08h                    ; first byte always has bit 3 set
        jz .more
.store: mov [pkt+bx], al
        inc bl
        mov [pktlen], bl
        cmp bl, [pktneed]
        jb .more
        mov byte [pktlen], 0
        ; decode
        mov cl, [pkt]
        movzx ax, byte [pkt+1]
        test cl, 10h
        jz .xp
        or ah, 0FFh
.xp:    movzx bx, byte [pkt+2]
        test cl, 20h
        jz .yp
        or bh, 0FFh
.yp:    neg bx                          ; PS/2 Y grows upwards
        and cl, 07h                     ; PS/2: bit0 left, bit1 right, bit2 middle
        xor dx, dx
        xor si, si
        cmp byte [pktneed], 4
        jne .now
        mov al, [pkt+3]
        cmp byte [imex], 0
        jne .imex
        movsx dx, al
        neg dx                          ; positive = away from the user
        jmp .now
.imex:  ; IntelliMouse Explorer: bits 7-6 = 10b 6-bit wheel, 01b 6-bit tilt, else 4-bit wheel
        ; and buttons 4 (bit 4) and 5 (bit 5)
        mov ah, al
        and ah, 0C0h
        cmp ah, 80h
        je .v6
        cmp ah, 40h
        je .h6
        test al, 10h
        jz .nb4
        or cl, 08h
.nb4:   test al, 20h
        jz .nb5
        or cl, 10h
.nb5:   shl al, 4
        sar al, 4
        movsx dx, al
        neg dx
        jmp .now
.v6:    shl al, 2
        sar al, 2
        movsx dx, al
        neg dx
        jmp .now
.h6:    shl al, 2
        sar al, 2
        movsx si, al
        neg si                          ; positive = right
.now:   call deliver
        jmp .more
.eoi:   mov al, 20h
        out 0A0h, al
        out 20h, al
        pop es
        pop ds
        pop bp
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        iret
