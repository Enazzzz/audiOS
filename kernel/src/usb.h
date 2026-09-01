#ifndef AUDIOS_USB_H
#define AUDIOS_USB_H

#include "blk.h"

#include <stdbool.h>

/**
 * Probe PCI EHCI, enumerate high-speed mass storage, and fill `out`.
 * Never formats a disk. Returns false if nothing usable is found.
 */
bool usb_msc_init(struct blkdev *out, void (*idle)(void));

#endif
