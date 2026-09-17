#!/bin/sh
# RP1 follow-on: build Library as a standalone executable rather than the
# normal LD_PRELOAD .so -- same source, same flags where they still apply,
# just standalone_main.c instead of the shared-library entry points, and no
# -shared/-fPIC since this isn't a shared object.
#
# File list and SQLite flags now kept in lockstep with build.sh by hand --
# this had drifted badly (missing podcast.c/lastfm.c/spotify.c/mseb.c/
# recent.c/index.c/scanner.c/zlbm.c/miniz, and SQLITE_THREADSAFE=0, which
# index.c's and scanner.c's background scan threads need =2 for, exactly as
# build.sh's own comment explains) until this pass caught it up. No
# constructor guard needed for music_hook.c's tile-hijack code despite it
# being linked in here too: music_init()'s own is_hiby_player() check reads
# /proc/self/comm, which is "library_standalone" here, not "hiby"/
# "system_main" -- it already no-ops correctly with zero changes.
#
# Still a live-test build in the sense that running it is a manual step, not
# yet something the boot sequence does on its own -- see standalone_main.c
# and BACKLOG.md's RP1 entry for what a real boot-replacement build still
# needs on top of this binary.
set -e
zig cc -target mipsel-linux-gnueabihf.2.22 \
  -Wall -Wextra -Wno-unused-parameter \
  -O2 -s -fvisibility=hidden -fno-common -Ivendor \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_TEMP_STORE=2 \
  -o library_standalone standalone_main.c music_hook.c library.c audiobook.c podcast.c lastfm.c spotify.c audio.c waveform.c radio_buffer.c wsola.c eq.c eqprofile.c mseb.c recent.c cover.c art.c status.c radio.c playlist.c tags.c index.c scanner.c zlbm.c aac.c alac.c mp4.c ts.c hls.c ogg_io.c vorbis_dec.c opus_dec.c vendor/ogg/framing.c vendor/dr_impl.c vendor/miniz/miniz.c text.c vendor/sqlite3.c -lpthread -lm -ldl
ls -la library_standalone
