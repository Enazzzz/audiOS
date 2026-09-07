; 16-bit boot for the audiOS i386 slave (A7V333).
; Works on a 1.44 MB floppy (CHS) and on an IDE HDD (INT 13h LBA).
; Loads the 32-bit kernel from the LBA stored at offset 0x1AC into 0x10000.
;
; Layout (same on floppy and HDD):
;   0x1AC  dd kernel_lba      (default 1)
;   0x1B0  dw kernel_sectors  (patched by the packer / `ide format`)
;   0x1BE  partition table (zeros on floppy; HDD may fill it)
;   0x1FE  dw 0xAA55

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

	mov eax, [kernel_lba]
	mov [cur_lba], eax
	mov cx, [kernel_sectors]
	test cx, cx
	jz fail

	; INT 13h AH=41h returns the LBA feature bitmask in CX. Reload the
	; sector count afterwards — that register is our remaining-sector loop.
	mov ah, 0x41
	mov bx, 0x55AA
	mov dl, [boot_drive]
	int 0x13
	jc chs_load
	mov ax, 0x1000
	mov es, ax
	xor bx, bx
	mov cx, [kernel_sectors]

lba_load:
	cmp cx, 0
	je go_pm
	mov byte [dap], 16
	mov byte [dap + 1], 0
	mov word [dap + 2], 1
	mov [dap + 4], bx
	mov [dap + 6], es
	mov eax, [cur_lba]
	mov [dap + 8], eax
	mov dword [dap + 12], 0
	push cx
	push es
	push bx
	mov si, dap
	mov ah, 0x42
	mov dl, [boot_drive]
	int 0x13
	pop bx
	pop es
	pop cx
	jc fail
	call advance_buf
	dec cx
	jmp lba_load

chs_load:
	; Floppy geometry 18 spt / 2 heads. HDD without LBA uses INT 13h AH=08.
	mov word [spt], 18
	mov word [heads], 2
	mov dl, [boot_drive]
	test dl, 0x80
	jz .got_geo
	mov ah, 0x08
	int 0x13
	jc .got_geo
	movzx ax, dh
	inc ax
	mov [heads], ax
	mov ax, cx
	and ax, 0x3F
	jnz .spt_ok
	mov ax, 63
.spt_ok:
	mov [spt], ax
.got_geo:
	; AH=08 can clobber ES. Reload the kernel buffer.
	mov ax, 0x1000
	mov es, ax
	xor bx, bx
	mov eax, [kernel_lba]
	mov [cur_lba], eax
	mov cx, [kernel_sectors]
.chs_one:
	cmp cx, 0
	je go_pm
	push cx
	push es
	push bx
	; LBA -> CHS. Do not keep the buffer in BX across DIV (DIV uses r32).
	mov eax, [cur_lba]
	xor edx, edx
	movzx ecx, word [spt]
	div ecx
	inc dx
	mov [tmp_sec], dx
	xor edx, edx
	movzx ecx, word [heads]
	div ecx
	mov [tmp_cyl], ax
	mov [tmp_head], dx
	pop bx
	pop es
	mov ax, [tmp_cyl]
	mov ch, al
	mov cl, [tmp_sec]
	mov dh, [tmp_head]
	mov dl, [boot_drive]
	mov ax, 0x0201
	int 0x13
	pop cx
	jc fail
	call advance_buf
	dec cx
	jmp .chs_one

advance_buf:
	add bx, 512
	jnc .same
	mov ax, es
	add ax, 0x1000
	mov es, ax
	xor bx, bx
.same:
	inc dword [cur_lba]
	ret

fail:
	; VGA + COM1 so QEMU -serial and a real Award screen both show the miss.
	mov si, err
.p:
	lodsb
	test al, al
	jz .h
	mov ah, 0x0E
	int 0x10
	mov dx, 0x3F8
	out dx, al
	jmp .p
.h:
	jmp .h

go_pm:
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

dap:
	times 16 db 0
cur_lba:
	dd 0
spt:
	dw 18
heads:
	dw 2
tmp_sec:
	dw 0
tmp_cyl:
	dw 0
tmp_head:
	dw 0

	times 0x1AC - ($ - $$) db 0
kernel_lba:
	dd 1
kernel_sectors:
	dw 0
	times 0x1BE - ($ - $$) db 0
	times 64 db 0
	dw 0xAA55
