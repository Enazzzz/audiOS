#include "help.h"
#include "klib.h"
#include "tty.h"

/** One short line, then a longer paragraph for `help <cmd>`. */
struct help_row {
	const char *name;
	const char *brief;
	const char *detail;
};

static const struct help_row rows[] = {
	{ "audio", "configuration; vol, limiter, gain, devices, info, set, status, test",
	  "audio                 print the selected device and engine rate\n"
	  "audio help             this audio list (also `help audio`)\n"
	  "audio devices          PCI HDA / AC97 functions\n"
	  "audio info             codec / stream details\n"
	  "audio set rate|buffer|format|channels|device <v>\n"
	  "audio status           underruns and frames played\n"
	  "audio test             440 Hz until stop\n"
	  "audio vol [0-100]      master volume (F11 down, F12 up)\n"
	  "audio limiter on|off   safety limiter (on for headphones)\n"
	  "audio gain [0-100]     analog input / capture gain\n"
	  "audio pause            toggle playback (also F5; F6 stop)\n" },
	{ "bpf", "band-pass filter on a clip",
	  "bpf [clip] <hz>\n  Centre frequency in Hz. Omits clip → current (`use`).\n" },
	{ "cd", "change directory (C: D: E: /os = C:)",
	  "cd [path]\n"
	  "  cd D:     switch to the data volume\n"
	  "  cd C:/audio   or  cd /os/audio\n"
	  "  cd        goes to D:/ if D: is mounted, else C:/\n" },
	{ "clear", "clear the console and reprint the banner",
	  "clear\n  Fills the framebuffer, resets the cursor, reprints the banner.\n" },
	{ "clip", "print rate/frames/peak for a clip",
	  "clip [name]\n  No name lists clips. Marks a/b show if set on the current clip.\n" },
	{ "clips", "list named clips in the pool",
	  "clips\n  Hidden working buffers (.play, .seq) are omitted.\n" },
	{ "cp", "copy a file",
	  "cp <src> <dst>\n  Copies across C:/D:/E:. Never formats a volume.\n" },
	{ "cpu", "processor identification",
	  "cpu\n  Vendor, family, model, stepping from CPUID.\n" },
	{ "crush", "bit crush",
	  "crush [clip] <bits>\n  bits 1..16. Omits clip → current.\n" },
	{ "cue", "set mark a and preview from there",
	  "cue <time>\n  Time is frames, 0.2s, 250ms, 4b. Plays a short preview from that point.\n" },
	{ "decimate", "sample-rate crush",
	  "decimate [clip] <hz>\n  Hold-sample downsample. Omits clip → current.\n" },
	{ "delay", "echo",
	  "delay [clip] <ms> <fb> <mix>\n  fb and mix are 0..1. Omits clip → current.\n" },
	{ "distort", "waveshape",
	  "distort [clip] <drive>\n  drive is a gain (1 = none). Omits clip → current.\n" },
	{ "drop", "free a clip slot",
	  "drop <name>\n  Releases the clip; pool memory is reused after compact.\n" },
	{ "drives", "C: D: E: and extra USB",
	  "drives\n  Same as mount, then prints cwd. Plug a second stick and type mount for E:.\n" },
	{ "edit", "text editor",
	  "edit <file>\n  Ctrl-O save, Ctrl-X quit. Lines wrap to the text grid.\n" },
	{ "fade", "fade in or out",
	  "fade [clip] in|out <dur>\n  dur is 10ms, 0.2s, or frames. Omits clip → current.\n" },
	{ "gain", "clip amplitude, or current clip if unnamed",
	  "gain [clip] <amp>\n  amp is 0.5, 1.2, or 80%. Omit clip to use the current one (`use`).\n" },
	{ "help", "this list, or details for one command",
	  "help              alphabetical list\n"
	  "help <command>    parameters and notes for that command\n"
	  "Keys: Up/Down history  PgUp/PgDn scroll  Ctrl-C copy  Ctrl-V paste\n"
	  "      Ctrl-Backspace delete word  F5 play/pause  F6 stop  F11/F12 volume\n" },
	{ "hpf", "high-pass filter",
	  "hpf [clip] <hz>\n  Omits clip → current.\n" },
	{ "info", "file metadata",
	  "info <path>\n  Kind, size, name. Path may be C:/ D:/ E: or /os.\n" },
	{ "join", "concatenate clips",
	  "join <dst> <a> <b> ...\n  Resamples sources to the first clip's rate.\n" },
	{ "limiter", "headphone safety limiter",
	  "audio limiter on|off\n  Soft-knee peak cap on the mix. Use on for headphones, off for speakers.\n" },
	{ "load", "WAV into a named clip",
	  "load <file.wav> [name]\n  Name defaults to the file stem. Sets the current clip.\n" },
	{ "lpf", "low-pass filter",
	  "lpf [clip] <hz>\n  Omits clip → current.\n" },
	{ "ls", "list directory",
	  "ls [path]\n  C: system, D: data, E: extra USB. /os is C:.\n" },
	{ "mark", "in/out points on the current clip",
	  "mark                print a/b\n"
	  "mark a|b <time>     time is frames, 0.2s, 250ms, 4b (beats)\n"
	  "slice with no range uses the marks. cue <time> sets mark a and previews.\n" },
	{ "mem", "physical memory map",
	  "mem\n  Usable RAM from the Limine memmap.\n" },
	{ "mix", "sum clips",
	  "mix <dst> <a> <b> ...\n  Clipping-safe sum. Length is the longest source.\n" },
	{ "mkdir", "create directory",
	  "mkdir <dir>\n  Creates on the current volume. Empty dirs only for rm.\n" },
	{ "mount", "mount status / scan extra USB",
	  "mount\n  Prints C:/D:/E:. Scans for a second MSC stick as E:. Never formats C:.\n"
	  "  If leftover USB already looks like FAT32 (old D: after a 64 MiB flash),\n"
	  "  it is mounted as D: and the MBR slot is restored — never reformatted.\n" },
	{ "music", "clip / DSP / seq / rec",
	  "music     full music-system list\n"
	  "See also: load save clip proc seq rec use undo slice mark.\n" },
	{ "mv", "rename or move file",
	  "mv <src> <dst>\n" },
	{ "new", "silent clip",
	  "new <name> <dur> [rate]\n  dur is 40ms, 1s, or a frame count.\n" },
	{ "noise", "white noise clip",
	  "noise <dst> <dur> [amp]\n  amp is 0..1 (default 0.5). seed <n> makes it repeatable.\n" },
	{ "norm", "normalize peak",
	  "norm [clip]\n  Scale so peak is full-scale. Omits clip → current.\n" },
	{ "pan", "stereo pan",
	  "pan [clip] <-100..100>\n  -100 left, 0 centre, 100 right. Omits clip → current.\n" },
	{ "pitch", "resample pitch",
	  "pitch [clip] <ratio|+st>\n  Length follows pitch unless you add keep. Omits clip → current.\n" },
	{ "play", "play a clip or WAV",
	  "play [clip|file] [loop|n]\n  Omit the name to play the current clip. F5 pauses, F6 stop.\n" },
	{ "proc", "DSP chain on a clip (or current)",
	  "proc [src] [dst] op args ...\n"
	  "  proc gain 0.5          acts on the current clip\n"
	  "  proc myclip gain 0.5   named clip\n"
	  "Ops: reverse gain norm fade pitch stretch rate crush decimate distort\n"
	  "     lpf hpf bpf delay pan vary\n" },
	{ "pwd", "print working directory",
	  "pwd\n  Prints C:/ D:/ or E:/ form.\n" },
	{ "rate", "change clip sample rate",
	  "rate [clip] <hz>\n  Resamples in place. Omits clip → current.\n" },
	{ "rec", "record mix or analog in",
	  "rec <name> <dur>           record the output mix\n"
	  "rec mic <name> <dur>       analog mic (ALC662 pink jack)\n"
	  "rec line <name> <dur>      analog line in (blue jack)\n"
	  "QEMU's hda-output has no ADC; mic/line need the FX board.\n" },
	{ "reboot", "restart the machine",
	  "reboot\n  8042 pulse, then triple-fault. Saves session to D:/session.aos first.\n" },
	{ "redo", "redo last clip op",
	  "redo\n  Restores the clip snapshot taken before the last undo.\n" },
	{ "repeat", "n copies of a clip",
	  "repeat <src> [dst] <n>\n  n is 1..64.\n" },
	{ "reverse", "reverse a clip",
	  "reverse [clip]\n  Omits clip → current.\n" },
	{ "rm", "delete file or empty directory",
	  "rm <path>\n" },
	{ "sample", "read or write one stereo frame",
	  "sample <name> <index> [L] [R]\n  L/R are s16 values. One value writes both channels.\n" },
	{ "save", "clip to WAV",
	  "save <name> <file.wav>\n" },
	{ "script", "run an .aos (or text) command file",
	  "script <file>\n  .aos files may start with a magic AOS1 line (skipped).\n"
	  "  D:/autoexec.aos runs at boot if present (must start with AOS1).\n" },
	{ "seed", "RNG seed for noise",
	  "seed [n]\n  No argument prints the current seed.\n" },
	{ "seq", "pattern: add, bpm/clock, play from a step/bar",
	  "seq bpm [n]              tempo / clock (default 120)\n"
	  "seq add <clip> <time>    time: frames, 20ms, 1s, 4b, 1bar\n"
	  "seq list|clear|len [t]\n"
	  "seq render [name]\n"
	  "seq play                 from the start\n"
	  "seq play <step>          from that event index\n"
	  "seq play bar <n>         from bar n (4/4)\n" },
	{ "shutdown", "power off (ACPI / QEMU)",
	  "shutdown\n  QEMU ports, then ACPI S5 on PM1a/PM1b. If the board stays on,\n"
	  "  control returns to the shell (do not yank the cord).\n" },
	{ "slice", "cut a region; plays a preview",
	  "slice <src> [dst] <start> <end>\n"
	  "slice <src>               uses mark a/b on that clip / current\n"
	  "Times: frames, 0.2s, 250ms. Sets current to the result and previews.\n" },
	{ "stop", "halt playback",
	  "stop\n  Also F6. Does not erase clips.\n" },
	{ "storage", "capacity and free space",
	  "storage\n  Per mounted FAT32 volume.\n" },
	{ "stretch", "change length",
	  "stretch [clip] <ratio>\n  0.5 = half length. Omits clip → current.\n" },
	{ "tetris", "NES-rules falling blocks",
	  "tetris [level]     play (0–19). Q quit, P pause, Z/X rotate\n"
	  "tetris scores      D:/tetris.scr (falls back to C:)\n"
	  "Hold left/right for NES DAS (16 then every 6). Cells are 2x2 glyphs.\n"
	  "The OS owns the paint frame (no serial CUP spam per cell).\n" },
	{ "tone", "oscillator",
	  "tone [sine|square|saw|noise|silence] [freq] [amp] [dur]\n"
	  "  No duration plays until stop.\n" },
	{ "touch", "create empty file",
	  "touch <file>\n" },
	{ "type", "show file bytes",
	  "type <file>\n  Dumps text/bytes. `cat` is only an ASCII cat, not a dump.\n" },
	{ "undo", "undo last clip op",
	  "undo\n  One snapshot of the last mutated clip (gain, slice, proc, …).\n" },
	{ "update", "copy a new kernel onto C: (D: kept)",
	  "update [kernel-path]\n  Writes C:/boot/kernel only. Never touches D:.\n" },
	{ "use", "set the current clip",
	  "use <name>\n  Later `proc gain 0.5`, `gain 0.5`, `play` omit the name.\n" },
	{ "vary", "amplitude jitter",
	  "vary [clip] <amount>\n  amount 0..1. Omits clip → current.\n" },
	{ "version", "system version and uptime",
	  "version\n  Banner, build, board, uptime, framebuffer size.\n" },
	{ "vol", "master volume",
	  "audio vol [0-100]\n  Also F11 / F12. Shown on the top-right HUD with meters.\n" },
	{ NULL, NULL, NULL }
};

