#ifndef AUDIOS_SHELL_H
#define AUDIOS_SHELL_H

/** Print the identity banner and enter the command loop. Never returns. */
void shell_run(void);

/** Run one command line (remote Audio Link / scripts). */
void shell_run_line(const char *line);

#endif
