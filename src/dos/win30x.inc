; win30x.inc - Windows 3.0 standard mode fix, shared by WIN30FIX.COM and the WIN*.SYS drivers.
; DOSX.EXE of Windows 3.0 (a 286 DOS extender, 24-bit addresses) takes the largest free XMS
; block; beyond 16 MB the machine resets or hangs on WIN /S. The XMS entry handed out by
; INT 2Fh AX=4310h is replaced; for DOSX and the programs it starts, function 08h (query
; free memory) reports only the part below 16 MB. Other programs see HIMEM unchanged.
; The including file provides the INT 2Fh hook (WIN30_INT2F) and the variables below.

win30xms          dd 0            ; the real XMS entry
win30enh          db 0            ; Windows enhanced mode runs: plain pass-through

; in the INT 2Fh handler, before anything else
%macro WIN30_INT2F 1              ; %1: old INT 2Fh vector
        cmp ax, 4310h
        je %%xms
        cmp ax, 1605h           ; Windows startup: enhanced mode (DX bit 0 clear) owns XMS
        je %%start
        cmp ax, 1606h
        je %%exit
        cmp ax, 0D6FFh          ; installation check: AX=FFFFh, ES = resident segment
        jne %%no
        mov ax, 0FFFFh
        push cs
        pop es
        iret
%%xms:  cmp byte [cs:win30enh], 0
        jne %%no
        pushf
        call far [cs:%1]
        mov [cs:win30xms], bx
        mov [cs:win30xms+2], es
        push cs
        pop es
        mov bx, win30entry
        iret
%%start:
        test dx, 1
        jnz %%no
        mov byte [cs:win30enh], 1
        jmp short %%no
%%exit: test dx, 1
        jnz %%no
        mov byte [cs:win30enh], 0
%%no:
%endmacro

; XMS entry: the first five bytes are the hookable header XMS clients expect
win30entry:
        jmp short .go
        nop
        nop
        nop
.go:
        cmp byte [cs:win30enh], 0
        jne .real
        cmp ah, 08h
        je .query
.real:  jmp far [cs:win30xms]

.query: call win30isdosx
        jne .real
        call far [cs:win30xms]
        test ax, ax
        jz .ret
        push bx
        push cx
        push si
        push di
        push bp
        mov si, ax              ; largest free block (KB)
        mov di, dx              ; total free (KB)
        ; where does the largest block start? allocate it, lock, free
        mov dx, si
        mov ah, 09h
        call far [cs:win30xms]
        test ax, ax
        jz .keep
        mov bp, dx              ; handle
        mov ah, 0Ch
        call far [cs:win30xms]
        test ax, ax
        jz .free
        ; DX:BX = linear address; KB below 16 MB = (1000000h - addr) / 1024
        cmp dx, 100h
        jae .none
        mov ax, 100h
        sub ax, dx              ; 64K units below 16 MB, minus the low part
        mov cx, 6
        shl ax, cl              ; * 64 = KB
        mov cx, bx
        shr cx, 10              ; KB of the low word (rounded down)
        sub ax, cx
        test bx, 3FFh
        jz .lim
        dec ax                  ; partial KB at the start does not count
.lim:   cmp si, ax
        jbe .unl
        mov si, ax
        jmp .unl
.none:  xor si, si
.unl:   mov dx, bp
        mov ah, 0Dh
        call far [cs:win30xms]
.free:  mov dx, bp
        mov ah, 0Ah
        call far [cs:win30xms]
.keep:  mov ax, si
        cmp di, si
        jbe .tot
        mov di, si
.tot:   mov dx, di
        xor bl, bl
        test ax, ax
        jnz .pop
        mov bl, 0A0h            ; all free memory is above 16 MB
.pop:   pop bp
        pop di
        pop si
        pop cx
        pop bx
        ; BL from the original call is kept unless set above
.ret:   retf

; ZF set when the current DOS process is DOSX.EXE or one it started (KRNL286): the MCB
; owner names of the process and its parents are checked
win30isdosx:
        push ax
        push bx
        push cx
        push es
        mov ah, 62h
        int 21h
        mov cx, 4
.l:     dec bx
        mov es, bx
        mov ax, [es:8]
        and ax, 0DFDFh          ; upper case (WIN.COM starts it as "dosx")
        cmp ax, 'DO'
        jne .n
        mov ax, [es:10]
        and ax, 0DFDFh
        cmp ax, 'SX'
        je .x
.n:     inc bx
        mov es, bx
        mov ax, [es:16h]        ; parent PSP
        cmp ax, bx
        je .no
        mov bx, ax
        loop .l
.no:    or cx, 1                ; ZF clear
.x:     pop es
        pop cx
        pop bx
        pop ax
        ret

