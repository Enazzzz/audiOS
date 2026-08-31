#ifndef AUDIOS_AC97_H
#define AUDIOS_AC97_H

#include <stdbool.h>
#include <stdint.h>

/** Probe PCI for an ICH AC97 function and prepare DMA. */
bool ac97_init(void);

/** True after a successful probe. */
bool ac97_present(void);

/** PCI table index of the bound AC97 function, or UINT32_MAX. */
uint32_t ac97_pci_index(void);

/** False if the function disappeared from PCI config. */
bool ac97_alive(void);

/** Human-readable device name. */
const char *ac97_name(void);

/** Hardware PCM rate in Hz (typically 48000). */
uint32_t ac97_hw_rate(void);

/** Start PCM-out DMA. Prefills every period via `fill`. */
bool ac97_start(uint32_t period_frames, void (*fill)(int16_t *dst, uint32_t frames));

/** Stop PCM-out DMA. */
void ac97_stop(void);

/**
 * Refill consumed DMA periods from `fill`.
 * `fill` writes `frames` stereo s16 samples into `dst`.
 * Returns the number of periods refilled.
 */
unsigned ac97_service(void (*fill)(int16_t *dst, uint32_t frames));

/** Hardware underrun counter since last start. */
uint32_t ac97_underruns(void);

#endif
