#!/usr/bin/env python3
# Rasterizes Font Awesome SVGs (pre-rendered to PNG via qlmanage) down to 8-bit
# alpha-coverage bitmaps and emits a C header. The device has no SVG or PNG
# decoder -- this is a one-time, dev-time step, mirroring how text.c's
# stb_truetype glyphs are stored: an 8-bit coverage array plus width/height,
# blended at draw time with blend() using a runtime color.
#
# Run from this directory. Requires the *.svg source files present here
# (Font Awesome Free) and macOS's qlmanage to rasterize them first, since
# qlmanage flattens onto opaque white with no real alpha channel -- coverage
# is recovered from luminance instead (see load_cropped()):
#
#   for f in *.svg; do qlmanage -t -s 512 -o . "$f" >/dev/null 2>&1; done
#   python3 gen_icons.py
#   cp icons_data.h ../../app/icons_data.h
from PIL import Image

# (family key, source png, C identifier, [(w, h, variant suffix), ...])
#
# Family key groups icons meant to swap for one another at the same on-screen
# spot (the 4 volume levels; the 5 battery levels). Every source in this set
# shares Font Awesome's 640x640 viewBox, confirmed by grepping the SVGs, so a
# family fit from one shared crop box keeps their common element (the speaker
# body, the battery outline) pixel-identical across states -- fitting each
# icon to its own individual content bbox, tried first, does not: FA trims
# each glyph's bbox to its own ink, which differs member to member (more sound
# waves is a wider bbox), so independent-fit scaling changed the size of the
# speaker itself between volume states, which is what this family grouping
# exists to prevent. Singletons (bt/usb/wifi) are families of one.
SPECS = [
    # Skip-back/skip-forward rings for the podcast/audiobook players,
    # replacing the old hand-drawn dots-along-an-arc approximation
    # (draw_skip_arc()). Family, not two singletons: these are mirror
    # images of each other and sit side by side around the same play
    # button, so they need the shared-crop-box treatment to come out the
    # same size -- an independent fit would size them off the individual
    # arrow's own bbox, which differs slightly left vs right.
    ("skiprot", "arrow-rotate-left-solid-full.svg.png",  "skip_back",    [(56, 56, "")]),
    ("skiprot", "arrow-rotate-right-solid-full.svg.png", "skip_forward", [(56, 56, "")]),
    ("bt",   "bluetooth-brands-solid-full.svg.png", "bt",   [(26, 38, "_qs"), (16, 16, "_sm")]),
    ("usb",  "usb-brands-solid-full.svg.png",       "usb",  [(16, 16, "_sm"), (32, 32, "_qs")]),
    ("wifi", "wifi-solid-full.svg.png",             "wifi", [(48, 34, "_qs")]),
    ("gear", "gear-solid-full.svg.png",             "gear", [(20, 20, "_qs")]),
    ("disc", "compact-disc-solid-full.svg.png",     "disc", [(28, 28, "")]),
    # Family, not three singletons: these three swap for one another at the
    # exact same on-screen spot (Now Playing's mode-cycle button), so they
    # need the shared-crop-box treatment the family mechanism exists for --
    # fit independently, bars' wide/flat bbox vs shuffle's/repeat's more
    # square ones would come out at visibly different sizes for what's
    # supposed to read as the same button in three states.
    ("mode", "bars-solid-full.svg.png",             "mode_off",     [(26, 26, "")]),
    ("mode", "repeat-solid-full.svg.png",           "mode_repeat",  [(26, 26, "")]),
    ("mode", "shuffle-solid-full.svg.png",          "mode_shuffle", [(26, 26, "")]),
    ("vol",  "volume-xmark-solid-full.svg.png",     "vol_mute", [(20, 20, "")]),
    ("vol",  "volume-low-solid-full.svg.png",       "vol_low",  [(20, 20, "")]),
    ("vol",  "volume-solid-full.svg.png",           "vol_mid",  [(20, 20, "")]),
    ("vol",  "volume-high-solid-full.svg.png",      "vol_high", [(20, 20, "")]),
    ("batt", "battery-empty-solid-full.svg.png",          "batt_0",  [(26, 14, "")]),
    ("batt", "battery-quarter-solid-full.svg.png",        "batt_25", [(26, 14, "")]),
    ("batt", "battery-half-solid-full.svg.png",           "batt_50", [(26, 14, "")]),
    ("batt", "battery-three-quarters-solid-full.svg.png", "batt_75", [(26, 14, "")]),
    ("batt", "battery-full-solid-full.svg.png",           "batt_100",[(26, 14, "")]),
    # Brightness glyph for the quick-settings slider, same left-of-bar spot
    # the volume popup's own icon_vol_* sits in. Font Awesome Free has no
    # graduated low/mid/high brightness family the way volume does (no
    # "brightness-low.svg" etc. exist) -- one glyph, themed/dimmed the same
    # way any other single-state icon in this app already is, rather than a
    # fabricated multi-shape family with no real source asset behind it.
    ("sun", "sun-solid-full.svg.png", "sun", [(20, 20, "")]),
    # Quick-settings row icon for the cover-colours toggle -- same 28x28
    # weight as the Wi-Fi/Bluetooth/EQ glyphs sharing that column.
    ("palette", "palette-solid-full.svg.png", "palette", [(28, 28, "_qs")]),
]

