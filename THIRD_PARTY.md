# Third-party code and credits

## Vendored sources

| Path | Project | Licence |
|---|---|---|
| `music-app/app/vendor/dr_flac.h`, `dr_mp3.h`, `dr_wav.h`, `dr_impl.c` | [dr_libs](https://github.com/mackron/dr_libs) by David Reid | Public domain / MIT-0 |
| `music-app/app/vendor/sqlite3.c`, `sqlite3.h` | [SQLite](https://www.sqlite.org/) | Public domain |
| `music-app/app/vendor/ogg/*` | [libogg](https://www.xiph.org/ogg/) (Xiph.Org) | BSD-style |
| `music-app/app/vendor/opus/*.h` | [libopus](https://opus-codec.org/) (Xiph.Org / IETF) | BSD-style |
| `music-app/app/vendor/vorbis/codec.h` | [libvorbis](https://xiph.org/vorbis/) (Xiph.Org) | BSD-style |
| `music-app/app/vendor/stb_truetype.h` | [stb](https://github.com/nothings/stb) by Sean Barrett | Public domain / MIT |
| `music-app/app/vendor/miniz/miniz.c`, `miniz.h` | [miniz](https://github.com/richgel999/miniz) by Rich Geldreich | MIT |
| `music-app/app/vendor/jpeg9/*.h` | [libjpeg 9](https://ijg.org/) by the Independent JPEG Group | IJG licence |
| `music-app/tools/icons/*.svg` (except below) | [Font Awesome Free](https://fontawesome.com) by Fonticons, Inc. | CC BY 4.0 (icons) |
| `music-app/tools/icons/home-*.svg`, `lucide-*.svg` | [Lucide](https://lucide.dev) by Lucide Contributors | ISC |

(This app used to share the device with a separate standalone Podcasts app,
retired once Podcasts was absorbed into this one directly — it vendored its
own copy of minimp3 for MP3 decoding, which went with it. This app's own MP3
decoding has always gone through `dr_mp3.h` above.)

The Font Awesome SVGs are source assets, not shipped as SVG: they are
rasterized offline by `music-app/tools/icons/gen_icons.py` into 8-bit
alpha-coverage bitmaps baked into `music-app/app/icons_data.h`, which *is*
compiled into the app (the device has no SVG renderer). CC BY 4.0 requires
attribution for a derivative work — this notice is it.

The Lucide icons (the home screen's section icons and the podcast sync
loader, tick and cross) go the same way, rasterized into `icons_data.h`;
`lucide-loader-*.svg` are rotated copies of Lucide's loader. Lucide's ISC
licence asks that its notice travel with copies: copyright (c) for portions
of Lucide are held by Cole Bemis 2013-2022 as part of Feather (MIT); all
other copyright (c) for Lucide are held by Lucide Contributors 2022.
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies. THE
SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES.

The Opus and Vorbis vendoring is **headers only**, the same pattern as the
libjpeg headers below: the R1's firmware provides the actual decode libraries
at runtime and they are `dlopen`'d, not linked. Only `libogg`'s bitstream
framing (`framing.c`) is compiled in directly, to demux the container without
a runtime dependency on a system libogg.

The libjpeg headers are **headers only**. No IJG code is compiled into or
shipped with this project — the R1's firmware already provides
`/usr/lib/libjpeg.so.9`, and the library is `dlopen`'d at runtime. The headers
are vendored because the device ships the library without them, and
`jpeg_CreateDecompress` validates `sizeof(struct jpeg_decompress_struct)` and
refuses to run if the caller's idea of the layout disagrees.

**This software is based in part on the work of the Independent JPEG Group.**

Those headers say "for conditions of distribution and use, see the accompanying
README file", and that README is not vendored here, so the conditions are
reproduced instead:

> The authors make NO WARRANTY or representation, either express or implied,
> with respect to this software, its quality, accuracy, merchantability, or
> fitness for a particular purpose. This software is provided "AS IS", and you,
> its user, assume the entire risk as to its quality and accuracy.
>
> Permission is hereby granted to use, copy, modify, and distribute this
> software (or portions thereof) for any purpose, without fee, subject to these
> conditions: (1) If any part of the source code for this software is
> distributed, then this README file must be included, with this copyright and
> no-warranty notice unaltered; and any additions, deletions, or changes to the
> original files must be clearly indicated in accompanying documentation.
> (2) If only executable code is distributed, then the accompanying
> documentation must state that "this software is based in part on the work of
> the Independent JPEG Group". (3) Permission for use of this software is
> granted only if the user accepts full responsibility for any undesirable
> consequences; the authors accept NO LIABILITY for damages of any kind.

The vendored headers are unmodified from libjpeg 9.

## Not shipped, but required at runtime

- **A CA bundle.** The installation instructions point at Mozilla's, as
  published by the curl project (MPL 2.0). It is downloaded by the user, not
  redistributed here.
- **A static `curl`.** Supplied by the user; not redistributed here.
- **`msyh.ttf`** (Microsoft YaHei), read from the device's own firmware at
  runtime for text rendering. It is proprietary and is deliberately **not**
  copied into this repository or any release.

## Credits

**[yetisoldier/Hiby-R1-Audiobook-Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod)**
— this app runs on top of that firmware mod, and several device findings it
established are load-bearing here: that a launcher tile can be re-pointed by
`LD_PRELOAD`ing into `hiby_player`, and that the tile callback must not point
into the injected object itself.

`tools/tap.py` deserves a more precise statement than "inspired by". It was
written against that project's `tools/adb_inject_touch_event.py` and follows its
structure: the same `abs_frame` decomposition and the same eight-frame press.
The `struct.pack("<llHHl", …)` layout is the Linux `input_event` ABI and the
frame count was determined empirically, so both are facts rather than authorship
— but the shape of the code is theirs, and it is fairer to call it derived than
independent.

**That project ships no licence file, so its terms are unstated.** Nothing here
redistributes any of its code or firmware, and `tap.py` is a development helper
rather than part of the app, but anyone reusing that file should know where it
came from. If yetisoldier would like it removed, rewritten, or licensed
differently, please open an issue and it will be dealt with.

**[bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod)** — surveyed
early on while working out how the firmware image is packed.

## Not included

HiBy's own resources — the stock launcher icons, the `.ini` string tables, and
the firmware itself — are not redistributed here. The tile icon is drawn from
scratch (`music-app/icon/make_icon.py`), and its label is produced at runtime
by rewriting a single string in whatever `settings.ini` the installed
firmware provides. Nothing in this repository is HiBy's.
