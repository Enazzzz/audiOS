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
all: $(IMAGE_NAME).iso $(IMAGE_NAME).img

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
test: $(IMAGE_NAME).iso $(IMAGE_NAME).img audios-fs.img
	python3 tools/test_fat_lfn.py
	python3 tools/qemu_smoke.py $(IMAGE_NAME).iso
	python3 tools/qemu_full.py $(IMAGE_NAME).iso
	python3 tools/qemu_sound.py $(IMAGE_NAME).iso
	python3 tools/qemu_img_boot.py $(IMAGE_NAME).img
	python3 tools/qemu_ps2.py $(IMAGE_NAME).img

# 16 MiB system partition, then pad the file so leftover USB exists for a
# second FAT32 data partition (created by the kernel on first mount).
audios-fs.img: tools/make_fat.py media/test.wav media/bad.wav media/float.wav tools/demo.aos tools/C_README.txt
	python3 tools/make_fat.py audios-fs.img --size-mb 16 --dir audio \
		--file media/test.wav:audio/test.wav \
		--file media/bad.wav:audio/bad.wav \
		--file media/float.wav:audio/float.wav \
		--file tools/demo.aos:demo.aos \
		--file tools/C_README.txt:README.TXT
	python3 -c "import os; os.truncate('audios-fs.img', 48 * 1024 * 1024)"

$(IMAGE_NAME).img: limine-binary/limine kernel media/test.wav tools/demo.aos tools/C_README.txt
	python3 tools/make_fat.py $(IMAGE_NAME).img --size-mb 64 --dir boot --dir audio --dir boot/limine \
		--file kernel/bin-$(ARCH)/kernel:boot/kernel \
		--file limine.conf:boot/limine/limine.conf \
		--file limine-binary/limine-bios.sys:boot/limine/limine-bios.sys \
		--file limine-binary/limine-bios.sys:limine-bios.sys \
		--file media/test.wav:audio/test.wav \
		--file media/bad.wav:audio/bad.wav \
		--file media/float.wav:audio/float.wav \
		--file tools/demo.aos:demo.aos \
		--file tools/C_README.txt:README.TXT
	./limine-binary/limine bios-install $(IMAGE_NAME).img

media/test.wav: tools/gen_wav.py
	python3 tools/gen_wav.py media

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).img audios-fs.img

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).img audios-fs.img limine-binary
