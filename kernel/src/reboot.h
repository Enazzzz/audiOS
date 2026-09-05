#ifndef AUDIOS_REBOOT_H
#define AUDIOS_REBOOT_H

/** Restart the machine via the keyboard controller, then triple-fault. */
void system_reboot(void);

/** ACPI S5 / QEMU power-off. Prints a hint and halts if power will not drop. */
void system_shutdown(void);

#endif
