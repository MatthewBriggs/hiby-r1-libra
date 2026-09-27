/* btplayer.h — Libra as the Bluetooth media player (AVRCP target).
 *
 * A headset's play/pause button only works if the headset knows whether
 * anything is playing. Without a media player registered with BlueZ, nothing
 * tells it: BlueZ turns its button presses into key events (the AVRCP input
 * device handle_keys() reads) but has no play status to report back, so a
 * headset that keeps its own idea of the state -- the FiiO BTR17 does, and
 * starts out assuming "paused" -- sends PLAY while music is playing, which
 * Libra rightly ignores. This registers Libra with BlueZ (org.bluez.Media1
 * RegisterPlayer, the MPRIS player interface BlueZ 5.54 bridges to AVRCP), so
 * the headset is told the real playback status and track as they change, and
 * its play/pause/next/previous arrive as calls here instead of as keys.
 *
 * libdbus is loaded at run time; if it or BlueZ is missing, nothing is
 * registered and the AVRCP input device keeps working exactly as before.
 * Everything D-Bus happens on this module's own thread. */
#ifndef BTPLAYER_H
#define BTPLAYER_H

#include <stdint.h>

/* Starts the thread. Safe to call more than once. `log` gets a line per
 * registration and per command; NULL for silence. */
void btplayer_start(void (*log)(const char *fmt, ...));

enum { BTP_STOPPED = 0, BTP_PLAYING, BTP_PAUSED };

/* What is playing now, from the UI thread -- cheap enough to call every
 * frame: it only compares, and passes a change on to the D-Bus thread when
 * there is one. Strings may be "" but not NULL. */
void btplayer_update(int status, const char *title, const char *artist,
                     const char *album, int64_t length_ms, int track_no);

/* What a Bluetooth device asked for, oldest first: one of these, or
 * BTP_CMD_NONE when nothing is waiting. Polled from the UI thread. */
enum { BTP_CMD_NONE = 0, BTP_CMD_PLAY, BTP_CMD_PAUSE, BTP_CMD_PLAYPAUSE,
       BTP_CMD_STOP, BTP_CMD_NEXT, BTP_CMD_PREVIOUS };
int btplayer_take_command(void);

#endif
