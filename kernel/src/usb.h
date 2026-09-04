#ifndef AUDIOS_USB_H
#define AUDIOS_USB_H

#include "blk.h"

#include <stdbool.h>

/**
 * Probe PCI EHCI, enumerate high-speed mass storage, and fill `out`.
 * Also parks full/low-speed ports onto OHCI companions and looks for a
 * boot-protocol HID keyboard. Never formats a disk.
 */
bool usb_msc_init(struct blkdev *out, void (*idle)(void));

/** Poll a USB HID keyboard into the same queue as PS/2. Harmless if none. */
void usb_poll(void);

/** Scan PCI OHCI companions (SB710 USB 1.1 / parked EHCI ports). */
void usb_ohci_init(void (*idle)(void));

#endif
