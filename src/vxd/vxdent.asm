; vxdent.asm - WINCORE.386 VxD entry: DDB, control dispatch, callback thunks
; and VMM/VxD service wrappers callable from C (cdecl).
        bits 32

%include "vxdsvc.inc"

%ifndef PKG_USB
%ifndef PKG_AUDIO
%define PKG_DISK
%endif
%endif

%macro VMMCALL 1
        int 20h
        dd %1
%endmacro

segment _LTEXT public class=CODE use32 align=16
segment _LDATA public class=CODE use32 align=16

extern _vxd_control
extern _vxd_v86api
extern _vxd_pmapi
extern _vxd_timer
extern _vxd_event
extern _vxd_hwint
extern _vxd_vmevent
%ifdef PKG_DISK
extern _vxd_int4b
extern _vxd_vdsret
extern _vxd_bdcmd
%endif
%ifndef PKG_AUDIO
extern _vxd_int13
%endif

; ---------------------------------------------------------------------------
segment _LDATA
global WINCORE_DDB
WINCORE_DDB:
        dd 0                    ; next
%ifdef WIN9X
        dw 0400h                ; SDK version: Windows 95 DDB layout (dynamic VxD)
%else
        dw 030Ah                ; SDK version
%endif
%ifdef PKG_USB
        dw 03D34h               ; device id
%elifdef PKG_AUDIO
        dw 03D35h
%else
        dw 03D33h
%endif
        db 1, 0                 ; version 1.0
        dw 0                    ; flags
%ifdef PKG_USB
        db 'WINUSB  '            ; name
%elifdef PKG_AUDIO
        db 'WINAUDIO'
%else
        db 'WINDISK '
%endif
%ifdef WIN9X
%ifndef PKG_AUDIO
        dd 015000000h           ; init order: after VTD, before VXDLDR, CONFIGMG and IOS (static load)
%else
        dd 0A0000000h           ; init order: after VPICD/VTD/VKD
%endif
%else
        dd 0A0000000h           ; init order: after VPICD/VTD/VKD
%endif
        dd Control
        dd V86Api
        dd PMApi
        dd 0, 0                 ; V86/PM API CS:IP
        dd 0                    ; reference data
%ifndef PKG_AUDIO
        dd svc_table            ; services for WINUSB.PDR / WINDISK.PDR (Windows 95/98/ME)
        dd svc_count
%else
        dd 0, 0
%endif
        dd 0                    ; Win32 services
        dd 0x50726576           ; 'Prev' as the MASM constant
        dd 80                   ; DDB size (Windows 95 layout)
        dd 0x52737631, 0x52737632, 0x52737633   ; 'Rsv1' 'Rsv2' 'Rsv3'

%ifndef PKG_AUDIO
extern _svc_get_version
extern _svc_blk_lookup
extern _svc_blk_io
extern _svc_blk_status
extern _svc_cd_media
extern _svc_cd_command
svc_table:
        dd _svc_get_version     ; 0: u32 (void) -> version
        dd _svc_blk_lookup      ; 1: int (u32 devnode, struct pdrinfo *out)
        dd _svc_blk_io          ; 2: int (u32 h, u32 lba_lo, u32 lba_hi, u32 count, void *buf, u32 write)
        dd _svc_blk_status      ; 3: u32 (u32 h) -> PDS_*
        dd _svc_cd_media        ; 4: int (u32 h) -> bit 0 medium, bits 8-15 medium changes
        dd _svc_cd_command      ; 5: int (u32 h, const u8 *cdb, void *buf, u32 len, u8 *sense)
svc_count equ ($ - svc_table) / 4
%endif

global _vxd_refdata
_vxd_refdata    dd 0

; uninitialized data: VMM does not clear these pages when Windows is started again,
; Sys_Critical_Init zeroes everything between the two labels. The C data segments are
; declared here first so that DGROUP keeps the order CONST, CONST2, _DATA, _BSS, ZZBSS.
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

; ---------------------------------------------------------------------------
segment _LTEXT

Control:
        pushad
        cld                     ; C code assumes DF = 0
        push edi
        push esi
        push edx
        push ebx
        push eax
        call _vxd_control
        add esp, 20
        cmp dword [esp+28], 22h ; PNP_New_DevNode: EAX = CONFIGRET, carry when not CR_SUCCESS
        jne .plain
        mov [esp+28], eax
        test eax, eax
        popad
        jnz .fail
        clc
        ret
.fail:  stc
        ret
.plain: bt eax, 0               ; return 1 -> carry set (fail)
        popad
        ret

