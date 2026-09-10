# HiBy R1 — Libra

A music, radio and podcast app for the HiBy R1, replacing the stock **About**
tile. It exists because the stock browser lists an album artist's *tracks*
rather than their *albums* — pick Berliner Philharmoniker and you get hundreds
of individual pieces instead of the sixteen albums they belong to.

No images in the lists, one screen of information at a time, and the lists
scroll properly.

## What it does

- **Browse** by album artist, album, artist or genre, straight out of the
  stock media index — no second scan of the card.
- **Gapless** between tracks: the output device is never closed at a boundary,
  so continuous recordings stay continuous.
- **Play** FLAC, MP3, WAV and M4A — the whole library. M4A goes through an MP4
  demuxer feeding the device's own libfdk-aac.
- **Now Playing** with large album art and no title bar, showing track
  position, elapsed and remaining, the format, bit depth, sample rate and bit
  rate, the output route, and a queue control in the corner.
- **Mini player** on every list screen with the track, artist and a pause
  button.
- **Queue** reachable from Now Playing, with the playing track marked. It is a
  copy of the album it was started from, so browsing elsewhere does not rename
  what is playing.
- **Internet radio** from a plain text station list: direct MP3 streams, and
  HLS playlists whose MPEG-TS segments are demuxed and decoded as AAC.
- **Output** to the 3.5 mm jack, a USB DAC or Bluetooth, picked automatically
  and shown on the Now Playing footer. Wired and USB are opened bit-perfect:
  `hw:` at the source's own rate and depth, with `plughw:` only as a fallback.
  On Bluetooth the footer shows the codec in use and the headset's battery
  where it reports one.
- **Hardware keys**: volume, skip and seek. Note that the unit's own buttons do
  not report what their labels say — the one marked skip-forward sends
  PLAYPAUSE and skip-back sends NEXTSONG — so the buttons and the inline remote
  are mapped separately. This unit has no play/pause button; that control is on
  screen.
- **Screen lock** on the power button, and after an idle timeout.
- **Quick settings**: pull down from the status strip for brightness, Wi-Fi and
  Bluetooth, driven through the firmware's own scripts.
- **Swipe in from the left edge** to go back, including out of the player. A
  sliver at the edge follows your finger and warms as the swipe passes the
  point where letting go will register.
- **Press and hold a track** to play it next, add it to the queue, or add it to
  a playlist.
- **Playlists** as plain `.m3u` files in `/data/mnt/sd_0/Playlists`, holding
  absolute paths so anything else can read and write them. A `Favourites` list
  is created on first use.

## Configuration

Both files are plain text and are created on first run.

`/usr/data/music.conf`

```
idle_lock_seconds = 30      # 0 disables the automatic lock
```

`/usr/data/radio_stations.conf`

```
Name | https://example.com/stream.mp3
```

Both direct MP3 streams and HLS (`.m3u8`) play; for HLS the highest-bandwidth
variant is chosen, since this is a player attached to a DAC.

On the BBC: their formerly public HLS endpoints now return 410 Gone, and the
remaining way to discover a stream URL is an endpoint that describes itself as
part of a content protection system — so this app does not go looking there.
Give it a URL and it plays it like any other; the player has no opinion about
where a URL came from.

## Install

Build, then copy onto the device:

```
app/build.sh
adb push app/libmusic_hook.so /usr/data/libpodcast_hook.so
adb push icon/about.png /usr/data/music_res/about.png
adb push icon/about_s.png /usr/data/music_res/about_s.png
```

The firmware preloads exactly one library, and that slot is
`/usr/data/libpodcast_hook.so` (a legacy name that predates this app
absorbing Podcasts — the device only looks at the path, so it's left as-is
rather than renamed for cosmetics).

Always check the object loads before rebooting; a link error otherwise costs a
boot cycle:

```
adb shell 'LD_PRELOAD=/usr/data/libpodcast_hook.so /bin/true'
```

## Notes for anyone reading the source

Three things cost real time and are worth knowing:

- The device's **libsndfile is built without libFLAC** — it links libc and libm
  and nothing else — so it rejects FLAC, 83% of the library. The bundled
  public-domain `dr_libs` decoders are used instead.
- The index stores paths in the player's own volume notation,
  `a:\Artist\Album\01 Title.flac`. Nothing opens until that is translated.
- Opening the PCM without sizing the buffer, setting a start threshold and
  calling `prepare` looks fine and then parks forever in `wait_for_avail`
  about eleven seconds in: the stream never starts, so nothing drains.
- This device's own `libjpeg` (dlopened by `cover.c`, not bundled) has a
  confirmed decode defect: for some source JPEGs, an interior band of
  scanlines comes back as literal zero bytes while `jpeg_read_scanlines`
  still reports success and raises no warning — seen both as an undersized
  embedded thumbnail returning blank interior rows (`cover_load()`'s own
  size-decline guard) and, separately, as a full-size album cover
  decoding correctly on top but leaving the rest a flat grey band. Worked
  around rather than fixed: a run of many consecutive byte-identical
  all-zero scanlines is essentially impossible from genuine compressed
  image data (DCT/quantization noise alone rules it out, even over a
  legitimately near-black region), so `cover_load()` treats a long enough
  run as this defect and fails the decode outright, falling through to
  the next art candidate. A real fix would mean building and bundling our
  own libjpeg for this target instead of dlopening the device's — not
  attempted here (a MIPS cross-compile + packaging job of its own), but
  the toolchain used for this project's kernel work (`mips-linux-gnu-gcc`,
  see `hiby-kernel-build`) could build one if this is ever worth doing
  properly.

## Licence

MIT. Bundles `dr_flac`, `dr_mp3`, `dr_wav` and SQLite, all public domain.
