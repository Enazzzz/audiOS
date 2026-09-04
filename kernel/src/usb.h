#ifndef AUDIOS_USB_H
#define AUDIOS_USB_H

#include "blk.h"

#include <stdbool.h>

/**
 * Probe PCI EHCI, enumerate high-speed mass storage, and fill `out`.
 * Full/low-speed ports are parked onto the companion controller so they
 * do not stall EHCI. Keyboard input is PS/2, not USB HID.
 * Never formats the boot/system partition. Leftover unpartitioned space
 * may become a new data volume on first mount.
 */
bool usb_msc_init(struct blkdev *out, void (*idle)(void));

#endif
