# 32-bit audiOS slave for the ASUS A7V333 (Athlon XP / VIA VT8233A).

.SUFFIXES:

CC := gcc
CFLAGS := -g -O2 -pipe -Wall -Wextra -std=gnu11 \
	-ffreestanding -fno-stack-protector -fno-pic -fno-lto \
	-m32 -march=i686 -mno-mmx -mno-sse -mno-sse2 -mno-80387
CPPFLAGS := -I src -I ../common -I ../kernel/src -DAUDIOS_LINK_SLAVE=1
LDFLAGS := -m elf_i386 -nostdlib -static -z noexecstack -T linker.ld --gc-sections
LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name)
ifeq ($(wildcard $(LIBGCC)),)
$(error 32-bit libgcc not found. Install gcc-multilib.)
endif

CFILES := $(wildcard src/*.c)
ASMFILES := $(wildcard src/*.asm) $(wildcard boot/*.asm)
# Shared C: klib/serial/pic/cpu from the 64-bit tree (no Limine, no long mode).
SHARED_C := ../kernel/src/klib.c ../kernel/src/serial.c ../kernel/src/pic.c
PHY_C := ../common/alink_phy.c

OBJ := obj/src/entry.asm.o \
	$(addprefix obj/,$(CFILES:.c=.c.o)) \
	obj/common/alink_phy.c.o \
	obj/shared/klib.c.o obj/shared/serial.c.o obj/shared/pic.c.o

.PHONY: all
all: bin/kernel.bin boot/boot.bin

bin/kernel.elf: linker.ld $(OBJ)
	mkdir -p bin
	ld $(LDFLAGS) $(OBJ) $(LIBGCC) -o $@

bin/kernel.bin: bin/kernel.elf
	objcopy -O binary $< $@

boot/boot.bin: boot/boot.asm
	nasm -f bin $< -o $@

obj/src/%.c.o: src/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/src/kmain.c.o: src/kmain.c ../kernel/src/version.h

obj/src/entry.asm.o: src/entry.asm
	mkdir -p "$(dir $@)"
	nasm -f elf32 $< -o $@


obj/common/alink_phy.c.o: $(PHY_C)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/shared/klib.c.o: ../kernel/src/klib.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/shared/serial.c.o: ../kernel/src/serial.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/shared/pic.c.o: ../kernel/src/pic.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf obj bin boot/boot.bin