V86Api:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_v86api
        add esp, 8
        popad
        ret

; Allocate_V86_Call_Back target: the callback must return to the caller itself
global _thk_v86api
_thk_v86api:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_v86api
        add esp, 8
        popad
        VMMCALL SVC_Simulate_Far_Ret
        ret

PMApi:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_pmapi
        add esp, 8
        popad
        ret

global _thk_timer
_thk_timer:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_timer
        add esp, 8
        popad
        ret

extern _vxd_pnpbatch
global _thk_pnpbatch
_thk_pnpbatch:
        pushad
        cld
        push ebp
        push ebx
        call _vxd_pnpbatch
        add esp, 8
        popad
        ret

global _thk_event
_thk_event:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_event
        add esp, 8
        popad
        ret

global _thk_vmevent
_thk_vmevent:
        pushad
        cld                     ; C code assumes DF = 0
        push ebp
        push ebx
        call _vxd_vmevent
        add esp, 8
        popad
        ret

; VPICD hardware interrupt: EAX = IRQ handle, EBX = VM. CF set = not ours.
global _thk_hwint
_thk_hwint:
        pushad
        cld                     ; C code assumes DF = 0
        push eax
        call _vxd_hwint
        add esp, 4
        bt eax, 0
        popad
        ret

%ifdef PKG_USB
; V86 interrupt chain hook (INT 15h): EBX = VM, EBP -> client registers.
; C returns 0 when it served the call, 1: the next handler runs (CF set).
extern _vxd_int15
global _thk_int15
_thk_int15:
        pushad
        cld
        push ebp
        push ebx
        call _vxd_int15
        add esp, 8
        bt eax, 0
        popad
        ret
%endif

%ifndef PKG_AUDIO
; V86 interrupt chain hook (INT 13h): same convention, C returns 0 when it served the call
global _thk_int13
_thk_int13:
        pushad
        cld
        push ebp
        push ebx
        call _vxd_int13
        add esp, 8
        bt eax, 0
        popad
        ret

%endif

%ifdef PKG_DISK
; V86 interrupt chain hook (INT 4Bh): EBX = VM, EBP -> client registers.
; C returns 1: not consumed, the next handler runs (CF set).
global _thk_int4b
_thk_int4b:
        pushad
        cld
        push ebp
        push ebx
        call _vxd_int4b
        add esp, 8
        bt eax, 0
        popad
        ret

; V86 callback: return point of a VDS call that needed fixing
global _thk_vdsret
_thk_vdsret:
        pushad
        cld
        push ebp
        push ebx
        call _vxd_vdsret
        add esp, 8
        popad
        ret

; BlockDev command entry: EDI -> BDD, ESI -> command block (interrupts disabled)
global _thk_bdcmd
_thk_bdcmd:
        pushad
        cld
        push esi
        push edi
        call _vxd_bdcmd
        add esp, 8
        popad
        ret

; BlockDev synchronous commands: only BD_SC_Get_Version
global _thk_bdsync
_thk_bdsync:
        test ax, ax
        jnz .bad
        mov ax, 0100h
        clc
        ret
.bad:   mov ax, 1
        stc
        ret

; u32 blockdev_register(void *bdd) -> 0 ok
global _blockdev_register
_blockdev_register:
        pushad
        mov edi, [esp+36]
        VMMCALL SVC_BlockDev_Register_Device
        popad
        sbb eax, eax
        ret

; void blockdev_complete(void *bdd, void *cmd)
global _blockdev_complete
_blockdev_complete:
        pushad
        mov edi, [esp+36]
        mov esi, [esp+40]
        VMMCALL SVC_BlockDev_Command_Complete
        popad
        ret
%endif

%ifndef PKG_AUDIO
; _CONFIGMG_Register_Device_Driver(devnode, handler, refdata, flags): the I/O Supervisor's
; registration for the disks of BIOS drives goes through pnp_regdrv_hook (pnpblk.c).
; HOOK_PROC layout: jmp short over "jmp [previous]".
extern _pnp_regdrv_hook
global _thk_cmregdrv
_thk_cmregdrv:
        jmp short .go
        jmp [cmregdrv_prev]
.go:    lea eax, [esp+4]
        push eax
        call _pnp_regdrv_hook   ; may change the arguments in place
        add esp, 4
        jmp [cmregdrv_prev]

; u32 vmm_hook_cm_regdrv(void) -> 0 ok
global _vmm_hook_cm_regdrv
_vmm_hook_cm_regdrv:
        push esi
        mov eax, 0033000Eh      ; _CONFIGMG_Register_Device_Driver
        mov esi, _thk_cmregdrv
        VMMCALL SVC_Hook_Device_Service
        jc .fail
        mov [cmregdrv_prev], esi
        xor eax, eax
        pop esi
        ret
