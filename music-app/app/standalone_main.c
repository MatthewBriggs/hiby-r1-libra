/* standalone_main.c — RP1 follow-on: run Libra as the only process, no
 * hiby_player at all. Calls music_entry() directly, bypassing the tile-hijack
 * trampoline entirely (there is no launcher to hijack a tile from).
 *
 * This is a live-test build, not the boot-replacement build: nothing here
 * touches the boot sequence or firmware. RP3 (where SD-card firmware
 * recovery actually lives) is still unanswered, and that question gates
 * anything more permanent than "run this by hand over adb, reboot to undo".
 */
#include <stdio.h>

#include "crash.h"

extern int music_entry(void *a0, void *a1);

/* music_entry() returns when go_back() hits the top of the navigation stack
 * (swipe-back or EXIT from the Main Menu) -- in the hooked build that hands
 * control back to the launcher; here there is no launcher, so the first
 * version of this just let the whole process end, leaving a frozen last
 * frame with nothing left running to draw or take input. Re-entering is the
 * standalone equivalent of "hand control back to the launcher": it's the
 * same menu you'd land on either way. */
int main(void) {
    crash_install();
    for (;;) {
        int rc = music_entry(0, 0);
        printf("[standalone] music_entry returned %d -- re-entering\n", rc);
    }
}
