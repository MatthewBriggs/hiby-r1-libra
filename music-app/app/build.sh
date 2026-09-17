#!/bin/sh
# Cross-compile the music hook for the R1 (MIPS32 little-endian, glibc 2.22).
set -e
# -O2, not -Os. This is a decoder: dr_flac's residual and LPC loops are the
# hot path on a 1 GHz in-order MIPS with no SIMD, and -Os deliberately
# suppresses the loop optimisations they depend on to save a few hundred KB
# nobody is short of. The stock player handles 192/24 over USB without
# trouble on this same CPU, so the headroom exists and we were leaving it on
# the floor. Size cost is a one-off; the decode cost is paid continuously.
# -Ivendor: vendor/vorbis/codec.h pulls in <ogg/ogg.h> (Xiph's own header,
# unmodified) as an angle-bracket system include, so vendor/ needs to be on
# the search path for it to resolve to vendor/ogg/ogg.h.
# SQLITE_THREADSAFE=2 (multi-thread mode), not 0: index.c's background
# scanner needs its own SQLite connections, used concurrently with library.c's
# g_db on the UI thread. Multi-thread mode is safe for that -- a single
# connection still may never be shared across threads, which nothing here
# does -- whereas 0 strips all internal mutexing and makes any two threads
# touching SQLite at the same time undefined, even on different connections.
zig cc -target mipsel-linux-gnueabihf.2.22 \
  -Wall -Wextra -Wno-unused-parameter \
  -shared -fPIC -O2 -s -fvisibility=hidden -fno-common -Ivendor \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_TEMP_STORE=2 \
  -o libmusic_hook.so music_hook.c library.c audiobook.c podcast.c lastfm.c spotify.c audio.c waveform.c radio_buffer.c wsola.c eq.c eqprofile.c mseb.c recent.c cover.c art.c status.c radio.c playlist.c tags.c index.c scanner.c zlbm.c aac.c alac.c mp4.c ts.c hls.c ogg_io.c vorbis_dec.c opus_dec.c vendor/ogg/framing.c vendor/dr_impl.c vendor/miniz/miniz.c text.c vendor/sqlite3.c -lpthread -lm -ldl
ls -la libmusic_hook.so
