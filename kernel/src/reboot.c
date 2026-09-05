#include "reboot.h"
#include "io.h"
#include "klib.h"
#include "phys.h"
#include "tty.h"

#include <limine.h>
#include <stdint.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST_ID,
	.revision = 0
};

/**
 * Ask the 8042 keyboard controller to pulse the reset line. If that does
 * not take the machine down, load an empty IDT and fire an interrupt so
 * the CPU triple-faults into a reset.
 */
void system_reboot(void)
{
	tty_puts("rebooting...\n");
	__asm__ volatile ("cli");
	for (int i = 0; i < 100000; i++) {
		if ((inb(0x64) & 0x02) == 0) {
			break;
		}
		io_wait();
	}
	outb(0x64, 0xFE);
	io_wait();
	struct {
		uint16_t limit;
		uint64_t base;
	} __attribute__((packed)) empty = { 0, 0 };
	__asm__ volatile ("lidt %0; int $0" : : "m"(empty));
	for (;;) {
		__asm__ volatile ("hlt");
	}
}

/** QEMU/Bochs/old PIIX power-off ports. Harmless if nothing is listening. */
static void try_qemu_poweroff(void)
{
	outw(0x604, 0x2000);
	outw(0xB004, 0x2000);
	outw(0x4004, 0x3400);
}

/** Scan a table blob for `_S5_` and read SLP_TYPa from the AML package. */
static int parse_s5(const uint8_t *p, uint32_t n, uint16_t *typa)
{
	for (uint32_t i = 0; i + 10 < n; i++) {
		if (p[i] != '_' || p[i + 1] != 'S' || p[i + 2] != '5' || p[i + 3] != '_') {
			continue;
		}
		uint32_t j = i + 4;
		while (j + 3 < n && p[j] != 0x12) {
			j++;
		}
		if (j + 3 >= n) {
			continue;
		}
		j++;
		while (j < n && p[j] == 0x0A) {
			j += 2;
			if (j >= n) {
				break;
			}
			*typa = p[j - 1];
			return 1;
		}
		if (j < n && p[j] < 10) {
			*typa = p[j];
			return 1;
		}
	}
	return 0;
}

/** FADT + DSDT S5, then PM1a sleep. */
static int try_acpi_s5(void)
{
	if (rsdp_request.response == NULL || rsdp_request.response->address == NULL) {
		return 0;
	}
	uint8_t *rsdp = rsdp_request.response->address;
	/* Limine base revision 6: this pointer is already virtual. Table
	 * pointers *inside* RSDP/RSDT/XSDT/FADT are still physical. */
	uint64_t xsdt_phys = 0;
	uint32_t rsdt_phys = 0;
	if (rsdp[15] >= 2) {
		memcpy(&xsdt_phys, rsdp + 24, 8);
	}
	memcpy(&rsdt_phys, rsdp + 16, 4);
	uint8_t *root = NULL;
	uint32_t root_len = 0;
	int wide = 0;
	if (xsdt_phys) {
		root = phys_to_virt(xsdt_phys);
		wide = 1;
	} else if (rsdt_phys) {
		root = phys_to_virt(rsdt_phys);
	}
	if (root == NULL) {
		return 0;
	}
	memcpy(&root_len, root + 4, 4);
	if (root_len < 36) {
		return 0;
	}
	uint8_t *fadt = NULL;
	unsigned esz = wide ? 8u : 4u;
	for (uint32_t off = 36; off + esz <= root_len; off += esz) {
		uint64_t phys = 0;
		if (wide) {
			memcpy(&phys, root + off, 8);
		} else {
			uint32_t p32 = 0;
			memcpy(&p32, root + off, 4);
			phys = p32;
		}
		uint8_t *tbl = phys_to_virt(phys);
		if (tbl && tbl[0] == 'F' && tbl[1] == 'A' && tbl[2] == 'C' && tbl[3] == 'P') {
			fadt = tbl;
			break;
		}
	}
	if (fadt == NULL) {
		return 0;
	}
	uint32_t pm1a = 0;
	memcpy(&pm1a, fadt + 64, 4);
	uint32_t dsdt_phys = 0;
	memcpy(&dsdt_phys, fadt + 40, 4);
	uint16_t typa = 0;
	if (dsdt_phys) {
		uint8_t *dsdt = phys_to_virt(dsdt_phys);
		uint32_t dlen = 0;
		if (dsdt) {
			memcpy(&dlen, dsdt + 4, 4);
			(void)parse_s5(dsdt, dlen, &typa);
		}
	}
	if (pm1a == 0) {
		return 0;
	}
	uint16_t slp_en = (uint16_t)(1u << 13);
	uint16_t val = (uint16_t)((typa << 10) | slp_en);
	outw((uint16_t)pm1a, val);
	return 1;
}

void system_shutdown(void)
{
	tty_puts("shutting down...\n");
	try_qemu_poweroff();
	(void)try_acpi_s5();
	try_qemu_poweroff();
	tty_set_color(TTY_COL_DIM);
	tty_puts("board did not power off — use the ATX button, or reboot\n");
	tty_set_color(TTY_COL_FG);
	__asm__ volatile ("cli");
	for (;;) {
		__asm__ volatile ("hlt");
	}
}
