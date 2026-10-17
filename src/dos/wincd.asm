; wincd.asm - WINCD.SYS: DOS CD-ROM device driver (device name WINCD001) for the CD/DVD
; drives that WINDISK.SYS finds on IDE and AHCI controllers. MSCDEX gives them drive
; letters:  DEVICE=C:\WINDRV\WINCD.SYS  (after WINDISK.SYS)
;           MSCDEX.EXE /D:WINCD001      (AUTOEXEC.BAT)
; WINUCD.SYS (device WINUCD01) is the same for the USB CD/DVD drives of WINUSB.SYS:
;           DEVICE=C:\WINDRV\WINUCD.SYS [/N:n]  (after WINUSB.SYS)
;           MSCDEX.EXE /D:WINUCD01
; USB drives come and go: WINUCD01 has as many units as drives were found at boot, at
; least one (/N:n sets 1-8); a unit without a drive reports "drive not ready", a drive
; plugged in later takes the lowest free unit.
; Every request goes to the core through its INT 2Fh API (WINF_CDREQ), in DOS, Windows
; standard and 386 enhanced mode alike.
;
; Build: nasm -f bin -o WINCD.SYS wincd.asm
;        nasm -f bin -DPKG_USB -o WINUCD.SYS wincd.asm

        bits 16
        org 0

%ifdef PKG_USB
MPLEX           equ 0D4h        ; WINUSB.SYS
%define DEVNAME 'WINUCD01'
%define CORESYS 'WINUSB.SYS'
%define CDSYS   'WINUCD'
%else
MPLEX           equ 0D3h        ; WINDISK.SYS
%define DEVNAME 'WINCD001'
%define CORESYS 'WINDISK.SYS'
%define CDSYS   'WINCD'
%endif
WINF_INFO       equ 0001h
WINF_CDREQ      equ 0023h

hdr:    dd -1
        dw 0C800h               ; character device, IOCTL, open/close/removable media
        dw strategy
        dw interrupt
        db DEVNAME
        dw 0                    ; reserved
        db 0                    ; drive letter (set by MSCDEX)
units   db 0                    ; number of drives

reqptr  dd 0

; struct winapireq (32 bytes)
req:
req_fn          dw 0
req_status      dw 0
req_arg0        dd 0
req_arg1        dd 0
req_arg2        dd 0
req_buflen      dd 0
req_retlen      dd 0
req_buf         dd 0
req_rsv         dd 0

strategy:
        mov [cs:reqptr], bx
        mov [cs:reqptr+2], es
        retf

interrupt:
        pushf
        push ds
        push es
        push ax
        push bx
        push si
        les bx, [cs:reqptr]
        mov al, [es:bx+2]
        test al, al
        jnz .req
        jmp init
.req:
        cmp al, 3                       ; IOCTL input 0: address of the device header
        jne .core
        lds si, [es:bx+0Eh]
        cmp byte [si], 0
        jne .core
        mov word [si+1], hdr
        mov [si+3], cs
        mov word [es:bx+3], 0100h
        jmp .done
.core:
        ; the WINDISK core handles the request header and sets its status
        mov word [cs:req_fn], WINF_CDREQ
        mov word [cs:req_status], 0FFFFh
        mov word [cs:req_buflen], 30
        mov [cs:req_buf], bx
        mov [cs:req_buf+2], es
        push es
        push bx
        push cs
        pop es
        mov bx, req
        mov ax, (MPLEX << 8) | 01h
        int 2Fh
        pop bx
        pop es
        cmp word [cs:req_status], 0
        je .done
        mov word [es:bx+3], 8102h       ; error, done, drive not ready
.done:
        pop si
        pop bx
        pop ax
        pop es
        pop ds
        popf
        retf

resident_end:

; ---------------------------------------------------------------------------
; INIT: WINDISK.SYS must be loaded and have found CD/DVD drives
info    times 64 db 0

init:
        push cx
        push dx
        push di
        push cs
        pop ds
        mov dx, msg_banner
        call print
        mov ax, MPLEX << 8
        xor bx, bx
        push es
        int 2Fh
        pop es
        cmp al, 0FFh
        jne .nodisk
        cmp bx, 3357h
        jne .nodisk
        ; number of CD/DVD drives: wininfo.ncd
        mov word [req_fn], WINF_INFO
        mov word [req_status], 0FFFFh
        mov word [req_buflen], 64
        mov word [req_buf], info
        mov [req_buf+2], cs
        push es
        push cs
        pop es
        mov bx, req
        mov ax, (MPLEX << 8) | 01h
        int 2Fh
        pop es
        cmp word [req_status], 0
        jne .nodisk
        mov al, [info+13]
%ifdef PKG_USB
        call parse_n                    ; /N:n
        test ah, ah
        jz .havecnt
        mov al, ah
.havecnt:
        test al, al
        jnz .setcnt
        mov al, 1                       ; one unit for a drive plugged in later
.setcnt:
%else
        test al, al
        jz .nocd
%endif
        mov [units], al
        add al, '0'
        mov [msg_n], al
        mov dx, msg_ok
        call print
        les bx, [reqptr]
        mov byte [es:bx+0Dh], 0
        mov word [es:bx+0Eh], resident_end
        mov [es:bx+10h], cs
        mov word [es:bx+3], 0100h
        jmp .ret
.nodisk:
        mov dx, msg_nodisk
        call print
        jmp .fail
%ifndef PKG_USB
.nocd:
        mov dx, msg_nocd
        call print
%endif
.fail:
        les bx, [reqptr]
        mov byte [es:bx+0Dh], 0
        mov word [es:bx+0Eh], 0
        mov [es:bx+10h], cs
        mov word [es:bx+3], 810Ch
.ret:
        pop di
        pop dx
        pop cx
        jmp interrupt.done

%ifdef PKG_USB
; AH = n of /N:n on the DEVICE= line (1-8), 0 without it
parse_n:
        push ds
        push si
        les bx, [cs:reqptr]
        lds si, [es:bx+12h]
        xor ah, ah
.next:  lodsb
        cmp al, 13
        je .end
        cmp al, 10
        je .end
        test al, al
        jz .end
        cmp al, '/'
        jne .next
        lodsb
        and al, 0DFh
        cmp al, 'N'
        jne .next
        lodsb
        cmp al, ':'
        jne .next
        lodsb
        sub al, '0'
        cmp al, 1
        jb .next
        cmp al, 8
        ja .next
        mov ah, al
.end:   pop si
        pop ds
        mov al, [cs:info+13]
        ret
%endif

print:  push ax
        mov ah, 09h
        int 21h
        pop ax
        ret

%ifdef PKG_USB
msg_banner      db 'WINUCD 1.0 - USB CD/DVD drives of WINUSB for MSCDEX', 13, 10, '$'
%else
msg_banner      db 'WINCD 1.0 - CD/DVD drives of WINDISK for MSCDEX', 13, 10, '$'
%endif
msg_ok          db CDSYS, ': device ', DEVNAME, ', '
msg_n           db '0 unit(s)', 13, 10, '$'
msg_nodisk      db CDSYS, ': ', CORESYS, ' is not loaded (it must come first in CONFIG.SYS)', 13, 10, '$'
%ifndef PKG_USB
msg_nocd        db CDSYS, ': no CD/DVD drive found, not installed', 13, 10, '$'
%endif
