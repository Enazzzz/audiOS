#ifndef AUDIOS_USB_H
#define AUDIOS_USB_H

#include "blk.h"

#include <stdbool.h>

/**
 * Probe PCI EHCI and bind high-speed MSC. `boot` is the stick we booted
 * from (C: + D:). `extra`, if non-NULL, receives a second stick (E:)
 * when another root port has mass storage. Keyboard input is PS/2.
 * Never formats the boot/system partition.
 */
bool usb_msc_init(struct blkdev *boot, struct blkdev *extra, void (*idle)(void));

/** Probe remaining EHCI ports for a newly connected stick without dropping the boot device. */
bool usb_msc_scan_extra(struct blkdev *extra);

/**
 * BOT reset + clear halt on the boot stick. Call after a failed SCSI
 * transfer so later FAT I/O can recover without a reboot.
 */
void usb_msc_kick(void);

#endif
