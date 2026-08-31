#ifndef AUDIOS_CPU_H
#define AUDIOS_CPU_H

#include <stdint.h>

#define CPU_VENDOR_LEN	13
#define CPU_BRAND_LEN	49

struct cpu_info {
	char vendor[CPU_VENDOR_LEN];
	char brand[CPU_BRAND_LEN];
	uint32_t family;
	uint32_t model;
	uint32_t stepping;
	uint32_t features_edx;
	uint32_t features_ecx;
};

/** Fill `out` from CPUID. Safe to call before interrupts are enabled. */
void cpu_detect(struct cpu_info *out);

/** Print the processor summary used by the `cpu` command. */
void cpu_print(void);

#endif
