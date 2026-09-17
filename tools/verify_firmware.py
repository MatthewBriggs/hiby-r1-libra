#!/usr/bin/env python3
"""Check a HiBy R1 .upt is internally consistent, and say what is in it.

Run this on anything patch_firmware.py produces before flashing it. A bad
image is not a bad build, it is a device that does not come back, so every
digest the updater checks is checked here first.

    ./verify_firmware.py r1-podcast-2.0.26.upt
    ./verify_firmware.py r1-podcast-2.0.26.upt --against r1-audiobooks-2.0.26.upt

With --against, the two images are compared file by file inside the rootfs, so
you can confirm the patch changed exactly what it claimed to and nothing else.
"""

import argparse
import hashlib
import io
import os
import re
import subprocess
import sys
import tempfile

CHUNK = 524288
SCRIPT = "usr/bin/hiby_player.sh"
MOUNT_SCRIPT = "usr/bin/mount_ubifs.sh"
BT_INIT = "usr/bin/bt_init"
CONFIG_JSON = "usr/resource/config.json"
VERSION_FILE = "etc/r1_audiobook_version"
EMBED_BINARY = "usr/lib/libra/library_standalone"   # see patch_firmware.py --embed-binary
SET_FUNCTIONS_FILES = ("usr/resource/set_functions.json",
                       "usr/resource/midi_set_functions.json")
HGL_SCRIPT = "module_driver/sa_hgl_dma.sh"
MODULE_INIT_SCRIPT = "module_driver/driver_default_init_script.sh"
ADB_INIT_SCRIPT = "etc/init.d/adb/S440adb"
BRCMFMAC_FILES = ("lib/firmware/brcm/brcmfmac43430-sdio.bin",
                  "lib/firmware/brcm/brcmfmac43430-sdio.txt")
# What --brcmfmac-switch itself produces, beyond the firmware blobs above: the
# mainline driver and its loader scripts in, the cywdhd blob out, bt_init moved
# to S12 (after the module script brings up the rail and the LPO), and the two
# wifi scripts rewritten to rmmod/insmod the driver.
#
# Without these the custom-kernel build could not be checked with --against at
# all -- every one of them read as an unexplained difference, so 0.52/0.52b
# verified that image by hand checklist instead. That is the exact check a
# machine should be doing: the 0.51.2 image that shipped and would not boot was
# a rootfs assembled outside this pipeline, which a base-diff would have caught
# immediately.
BRCMFMAC_SWITCH_ADDED = ("module_driver/brcmfmac.ko", "module_driver/brcmutil.ko",
                         "module_driver/r1_wlan_up.ko", "module_driver/brcmfmac.sh",
                         "module_driver/wifi_late.sh",
                         "etc/init.d/S12_bt_init", "etc/init.d/S91wifi")
BRCMFMAC_SWITCH_REMOVED = ("module_driver/cywdhd.ko", "module_driver/cywdhd.sh")
WIFI_SCRIPTS = ("usr/bin/wifi_on.sh", "usr/bin/wifi_off.sh",
                # soc_msc gets wifi_reg_on=PB03 from the same switch.
                "module_driver/soc_msc.sh")
# Mirrors patch_firmware.py's own UNUSED_FONTS/UNUSED_BINARIES/LITEGUI_DIR --
# strip_unused_resources() deletes these unconditionally on any standalone
# build (not gated on --brcmfmac-switch), so a --against diff run against a
# vanilla base legitimately shows them all as removed. Kept as a duplicate
# set rather than importing patch_firmware.py, matching how the other
# constants above (SET_FUNCTIONS_FILES, BRCMFMAC_FILES) are already done.
STRIPPED_FONTS_AND_BINARIES = ("usr/resource/fonts/default.otf",
                               "usr/resource/fonts/Thai.ttf",
                               "usr/bin/hiby_player", "usr/bin/dmrd")
LITEGUI_DIR = "usr/resource/litegui/"