.fail:  mov eax, 1
        pop esi
        ret

segment _LDATA
cmregdrv_prev   dd 0
segment _LTEXT
%endif

; u32 vmm_hook_v86_int(u32 n, void *f) -> 0 ok
global _vmm_hook_v86_int
_vmm_hook_v86_int:
        push esi
        mov eax, [esp+8]
        mov esi, [esp+12]
        VMMCALL SVC_Hook_V86_Int_Chain
        sbb eax, eax
        pop esi
        ret

; u32 vmm_profile_bool(const char *key, u32 def): [386Enh] key=on/off
global _vmm_profile_bool
_vmm_profile_bool:
        push esi
        push edi
        mov edi, [esp+12]
        mov eax, [esp+16]
        xor esi, esi
        VMMCALL SVC_Get_Profile_Boolean
        pop edi
        pop esi
        ret

; u64 cpu_rdtsc64(void)
global _cpu_rdtsc64
_cpu_rdtsc64:
        rdtsc
        ret

; ---------------------------------------------------------------------------
; service wrappers (cdecl, preserve ebx esi edi ebp)

global _vmm_sys_vm
_vmm_sys_vm:
        push ebx
        VMMCALL SVC_Get_Sys_VM_Handle
        mov eax, ebx
        pop ebx
        ret

global _vmm_cur_vm
_vmm_cur_vm:
        push ebx
        VMMCALL SVC_Get_Cur_VM_Handle
        mov eax, ebx
        pop ebx
        ret

; u32 vmm_version(void): 0300h Windows 3.0, 030Ah Windows 3.1
global _vmm_version
_vmm_version:
        push ecx
        push esi
        VMMCALL SVC_Get_VMM_Version
        movzx eax, ax
        pop esi
        pop ecx
        ret

; u32 vmm_init_state(void): VMM_GetSystemInitState of Windows 95/98/ME (VMM 4.0)
global _vmm_init_state
_vmm_init_state:
        push ecx
        push edx
        VMMCALL 00010111h       ; VMM_GetSystemInitState
        pop edx
        pop ecx
        ret

global _vmm_system_time
_vmm_system_time:
        VMMCALL SVC_Get_System_Time
        ret

; u32 vmm_page_alloc(u32 npages, u32 alignmask, u32 maxphyspg, u32 flags, u32 *phys, u32 *lin)
global _vmm_page_alloc
_vmm_page_alloc:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword [ebp+20]     ; flags
        push dword [ebp+24]     ; PhysAddr ptr
        push dword [ebp+16]     ; maxPhys
        push dword 0            ; minPhys
        push dword [ebp+12]     ; AlignMask
        push dword 0            ; VM
        push dword 1            ; PG_SYS
        push dword [ebp+8]      ; nPages
        VMMCALL SVC__PageAllocate
        add esp, 32
        mov ecx, [ebp+28]
        mov [ecx], edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 vmm_map_phys(u32 phys, u32 len)
global _vmm_map_phys
_vmm_map_phys:
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

; const char *vmm_exec_path(u32 *dirlen): full path of the VMM (WIN386.EXE / VMM32.VXD),
; dirlen = characters before its file name
global _vmm_exec_path
_vmm_exec_path:
        push ebx
        VMMCALL SVC_Get_Exec_Path
        mov eax, [esp+8]
        mov [eax], ecx
        mov eax, edx
        pop ebx
        ret

; u32 ifs_r0io(u32 func, u32 ebx, u32 ecx, u32 edx, void *esi, u32 *err): IFSMgr_Ring0_FileIO
; (Windows 95/98/ME); err = 1 when the call failed (carry)
global _ifs_r0io
_ifs_r0io:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov eax, [ebp+8]
        mov ebx, [ebp+12]
        mov ecx, [ebp+16]
        mov edx, [ebp+20]
        mov esi, [ebp+24]
        int 20h
        dd 00400032h            ; IFSMgr_Ring0_FileIO
        mov ecx, [ebp+28]
        mov dword [ecx], 0
        jnc .ok
        mov dword [ecx], 1
.ok:    pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 vmm_copy_page_table(u32 linpg, u32 npages, u32 *buf, u32 flags): _CopyPageTable
global _vmm_copy_page_table
_vmm_copy_page_table:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword [ebp+20]
        push dword [ebp+16]
        push dword [ebp+12]
        push dword [ebp+8]
        VMMCALL SVC__CopyPageTable
        add esp, 16
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; const char *vmm_profile_string(const char *section, const char *key, const char *def):
; SYSTEM.INI [section] key=; a NULL section means [386Enh]. Returns the value or def.
global _vmm_profile_string
_vmm_profile_string:
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

