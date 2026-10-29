# libra-lvgl

Libra with its screens drawn by [LVGL](https://lvgl.io) 9.5 instead of Libra's
own software renderer. Everything below the screen is Libra's: playback,
library, podcasts, audiobooks, Bluetooth, settings and so on come from
`music-app/app` unchanged. Only what is drawn, and how touch reaches it, is
new.

- `lvgl-ui/`: the LVGL side.
  - `src/ui_*.c`: the screens.
  - `src/r1_display.c`: the R1's framebuffer pages as LVGL's draw buffers.
  - `lv_conf.h`: LVGL's configuration.
  - `Makefile`: the build.
- `music_hook.c`: Libra's `music-app/app/music_hook.c` with its framebuffer
  drawing and touch reading swapped for LVGL's under `#ifdef LIBRA_LVGL`. It
  `#include`s `lvgl-ui/src/ui_libra.c` at its end, so the screens read
  Libra's own state directly. Every other file in `app/` is identical to
  `music-app/app`.

## Building

You need `zig` (it is the cross compiler, `zig cc -target
mipsel-linux-gnueabihf.2.22`), `git`, and SQLite's amalgamation
(`sqlite3.c`/`sqlite3.h`) in `app/vendor/`, as for `music-app`.

```sh
cd libra-lvgl
cp -R ../music-app/app app          # Libra's engine...
cp music_hook.c app/music_hook.c    # ...with the LVGL hook in place of the framebuffer one
cd lvgl-ui
make lvgl                           # fetches LVGL v9.5.0 into lvgl/
make app                            # -> library_standalone (and .debug)
```

`library_standalone` replaces Libra's binary of the same name: on the R1 at
`/usr/data/library_standalone`, or in a firmware image through
`tools/patch_firmware.py --embed-binary`.

LVGL is MIT-licensed; see `THIRD_PARTY.md` at the top of this repository.
