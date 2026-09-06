; INT13 EDD shim for a 1.44 MB floppy. Limine stage 1/2 only talk AH=41/42/48
; and only scan BIOS drives 0x80–0xEF, so a real floppy (DL=00h) is invisible.
; This lives at 0x600 (below Limine's conv_mem_alloc base of 0x1000).
; It emulates EDD on the boot floppy and on alias 0xEF (the scan range).
;
; Loaded from gap LBA 63 by a 19-byte MBR stub; first instruction is `install`.
; CHS reads restore the original IVT, STI (INT13 enters with IF=0; IRQ6 must
; fire), and far-call SeaBIOS so we are not nested in INT13.

	bits 16
	org 0x600

ALIAS		equ 0xEF
SPT		equ 18
HEADS		equ 2
NSECTORS	equ 2880

install:
	cmp byte [cs:installed], 1
	je .done
	cli
	push ds
	xor ax, ax
	mov ds, ax
	mov [boot_dl], dl
	mov ax, [0x4C]
	mov [old_off], ax
	mov ax, [0x4E]
	mov [old_seg], ax
	; Refuse to save ourselves as the original vector.
	cmp ax, 0
	jne .okvec
	cmp word [old_off], irq13
	je .skipivt
	cmp word [old_off], install
	je .skipivt
.okvec:
	mov word [0x4C], irq13
	mov word [0x4E], 0
	mov byte [installed], 1
.skipivt:
	pop ds
	sti
.done:
	ret

irq13:
	cmp dl, [cs:boot_dl]
	je .ours
	cmp dl, ALIAS
	je .ours
.chain:
	jmp far [cs:old_off]
.ours:
	cmp ah, 0x41
	je do_41
	cmp ah, 0x42
	je do_42
	cmp ah, 0x48
	je do_48
	cmp ah, 0x08
	je do_08
	cmp ah, 0x00
	je do_00
	jmp .chain

unhook:
	push ax
	push ds
	xor ax, ax
	mov ds, ax
	mov ax, [cs:old_off]
	mov [0x4C], ax
	mov ax, [cs:old_seg]
	mov [0x4E], ax
	pop ds
	pop ax
	ret

rehook:
	push ax
	push ds
	xor ax, ax
	mov ds, ax
	mov word [0x4C], irq13
	mov word [0x4E], 0
	pop ds
	pop ax
	ret

; IF=1 so IRQ6 can complete AH=02. Far-call the original BIOS.
bios_read:
	call unhook
	sti
	pushf
	call far [cs:old_off]
	pushf
	cli
	call rehook
	popf
	ret

iret_ok:
	xor ah, ah
	push bp
	mov bp, sp
	and byte [bp + 6], 0xFE
	pop bp
	iret

iret_fail:
	push bp
	mov bp, sp
	or byte [bp + 6], 0x01
	pop bp
	iret

do_41:
	mov bx, 0xAA55
	mov cx, 0x0001
	mov ah, 0x21
	push bp
	mov bp, sp
	and byte [bp + 6], 0xFE
	pop bp
	iret

do_00:
	push ax
	mov ax, 0x0000
	mov dl, [cs:boot_dl]
	call bios_read
	pop ax
	jmp iret_ok

do_08:
	mov ah, 0
	mov bl, 0x04
	mov cx, 0x4F12
	mov dh, 0x01
	mov dl, 0x01
	jmp iret_ok

do_48:
	pusha
	cmp word [si], 26
	jb .bad
	mov word [si], 26
	mov word [si + 2], 0x0007
	mov dword [si + 4], 80
	mov dword [si + 8], 2
	mov dword [si + 12], 18
	mov dword [si + 16], NSECTORS
	mov dword [si + 20], 0
	mov word [si + 24], 512
	popa
	jmp iret_ok
.bad:
	popa
	jmp iret_fail

do_42:
	pusha
	push ds
	push es
	mov ax, [si + 2]
	mov [cs:remain], ax
	mov ax, [si + 4]
	mov [cs:buf_off], ax
	mov ax, [si + 6]
	mov [cs:buf_seg], ax
	mov ax, [si + 8]
	mov [cs:lba], ax
.next:
	cmp word [cs:remain], 0
	je .ok
	cmp word [cs:lba], NSECTORS
	jae .fail
	mov ax, [cs:lba]
	xor dx, dx
	mov cx, SPT
	div cx
	inc dx
	push dx
	xor dx, dx
	mov cx, HEADS
	div cx
	mov ch, al
	mov dh, dl
	pop ax
	mov cl, al
	mov es, [cs:buf_seg]
	mov bx, [cs:buf_off]
	mov byte [cs:tries], 4
.retry:
	mov ax, 0x0201
	mov dl, [cs:boot_dl]
	call bios_read
	jnc .got
	mov ax, 0x0000
	mov dl, [cs:boot_dl]
	call bios_read
	dec byte [cs:tries]
	jnz .retry
	jmp .fail
.got:
	add word [cs:buf_off], 512
	jnc .nowrap
	add word [cs:buf_seg], (512 / 16)
.nowrap:
	inc word [cs:lba]
	dec word [cs:remain]
	jmp .next
.ok:
	pop es
	pop ds
	popa
	jmp iret_ok
.fail:
	pop es
	pop ds
	popa
	jmp iret_fail

	align 2
old_off:	dw 0
old_seg:	dw 0
boot_dl:	db 0
tries:		db 0
installed:	db 0
remain:		dw 0
buf_off:	dw 0
buf_seg:	dw 0
lba:		dw 0
		db "AOSI13"