; u32 vmm_heap_alloc(u32 size)
global _vmm_heap_alloc
_vmm_heap_alloc:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword 1            ; HeapZeroInit
        push dword [ebp+8]
        VMMCALL SVC__HeapAllocate
        add esp, 8
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 vmm_alloc_v86_cb(void *func, u32 ref)  -> seg:off or 0
global _vmm_alloc_v86_cb
_vmm_alloc_v86_cb:
        push ebx
        push esi
        push edi
        mov esi, [esp+16]
        mov edx, [esp+20]
        VMMCALL SVC_Allocate_V86_Call_Back
        jnc .ok
        xor eax, eax
.ok:    pop edi
        pop esi
        pop ebx
        ret

; u32 vmm_set_timeout(u32 ms, void *func, u32 ref) -> handle
global _vmm_set_timeout
_vmm_set_timeout:
        push ebx
        push esi
        push edi
        mov eax, [esp+16]
        mov esi, [esp+20]
        mov edx, [esp+24]
        VMMCALL SVC_Set_Global_Time_Out
        mov eax, esi
        pop edi
        pop esi
        pop ebx
        ret

; void vmm_cancel_timeout(u32 h)
global _vmm_cancel_timeout
_vmm_cancel_timeout:
        push esi
        mov esi, [esp+8]
        VMMCALL SVC_Cancel_Time_Out
        pop esi
        ret

; u32 vmm_global_event(void *func, u32 ref)
global _vmm_global_event
_vmm_global_event:
        push ebx
        push esi
        push edi
        mov esi, [esp+16]
        mov edx, [esp+20]
        VMMCALL SVC_Schedule_Global_Event
        mov eax, esi
        pop edi
        pop esi
        pop ebx
        ret

; u32 vmm_priority_event(u32 boost, u32 vm, u32 flags, u32 ref, void *func, u32 timeout)
global _vmm_priority_event
_vmm_priority_event:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov eax, [ebp+8]
        mov ebx, [ebp+12]
        mov ecx, [ebp+16]
        mov edx, [ebp+20]
        mov esi, [ebp+24]
        mov edi, [ebp+28]
        VMMCALL SVC_Call_Priority_VM_Event
        mov eax, esi
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; void vmm_heap_free(void *p)
global _vmm_heap_free
_vmm_heap_free:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword 0
        push dword [ebp+8]
        VMMCALL SVC__HeapFree
        add esp, 8
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; Configuration Manager (Windows 95/98/ME) services: C functions, arguments copied through
%macro CMWRAP 3
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
        dd (0x33 << 16) | %2
        add esp, 4*%3
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
%endmacro

CMWRAP _cm_get_version, 0, 0
CMWRAP _cm_locate_devnode, 2, 3
CMWRAP _cm_get_parent, 3, 3
CMWRAP _cm_get_child, 4, 3
CMWRAP _cm_get_sibling, 5, 3
CMWRAP _cm_get_device_id, 7, 4
CMWRAP _cm_create_devnode, 11, 4
CMWRAP _cm_remove_subtree, 13, 2
CMWRAP _cm_register_device_driver, 14, 4
CMWRAP _cm_register_enumerator, 15, 3
CMWRAP _cm_call_at_appy_time, 78, 3
CMWRAP _cm_get_devnode_status, 54, 4
CMWRAP _cm_get_alloc_log_conf, 59, 3
CMWRAP _cm_read_registry_value, 62, 7
CMWRAP _cm_write_registry_value, 63, 7
CMWRAP _cm_set_private_problem, 80, 3

; CM_Call_At_Appy_Time callback: C function with the reference data on the stack
extern _vxd_appy
global _thk_appy
_thk_appy:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        cld
        push dword [ebp+8]
        call _vxd_appy
        add esp, 4
        xor eax, eax            ; CR_SUCCESS
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; CONFIGMG calls the enumerator handler as a C function
extern _vxd_cmenum
global _thk_cmenum
_thk_cmenum:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        cld
        push dword [ebp+24]
        push dword [ebp+20]
        push dword [ebp+16]
        push dword [ebp+12]
        push dword [ebp+8]
        call _vxd_cmenum
        add esp, 20
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; CONFIGMG calls the configuration handler as a C function
extern _vxd_cmconfig
global _thk_cmconfig
_thk_cmconfig:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        cld
        push dword [ebp+24]
        push dword [ebp+20]
        push dword [ebp+16]
        push dword [ebp+12]
        push dword [ebp+8]
        call _vxd_cmconfig
        add esp, 20
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; nested execution helpers: all take the client register pointer (crs) last
%macro NESTWRAP 2
global %1
%1:
        push ebp
        push ebx
        mov ebp, [esp+12]
        VMMCALL SVC_Get_Cur_VM_Handle
        VMMCALL %2
        pop ebx
        pop ebp
        ret