def load_full(path):
    # qlmanage flattens SVGs onto an opaque white background -- there is no
    # real alpha channel to read (checked: every corner and background pixel
    # comes back (255,255,255,255), fully opaque). Coverage is recovered from
    # luminance instead: these are solid black-on-white glyphs, so black =
    # full coverage, white = none, which is exactly backwards from a normal
    # alpha channel and why this cannot just reuse im.split()[3]. Returns the
    # full, uncropped canvas -- cropping happens later, per family.
    im = Image.open(path).convert("RGBA")
    r, g, b, _ = im.split()
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    coverage = lum.point(lambda p: 255 - p)
    return Image.merge("RGBA", (r, g, b, coverage))

out = []
out.append("/* icons_data.h -- generated by gen_icons.py from Font Awesome Free SVGs.")
out.append(" * Do not hand-edit; re-run the generator to change source assets or sizes.")
out.append(" * Included from icons.h, which defines icon_t and draw_icon() -- this file")
out.append(" * is data only. Each bitmap is 8-bit alpha coverage, identical in kind to")
out.append(" * text.c's glyph bitmaps: no RGB is stored, since source icons are solid")
out.append(" * single-color shapes and alpha alone is the shape. */")
out.append("#ifndef ICONS_DATA_H")
out.append("#define ICONS_DATA_H")
out.append("")

# Load every source once, grouped by family, then compute one shared crop
# box per family from the union of every member's own bbox in the SAME
# (uncropped, 640-viewBox) coordinate space.
loaded = {}   # ident -> (family, image, [(w,h,suffix)])
families = {}  # family -> [ident,...]
for family, src, ident, variants in SPECS:
    loaded[ident] = (family, load_full(src), variants)
    families.setdefault(family, []).append(ident)

crop_box = {}
for family, idents in families.items():
    boxes = [loaded[i][1].getbbox() for i in idents]
    boxes = [b for b in boxes if b]
    if not boxes:
        continue
    crop_box[family] = (min(b[0] for b in boxes), min(b[1] for b in boxes),
                         max(b[2] for b in boxes), max(b[3] for b in boxes))

names = []
for ident, (family, im, variants) in loaded.items():
    box = crop_box.get(family)
    cropped = im.crop(box) if box else im
    for w, h, suf in variants:
        # Fit the shared family box within (w,h) preserving aspect, then
        # center-pad. Every member of a family is scaled by the identical
        # factor here, since they all crop from the identical box size.
        iw, ih = cropped.size
        scale = min(w / iw, h / ih)
        rw, rh = max(1, round(iw * scale)), max(1, round(ih * scale))
        resized = cropped.resize((rw, rh), Image.LANCZOS)
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(resized, ((w - rw) // 2, (h - rh) // 2), resized)
        alpha = canvas.split()[3]
        data = list(alpha.getdata())
        name = f"icon_{ident}{suf}"
        out.append(f"static const uint8_t {name}_a[{w * h}] = {{")
        for row in range(h):
            rowvals = data[row * w:(row + 1) * w]
            out.append("    " + ",".join(str(v) for v in rowvals) + ",")
        out.append("};")
        out.append(f"static const icon_t {name} = {{ {w}, {h}, {name}_a }};")
        out.append("")
        names.append(name)

out.append("#endif")

with open("icons_data.h", "w") as f:
    f.write("\n".join(out) + "\n")

print("generated:", ", ".join(names))
