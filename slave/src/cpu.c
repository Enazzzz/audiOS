#include "cpu.h"
#include "tty.h"

#include <stdint.h>

/** Zero a cpu_info record. */
static void memset_cpu(struct cpu_info *out)
{
	uint8_t *p = (uint8_t *)out;
	for (unsigned i = 0; i < sizeof(*out); i++) {
		p[i] = 0;
	}
}

/** Execute CPUID with the given leaf and subleaf 0. */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile ("cpuid"
		: "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
		: "a"(leaf), "c"(0));
}

/** Copy 4 CPUID register bytes into a string buffer. */
static void store_reg(char *dst, uint32_t value)
{
	dst[0] = (char)(value & 0xFF);
	dst[1] = (char)((value >> 8) & 0xFF);
	dst[2] = (char)((value >> 16) & 0xFF);
	dst[3] = (char)((value >> 24) & 0xFF);
}

/** Fill `out` from CPUID leaves 0, 1, and the brand string if present. */
void cpu_detect(struct cpu_info *out)
{
	uint32_t a, b, c, d;
	memset_cpu(out);
	cpuid(0, &a, &b, &c, &d);
	store_reg(out->vendor + 0, b);
	store_reg(out->vendor + 4, d);
	store_reg(out->vendor + 8, c);
	out->vendor[12] = '\0';

	cpuid(1, &a, &b, &c, &d);
	unsigned family_id = (a >> 8) & 0xF;
	unsigned model_id = (a >> 4) & 0xF;
	unsigned ext_family = (a >> 20) & 0xFF;
	unsigned ext_model = (a >> 16) & 0xF;
	out->stepping = a & 0xF;
	out->family = family_id;
	if (family_id == 0xF) {
		out->family += ext_family;
	}
	out->model = model_id;
	if (family_id == 0x6 || family_id == 0xF) {
		out->model += ext_model << 4;
	}
	out->features_edx = d;
	out->features_ecx = c;

	cpuid(0x80000000u, &a, &b, &c, &d);
	if (a >= 0x80000004u) {
		uint32_t *brand = (uint32_t *)out->brand;
		for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
			cpuid(leaf, &a, &b, &c, &d);
			*brand++ = a;
			*brand++ = b;
			*brand++ = c;
			*brand++ = d;
		}
		out->brand[48] = '\0';
	} else {
		out->brand[0] = '\0';
	}
}

/** Print a compact processor report for the shell. */
void cpu_print(void)
{
	struct cpu_info info;
	cpu_detect(&info);
	tty_set_color(TTY_COL_FG);
	tty_puts("Processor\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("vendor: %s\n", info.vendor);
	if (info.brand[0] != '\0') {
		const char *brand = info.brand;
		while (*brand == ' ') {
			brand++;
		}
		tty_printf("brand:  %s\n", brand);
	}
	tty_printf("family: %u  model: %u  stepping: %u\n",
		info.family, info.model, info.stepping);
	tty_puts("features:");
	if (info.features_edx & (1u << 0)) {
		tty_puts(" fpu");
	}
	if (info.features_edx & (1u << 4)) {
		tty_puts(" tsc");
	}
	if (info.features_edx & (1u << 9)) {
		tty_puts(" apic");
	}
	if (info.features_edx & (1u << 15)) {
		tty_puts(" cmov");
	}
	if (info.features_edx & (1u << 23)) {
		tty_puts(" mmx");
	}
	if (info.features_edx & (1u << 25)) {
		tty_puts(" sse");
	}
	if (info.features_edx & (1u << 26)) {
		tty_puts(" sse2");
	}
	if (info.features_ecx & (1u << 0)) {
		tty_puts(" sse3");
	}
	if (info.features_ecx & (1u << 31)) {
		tty_puts(" hypervisor");
	}
	tty_puts("\n");
	tty_set_color(TTY_COL_FG);
}
