; win30fix.asm - WIN30FIX.COM: lets Windows 3.0 start in standard mode on machines with more
; than 16 MB of memory.
;
; The 286 DOS extender of Windows 3.0 (DOSX.EXE) works with 24-bit addresses. It takes the
; largest free XMS block, which on today's machines reaches beyond 16 MB, and the computer
; resets or hangs on WIN /S. WIN30FIX hooks the XMS entry point (INT 2Fh AX=4310h) and, when
; the caller is DOSX, reports only the part of the free memory that lies below 16 MB.
; Every other program sees the XMS driver unchanged.
;
; Build: nasm -f bin -o WIN30FIX.COM win30fix.asm
; Use:   WIN30FIX      (AUTOEXEC.BAT, after HIMEM.SYS is loaded; about 400 bytes resident)
;        WIN30FIX /U   remove it again (if nothing hooked INT 2Fh after it)

        bits 16
        org 100h

start:  jmp init

; ---------------------------------------------------------------------------
; resident part
old2f           dd 0
sig             db 'WIN30FIX'

%include "win30x.inc"

int2f:
        WIN30_INT2F old2f
        jmp far [cs:old2f]

resident_end:

; ---------------------------------------------------------------------------
init:
        ; /U: uninstall
        mov si, 81h
.sk:    lodsb
        cmp al, ' '
        je .sk
        cmp al, '/'
        jne .inst
        lodsb
        or al, 20h
        cmp al, 'u'
        je uninstall
        mov dx, msg_usage
        jmp exit_msg
.inst:
        mov ax, 0D6FFh
        int 2Fh
        cmp ax, 0FFFFh
        jne .new
        mov dx, msg_already
        jmp exit_msg
.new:   mov ax, 4300h
        int 2Fh
        cmp al, 80h
        je .ok
        mov dx, msg_noxms
        jmp exit_msg
.ok:    mov ax, 352Fh
        int 21h
        mov [old2f], bx
        mov [old2f+2], es
        mov ax, 4310h
        int 2Fh
        mov [win30xms], bx
        mov [win30xms+2], es
        mov ax, 252Fh
        mov dx, int2f
        int 21h
        ; free the environment
        mov es, [2Ch]
        mov ah, 49h
        int 21h
        mov dx, msg_ok
        mov ah, 09h
        int 21h
        mov dx, (resident_end - start + 100h + 15) / 16
        mov ax, 3100h
        int 21h

uninstall:
        mov ax, 0D6FFh
        int 2Fh
        cmp ax, 0FFFFh
        jne .not
        mov [ressg], es
        mov ax, 352Fh
        int 21h
        mov ax, es
        cmp ax, [ressg]
        jne .busy
        cmp bx, int2f
        jne .busy
        push ds
        lds dx, [es:old2f]
        mov ax, 252Fh
        int 21h
        pop ds
        mov ah, 49h             ; ES = resident copy
        int 21h
        mov dx, msg_removed
        jmp exit_msg
.busy:  mov dx, msg_busy
        jmp exit_msg
.not:   mov dx, msg_notin
exit_msg:
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h

ressg           dw 0
msg_ok          db 'WIN30FIX: Windows 3.0 standard mode limited to the memory below 16 MB', 13, 10, '$'
msg_already     db 'WIN30FIX is already loaded', 13, 10, '$'
msg_noxms       db 'WIN30FIX: no XMS driver (HIMEM.SYS) loaded', 13, 10, '$'
msg_removed     db 'WIN30FIX removed', 13, 10, '$'
msg_busy        db 'WIN30FIX: another program hooked INT 2Fh after it; cannot remove', 13, 10, '$'
msg_notin       db 'WIN30FIX is not loaded', 13, 10, '$'
msg_usage       db 'Usage: WIN30FIX [/U]', 13, 10, '$'
