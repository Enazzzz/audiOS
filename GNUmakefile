# Nuke built-in rules.
.SUFFIXES:

ARCH := x86_64
QEMUFLAGS := -m 128M
IMAGE_NAME := audios
QEMU_AUDIO := -audiodev wav,id=snd0,path=audios-out.wav -device ich9-intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0
QEMU_USB := -drive if=none,id=stick,file=audios-fs.img,format=raw,cache=directsync -device usb-ehci,id=ehci -device usb-storage,bus=ehci.0,drive=stick

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

.PHONY: all
all: $(IMAGE_NAME).iso $(IMAGE_NAME).img $(IMAGE_NAME).flp audios32.flp audios32.hdd

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel

kernel/.deps-obtained:
	./kernel/get-deps

limine-binary/limine:
	rm -rf limine-binary
	curl -L https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.gz | gunzip | tar -xf -
	$(MAKE) -C limine-binary \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

$(IMAGE_NAME).iso: limine-binary/limine kernel media/test.wav
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT iso_root/audio
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	cp -v limine.conf iso_root/boot/limine/
	cp -v media/test.wav media/bad.wav media/float.wav iso_root/audio/
	cp -v limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin limine-binary/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine-binary/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine-binary/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine-binary/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: run
run: $(IMAGE_NAME).iso audios-fs.img
	qemu-system-$(ARCH) -M q35 -cdrom $(IMAGE_NAME).iso -boot d -serial stdio $(QEMUFLAGS) $(QEMU_AUDIO) $(QEMU_USB)

.PHONY: test
test: $(IMAGE_NAME).iso $(IMAGE_NAME).img $(IMAGE_NAME).flp audios-fs.img audios32.flp audios32.hdd
	$(HOST_CC) $(HOST_CFLAGS) -o tools/test_alink_phy tools/test_alink_phy.c common/alink_phy.c
	./tools/test_alink_phy
	python3 tools/test_fat_lfn.py
	python3 tools/test_floppy.py $(IMAGE_NAME).flp
	python3 tools/qemu_smoke.py $(IMAGE_NAME).iso
	python3 tools/qemu_full.py $(IMAGE_NAME).iso
	python3 tools/qemu_sound.py $(IMAGE_NAME).iso
	python3 tools/qemu_img_boot.py $(IMAGE_NAME).img
	python3 tools/qemu_floppy.py $(IMAGE_NAME).flp
	python3 tools/qemu_ps2.py $(IMAGE_NAME).img
	python3 tools/qemu_slave.py audios32.flp
	python3 tools/qemu_slave.py audios32.hdd
	python3 tools/qemu_ide.py $(IMAGE_NAME).iso

# 16 MiB system partition, then pad the file so leftover USB exists for a
# second FAT32 data partition (created by the kernel on first mount).
audios-fs.img: tools/make_fat.py media/test.wav media/bad.wav media/float.wav tools/demo.aos tools/C_README.txt tools/plant_leftover_fat.py slave
	python3 tools/make_fat.py audios-fs.img --size-mb 16 --dir audio --dir boot \
		--file media/test.wav:audio/test.wav \
		--file media/bad.wav:audio/bad.wav \
		--file media/float.wav:audio/float.wav \
		--file tools/demo.aos:demo.aos \
		--file tools/C_README.txt:README.TXT \
		--file slave/boot/boot.bin:boot/slave.mbr \
		--file slave/bin/kernel.bin:boot/slave.bin
	python3 -c "import os; os.truncate('audios-fs.img', 48 * 1024 * 1024)"
	python3 tools/plant_leftover_fat.py audios-fs.img

.PHONY: slave
slave:
	$(MAKE) -C slave

audios32.flp: slave tools/make_slave_floppy.py
	python3 tools/make_slave_floppy.py -o audios32.flp \
		--boot slave/boot/boot.bin \
		--kernel slave/bin/kernel.bin

audios32.hdd: slave tools/make_slave_hdd.py tools/make_slave_floppy.py
	python3 tools/make_slave_hdd.py -o audios32.hdd \
		--boot slave/boot/boot.bin \
		--kernel slave/bin/kernel.bin

$(IMAGE_NAME).flp: limine-binary/limine kernel limine-floppy.conf tools/make_floppy.py tools/limine_int13_hook.asm
	python3 tools/make_floppy.py $(IMAGE_NAME).flp \
		--kernel kernel/bin-$(ARCH)/kernel \
		--limine-sys limine-binary/limine-bios.sys \
		--limine-conf limine-floppy.conf \
		--limine-tool limine-binary/limine

$(IMAGE_NAME).img: limine-binary/limine kernel media/test.wav tools/demo.aos tools/C_README.txt $(IMAGE_NAME).flp slave
	python3 tools/make_fat.py $(IMAGE_NAME).img --size-mb 64 --dir boot --dir audio --dir boot/limine \
		--file kernel/bin-$(ARCH)/kernel:boot/kernel \
		--file limine.conf:boot/limine/limine.conf \
		--file limine-binary/limine-bios.sys:boot/limine/limine-bios.sys \
		--file limine-binary/limine-bios.sys:limine-bios.sys \
		--file media/test.wav:audio/test.wav \
		--file media/bad.wav:audio/bad.wav \
		--file media/float.wav:audio/float.wav \
		--file tools/demo.aos:demo.aos \
		--file tools/C_README.txt:README.TXT \
		--file $(IMAGE_NAME).flp:boot/floppy.img \
		--file slave/boot/boot.bin:boot/slave.mbr \
		--file slave/bin/kernel.bin:boot/slave.bin
	./limine-binary/limine bios-install $(IMAGE_NAME).img

media/test.wav: tools/gen_wav.py
	python3 tools/gen_wav.py media

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C slave clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).img $(IMAGE_NAME).flp audios32.flp audios32.hdd audios-fs.img audios-ide-scratch.hdd tools/test_alink_phy

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	$(MAKE) -C slave clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).img $(IMAGE_NAME).flp audios32.flp audios32.hdd audios-fs.img audios-ide-scratch.hdd limine-binary tools/test_alink_phy