/** Alphabetical one-line command list (`help`). */
void help_list(void)
{
	unsigned i;
	tty_puts("audiOS commands  (help <command> for parameters)\n");
	tty_set_color(TTY_COL_DIM);
	for (i = 0; rows[i].name; i++) {
		char line[96];
		ksnprintf(line, sizeof(line), "  %-16s %s\n", rows[i].name, rows[i].brief);
		tty_puts(line);
		if (strcmp(rows[i].name, "audio") == 0) {
			tty_puts("  audio help        audio / tone / play commands\n");
		}
	}
	tty_puts("  PgUp / PgDn       scroll one line (hold for continuous)\n");
	tty_puts("  Up / Down         previous / next command\n");
	tty_puts("  Ctrl-C / Ctrl-V   copy / paste selection  Ctrl-Backspace word\n");
	tty_puts("  F5 play/pause  F6 stop  F11/F12 volume\n");
	tty_set_color(TTY_COL_FG);
}

/** Long help for one command. Aliases: tempo/clock/bpm → seq, volume → vol. */
int help_topic(const char *name)
{
	unsigned i;
	if (name == NULL || name[0] == '\0') {
		help_list();
		return 1;
	}
	if (strcmp(name, "audiohelp") == 0) {
		name = "audio";
	}
	if (strcmp(name, "tempo") == 0 || strcmp(name, "clock") == 0 || strcmp(name, "bpm") == 0) {
		name = "seq";
	}
	if (strcmp(name, "volume") == 0) {
		name = "vol";
	}
	if (strcmp(name, "pause") == 0) {
		name = "audio";
	}
	for (i = 0; rows[i].name; i++) {
		if (strcmp(rows[i].name, name) == 0) {
			tty_printf("%s — %s\n", rows[i].name, rows[i].brief);
			tty_set_color(TTY_COL_DIM);
			tty_puts(rows[i].detail);
			tty_set_color(TTY_COL_FG);
			return 1;
		}
	}
	return 0;
}
