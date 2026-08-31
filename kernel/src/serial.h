#ifndef AUDIOS_SERIAL_H
#define AUDIOS_SERIAL_H

/** Initialise COM1 at 115200 8N1 for headless tests and bring-up. */
void serial_init(void);

/** Write one byte to COM1, waiting for the transmitter to be ready. */
void serial_putc(char c);

/** Non-blocking read. Returns -1 if the UART has no data. */
int serial_getc(void);

#endif
