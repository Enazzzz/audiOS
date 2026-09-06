; 16-bit floppy boot for the audiOS i386 slave (A7V333).
; Loads the 32-bit kernel from LBA 1 into 0x10000 and enters protected mode.

	BITS 16
	ORG 0x7C00

start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7C00
	mov [boot_drive], dl
	sti

	; Fast A20 (port 0x92). Harmless if already on.
	in al, 0x92
	or al, 2
	out 0x92, al

	mov ax, 0x1000
	mov es, ax
	xor bx, bx
	mov cx, 1			; LBA 1 = first kernel sector
	mov si, [k_sectors]

.load:
	cmp si, 0
	je .pm
	push cx
	push si
	; CHS: 18 spt, 2 heads (1.44 MB).
	mov ax, cx
	xor dx, dx
	mov di, 18
	div di				; ax = temp, dx = s-1
	inc dx
	mov cl, dl			; sector
	xor dx, dx
	mov di, 2
	div di				; ax = cyl, dx = head
	mov ch, al
	mov dh, dl
	mov dl, [boot_drive]
	mov ax, 0x0201
	int 0x13
	jc .fail
	pop si
	pop cx
	add bx, 512
	jnc .same_seg
	mov ax, es
	add ax, 0x1000
	mov es, ax
	xor bx, bx
.same_seg:
	inc cx
	dec si
	jmp .load

.fail:
	mov si, err
.p:
	lodsb
	test al, al
	jz .h
	mov ah, 0x0E
	int 0x10
	jmp .p
.h:
	jmp .h

.pm:
	cli
	lgdt [gdt_desc]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	jmp dword 0x08:0x10000

err:
	db "slave load", 0
boot_drive:
	db 0

	ALIGN 4
gdt:
	dq 0
	dw 0xFFFF, 0x0000
	db 0x00, 10011010b, 11001111b, 0x00
	dw 0xFFFF, 0x0000
	db 0x00, 10010010b, 11001111b, 0x00
gdt_desc:
	dw gdt_desc - gdt - 1
	dd gdt

	times 507 - ($ - $$) db 0
k_sectors:
	dw 0
	db 0
	dw 0xAA55
