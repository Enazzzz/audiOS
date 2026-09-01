#ifndef AUDIOS_HDA_H
#define AUDIOS_HDA_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Probe PCI for an HDA controller and bring up an analog output path.
 * Tuned for AMD SB710 + Realtek ALC662 (ASRock 960GM-GS3 FX) but walks
 * any codec that exposes a connected line-out / HP / speaker pin.
 */
bool hda_init(void);

/** True after a successful probe. */
bool hda_present(void);

/** PCI table index of the bound HDA function, or UINT32_MAX. */
uint32_t hda_pci_index(void);

/** False if the function disappeared from PCI config. */
bool hda_alive(void);

/** Human-readable device name (includes codec when known). */
const char *hda_name(void);

/** Hardware PCM rate in Hz (48 kHz on this path). */
uint32_t hda_hw_rate(void);

/** Start the output stream. Prefills every period via `fill`. */
bool hda_start(uint32_t period_frames, void (*fill)(int16_t *dst, uint32_t frames));

/** Stop the output stream. */
void hda_stop(void);

/**
 * Refill consumed DMA periods from `fill`.
 * `fill` writes `frames` stereo s16 samples into `dst`.
 */
unsigned hda_service(void (*fill)(int16_t *dst, uint32_t frames));

/** Hardware underrun counter since last start. */
uint32_t hda_underruns(void);

#endif
