; 32-bit entry, GDT reload, interrupt stubs, and a small stack.

	BITS 32
	SECTION .text

	GLOBAL _start
	GLOBAL isr_stub_table
	EXTERN kmain
	EXTERN interrupt_dispatch

_start:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov esp, stack_end
	lgdt [gdt_desc]
	jmp 0x08:.cs
.cs:
	call kmain
.hang:
	hlt
	jmp .hang

isr_common:
	pusha
	push ds
	push es
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	push esp
	call interrupt_dispatch
	add esp, 4
	pop es
	pop ds
	popa
	add esp, 8			; vector + error
	iret

%macro ISR_NOERR 1
isr_%1:
	push dword 0
	push dword %1
	jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
	push dword %1
	jmp isr_common
%endmacro

	ISR_NOERR 0
	ISR_NOERR 1
	ISR_NOERR 2
	ISR_NOERR 3
	ISR_NOERR 4
	ISR_NOERR 5
	ISR_NOERR 6
	ISR_NOERR 7
	ISR_ERR 8
	ISR_NOERR 9
	ISR_ERR 10
	ISR_ERR 11
	ISR_ERR 12
	ISR_ERR 13
	ISR_ERR 14
	ISR_NOERR 15
	ISR_NOERR 16
	ISR_ERR 17
	ISR_NOERR 18
	ISR_NOERR 19
	ISR_NOERR 20
	ISR_NOERR 21
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_NOERR 29
	ISR_NOERR 30
	ISR_NOERR 31
%assign i 32
%rep 16
	ISR_NOERR i
%assign i i+1
%endrep

	ALIGN 4
isr_stub_table:
%assign i 0
%rep 48
	dd isr_%+i
%assign i i+1
%endrep

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

	SECTION .bss
	ALIGN 16
stack_start:
	resb 16384
stack_end:

	SECTION .note.GNU-stack noalloc noexec nowrite progbits

