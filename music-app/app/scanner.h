/* scanner.h — RP6: a library index built by walking the SD card directly,
 * independent of hiby_player's own stock scanner. See scanner.c for why
 * this exists and how it stays out of the stock scanner's way while
 * hiby_player is still present. */
#ifndef SCANNER_H
#define SCANNER_H

/* Exported so library.c's lib_open() can fall back to it -- see that
 * function's own comment for when. */
#define SCANNER_DB_PATH "/usr/data/scanned_media.db"

/* Same shape as index.c's own trio, deliberately -- this is the same kind
 * of "Settings row with a progress readout" feature, so it gets the same
 * three-function interface rather than inventing a new one. */
void scanner_scan_start(void);
int  scanner_scan_running(void);
int  scanner_scan_progress(int *scanned, int *written);
void scanner_rescan_now(void);
void scanner_pause_for_usb(int paused);

#endif
