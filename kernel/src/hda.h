#ifndef AUDIOS_HDA_H
#define AUDIOS_HDA_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Probe PCI for an HDA controller and bring up an analog output path.
 * Tuned for AMD SB710 + Realtek ALC662 (ASRock 960GM-GS3 FX): ATI snoop,
 * CORB/RIRB verbs, ICS fallback. Walks any codec with a connected
 * line-out / HP / speaker pin.
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

/** Output amplifier 0–100. Headphones often want the software limiter too. */
void hda_set_volume(unsigned pct);
unsigned hda_volume(void);

/** True if a mic or line-in ADC path was found. */
int hda_has_capture(void);

/** 1 = mic jack, 0 = line in. */
int hda_select_input(int mic);

/** 1 after `hda_select_input(1)`. */
int hda_mic_selected(void);

/** Input analog gain 0–100. */
void hda_set_ingain(unsigned pct);
unsigned hda_ingain(void);

/** Peak of the last capture period (0–32767) for the selected input. */
unsigned hda_peak_in(void);

/** Pull analog capture into `dst` (stereo s16 at the hardware rate). */
uint32_t hda_cap_take(int16_t *dst, uint32_t frames);

/** Frames written into the buffer armed by `hda_cap_take`. */
uint32_t hda_cap_filled(void);

/** Pump input DMA and meter peaks (safe when playback is stopped). */
void hda_cap_poll(void);

#endif
