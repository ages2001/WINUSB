; winsndlb.asm - WINSOUND.DRV library entry and WEP
        bits 16

segment _TEXT public class=CODE align=16

; Windows calls the entry of a library once: DI = instance, DS = data segment,
; CX = heap size, ES:SI = command line. AX <> 0: loaded.
..start:
global LIBENTRY
LIBENTRY:
        mov ax, 1
        retf

global WEP
WEP:
        mov ax, 1
        retf 2