def detect_format(path):
    """'mod' (the D0000001/... layout every prior release used) or 'stock'
    (the OTA_V0/... layout vanilla 1.6 uses) -- see patch_firmware.py's own
    detect_format() for how this was confirmed against a real stock image.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(path)
    fmt = None
    for iso_path, name in (("/D0000001", "mod"), ("/OTA_V0", "stock")):
        try:
            next(iso.list_children(iso_path=iso_path))
            fmt = name
            break
        except Exception:
            continue
    iso.close()
    return fmt


def load(path):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(path)

    def rd(p):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=p)
        return b.getvalue()

    manifest = rd("/D0000001/F0000004.TXT;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    # Chunk order is not manifest order, so take the layout from the digest
    # lists and match each assembled image to its manifest entry by md5.
    listed = [rd("/D0000001/F0000002.BIN;1").decode().split(),
              rd("/D0000001/F0000003.BIN;1").decode().split()]
    by_md5 = {md5: (t, name, int(size)) for t, name, size, md5 in entries}

    images, n, chunk_digests = [], 6, []
    for lst in listed:
        parts = [rd(f"/D0000001/F{i:07d}.BIN;1") for i in range(n, n + len(lst))]
        data = b"".join(parts)
        chunk_digests.append([hashlib.md5(p).hexdigest() for p in parts])
        got = hashlib.md5(data).hexdigest()
        # The manifest is right about this image only if the bytes we actually
        # assembled hash to something it declares.
        img_type, name, size = by_md5.get(got, ("unknown", "unmatched", -1))
        images.append({"type": img_type, "name": name, "size": size,
                       "matched": got in by_md5, "data": data, "first": n})
        n += len(lst)
    version = rd(f"/F{n:07d}.TXT;1")
    iso.close()
    return images, chunk_digests, listed, version, n


def check_structure(path, images, chunk_digests):
    """Validate the parts a checksum test cannot see.

    An image can have every digest correct and still be unreadable to the
    device. The stock images carry Joliet and Rock Ridge, and those extensions
    hold the *real* filenames — the F00000NN.BIN names are 8.3 fallbacks. Those
    real names encode a chain: index 0000 carries the digest of the whole image
    and each later index carries the digest of the chunk before it. An image
    built without any of this verifies perfectly here and then hangs the
    updater on "Upgrading..." forever, which is exactly what happened once.
    """
    import pycdlib
    iso = pycdlib.PyCdlib(); iso.open(path)
    problems = []

    if not iso.has_joliet():
        problems.append("no Joliet extension")
    if not iso.has_rock_ridge():
        problems.append("no Rock Ridge extension")

    def rr(rec):
        return rec.rock_ridge.name().decode() if rec.rock_ridge and rec.rock_ridge.name() else ""

    root = {}
    for c in iso.list_children(iso_path="/"):
        n = c.file_identifier().decode(errors="replace")
        if n not in (".", ".."):
            root[n] = rr(c)
    if root.get("D0000001") != "ota_v0":
        problems.append(f"payload dir is {root.get('D0000001')!r}, expected 'ota_v0'")
    if "ota_config.in" not in root.values():
        problems.append("no ota_config.in at the root")

    names = []
    for c in iso.list_children(iso_path="/D0000001"):
        n = c.file_identifier().decode(errors="replace")
        if n not in (".", ".."):
            names.append(rr(c))
    for want in ("ota_update.in", "ota_v0.ok"):
        if want not in names:
            problems.append(f"missing {want}")

    # The chain: <image>.<index>.<digest of the previous item>.
    chunk_names = [n for n in names if re.match(r".+\.\d{4}\.[0-9a-f]{32}$", n)]
    expected = []
    for i, img in enumerate(images):
        whole = hashlib.md5(img["data"]).hexdigest()
        for k in range(len(chunk_digests[i])):
            digest = whole if k == 0 else chunk_digests[i][k - 1]
            expected.append(f"{img['name']}.{k:04d}.{digest}")
    if chunk_names != expected:
        bad = next((a for a, b in zip(chunk_names, expected) if a != b), None)
        problems.append(f"chunk name chain wrong (first bad: {bad or 'count mismatch'})")

    first = iso.get_record(iso_path="/D0000001/F0000006.BIN;1").extent_location()
    iso.close()
    return problems, first


def load_ota_v0(path):
    """Stock/vanilla layout counterpart to load(). See patch_firmware.py's
    read_images_ota_v0() for the full account: manifest at
    OTA_V0/OTA_UPDA.IN, per-image ordered chunk-digest lists at
    OTA_V0/OTA_MD5_.<xxx> (xxx = the image's own md5, first 3 hex chars,
    uppercase), chunk files at OTA_V0/ROOTFS_S.<xxx> / OTA_V0/XIMAGE_0.<xxx>
    matched to the digest list by each chunk's own real md5.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(path)

    def rd(p):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=p)
        return b.getvalue()

    def list_dir(p):
        return sorted(c.file_identifier().decode()
                     for c in iso.list_children(iso_path=p)
                     if c.file_identifier() not in (b'.', b'..'))

    manifest = rd("/OTA_V0/OTA_UPDA.IN;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    by_md5 = {md5: (t, name, int(size)) for t, name, size, md5 in entries}

    names = list_dir("/OTA_V0")
    chunk_files = [n for n in names if n.startswith("ROOTFS_S.") or n.startswith("XIMAGE_0.")]
    by_own_md5 = {}
    for n in chunk_files:
        data = rd(f"/OTA_V0/{n}")
        by_own_md5[hashlib.md5(data).hexdigest()] = data

    images, chunk_digests, listed = [], [], []
    for img_type, name, size, md5 in entries:
        prefix = md5[:3].upper()
        dfile = next((n for n in names if n.upper().startswith(f"OTA_MD5_.{prefix}")), None)
        digest_list = rd(f"/OTA_V0/{dfile}").decode().split() if dfile else []
        parts = [by_own_md5.get(d, b"") for d in digest_list]
        data = b"".join(parts)
        got = hashlib.md5(data).hexdigest()
        img_type2, name2, size2 = by_md5.get(got, ("unknown", "unmatched", -1))
        images.append({"type": img_type2, "name": name2, "size": size2,
                       "matched": got in by_md5, "data": data, "first": None})
        chunk_digests.append([hashlib.md5(p).hexdigest() for p in parts])
        listed.append(digest_list)
    version = rd("/OTA_CONF.IN;1")
    iso.close()
    return images, chunk_digests, listed, version, None


def check_structure_ota_v0(path, images, chunk_digests):
    """OTA_V0 counterpart to check_structure() -- same idea (checksums alone
    don't prove the updater can navigate the image), adapted to this format's
    own directory (OTA_V0, not D0000001) and fixed per-type chunk base names
    (ROOTFS_S/XIMAGE_0, not the image's own real filename) -- see
    patch_firmware.py's write_upt_ota_v0() for where those were confirmed
    against a real stock image.
    """
    import pycdlib
    iso = pycdlib.PyCdlib(); iso.open(path)
    problems = []

    if not iso.has_joliet():
        problems.append("no Joliet extension")
    if not iso.has_rock_ridge():
        problems.append("no Rock Ridge extension")

    def rr(rec):
        return rec.rock_ridge.name().decode() if rec.rock_ridge and rec.rock_ridge.name() else ""

    root = {}
    for c in iso.list_children(iso_path="/"):
        n = c.file_identifier().decode(errors="replace")
        if n not in (".", ".."):
            root[n] = rr(c)
    if root.get("OTA_V0") != "ota_v0":
        problems.append(f"payload dir is {root.get('OTA_V0')!r}, expected 'ota_v0'")
    if "ota_config.in" not in root.values():
        problems.append("no ota_config.in at the root")

    names = []
    for c in iso.list_children(iso_path="/OTA_V0"):
        n = c.file_identifier().decode(errors="replace")
        if n not in (".", ".."):
            names.append(rr(c))
    for want in ("ota_update.in", "ota_v0.ok"):
        if want not in names:
            problems.append(f"missing {want}")

    chunk_names = [n for n in names if re.match(r".+\.\d{4}\.[0-9a-f]{32}$", n)]
    expected = []
    for i, img in enumerate(images):
        whole = hashlib.md5(img["data"]).hexdigest()
        for k in range(len(chunk_digests[i])):
            digest = whole if k == 0 else chunk_digests[i][k - 1]
            expected.append(f"{img['name']}.{k:04d}.{digest}")
    if sorted(chunk_names) != sorted(expected):
        missing = set(expected) - set(chunk_names)
        extra = set(chunk_names) - set(expected)
        problems.append(f"chunk name chain wrong "
                        f"({len(missing)} expected missing, {len(extra)} unexpected present)")

    iso.close()
    return problems


def unpack_rootfs(data, dest):
    sq = os.path.join(dest, "r.squashfs")
    with open(sq, "wb") as fh:
        fh.write(data)
    root = os.path.join(dest, "root")
    subprocess.run(["unsquashfs", "-d", root, "-q", sq],
                   check=True, stdout=subprocess.DEVNULL)
    return root


def tree(root):
    out = {}
    for dp, _, fn in os.walk(root, followlinks=False):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root)
            if os.path.islink(p):
                out[rel] = ("link", os.readlink(p))
            else:
                with open(p, "rb") as fh:
                    out[rel] = ("file", hashlib.md5(fh.read()).hexdigest())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--against", help="original .upt to diff the rootfs against")
    args = ap.parse_args()

    try:
        import pycdlib  # noqa: F401
    except ImportError:
        sys.exit("error: pycdlib not installed — pip install pycdlib")

    fmt = detect_format(args.image)
    if fmt is None:
        sys.exit(f"error: unrecognised .upt layout in {args.image} -- "
                 f"neither /D0000001 (mod) nor /OTA_V0 (stock) found")
    if fmt == "mod":
        images, chunk_digests, listed, version, last = load(args.image)
    else:
        images, chunk_digests, listed, version, last = load_ota_v0(args.image)
    ok = True

    print(f"{args.image}  ({os.path.getsize(args.image)} bytes)  [{fmt} layout]\n")
    for i, img in enumerate(images):
        size_ok = len(img["data"]) == img["size"]
        chunks_ok = chunk_digests[i] == listed[i]
        ok &= size_ok and img["matched"] and chunks_ok
        print(f"  {img['type']:8s} {img['name']}")
        print(f"    size          {len(img['data'])} {'OK' if size_ok else 'MISMATCH'}")
        print(f"    manifest md5  {'OK' if img['matched'] else 'MISMATCH'}")
        from_str = f" from F{img['first']:07d}" if img['first'] is not None else ""
        print(f"    chunk digests {'OK' if chunks_ok else 'MISMATCH'} "
              f"({len(chunk_digests[i])} chunks{from_str})")
    if fmt == "mod":
        print(f"\n  version entry   F{last:07d}.TXT = {version!r}")
    else:
        print(f"\n  version entry   OTA_CONF.IN = {version!r}")

    if fmt == "mod":
        problems, first_extent = check_structure(args.image, images, chunk_digests)
    else:
        problems, first_extent = check_structure_ota_v0(args.image, images, chunk_digests), None
    print(f"\n  structure       Joliet + Rock Ridge, chunk-name chain")
    if first_extent is not None:
        print(f"    first chunk extent {first_extent}"
              f"{'' if first_extent == 51 else '  (stock images use 51)'}")
    if problems:
        ok = False
        for p in problems:
            print(f"    PROBLEM: {p}")
    else:
        print("    OK - matches the layout the updater expects")

    rootfs = next(i for i in images if i["type"] == "rootfs")
    with tempfile.TemporaryDirectory(prefix="r1verify-") as tmp:
        root = unpack_rootfs(rootfs["data"], tmp)
        script = os.path.join(root, SCRIPT)
        if os.path.exists(script):
            with open(script) as fh:
                body = fh.read()
            has = "DEV_HOOK" in body
            standalone = "BINARY=" in body and "DEV_HOOK" not in body
            healthy = "HEALTHY_RUN" in body
            print(f"\n  {SCRIPT}")
            if standalone:
                print(f"    RP1 standalone launcher    yes (no hiby_player)")
            else:
                print(f"    loads libpodcast_hook.so   {'yes' if has else 'NO'}")
            print(f"    consecutive-crash fix      {'yes' if healthy else 'NO'}")
            # Either shape is a legitimate supervisor -- DEV_HOOK for the
            # LD_PRELOAD-into-hiby_player build, BINARY= for --standalone's
            # own full replacement (see build_standalone_supervisor() in
            # patch_firmware.py). Only neither is a real problem.
            ok &= (has or standalone)
            if standalone and "SEED_BINARY=" in body:
                embed_path = os.path.join(root, EMBED_BINARY)
                if os.path.exists(embed_path):
                    size = os.path.getsize(embed_path)
                    print(f"    embedded binary            yes ({EMBED_BINARY}, {size} bytes)")
                else:
                    print(f"    embedded binary            NO -- supervisor "
                          f"expects one at /{EMBED_BINARY} but it isn't in "
                          f"the image (a fresh device would boot with "
                          f"nothing to seed /usr/data with)")
                    ok = False
        else:
            print(f"\n  {SCRIPT} MISSING")
            ok = False

        vf = os.path.join(root, VERSION_FILE)
        if os.path.exists(vf):
            with open(vf) as fh:
                for line in fh:
                    if line.startswith(("podcast_rom", "label=", "version=")):
                        print(f"    {line.strip()}")

        if args.against:
            against_fmt = detect_format(args.against)
            if against_fmt is None:
                sys.exit(f"error: unrecognised .upt layout in {args.against}")
            loader = load if against_fmt == "mod" else load_ota_v0
            other, *_ = loader(args.against)
            other_rootfs = next(i for i in other if i["type"] == "rootfs")
            with tempfile.TemporaryDirectory(prefix="r1orig-") as tmp2:
                oroot = unpack_rootfs(other_rootfs["data"], tmp2)
                a, b = tree(oroot), tree(root)
                changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
                added = sorted(set(b) - set(a))
                removed = sorted(set(a) - set(b))
                print(f"\n  vs {args.against}:")
                print(f"    {len(a)} files -> {len(b)} files")
                for k in changed:
                    print(f"    ~ {k}")
                for k in added:
                    print(f"    + {k}")
                for k in removed:
                    print(f"    - {k}")
                expected = {SCRIPT, MOUNT_SCRIPT, CONFIG_JSON, VERSION_FILE,
                            BT_INIT, HGL_SCRIPT, MODULE_INIT_SCRIPT,
                            ADB_INIT_SCRIPT, "usr/bin/adboff", "usr/bin/adbon",
                            # enable_rtc32k_at_boot() flips rtc32k_init_on=1
                            # here (enables the 32kHz LPO at module load) --
                            # part of the same boot-timing patch set as
                            # BT_INIT/MODULE_INIT_SCRIPT above, just missed
                            # when this allowlist was last updated.
                            "module_driver/soc_utils.sh"}
                expected |= set(SET_FUNCTIONS_FILES)
                expected |= set(WIFI_SCRIPTS)   # --brcmfmac-switch rewrites both
                # kernel_build_id: --kernel-build-id re-stamps this file, which
                # shows up as "changed" (not "added") whenever the base image
                # already carried a stamp from an earlier pass -- both are
                # legitimate, so it belongs in both sets rather than only
                # expected_added below.
                expected |= {"usr/resource/kernel_build_id"}
                # Internet radio swaps each theme's Stream media layout for
                # HiBy's own CN variant, which is the one carrying the tile.
                expected |= {f"{d}/hiby_stream_media.view" for d in (
                    "usr/resource/layout/theme1",
                    "usr/resource/layout/theme2",
                    "usr/resource/layout/midi/theme1")}
                # New, not changed, on a vanilla base -- a mod base already
                # carries this file, so there it would show up as "changed"
                # (or not at all, if identical) instead.
                expected_added = {"etc/init.d/S90adb", "usr/resource/kernel_build_id",
                                  "etc/init.d/S22_bt_init", EMBED_BINARY}
                expected_added |= set(BRCMFMAC_FILES)
                expected_added |= set(BRCMFMAC_SWITCH_ADDED)
                # start_patchram_earlier() runs before hasten_bt_init() and
                # renames whichever of S80_bt_init/S22_bt_init it finds
                # straight to S11b_bt_init (plus S21mount_ubifs ->
                # S11amount_ubifs, unconditionally) -- so on a vanilla base
                # S22_bt_init never actually survives to the final tree,
                # despite still being listed above for whichever build
                # order or code path leaves it as the last rename applied.
                expected_added |= {"etc/init.d/S11amount_ubifs", "etc/init.d/S11b_bt_init"}
                # hasten_bt_init() renames S80_bt_init -> S22_bt_init (the
                # first slot after /usr/data is mounted) rather than editing
                # it in place, so the old name legitimately disappears --
                # and start_patchram_earlier() above does the same to
                # S21mount_ubifs on its way to S11amount_ubifs.
                expected_removed = {"etc/init.d/S80_bt_init", "etc/init.d/S21mount_ubifs"}
                expected_removed |= set(STRIPPED_FONTS_AND_BINARIES)
                expected_removed |= set(BRCMFMAC_SWITCH_REMOVED)

                def removed_ok(path):
                    return path in expected_removed or path.startswith(LITEGUI_DIR)

                if (set(changed) <= expected and set(added) <= expected_added
                        and all(removed_ok(k) for k in removed)):
                    print(f"    only expected files changed ({len(changed)}), "
                          f"as intended")
                else:
                    print("    UNEXPECTED: something other than the supervisor "
                          "and mount script differs")
                    ok = False

    print("\nRESULT:", "image verifies" if ok else "PROBLEM — do not flash")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
