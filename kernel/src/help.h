#ifndef AUDIOS_HELP_H
#define AUDIOS_HELP_H

/** Alphabetical one-line command list. */
void help_list(void);

/** Long help for one command. Returns 0 if unknown. */
int help_topic(const char *name);

#endif
