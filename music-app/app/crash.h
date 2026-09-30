#ifndef CRASH_H
#define CRASH_H

/* A fatal signal writes a report to music.log, then Libra dies of it as
 * before and the supervisor restarts it. Standalone build only: the hooked
 * .so must not take over hiby_player's own signal handling. */
void crash_install(void);

/* Context for the report. Each copies (or points at) what the handler prints;
 * cheap enough to call on every track and every scanned file. */
void crash_note_track(const char *path);
void crash_note_scan(const char *path);
void crash_note_phase(volatile const char *const volatile *phase);

#endif