%endmacro

NESTWRAP _vmm_begin_nest, SVC_Begin_Nest_Exec
NESTWRAP _vmm_begin_nest_v86, SVC_Begin_Nest_V86_Exec
NESTWRAP _vmm_end_nest, SVC_End_Nest_Exec
NESTWRAP _vmm_resume_exec, SVC_Resume_Exec

; void vmm_far_call(u32 seg, u32 off, void *crs)
global _vmm_far_call
_vmm_far_call:
        push ebp
        push ebx
        mov ecx, [esp+12]
        mov edx, [esp+16]
        mov ebp, [esp+20]
        VMMCALL SVC_Get_Cur_VM_Handle
        VMMCALL SVC_Simulate_Far_Call
        pop ebx
        pop ebp
        ret

; void vmm_exec_int(u32 n, void *crs)
global _vmm_exec_int
_vmm_exec_int:
        push ebp
        push ebx
        mov eax, [esp+12]
        mov ebp, [esp+16]
        VMMCALL SVC_Get_Cur_VM_Handle
        VMMCALL SVC_Exec_Int
        pop ebx
        pop ebp
        ret

; void vmm_save_client(void *buf, void *crs) / vmm_restore_client(void *buf, void *crs)
global _vmm_save_client
_vmm_save_client:
        push ebp
        push edi
        mov edi, [esp+12]
        mov ebp, [esp+16]
        VMMCALL SVC_Save_Client_State
        pop edi
        pop ebp
        ret

global _vmm_restore_client
_vmm_restore_client:
        push ebp
        push esi
        mov esi, [esp+12]
        mov ebp, [esp+16]
        VMMCALL SVC_Restore_Client_State
        pop esi
        pop ebp
        ret

; void vmm_begin_crit(void) / vmm_end_crit(void)
global _vmm_begin_crit
_vmm_begin_crit:
        push ecx
        mov ecx, 1              ; Block_Svc_Ints
        VMMCALL SVC_Begin_Critical_Section
        pop ecx
        ret

global _vmm_end_crit
_vmm_end_crit:
        VMMCALL SVC_End_Critical_Section
        ret

; u32 vpicd_virtualize(void *vid) -> handle or 0
global _vpicd_virtualize
_vpicd_virtualize:
        push edi
        mov edi, [esp+8]
        VMMCALL SVC_VPICD_Virtualize_IRQ
        jnc .ok
        xor eax, eax
.ok:    pop edi
        ret

global _vpicd_force_default
_vpicd_force_default:
        mov eax, [esp+4]
        VMMCALL SVC_VPICD_Force_Default_Behavior
        ret

global _vpicd_phys_eoi
_vpicd_phys_eoi:
        mov eax, [esp+4]
        VMMCALL SVC_VPICD_Phys_EOI
        ret

global _vpicd_unmask
_vpicd_unmask:
        mov eax, [esp+4]
        VMMCALL SVC_VPICD_Physically_Unmask
        ret

global _vpicd_mask
_vpicd_mask:
        mov eax, [esp+4]
        VMMCALL SVC_VPICD_Physically_Mask
        ret

; int vkd_force_keys(u8 *buf, u32 n) -> number of keys not accepted
global _vkd_force_keys
_vkd_force_keys:
        push esi
        mov esi, [esp+8]
        mov ecx, [esp+12]
        VMMCALL SVC_VKD_Force_Keys
        jc .full
        xor ecx, ecx
.full:  mov eax, ecx
        pop esi
        ret

; u32 vkd_kbd_owner(void)
global _vkd_kbd_owner
_vkd_kbd_owner:
        push ebx
        VMMCALL SVC_VKD_Get_Kbd_Owner
        mov eax, ebx
        pop ebx
        ret

; void vtd_min_period(u32 ms)
global _vtd_min_period
_vtd_min_period:
        mov eax, [esp+4]
        VMMCALL SVC_VTD_Begin_Min_Int_Period
        ret

; u64-free cpu helpers
global _cpu_rdtsc_lo
_cpu_rdtsc_lo:
        rdtsc
        ret
