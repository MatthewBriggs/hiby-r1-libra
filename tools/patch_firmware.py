#!/usr/bin/env python3
"""Patch a HiBy R1 Audiobook Mod firmware image so it loads the Podcasts app.

The app is an LD_PRELOAD library, and what loads it is /usr/bin/hiby_player.sh.
That file lives on a read-only squashfs, so it cannot be pushed to the device —
the firmware has to be repacked. This takes *your own* .upt, applies one patch
to that script, and writes a new .upt. No one else's firmware is redistributed.

    ./patch_firmware.py r1-audiobooks-2.0.26.upt r1-podcast-2.0.26.upt

Needs pycdlib (pip install pycdlib) and squashfs-tools (mksquashfs/unsquashfs).
Verified against the mod's 2.0.25 and 2.0.26 releases.

Image layout, worked out by inspection:

    /D0000001/F0000002.BIN   md5 of each rootfs chunk, one per line
    /D0000001/F0000003.BIN   the same for the kernel chunks
    /D0000001/F0000004.TXT   manifest: img_name / img_size / img_md5 per image
    /D0000001/F0000005.TXT   empty
    /D0000001/F0000006...    rootfs.squashfs, then xImage, in 512 KiB chunks
    /F00000NN.TXT            current_version=0, numbered after the last chunk

Both the manifest digests and the per-chunk digests have to be regenerated or
the device rejects the update.
"""

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

CHUNK = 524288
SCRIPT = "usr/bin/hiby_player.sh"
MOUNT_SCRIPT = "usr/bin/mount_ubifs.sh"
VERSION_FILE = "etc/r1_audiobook_version"
BT_INIT = "usr/bin/bt_init"

# Default stamp for a build. Override with --rom-version. It ends up in
# System -> About so a device can be identified without a laptop, which
# matters once several builds exist that differ only in what was patched in.
DEFAULT_ROM_VERSION = "pod1.0"

# System -> About renders config.json's `version`, and the field is cut to
# SEVEN characters — "2.0.26ABCDEFGHIJ" displays as "2.0.26A". The stock string
# is already six, so exactly one is left. A revision letter is what fits: a, b,
# c... The full build string goes in etc/r1_audiobook_version, which adb can
# read, but the letter is what identifies a device you are holding.
CONFIG_JSON = "usr/resource/config.json"
ABOUT_VERSION_MAX = 7

# Internet radio is already in the firmware — the strings are localised in every
# settings.ini and the icons ship in every theme — but the Stream media screen
# only lists it in HiBy's China-region layout. The plain layout has Tidal and
# Qobuz; the _cn one has those plus net_radio. So "unlocking" it is just using
# the layout HiBy already wrote, with no invented assets.
STREAM_LAYOUT_DIRS = (
    "usr/resource/layout/theme1",
    "usr/resource/layout/theme2",
    "usr/resource/layout/midi/theme1",
)


def enable_internet_radio(root):
    """Point each theme's Stream media screen at the layout with the radio tile."""
    changed = []
    for d in STREAM_LAYOUT_DIRS:
        plain = os.path.join(root, d, "hiby_stream_media.view")
        cn = os.path.join(root, d, "hiby_stream_media_cn.view")
        if not (os.path.exists(plain) and os.path.exists(cn)):
            continue
        with open(cn, "rb") as fh:
            want = fh.read()
        with open(plain, "rb") as fh:
            if fh.read() == want:
                continue                 # already swapped
        with open(plain, "wb") as fh:
            fh.write(want)
        changed.append(d)
    return changed


def stamp_config_json(text, rom_rev):
    """Append a one-character revision to the version About displays."""
    m = re.search(r'("version"\s*:\s*")([^"]+)(")', text)
    if not m:
        return None
    cur = m.group(2)
    if len(cur) >= ABOUT_VERSION_MAX:
        return None                      # already stamped, or no room left
    new = (cur + rom_rev)[:ABOUT_VERSION_MAX]
    return text[:m.start(2)] + new + text[m.end(2):]


def stamp_version_file(text, rom_version):
    """Stamp the build into the string System -> About displays.

    Neither about_dev.ini's <model> nor config.json's version drives that text —
    both were tried and neither showed. The Audiobook Mod renders `label` from
    this file, which is why About reads exactly "HiBy R1 2.0.26". Appending here
    is what actually reaches the screen. The extra keys are for scripts and adb.
    """
    if "podcast_rom=" in text:
        return None
    out = []
    for line in text.splitlines():
        if line.startswith("label=") and rom_version not in line:
            line = f"{line} {rom_version}"
        out.append(line)
    out.append(f"podcast_rom={rom_version}")
    out.append(f"podcast_rom_built="
               f"{__import__('datetime').date.today().isoformat()}")
    return "\n".join(out) + "\n"

# /usr/data — the writable UBIFS holding the library database, settings and the
# preload libraries — is mounted with -o sync, which makes every write hit NAND
# synchronously. That is a large part of why saving settings and progress feels
# slow, and it wears the flash unnecessarily. SQLite still calls fsync at commit
# on its own, so durability for the database does not depend on this flag.
# noatime additionally stops a metadata write every time a file is merely read.
# BlueALSA is started with A2DP only, which is all that playback needs — but it
# also means there is no RFCOMM link, and a headset's battery level arrives over
# HFP. Adding the Hands-Free Audio Gateway profile gives bluealsa-rfcomm
# something to report.
#
# The XAPL name is "iPhone" deliberately, and it is the difference between a
# battery reading and none. Apple's handshake has the headset send
# AT+XAPL=<vendor>-<product>-<version> and the gateway answer with an
# identifier and its feature bits. Answering "HiBy" was captured on the wire
# being accepted and then ignored — the WH-1000XM4 simply never sent the
# IPHONEACCEV that carries the level. Answering "iPhone" produced it
# immediately.
ANCHOR_BT = ('/usr/bin/bluealsa -p a2dp-source '
             '--a2dp-volume --sbc-quality=xq &')
INSERT_BT = ('/usr/bin/bluealsa -p a2dp-source -p hfp-ag '
             '--a2dp-volume --sbc-quality=xq --xapl-resp-name=iPhone &')


ANCHOR_BT_VANILLA = '/usr/bin/bluealsa -p a2dp-source --a2dp-volume &'
INSERT_BT_VANILLA = ('/usr/bin/bluealsa -p a2dp-source -p hfp-ag '
                     '--a2dp-volume --xapl-resp-name=iPhone &')


def patch_bt_init(text):
    """Add the HFP profile so a headset can report its battery.

    Two anchors: the mod's own bt_init already carries --sbc-quality=xq (a
    separate, earlier tweak this project did not introduce), vanilla 1.6's
    does not -- confirmed by reading it directly. Trying the vanilla anchor
    only when the mod one does not match keeps this working against either
    base without having to be told which one a given .upt actually is.
    """
    if "hfp-ag" in text:
        return None                      # already done
    if ANCHOR_BT in text:
        return text.replace(ANCHOR_BT, INSERT_BT, 1)
    if ANCHOR_BT_VANILLA in text:
        return text.replace(ANCHOR_BT_VANILLA, INSERT_BT_VANILLA, 1)
    return None



# ---------------------------------------------------------------- bt timing
#
# Stock bt_init spends ~12s in sleep(1): 1 after the rfkill write, 5 after
# brcm_patchram_plus, 1 after "hciconfig up", 1 after the reset, 2 after
# bt-agent, and 1 each around the alsa.conf write and the bt-adapter calls.
#
# Measured on hardware (LEAN_4, 2026-09-02), stock, from a cold boot:
#
#     rfkill0 present (cywdhd finished)   uptime  3.88s
#     hci0 present                        uptime  8.98s
#     /tmp/bt_init_ok                     uptime 16.29s
#
# So Bluetooth was not actually usable until 16.3s, and cywdhd -- the thing
# previously blamed for BT being late -- accounted for 0.44s of that, under
# 3%. The sleeps are the whole story.
#
# Each sleep below becomes "wait until the thing we were sleeping for is
# true", polling every 50ms, with a timeout. The timeout is the failure
# bound, not the normal path, so the worst case is no worse than stock's
# blind sleep and the normal case is much faster. Re-running the rewritten
# script on hardware, against a full rfkill power-cycle of the radio, it
# completes in 5.79s instead of ~12.4s with every milestone reached
# (bluetoothd, bt-agent and bluealsa all up, alias set, Hibylink SP
# registered, same "Powered 1 -> 0" end state).
#
# The rfkill wait is also what makes backgrounding cywdhd safe: with
# defer_wifi_module in play, rfkill0 may not exist yet when this script runs.

BT_WAIT_HELPER = '# bt_wait <max_seconds> "<shell test>" -- poll every 50ms until true.\n# 50ms, NOT tighter. Tried 10ms on the theory that the granularity was ~0.2s\n# of free rounding; it is not free. Process spawn costs 4.3ms on this device\n# and most of these conditions spawn -- hciconfig|grep is two spawns, so a\n# 10ms tick is 8.6ms of work, ~86% of a single core, precisely while\n# brcm_patchram_plus is servicing 746 UART reads. Measured 1 boot in 7 where\n# hci0 never appeared and bt_init limped to 47s. At 50ms the same poll is 17%.\n# Replaces the fixed sleeps this script used to use; see patch_firmware.py.\nbt_wait() {\n    _n=$(( $1 * 20 )); _c="$2"; _i=0\n    while [ "$_i" -lt "$_n" ]; do\n        eval "$_c" >/dev/null 2>&1 && return 0\n        usleep 50000 2>/dev/null || sleep 1\n        _i=$(( _i + 1 ))\n    done\n    return 1\n}\n\nrm /var/run/messagebus.pid -rf'

# (anchor, replacement, description) -- each must match exactly once.
BT_TIMING_EDITS = (
    ("rm /var/run/messagebus.pid -rf",
     BT_WAIT_HELPER,
     "bt_wait helper"),

    ("echo 1 > /sys/class/rfkill/rfkill0/state\n"
     "sleep 1 # if invoke this script in c with system(), must sleep for a while!!!!!",
     "# Power EVERY bluetooth rfkill, not the hardcoded rfkill0.\n"
     "#\n"
     "# rfkill0 used to be cywdhd's (md_bcmdhd_bt_power) by position, which\n"
     "# was always fragile and became wrong the moment a second provider\n"
     "# existed: with bt_power_bluesleep built in, IT registers first and takes\n"
     "# rfkill0, while cywdhd's moves to rfkill1. Measured on hardware\n"
     "# 2026-09-07: writing our rfkill0 leaves the chip dead (hci0 only after\n"
     "# ~32s, HCI reset 6s), writing cywdhd's brings it up in 4.62s -- its\n"
     "# power path does more than toggle BT_REG_ON. Writing all of them costs\n"
     "# one extra echo and does not care which driver landed where.\n"
     "bt_wait 10 'grep -ql bluetooth /sys/class/rfkill/rfkill*/name'\n"
     "for _rf in /sys/class/rfkill/rfkill*; do\n"
     "    [ \"$(cat $_rf/name 2>/dev/null)\" = bluetooth ] || continue\n"
     "    echo 1 > $_rf/state 2>/dev/null\n"
     "done",
     "rfkill write (-1s, all bluetooth rfkills)"),

    ("&\nsleep 5\n",
     "&\n# was: sleep 5 -- wait for patchram to register the adapter instead\n"
     "bt_wait 15 '[ -d /sys/class/bluetooth/hci0 ]'\n",
     "patchram (-5s)"),

    ("hciconfig hci0 up\nsleep 1",
     "hciconfig hci0 up\n"
     "bt_wait 5 'hciconfig hci0 | grep -q \"UP RUNNING\"'",
     "hci0 up (-1s)"),

    ("hciconfig hci0 reset\nsleep 1",
     "# bluez has to be on the bus before the reset is meaningful\n"
     "bt_wait 5 'dbus-send --system --print-reply --dest=org.bluez / "
     "org.freedesktop.DBus.Peer.Ping'\n"
     "hciconfig hci0 reset\n"
     "bt_wait 5 'hciconfig hci0 | grep -q \"UP RUNNING\"'",
     "bluez reset (-1s, +wait for bus)"),

    ("bt-agent -c NoInputNoOutput &\nsleep 2",
     "bt-agent -c NoInputNoOutput &\nbt_wait 5 'pidof bt-agent'",
     "bt-agent (-2s)"),

    ("# add hibylink serial port\nsleep 1",
     "# add hibylink serial port\n# was: bt_wait 5 'bt-adapter --list' -- that spawns bt-adapter (~0.17s\n# measured); the same readiness question costs ~0.03s over dbus.\nbt_wait 5 'dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.Peer.Ping'",
     "hibylink SP (-1s)"),
)


def patch_bt_init_timing(text):
    """Replace bt_init's fixed sleeps with condition polling.

    Returns (new_text, [descriptions]) or None if already applied. Anchors
    that do not match are skipped and reported rather than being fatal --
    this script differs slightly between the vanilla and mod images, and a
    partially-applied speedup is still a speedup.
    """
    if "bt_wait()" in text:
        return None                      # already done
    applied = []
    for anchor, repl, desc in BT_TIMING_EDITS:
        if text.count(anchor) == 1:
            text = text.replace(anchor, repl, 1)
            applied.append(desc)
    if not applied:
        return None
    # The lone "sleep 1" between the alsa.conf block and the bt-adapter calls
    # has no unique neighbouring text, so it is handled positionally: it is
    # the sleep that guards bluealsa coming up.
    if "\nsleep 1\n\nif [ -f /usr/resource/bt_name ]" in text:
        text = text.replace(
            "\nsleep 1\n\nif [ -f /usr/resource/bt_name ]",
            "\nbt_wait 5 'pidof bluealsa'\n\nif [ -f /usr/resource/bt_name ]", 1)
        applied.append("bluealsa (-1s)")
    return text, applied


ANCHOR_MOUNT = 'mount -o sync -t ubifs /dev/${ubi_name}_0 $mount_path'
INSERT_MOUNT = 'mount -o noatime -t ubifs /dev/${ubi_name}_0 $mount_path'


def patch_mount_script(text):
    """Return the patched mount script, or None if it needs no change."""
    if ANCHOR_MOUNT not in text:
        return None
    return text.replace(ANCHOR_MOUNT, INSERT_MOUNT)

# Applied to the mod's stock supervisor. Each anchor is matched exactly; if one
# is missing the script has changed and we stop rather than guess, because a
# half-applied patch to init would be a brick rather than a bug.
ANCHOR_CONFIG = 'MAX_CRASHES=5\n'
INSERT_CONFIG = '''MAX_CRASHES=5

# --- podcast app additions -------------------------------------------------
# The rootfs is read-only squashfs, so there is otherwise no way to start user
# code at boot or to load a second preload without reflashing. Both hooks below
# read from /usr/data, which is writable and survives a firmware update.
#
# DEV_HOOK is dropped after DEV_HOOK_GIVEUP consecutive crashes so that a broken
# library degrades to a working player instead of a reboot loop.
DEV_HOOK="/usr/data/libpodcast_hook.so"
DEV_HOOK_GIVEUP=2
HEALTHY_RUN=60          # seconds up before a run counts as a success
USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &

# Performance tuning, applied once at startup.
#
# Read-ahead: the card ships at 128 KB. Library scans and large FLAC reads are
# sequential, and on a slow SD card a bigger window is most of the difference
# between smooth and stuttering. 2 MB costs a little RAM for a lot of
# throughput.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done

# vfs_cache_pressure: at the default 100 the kernel reclaims dentries and
# inodes as eagerly as page cache. With 56 MB of RAM that means the library UI
# keeps re-reading the same directory metadata and cover paths. Halving it
# keeps them resident longer without pinning them outright.
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
# ---------------------------------------------------------------------------
'''

ANCHOR_LAUNCH = '''    if [ -f "$HOOK_LIB" ]; then
        LD_PRELOAD="$HOOK_LIB" "$PLAYER" &
    else
        "$PLAYER" &
    fi
'''
INSERT_LAUNCH = '''    PRELOAD=""
    if [ "$CRASH_COUNT" -lt "$DEV_HOOK_GIVEUP" ] && [ -f "$DEV_HOOK" ]; then
        PRELOAD="$DEV_HOOK"
    fi
    if [ -f "$HOOK_LIB" ]; then
        PRELOAD="${PRELOAD:+$PRELOAD }$HOOK_LIB"
    fi

    if [ -n "$PRELOAD" ]; then
        LD_PRELOAD="$PRELOAD" "$PLAYER" &
    else
        "$PLAYER" &
    fi
'''

ANCHOR_COUNT = '''    wait "$HP_PID" 2>/dev/null
    CRASH_COUNT=$((CRASH_COUNT + 1))
'''
INSERT_COUNT = '''    STARTED=$(date +%s)
    wait "$HP_PID" 2>/dev/null

    # The counter is meant to catch a hook that cannot survive startup, so it
    # has to measure *consecutive* failures. Incrementing unconditionally makes
    # it count every exit since boot instead, and two unrelated ones hours apart
    # were enough to silently drop DEV_HOOK for the rest of the session — the
    # Podcasts tile just reverts to About with nothing to say why. A player that
    # stayed up long enough to be useful is evidence the hook is fine.
    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
'''


def die(msg):
    sys.exit(f"error: {msg}")


def need(tool):
    if not shutil.which(tool):
        die(f"{tool} not found — install squashfs-tools")


# Vanilla 1.6's usr/bin/hiby_player.sh has none of the structure
# patch_script() patches -- no MAX_CRASHES, no LD_PRELOAD, no crash counting.
# It is thirteen bare lines: kill any stale hiby_player, start batd if
# present, run the player once, reboot when it exits. There is no anchor to
# key an incremental patch off, so build_vanilla_supervisor() below
# constructs the complete replacement directly instead, reusing the mod's
# own already-shipped supervisor loop (as it existed in a mod-based
# yetisoldier Audiobook Mod install) as the reference for the parts that are
# proven -- the crash-counting shape,
# DEV_HOOK_GIVEUP, HEALTHY_RUN -- with three differences: vanilla's own batd
# preamble is kept rather than introduced, HOOK_LIB is dropped entirely (the
# yetisoldier Audiobook Mod's own separate tile hijack has no reason to exist
# against a vanilla base -- Library has its own Audiobooks section, reached
# through the same DEV_HOOK), and the performance tuning that used to be a
# separate INSERT_CONFIG patch step is folded straight in rather than
# layered on afterward.
VANILLA_ANCHOR_PREAMBLE_END = '/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &\nfi\n'

VANILLA_SUPERVISOR_BODY = '''PLAYER="/usr/bin/hiby_player"
CRASH_COUNT=0
MAX_CRASHES=5

# --- Library/Podcasts additions ---------------------------------------------
# The rootfs is read-only squashfs, so there is otherwise no way to start user
# code at boot or to load a preload without reflashing. DEV_HOOK reads from
# /usr/data, which is writable and survives a firmware update.
#
# DEV_HOOK is dropped after DEV_HOOK_GIVEUP consecutive crashes so that a
# broken library degrades to a working stock player instead of a reboot loop.
DEV_HOOK="/usr/data/libpodcast_hook.so"
DEV_HOOK_GIVEUP=2
HEALTHY_RUN=60          # seconds up before a run counts as a success
USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &

# Performance tuning, applied once at startup.
#
# Read-ahead: the card ships at 128 KB. Library scans and large FLAC reads are
# sequential, and on a slow SD card a bigger window is most of the difference
# between smooth and stuttering. 2 MB costs a little RAM for a lot of
# throughput.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done

# vfs_cache_pressure: at the default 100 the kernel reclaims dentries and
# inodes as eagerly as page cache. With 56 MB of RAM that means the library UI
# keeps re-reading the same directory metadata and cover paths. Halving it
# keeps them resident longer without pinning them outright.
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
# -----------------------------------------------------------------------------

while true; do
    if [ "$CRASH_COUNT" -lt "$DEV_HOOK_GIVEUP" ] && [ -f "$DEV_HOOK" ]; then
        LD_PRELOAD="$DEV_HOOK" "$PLAYER" &
    else
        "$PLAYER" &
    fi
    HP_PID=$!
    STARTED=$(date +%s)
    wait "$HP_PID" 2>/dev/null

    # The counter is meant to catch a hook that cannot survive startup, so it
    # has to measure *consecutive* failures. A player that stayed up long
    # enough to be useful is evidence the hook is fine.
    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
    if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
        reboot
    fi
    sleep 1
done
'''


STANDALONE_SUPERVISOR_BODY = '''BINARY="{binary_path}"
CRASH_COUNT=0
MAX_CRASHES=5
HEALTHY_RUN=60          # seconds up before a run counts as a success

# --- Library/Podcasts additions ---------------------------------------------
# RP1: no hiby_player at all. BINARY is a real, independent executable (not
# an LD_PRELOAD hook, so there is nothing to inject into another process's
# address space) living on /usr/data, which is writable and survives a
# firmware update -- the same reason the old preloaded hook .so used to live
# there too. A missing BINARY just means CRASH_COUNT climbs to MAX_CRASHES
# and the device reboots rather than spinning silently, the same failure
# shape the old hook-based supervisor already had for a missing hook.
#
# Mounting the SD card was never any init.d script's job or this script's
# own -- grepped the whole stock rootfs for it and came up empty. It happens
# inside the hiby_player *binary* itself at startup (confirmed live: without
# it, dmesg never shows "exFAT-fs ... mounted successfully", every path
# under /data/mnt/sd_0 stat()s as missing, and the library shows real album
# names -- index.c's own stale cache -- with every album reporting 0 tracks,
# since tracks_query() skips any row whose file doesn't actually exist).
# With hiby_player gone, nothing else will ever do this, so it has to happen
# here. -t exfat matches this card's real format exactly (confirmed via the
# same dmesg line); mount is idempotent enough in practice that a harmless
# "already mounted" on a crash-restart is fine to ignore.
mount -t exfat /dev/mmcblk0p1 /data/mnt/sd_0 2>/dev/null

# USB role, switchable from the SD card.
#
# The Type-C controller's role is set by /sys/devices/platform/tcs1421/
# tcs1421_cfg (Sink / Source / StrongDRP / NormalDRP). "Source" makes this
# board a USB HOST, which is what an OTG peripheral needs -- but it also
# stops the board being a USB *device*, so adb disappears entirely.
#
# That is a trap when the only way in is adb: a bad value on internal
# storage locks you out, and a firmware flash does NOT clear it because
# /usr/data is deliberately preserved. So the control lives on the SD
# CARD, which can always be read in any card reader.
#
# Write one word into /data/mnt/sd_0/usb-mode.txt:
#     device  (or missing/anything else) -> NormalDRP, adb works [DEFAULT]
#     otg     (or host)                  -> Source, USB host for peripherals
#
# Defaulting to device-mode on an absent/unreadable file is deliberate:
# the failure mode of a lost card or a typo must be "adb still works".
USB_MODE_FILE="/data/mnt/sd_0/usb-mode.txt"
TCS_CFG="/sys/devices/platform/tcs1421/tcs1421_cfg"
if [ -w "$TCS_CFG" ]; then
    USB_MODE="device"
    [ -f "$USB_MODE_FILE" ] && USB_MODE=$(cat "$USB_MODE_FILE" 2>/dev/null | tr -d " \t\r\n")
    case "$USB_MODE" in
        otg|host|OTG|HOST) USB_WANT="Source" ;;
        *)                 USB_WANT="NormalDRP" ;;
    esac
    # ONLY write when the value actually differs. Writing this attribute
    # re-drives the Type-C strap GPIOs and forces a USB re-attach even when
    # writing the value already held -- doing that unconditionally at boot
    # races adbd binding its gadget and can leave USB dead. Stock default is
    # already NormalDRP, so the common path must be a no-op, not a rewrite.
    USB_HAVE=$(cat "$TCS_CFG" 2>/dev/null)
    if [ "$USB_HAVE" != "$USB_WANT" ]; then
        echo "$USB_WANT" > "$TCS_CFG" 2>/dev/null
    fi
fi

# Stale user-init guard. /usr/data survives firmware flashes, so a script
# left there can persist across every recovery attempt. If the SD card
# carries the file below, remove it -- the only lever that reaches internal
# storage when adb is unavailable.
[ -f /data/mnt/sd_0/reset-init-sh ] && rm -f /usr/data/init.sh

# Boot breadcrumb to the SD card. USB is the only channel in, so when it
# fails there is otherwise no way to see how far boot got. Overwritten each
# boot; costs one small write.
{{
    echo "boot $(date)"
    echo "  tcs1421_cfg = $(cat /sys/devices/platform/tcs1421/tcs1421_cfg 2>/dev/null)"
    echo "  usb-mode.txt = $(cat /data/mnt/sd_0/usb-mode.txt 2>/dev/null)"
    echo "  udc         = $(ls /sys/class/udc/ 2>/dev/null)"
    echo "  adb gadget  = [$(cat /sys/kernel/config/usb_gadget/adb_demo/UDC 2>/dev/null)]"
    echo "  mass gadget = [$(cat /sys/kernel/config/usb_gadget/android0/UDC 2>/dev/null)]"
    echo "  adbd running= $(ps 2>/dev/null | grep -c "[a]dbd")"
    echo "  init.sh     = $([ -f /usr/data/init.sh ] && echo present || echo absent)"
    echo "  kernel      = $(cat /usr/resource/kernel_build_id 2>/dev/null) / $(uname -r)"
}} > /data/mnt/sd_0/boot-state.txt 2>&1

USER_INIT="/usr/data/init.sh"

[ -f "$USER_INIT" ] && sh "$USER_INIT" >/dev/null 2>&1 &

# Performance tuning, applied once at startup -- same as the old hook-based
# supervisor's own copy of this block, kept verbatim since Library benefits
# from it regardless of what launches it.
for q in /sys/block/mmcblk*/queue/read_ahead_kb; do
    [ -w "$q" ] && echo 2048 > "$q"
done
[ -w /proc/sys/vm/vfs_cache_pressure ] && echo 50 > /proc/sys/vm/vfs_cache_pressure
# -----------------------------------------------------------------------------

while true; do
    if [ -x "$BINARY" ]; then
        # RP8: this SoC is single-core, so scheduling contention against the
        # still-resident bluealsa/bluetoothd/dbus-daemon(x2)/sys_server is a
        # more plausible stutter cause than anything RAM-related turned out
        # to be (see BACKLOG.md's RP8) -- there is no second core for the UI
        # to hide behind. A one-line nice bump costs nothing to try and
        # directly targets that: -5 is a real edge over the others' default
        # 0 without going so negative it starves them outright (none of them
        # need to run often, but bluetoothd/dbus still need to be schedulable
        # at all for pairing/connection events to work).
        nice -n -5 "$BINARY" &
    else
        sleep 5 &   # nothing to run; still counts as a fast "crash" below
    fi
    B_PID=$!
    STARTED=$(date +%s)
    wait "$B_PID" 2>/dev/null

    if [ $(( $(date +%s) - STARTED )) -ge "$HEALTHY_RUN" ]; then
        CRASH_COUNT=0
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi
    if [ "$CRASH_COUNT" -ge "$MAX_CRASHES" ]; then
        reboot
    fi
    sleep 1
done
'''


# Rootfs-relative home for an embedded release binary -- NOT /usr/data,
# which is a separate mount that shadows anything placed there in the
# image itself, so a file living only in the squashfs tree at that path
# would just be invisible at runtime. usr/lib is ordinary rootfs, always
# present after unsquashfs regardless of mount order.
EMBED_DIR = "usr/lib/libra"
EMBED_BINARY_NAME = "library_standalone"

STANDALONE_SEED_BLOCK = '''
# Release-embedded binary (--embed-binary): this rootfs carries its own
# copy of BINARY, seeded at build time. If /usr/data doesn't already hold
# a matching version -- a fresh device, or one that was never separately
# adb-pushed a build for this exact release -- copy it in before the loop
# below ever tries to launch it. Once /usr/data holds any version, plain
# adb-push iteration keeps overwriting it exactly as before this existed;
# this only fires when the two disagree, so it can never clobber a dev
# build mid-session, only bring a version-mismatched or empty /usr/data
# up to what this firmware actually shipped with.
SEED_BINARY="/{embed_dir}/{embed_name}"
SEED_VERSION="{seed_version}"
DATA_VERSION_FILE="/usr/data/.library_standalone_version"
if [ -f "$SEED_BINARY" ] && [ "$(cat "$DATA_VERSION_FILE" 2>/dev/null)" != "$SEED_VERSION" ]; then
    cp -f "$SEED_BINARY" "$BINARY" && chmod 755 "$BINARY"
    echo "$SEED_VERSION" > "$DATA_VERSION_FILE"
fi
'''


def build_standalone_supervisor(original_text, binary_path, seed_version=None):
    """RP1: full-replacement supervisor that launches `binary_path` (an
    on-device path, e.g. /usr/data/library_standalone) directly in a
    crash-counted loop -- no hiby_player, no DEV_HOOK/LD_PRELOAD at all.

    Reuses the same batd-preamble anchor build_vanilla_supervisor() keys
    off, since the input this is written against is the same bare vanilla
    hiby_player.sh either way -- battery monitoring has nothing to do with
    which player runs after it, so that preamble is kept unmodified.
    Returns None if the anchor isn't found, same "stop rather than guess"
    contract every other supervisor-shape function here follows.

    seed_version, if given (see --embed-binary/--embed-version), inserts
    STANDALONE_SEED_BLOCK right before the crash-loop starts -- once per
    boot, not once per crash-restart within a boot.
    """
    if VANILLA_ANCHOR_PREAMBLE_END not in original_text:
        return None
    preamble = original_text.split(VANILLA_ANCHOR_PREAMBLE_END)[0] + VANILLA_ANCHOR_PREAMBLE_END
    body = STANDALONE_SUPERVISOR_BODY.format(binary_path=binary_path)
    if seed_version:
        seed_block = STANDALONE_SEED_BLOCK.format(
            embed_dir=EMBED_DIR, embed_name=EMBED_BINARY_NAME, seed_version=seed_version)
        marker = "while true; do"
        idx = body.index(marker)
        body = body[:idx] + seed_block + "\n" + body[idx:]
    return preamble + "\n" + body


def build_vanilla_supervisor(original_text):
    """Full-replacement supervisor for a bare vanilla hiby_player.sh.

    Returns None if this is not that script -- already has MAX_CRASHES (a
    mod-based image; patch_script() handles that case), or is missing the
    batd-preamble anchor this was written against, in which case the caller
    should stop rather than silently produce a half-built script.
    """
    if "MAX_CRASHES" in original_text:
        return None
    if VANILLA_ANCHOR_PREAMBLE_END not in original_text:
        return None
    preamble = original_text.split(VANILLA_ANCHOR_PREAMBLE_END)[0] + VANILLA_ANCHOR_PREAMBLE_END
    return preamble + "\n" + VANILLA_SUPERVISOR_BODY


# Settings -> About only shows because Podcasts took the top-level slot that
# used to say About; flipping this is what puts it back, one level in.
# Present verbatim on both stock 1.6 and the mod's 2.0.26 (the mod already
# flips it itself, which is why this is a no-op there -- confirmed by reading
# both directly), so this one function is correct against either base.
SET_FUNCTIONS_FILES = ("usr/resource/set_functions.json",
                       "usr/resource/midi_set_functions.json")


def enable_about_tile(root):
    """Flip about:0 -> 1 in every set_functions.json this rootfs has."""
    changed = []
    for rel in SET_FUNCTIONS_FILES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        with open(path) as fh:
            text = fh.read()
        if '"about":0' not in text:
            continue                     # already enabled, or key not present
        with open(path, "w") as fh:
            fh.write(text.replace('"about":0', '"about":1'))
        changed.append(rel)
    return changed


# Stock 1.6 ships /etc/init.d/T90adb (adbd start/stop, dispatched to S310adb
# or S440adb depending on the USB gadget interface) but rcS only runs S??*
# scripts at boot, so T90adb is never invoked and the device boots with no
# ADB at all -- confirmed empirically: the first vanilla-based flash of this
# project booted fine but never enumerated over USB. S90adb is a wrapper this
# project wrote (not HiBy's, not the mod's) that starts adb through T90adb's
# own S310adb/S440adb when USB working mode is Auto/Device, with retries for
# a host that didn't finish enumerating; it has shipped inside every mod-base
# .upt used by this project since before patch_firmware.py existed in its
# current form, which is why the mod code path never had to install it. S310
# and S440 themselves are confirmed byte-identical between stock 1.6 and the
# mod base, so only the wrapper is missing here, not any underlying support.
S90ADB_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "S90adb")


# RP7 follow-up (BG-none-yet, live measurement 2026-08-24): sa_hgl_dma is
# HGL's graphics-acceleration DMA pool -- 6MB reserved at boot from a fixed
# `sahd_hgl_mem_size` module parameter, confirmed live to be a boot-time
# reservation rather than a lazy per-open allocation (force-opening the
# device and separately `rmmod`-ing the module both left MemAvailable
# completely unchanged, ruling out "nothing opens it so it's already free"
# -- that assumption was wrong and had never actually been tested before
# this). Library's own UI never touches HGL, confirmed by source: it mmaps
# /dev/fb0 directly. Only meaningful for --standalone: a hooked build still
# runs hiby_player, whose own native screens do use this. Loaded from
# module_driver/sa_hgl_dma.sh (`insmod sa_hgl_dma.ko
# sahd_hgl_mem_size=6291456`), called after soc_fb (the real framebuffer
# driver, already up) in driver_default_init_script.sh's own load order --
# nothing loads after it depends on it, confirmed by reading that order
# directly, so skipping it outright is safe. Made a no-op rather than
# deleted or removed from the load order: the orchestrator script's own
# `sh sa_hgl_dma.sh` call is left completely alone, so a smaller, more
# obviously-correct diff.
HGL_SCRIPT = "module_driver/sa_hgl_dma.sh"
MODULE_INIT_SCRIPT = "module_driver/driver_default_init_script.sh"
# brcmfmac (docs/10 in the kernel repo) looks for firmware under these exact
# names. The vendor ships the same blobs under its own names in wifi_bcm/.
BRCMFMAC_FW = [
    ("lib/firmware/wifi_bcm/fw_bcm43438a1.bin", "lib/firmware/brcm/brcmfmac43430-sdio.bin"),
    ("lib/firmware/wifi_bcm/nvram_ap6212a.txt", "lib/firmware/brcm/brcmfmac43430-sdio.txt"),
]


def disable_hgl(root):
    """Turn module_driver/sa_hgl_dma.sh into a no-op. Standalone builds only
    -- see this module's own comment for why. Returns False if the file
    isn't where expected, so the caller can decide whether that's fatal."""
    path = os.path.join(root, HGL_SCRIPT)
    if not os.path.exists(path):
        return False
    with open(path, "w") as fh:
        fh.write("# RP1/RP7: no hiby_player, and Library's own UI never "
                 "touches HGL (mmaps /dev/fb0 directly) -- this 6MB boot-"
                 "time reservation has no consumer in a standalone build. "
                 "Originally: insmod sa_hgl_dma.ko sahd_hgl_mem_size=6291456\n")
    return True


# RP-follow-up, live measurement 2026-09-10: hiby_player itself and the
# stock assets only it ever drew are dead weight in a standalone build, same
# reasoning as HGL above -- confirmed by cross-referencing `strings` on
# every binary/script that still runs (bt_init, bt_resume, wifi_on.sh,
# library_standalone itself) against usr/resource: nothing outside
# hiby_player references usr/resource/fonts/default.otf (8.3MB, the stock
# UI's own font -- Library draws its own bitmap font) or Thai.ttf (30KB).
# Korean.ttf and msyh.ttf ARE kept -- library_standalone's own strings
# reference them directly as CJK fallback glyphs. Of litegui's 10.7MB
# (theme1/theme2/midi), only four files are ever touched by anything that
# still runs: theme1 and theme2's launcher/about.png + about_s.png, and
# only as shadow_resources()'s own bind-mount *targets* -- their content is
# replaced by that mount, so an empty placeholder at the same path is all a
# mount point needs. dmrd (72KB, DLNA renderer) is not started by any
# init.d script -- confirmed by grepping all of them -- and the
# usr/resource/upnp/ icons it would have used do not even exist on this
# rootfs, so it has literally never been reachable.
LITEGUI_DIR = "usr/resource/litegui"
LITEGUI_KEEP = [  # shadow_resources()'s own bind-mount targets -- content
    "litegui/theme1/launcher/about.png",     # is irrelevant, only the path
    "litegui/theme1/launcher/about_s.png",   # needs to exist for the mount
    "litegui/theme2/launcher/about.png",     # call in music_hook.c to
    "litegui/theme2/launcher/about_s.png",   # succeed.
]
UNUSED_FONTS = ["usr/resource/fonts/default.otf", "usr/resource/fonts/Thai.ttf"]
UNUSED_BINARIES = ["usr/bin/hiby_player", "usr/bin/dmrd"]


def strip_unused_resources(root):
    """Delete hiby_player, its stock-only fonts, and all of litegui except
    the four files shadow_resources() bind-mounts over. Standalone builds
    only -- see this module's own comment for why each is safe. Returns
    (files_removed, bytes_freed)."""
    n, freed = 0, 0

    def rm(rel):
        nonlocal n, freed
        path = os.path.join(root, rel)
        if os.path.exists(path):
            freed += os.path.getsize(path)
            os.remove(path)
            n += 1

    for rel in UNUSED_FONTS + UNUSED_BINARIES:
        rm(rel)

    litegui = os.path.join(root, LITEGUI_DIR)
    if os.path.isdir(litegui):
        keep = {os.path.join(root, "usr/resource", p) for p in LITEGUI_KEEP}
        for dirpath, _, filenames in os.walk(litegui):
            for name in filenames:
                path = os.path.join(dirpath, name)
                if path not in keep:
                    freed += os.path.getsize(path)
                    os.remove(path)
                    n += 1
        # Prune whatever directories that emptied out, deepest first, but
        # never the four kept files' own parents.
        keep_dirs = {os.path.dirname(p) for p in keep}
        for dirpath, dirnames, filenames in os.walk(litegui, topdown=False):
            if dirpath in keep_dirs or dirpath == litegui:
                continue
            if not os.listdir(dirpath):
                os.rmdir(dirpath)
        for rel in LITEGUI_KEEP:
            path = os.path.join(root, "usr/resource", rel)
            if not os.path.exists(path):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                open(path, "wb").close()

    return n, freed


def install_brcmfmac_firmware(root):
    """Copy the vendor WiFi firmware to the names mainline brcmfmac looks for.

    Verified on hardware (kernel repo docs/10): the vendor blob identifies
    itself as "43430a1-roml ... Version: 7.45.96.123", i.e. BCM43430 rev A1 --
    exactly what brcmfmac's BCM43430_FIRMWARE_NAME expects -- and the NVRAM is
    already in the standard Broadcom text format brcmfmac parses, not a
    vendor-specific one. So this is a copy under a different name, not a
    conversion.

    Harmless when brcmfmac is not in the running kernel: they are two extra
    files nothing opens. Installing them unconditionally means the firmware is
    already in place whenever a brcmfmac-capable kernel is flashed, rather
    than being a separate step someone has to remember.

    Returns the number of files installed."""
    n = 0
    for src_rel, dst_rel in BRCMFMAC_FW:
        src = os.path.join(root, src_rel)
        dst = os.path.join(root, dst_rel)
        if not os.path.exists(src):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        n += 1
    return n


def defer_wifi_module(root):
    """Background the WiFi module load IN PLACE, so its chip init overlaps the
    remaining module loads and app startup instead of blocking them.

    Measured on hardware (LEAN_4, 2026-09-02): dhd_module_init in -> out is
    0.44s, and the kernel proper is done at 0.357s of a 3.95s boot-to-adb --
    the rest is this script insmod'ing 28 vendor modules sequentially, with the
    WiFi chip by far the most expensive single one. (An earlier note here said
    ~1.1s; that was an over-read of the log, corrected by direct measurement.)

    Two things this deliberately does NOT do, both learned the hard way:

    1. It does not skip the module. cywdhd.ko is the only provider of
       md_bcmdhd_bt_power, the platform device behind
       /sys/class/rfkill/rfkill0 -- the switch /usr/bin/bt_init writes to in
       order to power the radio. It is a combo chip and one driver owns power
       for both, so never loading it would silently kill Bluetooth.

    2. It does not move the line to the end of the script. That was tried and
       reverted: it works, and the UI does come up sooner, but Bluetooth then
       becomes available noticeably later, because rfkill0 only appears once
       the WiFi chip has finished initialising. Backgrounding in place starts
       that init at the same moment as before -- Bluetooth comes up on the
       original schedule -- while the eight module loads behind it no longer
       have to wait for it.

    Returns False if the script isn't where expected or lacks the line."""
    path = os.path.join(root, MODULE_INIT_SCRIPT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        lines = fh.read().splitlines()
    target = "sh cywdhd.sh"
    if not any(l.strip() == target for l in lines):
        return False
    out = []
    for l in lines:
        if l.strip() == target:
            out += [
                "# WiFi/BT combo chip: backgrounded so its 0.44s of init overlaps",
                "# the module loads below and app startup. Kept in position, NOT",
                "# moved later -- this module owns md_bcmdhd_bt_power and therefore",
                "# rfkill0, so moving it delays Bluetooth becoming available.",
                "# bt_init now waits for rfkill0 rather than assuming it exists.",
                target + " &",
            ]
        else:
            out.append(l)
    with open(path, "w") as fh:
        fh.write("\n".join(out) + "\n")
    return True




# The D-Bus preamble bt_init runs before it touches the radio at all.
# Measured on hardware: dbus-uuidgen 0.050s + dbus-daemon spawn 0.180s =
# ~0.23s, and brcm_patchram_plus is backgrounded anyway, so the chip can be
# downloading its firmware during that instead of after it.
BT_DBUS_BLOCK = """rm /var/run/messagebus.pid -rf
# turn on dbus-daemon service
mkdir -p /tmp/dbus
mkdir -p /var/lib/dbus
dbus-uuidgen > /var/lib/dbus/machine-id
dbus-daemon --config-file=/usr/share/dbus-1/system.conf
"""

BT_PATCHRAM_TAIL = """&
# was: sleep 5 -- wait for patchram to register the adapter instead
"""


def reorder_bt_init_radio_first(text):
    """Start the radio before the D-Bus preamble, not after it.

    Runs after patch_bt_init_timing() and keys off that function's own
    marker comment, so it only applies to a script this tool has already
    converted to polling.

    Why this is safe: nothing between the top of the script and the
    brcm_patchram_plus launch needs D-Bus. The rfkill write is a sysfs poke,
    the BD address comes from a file, and the firmware choice is a sysfs read
    plus a case statement. bluetoothd -- the first thing that actually needs a
    bus -- is started well after the hci0 wait, by which point the moved block
    has long since run.

    The 4.26s that patchram spends is chip-side (measured: 4.188s of it is
    read() blocking on the UART across 746 reads, 191 HCI commands, ~22ms of
    chip processing each), so overlapping anything with it is a real gain.

    Returns the new text, or None if the anchors are not both present exactly
    once -- same stop-rather-than-guess contract as the rest of this file.
    """
    if text.count(BT_DBUS_BLOCK) != 1 or text.count(BT_PATCHRAM_TAIL) != 1:
        return None
    if text.index(BT_DBUS_BLOCK) > text.index(BT_PATCHRAM_TAIL):
        return None                      # already reordered
    moved = ("# D-Bus setup, moved down from the top of the script: it costs\n"
             "# ~0.23s and the radio does not need it, so it now runs while\n"
             "# brcm_patchram_plus is already downloading firmware.\n"
             + BT_DBUS_BLOCK)
    text = text.replace(BT_DBUS_BLOCK, "", 1)
    text = text.replace(BT_PATCHRAM_TAIL,
                        "&\n\n" + moved + "\n"
                        "# was: sleep 5 -- wait for patchram to register the adapter instead\n",
                        1)
    return text


def background_touch_module(root):
    """Background the touchscreen module load.

    Measured: the cst8xx_touch i2c probe costs 0.298s (1.359 -> 1.657 in
    dmesg), the largest single schedulable gap left in the module init
    script now that cywdhd is backgrounded. Nothing needs the touchscreen
    until the player starts at S92, roughly 1.5s later.

    OFF BY DEFAULT -- tried on hardware 2026-09-03 and it broke the
    touchscreen. Not a driver problem: the module still loads and
    hyn_ts_probe still succeeds (at 2.117s). What changes is the input
    ENUMERATION ORDER. Backgrounded, "jz adc keyboard" wins the race:

        working:      event1 = hyn_ts,          event2 = jz adc keyboard
        backgrounded: event1 = jz adc keyboard, event2 = hyn_ts

    and music_hook.c hardcodes both -- it opens /dev/input/event1 for touch
    and lists event2 in scan_inputs()'s fixed[] table as "side buttons",
    which it then EVIOCGRABs exclusively. So the app reads the keyboard as
    touch and grabs the touchscreen as buttons.

    The technique itself is sound and worth ~0.3s. Enabling it needs
    music_hook.c to resolve nodes by name out of /proc/bus/input/devices --
    which scan_inputs() already does for AVRCP devices, so the pattern is
    right there. Until that lands, leave this off.

    Returns True if the line was changed.
    """
    path = os.path.join(root, MODULE_INIT_SCRIPT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        lines = fh.read().splitlines()
    target = "sh cst8xx_touch.sh"
    if not any(l.strip() == target for l in lines):
        return False
    out = []
    for l in lines:
        if l.strip() == target:
            out += [
                "# Touchscreen probe costs ~0.3s and nothing needs it until the",
                "# player starts at S92. Backgrounded -- see background_touch_module().",
                target + " &",
            ]
        else:
            out.append(l)
    with open(path, "w") as fh:
        fh.write("\n".join(out) + "\n")
    return True



ADB_INIT_SCRIPT = "etc/init.d/adb/S440adb"

# Stock's ADB<->USB-storage switch is one-way on this build, and the reason is
# a pair of assumptions in the vendor scripts that only hold if ADB is *not* a
# permanent fixture.
#
# /usr/bin/adboff  = S440adb stop      + usb_dev_mass_storage.sh start
# /usr/bin/adbon   = mass_storage stop + S440adb start
#
# usb_dev_mass_storage.sh's storage_stop() finishes with
# "umount /sys/kernel/config" -- it tears down the whole configfs mount, not
# just its own gadget. On this device that umount always fails EBUSY (observed
# on every run, in both directions). S440adb start then hits
#
#     if [ -d /sys/kernel/config/usb_gadget ]; then
#         echo "Usage: usb configfs already mounted"
#         exit 1
#     fi
#
# and exits 1 without recreating the ADB gadget -- silently, since adbon
# returns 0 regardless. Net effect measured on hardware: after switching to
# storage and back, there is NO gadget bound at all. No ADB, no storage, no
# USB device of any kind, recoverable only by a power cycle.
#
# Both edits below make S440adb tolerate a configfs that someone else mounted:
# mount it only if it is not already there, and refuse only if the ADB gadget
# itself already exists. Verified on hardware: full ADB -> storage -> ADB round
# trip, the card enumerating on the host as a 511.9GB volume, and adb
# reconnecting on its own with no reboot.
ADB_MOUNT_ANCHOR = "\tmount -t configfs none /sys/kernel/config\n"
ADB_MOUNT_FIXED = ("\t[ -d /sys/kernel/config/usb_gadget ] || "
                   "mount -t configfs none /sys/kernel/config\n")
ADB_GUARD_ANCHOR = "\tif [ -d /sys/kernel/config/usb_gadget ]; then\n"
ADB_GUARD_FIXED = "\tif [ -d /sys/kernel/config/usb_gadget/adb_demo ]; then\n"


def fix_usb_mode_switch(root):
    """Make switching back from USB storage to ADB actually work.

    See the comment above for the failure and the evidence. Returns a list of
    the edits applied, or None if the script is missing or already fixed.
    """
    path = os.path.join(root, ADB_INIT_SCRIPT)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        text = fh.read()
    # NB: a plain "usb_gadget/adb_demo ]; then" test is a false positive --
    # the stock stop) branch already contains "if [ ! -d .../adb_demo ]; then".
    # Match the fixed guard line exactly instead.
    if ADB_GUARD_FIXED in text:
        return None                      # already fixed
    applied = []
    if text.count(ADB_MOUNT_ANCHOR) == 1:
        text = text.replace(ADB_MOUNT_ANCHOR, ADB_MOUNT_FIXED, 1)
        applied.append("mount configfs only if not already mounted")
    if text.count(ADB_GUARD_ANCHOR) == 1:
        text = text.replace(ADB_GUARD_ANCHOR, ADB_GUARD_FIXED, 1)
        applied.append("refuse only if adb_demo already exists")
    if not applied:
        return None
    with open(path, "w") as fh:
        fh.write(text)
    return applied



ADBOFF = "usr/bin/adboff"
ADBON = "usr/bin/adbon"

# Stock hands the whole card to the host without ever letting go of it:
# usb_dev_mass_storage.sh exports /dev/mmcblk0 while /dev/mmcblk0p1 is still
# mounted read-write on the device. Two independent writers on one exFAT
# filesystem is a corruption risk, and it only ever worked because stock's
# hiby_player owned the card itself -- under --standalone the supervisor
# mounts it and nothing releases it.
#
# These replacements wrap the vendor's own two commands, unchanged, with a
# release on the way out and a re-mount on the way back. Verified on
# hardware: "sd mounted=0" while android0 is bound and the host has the
# volume, then remounted with 135 library entries visible and the player
# still running.
ADBOFF_SAFE = '#!/bin/sh\n# ADB -> USB mass storage.\n#\n# Locking: flock, not a lockdir. The app backgrounds this on every tap, so\n# concurrent copies must not race over the single UDC -- but a lock is only\n# safe if a holder that dies or hangs cannot wedge the feature. flock is\n# released by the kernel when the holder dies (SIGKILL included), and the\n# critical section below is kept to gadget manipulation only: the SD card\n# mount, which is the operation that can block on I/O, is deliberately\n# outside it. An earlier mkdir-based lock wedged exactly this way and needed\n# a reboot to clear.\n#\n# The card is released before exporting it: stock hands /dev/mmcblk0 to the\n# host while /dev/mmcblk0p1 is still mounted rw here, which is two writers on\n# one exFAT filesystem.\nSD=/usr/data/mnt/sd_0\nLOG=/usr/data/usbswitch.log\nstep() { echo "$(date) adboff: $*" >> "$LOG"; }\n\nexec 9>/tmp/usbswitch.lock\nif ! flock -n 9; then step "switch already in progress, ignoring"; exit 0; fi\n\nif [ -n "$(cat /sys/kernel/config/usb_gadget/android0/UDC 2>/dev/null)" ]; then\n    step "already in storage mode, nothing to do"; exit 0\nfi\n\nsync\nif mount | grep -q " $SD "; then\n    if umount "$SD" 2>/dev/null; then step "card unmounted"\n    elif mount -o remount,ro "$SD" 2>/dev/null; then step "card busy, remounted read-only"\n    else step "WARNING could not release card"; fi\nfi\nsync\n\nstep "stopping adb gadget"\n/etc/init.d/adb/S440adb stop >/dev/null 2>&1\nstep "starting mass storage"\n/usr/bin/usb_dev_mass_storage.sh start /dev/mmcblk0 >/dev/null 2>&1\nstep "done, android0=[$(cat /sys/kernel/config/usb_gadget/android0/UDC 2>/dev/null)]"\nexit 0\n'

ADBON_SAFE = '#!/bin/sh\n# USB mass storage -> ADB. Shares adboff\'s flock; see that script\'s header.\n#\n# Order matters here. First ask the gadget to eject the medium: writing an\n# empty string to lun.0/file closes the LUN and reports SS_MEDIUM_NOT_PRESENT,\n# which is a far politer teardown than yanking the gadget while the host has\n# it open. The kernel refuses this with EBUSY if the host has set PREVENT\n# MEDIUM REMOVAL (macOS does while a volume is mounted), so it is best-effort\n# and its failure is not fatal.\n#\n# The card remount happens AFTER the lock is dropped. mount can block in\n# uninterruptible I/O when the host has not let go, and holding the switch\n# lock across that is what previously wedged the feature until a reboot.\nSD=/usr/data/mnt/sd_0\nLUN=/sys/kernel/config/usb_gadget/android0/functions/mass_storage.0/lun.0/file\nLOG=/usr/data/usbswitch.log\nstep() { echo "$(date) adbon: $*" >> "$LOG"; }\n\nexec 9>/tmp/usbswitch.lock\nif ! flock -n 9; then step "switch already in progress, ignoring"; exit 0; fi\n\nif [ -n "$(cat /sys/kernel/config/usb_gadget/adb_demo/UDC 2>/dev/null)" ]; then\n    step "already in ADB mode, nothing to do"; exit 0\nfi\n\nif [ -f "$LUN" ]; then\n    if echo "" > "$LUN" 2>/dev/null; then step "medium ejected"\n    else step "medium eject refused (host holds it) -- continuing"; fi\nfi\n\nstep "stopping mass storage"\n/usr/bin/usb_dev_mass_storage.sh stop /dev/mmcblk0 >/dev/null 2>&1\nstep "starting adb gadget"\n/etc/init.d/adb/S440adb start >/dev/null 2>&1\nstep "adb gadget up, adb_demo=[$(cat /sys/kernel/config/usb_gadget/adb_demo/UDC 2>/dev/null)]"\n\n# Drop the lock before touching the card: a blocking mount must never be able\n# to stop the next switch.\nflock -u 9 2>/dev/null\nexec 9>&-\n\nif mount | grep -q " $SD "; then\n    umount "$SD" 2>/dev/null || mount -o remount,rw "$SD" 2>/dev/null\nfi\nmkdir -p "$SD"\nif mount | grep -q " $SD "; then\n    step "card already mounted (rw)"\nelse\n    step "remounting card"\n    mount -t exfat -o fmask=0022,dmask=0022 /dev/mmcblk0p1 "$SD" 2>/dev/null \\\n        && step "card remounted" || step "WARNING remount failed"\nfi\nstep "done"\nexit 0\n'


def protect_sd_during_storage(root):
    """Release the SD card before exporting it, and re-mount it afterwards.

    Replaces adboff/adbon rather than patching them -- they are four lines
    each and the replacements run the vendor's identical commands. Both are
    checked for the expected vendor content first, so a firmware whose
    scripts differ is left alone rather than silently overwritten.

    They also serialise, on an flock rather than a lockdir. The app
    backgrounds these on every tap, so a user tapping again because nothing
    visibly happened got a second copy racing the first over the single UDC
    -- observed live as three adboff invocations in three seconds, and
    adbon/adboff overlapping in the same second, leaving no gadget bound at
    all. Verified by firing five concurrent adboff and three concurrent adbon
    on hardware: four and two respectively were refused, and the switch
    completed correctly both ways with the player still alive.

    A first attempt used a mkdir lockdir with a pid file and treated a lock
    as stale only if the recorded pid was dead. That is defeated by a holder
    which is alive but BLOCKED, and it duly wedged: one adbon took the lock,
    never returned, and every later tap was correctly refused until a reboot
    cleared /tmp. flock is used instead because the kernel releases it when
    the holder dies, SIGKILL included -- verified by killing a holder and
    confirming the next run proceeds.

    More importantly the card remount now happens OUTSIDE the critical
    section. mount is the operation that can block in uninterruptible I/O
    when the host has not released the volume, and holding the switch lock
    across it is what caused that wedge. adbon also asks the gadget to eject
    the medium first (an empty write to lun.0/file closes the LUN and reports
    SS_MEDIUM_NOT_PRESENT) rather than yanking it, which is best-effort only:
    fsg_store_file returns -EBUSY when the host has set PREVENT MEDIUM
    REMOVAL, as macOS does whenever a volume is mounted.

    Preferred path is a full umount: measured on hardware, library_standalone
    holds zero fds under the mount when idle, so it succeeds. The fallback is
    remount read-only, which still leaves the host as the only writer -- the
    property that actually prevents corruption -- for the case where a track
    is open mid-playback.

    Returns the list of files rewritten, or None if there is nothing to do.
    """
    done = []
    for name, body, must_have in (
            (ADBOFF, ADBOFF_SAFE, ("S440adb stop", "usb_dev_mass_storage.sh start")),
            (ADBON, ADBON_SAFE, ("usb_dev_mass_storage.sh stop", "S440adb start"))):
        path = os.path.join(root, name)
        if not os.path.exists(path):
            continue
        with open(path) as fh:
            cur = fh.read()
        if "flock -n 9" in cur:
            continue                     # already ours (flock version)
        if not all(m in cur for m in must_have):
            continue                     # not the script this was written against
        with open(path, "w") as fh:
            fh.write(body)
        os.chmod(path, 0o755)
        done.append(name)
    return done or None



def start_patchram_earlier(root):
    """Get brcm_patchram_plus started before the module script, not after it.

    patchram costs 4.26s and cannot be shortened -- strace showed 4.188s of it
    blocked in read() on the UART, 191 HCI commands at ~22ms of chip-side
    processing each. But it is *idle* waiting, so overlapping it with the
    module loads is a genuine win on this single-core device, unlike
    backgrounding CPU-bound work (see background_touch_module(), which
    measured flat over three boots for exactly that reason).

    Today bt_init cannot start until S21 mounts /usr/data, purely to read
    bt_macaddr.txt, and S21 runs after S11module_driver_default. So patchram
    begins around 2.8s. ubifs needs no vendor module -- mtd/NAND is built into
    the kernel -- so the mount can move ahead of the module script:

        S11amount_ubifs   (was S21mount_ubifs)
        S11b_bt_init      (was S22_bt_init) -- already backgrounded by its own
                          init script, so it returns at once and waits on
                          rfkill0 while the modules load
        S11module_driver_default

    and cywdhd moves up within the module script so rfkill0 appears sooner.
    It cannot go first: lsmod shows it depends on soc_msc (SDIO) and
    soc_utils, so straight after soc_msc.sh is the earliest slot.

    Returns a list of what changed, or None.
    """
    initd = os.path.join(root, "etc/init.d")
    done = []
    for old, new in (("S21mount_ubifs", "S11amount_ubifs"),
                     ("S22_bt_init", "S11b_bt_init"),
                     ("S80_bt_init", "S11b_bt_init")):
        src, dst = os.path.join(initd, old), os.path.join(initd, new)
        if os.path.exists(src) and not os.path.exists(dst):
            os.rename(src, dst)
            done.append("%s -> %s" % (old, new))

    path = os.path.join(root, MODULE_INIT_SCRIPT)
    if os.path.exists(path):
        with open(path) as fh:
            lines = fh.read().splitlines()
        cyw = [l for l in lines if l.strip().rstrip("&").strip() == "sh cywdhd.sh"]
        if cyw and any(l.strip() == "sh soc_msc.sh" for l in lines) \
               and any(l.strip() == "sh soc_gpio.sh" for l in lines):
            # Pull soc_msc up too, not just cywdhd: measured on LEAN_4h, moving
            # cywdhd alone only bought 0.19s because it still had to wait for
            # soc_msc sitting 14 modules deep. Placed straight after soc_gpio so
            # utils/soc_utils/soc_i2c/axp2101 (power) and soc_gpio (pins) still
            # precede it; only the display/audio stack (soc_pwm, pwm_backlight,
            # soc_fb, soc_aic) is skipped, which SDIO does not need.
            drop = ("sh cywdhd.sh", "sh soc_msc.sh")
            rest = [l for l in lines if l.strip().rstrip("&").strip() not in drop]
            out = []
            for l in rest:
                out.append(l)
                if l.strip() == "sh soc_gpio.sh":
                    out += ["# soc_msc + cywdhd moved up from positions 14 and 20:",
                            "# cywdhd registers md_bcmdhd_bt_power (and so rfkill0) at the",
                            "# start of dhd_module_init, and bt_init is now waiting on it",
                            "# from S11b. cywdhd depends on soc_msc and soc_utils, so this",
                            "# is the earliest either can go.",
                            "sh soc_msc.sh",
                            cyw[0].strip()]
            with open(path, "w") as fh:
                fh.write("\n".join(out) + "\n")
            done.append("soc_msc + cywdhd moved up to follow soc_gpio")
    return done or None



BT_TRACE_HELPER = """
# Boot-time tracing. Each stage appends uptime to /usr/data/btboot.log, so the
# ~1s gap between rfkill0 appearing and patchram actually starting can be
# attributed instead of guessed at. One echo per stage; negligible against a
# 4.26s patchram.
bt_t() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> /usr/data/btboot.log; }
rm -f /usr/data/btboot.log
bt_t "script start"
"""


def trace_bt_init(root):
    """Add per-stage uptime stamps to bt_init. Runs after the timing patch."""
    path = os.path.join(root, BT_INIT)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        t = fh.read()
    if "bt_t()" in t:
        return None
    marks = (
        ("bt_wait 10 'grep -ql bluetooth /sys/class/rfkill/rfkill*/name'", "a bluetooth rfkill present", True),
        ('echo "BT_MACADDR $bt_addr"', "mac resolved", True),
        ('echo "Selected firmware: $firmware"', "firmware chosen", True),
        ("bt_wait 15 '[ -d /sys/class/bluetooth/hci0 ]'", "hci0 present", True),
        ("hciconfig hci0 up", "hci0 up issued", False),
        ("/usr/libexec/bluetooth/bluetoothd -E -C &", "bluetoothd started", True),
        ("hciconfig hci0 reset", "reset issued", False),
        ("bt-agent -c NoInputNoOutput &", "bt-agent started", True),
        ("/usr/bin/bluealsa -p a2dp-source", "bluealsa started", True),
        ("sdptool add HIBYLINK_SP", "sdptool done", True),
        ("echo   > /tmp/bt_init_ok", "done", False),
    )
    n = 0
    for anchor, label, after in marks:
        if t.count(anchor) != 1:
            continue
        stamp = 'bt_t "%s"\n' % label
        t = t.replace(anchor, (anchor + "\n" + stamp) if after else (stamp + anchor), 1)
        n += 1
    # helper goes right after the shebang
    t = t.replace("#!/bin/sh\n", "#!/bin/sh\n" + BT_TRACE_HELPER, 1)
    with open(path, "w") as fh:
        fh.write(t)
    return n



def bt_mac_before_rfkill_wait(root):
    """Do the MAC lookup while bt_init is waiting for rfkill0, not after it.

    Boot trace (LEAN_4trace, /usr/data/btboot.log) against dmesg:

        1.04  script start
        1.67  rfkill0 appears (dhd_module_init)      0.63s of idle waiting
        2.50  mac resolved                           0.83s to read one file
        2.54  firmware chosen                        (+0.04s)
        6.90  hci0                                   4.36s of patchram

    That 0.83s is a $(cat) on a 17-byte file plus a couple of tests, and it
    is slow only because the module script is saturating the single core at
    that moment -- SDIO enumerates at 2.08s, right in the middle of it. The
    work has no dependency on rfkill0, so it can happen during the 0.63s the
    script is already blocked, instead of after.

    The firmware selection deliberately stays where it is: it reads
    chipvendor from an SDIO sysfs path that does not exist until the card
    enumerates, and its else-branch picks the aw-nb372sm blob rather than
    this board's AP6212 one -- so running it early would silently load the
    wrong firmware.
    """
    path = os.path.join(root, BT_INIT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        t = fh.read()
    # Anchor on text present both before and after patch_bt_init_timing(),
    # since this runs earlier in the pass than that does.
    marker = "# bluetooth power on\n"
    if marker not in t:
        return False
    m = re.search(r"# Delete the previous Bluetooth address storage file\.\n"
                  r".*?echo \"BT_MACADDR \$bt_addr\"\n", t, re.S)
    if not m:
        return False
    block = m.group(0)
    if t.index(block) < t.index(marker):
        return False                     # already ahead of the radio bring-up
    t = t.replace(block, "", 1)
    t = t.replace(marker,
                  "# MAC first: it needs nothing from the radio, and doing it here\n"
                  "# overlaps it with the rfkill0 wait instead of paying for it after.\n"
                  + block + "\n" + marker, 1)
    with open(path, "w") as fh:
        fh.write(t)
    return True



def bt_gpio_power_test(root):
    """EXPERIMENT: power the BT radio from userspace, without cywdhd.

    Today bt_init reaches BT_REG_ON only via cywdhd: wait for it to register
    rfkill0 (1.67s), then write rfkill0/state, which costs a further 0.86s
    inside the driver. That is ~2.5s spent to raise one GPIO before patchram
    can even start.

    cywdhd itself names the pin: gpio_bt_reg_on=PB04, i.e. gpio 36 (bank B
    starts at 32). Driving it directly from /sys/class/gpio should let
    patchram start at ~1.05s instead of 2.52s -- worth ~1.5s, which is the
    difference between ~8.6s and ~7.0s to bt_init_ok.

    The open question this build exists to answer: does the BT side need the
    32kHz LPO clock enabled by cywdhd? If the LPO comes from the SoC or PMIC
    instead, patchram will succeed with cywdhd not yet loaded and the idea is
    live. If hci0 never appears, it is dead and the seam is closed.

    To keep that question clean, cywdhd is deferred to the END of the module
    script here, so it cannot drive PB04 low mid-download. That is a test
    configuration only -- it delays WiFi and may leave cywdhd unable to claim
    the pin we are holding, which would take rfkill0 (and so the app's BT
    toggle, bt_suspend and bt_resume) with it.

    The firmware blob is hardcoded to the AP6212 one. Its usual selector
    reads chipvendor from an SDIO path that will not exist this early, and
    that path's else-branch picks the aw-nb372sm blob -- wrong for this board.
    """
    path = os.path.join(root, BT_INIT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        t = fh.read()

    # Anchor on the bare rfkill write: present in stock and in every patched
    # form. This runs before patch_bt_init_timing(), so the bt_wait variant
    # does not exist yet.
    old_power = "echo 1 > /sys/class/rfkill/rfkill0/state"
    if old_power not in t:
        return False
    t = t.replace("sleep 1 # if invoke this script in c with system(), "
                  "must sleep for a while!!!!!\n", "", 1)
    t = t.replace("bt_wait 10 \'[ -e /sys/class/rfkill/rfkill0/state ]\'\n", "", 1)
    new_power = """# EXPERIMENT: raise BT_REG_ON (PB04 = gpio 36) directly, rather than
# waiting for cywdhd to register rfkill0 and then writing to it.
# Both rails, not just BT. On these combo parts WL_REG_ON (PB03, gpio 35)\n# gates the chip's internal supply, and the BT core does not run without it:\n# raising BT_REG_ON alone attached the UART line discipline and created hci0,\n# but every HCI command then timed out (0x1003/0x1001/0x1009, repeatedly) on\n# the BT_GPIO_TEST build.\nWL_GPIO=35\nBT_GPIO=36\nBTLOG=/usr/data/btboot.log\nfor g in $WL_GPIO $BT_GPIO; do\n    [ -d /sys/class/gpio/gpio$g ] || echo $g > /sys/class/gpio/export 2>/dev/null\n    echo out > /sys/class/gpio/gpio$g/direction 2>/dev/null\n    echo 1   > /sys/class/gpio/gpio$g/value     2>/dev/null\ndone
if [ ! -d /sys/class/gpio/gpio$BT_GPIO ]; then
    echo $BT_GPIO > /sys/class/gpio/export 2>/dev/null \
        && echo "$(cat /proc/uptime | cut -d\" \" -f1) gpio exported" >> $BTLOG \\
        || echo "$(cat /proc/uptime | cut -d\" \" -f1) gpio EXPORT FAILED" >> $BTLOG
fi
echo out  > /sys/class/gpio/gpio$BT_GPIO/direction 2>/dev/null
echo 1    > /sys/class/gpio/gpio$BT_GPIO/value     2>/dev/null
echo "$(cat /proc/uptime | cut -d\" \" -f1) BT_REG_ON=$(cat /sys/class/gpio/gpio$BT_GPIO/value 2>/dev/null)" >> $BTLOG
"""
    t = t.replace(old_power, new_power, 1)

    # Hardcode the AP6212 blob: the chipvendor path needs SDIO, which has not
    # enumerated yet at this point in boot.
    import re
    t = re.sub(r"CHIPVENDOR_PATH=.*?\nfi\n",
               'firmware=BCM4343A1_001.002.009.1010.1030.hcd   # AP6212, hardcoded for the test\n',
               t, count=1, flags=re.S)

    with open(path, "w") as fh:
        fh.write(t)

    # cywdhd to the very end, so it cannot touch PB04 during the download
    mpath = os.path.join(root, MODULE_INIT_SCRIPT)
    if os.path.exists(mpath):
        with open(mpath) as fh:
            lines = fh.read().splitlines()
        cyw = [l for l in lines if l.strip().rstrip("&").strip() == "sh cywdhd.sh"]
        if cyw:
            rest = [l for l in lines if l.strip().rstrip("&").strip() != "sh cywdhd.sh"]
            rest += ["# TEST BUILD: deferred to the end so it cannot drive PB04 low",
                     "# while brcm_patchram_plus is mid-download.", cyw[0].strip()]
            with open(mpath, "w") as fh:
                fh.write("\n".join(rest) + "\n")
    return True



def source_module_scripts(root):
    """Source the per-module scripts instead of spawning a shell for each.

    driver_default_init_script.sh runs "sh foo.sh" 28 times; each foo.sh then
    execs insmod. That is 56 process spawns, and spawn costs 4.3ms on this
    device (measured: 28 spawns = 0.120s). Sourcing runs them in the current
    shell, so the wrapper spawn disappears -- measured 0.120s -> 0.010s for 28,
    about 110ms. The insmod exec itself remains; only built-in modules would
    remove that, and these are closed blobs with no source.

    Safe here because none of the 28 scripts contains "exit" or uses "$0" --
    checked on device. They are also already run in sequence, so the shared
    shell state sourcing introduces changes nothing about ordering.
    """
    path = os.path.join(root, MODULE_INIT_SCRIPT)
    if not os.path.exists(path):
        return 0
    with open(path) as fh:
        lines = fh.read().splitlines()
    n, out = 0, []
    for l in lines:
        st = l.strip()
        if st.startswith("sh ") and st.endswith(".sh"):
            out.append(l.replace("sh ", ". ./", 1)); n += 1
        elif st.startswith("sh ") and st.endswith(".sh &"):
            out.append(l)          # backgrounded: must stay a subshell
        else:
            out.append(l)
    if n:
        with open(path, "w") as fh:
            fh.write("\n".join(out) + "\n")
    return n



BT_POWER_OFF_LINE = 'bt-adapter --set "Powered" "Off"\n'

BT_KEEP_POWERED = """# Stock ends by switching the radio off, on the assumption Bluetooth should
# start disabled. But library_standalone restores the user's saved state
# moments later (music_hook.c's restore_conf -> st_bt_set), so with
# bt_enabled=1 the radio is powered down and straight back up again. Measured
# on hardware: adapter UP at 6.88s, DOWN, UP again at 8.96s -- 2.1s of
# Bluetooth being taken away and given back on every boot.
#
# st_bt_set() is only called when the saved state differs from the live one
# ("bt_saved != st_bt_on()"), so leaving the adapter powered here means the
# app does nothing and the whole round trip disappears. Discoverable is set
# because that is the other half of what bt_enable would have done.
#
# music.conf lives on /usr/data (internal NAND), so it is readable at this
# point in boot -- S11amount_ubifs has already mounted it.
if grep -qE "^[[:space:]]*bt_enabled[[:space:]]*=[[:space:]]*1" /usr/data/music.conf 2>/dev/null; then
    bt-adapter --set "Discoverable" "On"
else
    bt-adapter --set "Powered" "Off"
fi
"""


def bt_keep_powered_when_enabled(root):
    """Don't power the radio down if the app is just going to power it up.

    Returns True if bt_init was changed.
    """
    path = os.path.join(root, BT_INIT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        t = fh.read()
    if "bt_enabled" in t:
        return False                     # already done
    if t.count(BT_POWER_OFF_LINE) != 1:
        return False
    t = t.replace(BT_POWER_OFF_LINE, BT_KEEP_POWERED, 1)
    with open(path, "w") as fh:
        fh.write(t)
    return True



# brcm_patchram_plus has no read timeout. When the chip stops responding
# mid-handshake it blocks in read(/dev/ttyS0, buf, 3) forever, waiting for a
# 3-byte HCI event header that never arrives -- caught live on 2026-09-06:
#
#   patchram pid=881 state=S wchan=wait_woken
#   syscall args: fd=0x4 (=/dev/ttyS0) buf=0x412980 count=0x3
#
# hci0 is only created once that handshake completes, so Bluetooth never
# appears at all. Measured rate across 60 boots: 4-5, roughly 7%. It is not
# something this project introduced -- it is upstream of every script change
# here, and stock hits the same stall but hides it, since its blind "sleep 5"
# just proceeds and bt_init finishes at ~16s with no adapter and no complaint.
#
# So: give patchram a watchdog. If hci0 has not appeared in PATCHRAM_WAIT
# seconds, kill it, power-cycle the radio through rfkill (the chip needs a
# real power cycle, not just a restart -- a stalled BT core does not recover
# otherwise), and try again. Turns "no Bluetooth until you reboot" into "a
# few seconds late" on the boots that stall.
BT_HCI0_WAIT = "bt_wait 15 '[ -d /sys/class/bluetooth/hci0 ]'\n"

BT_RETRY_BLOCK = """# patchram watchdog. Normal completion is ~4.3s (measured), so 8s without
# hci0 means the chip has stopped answering, not that it is being slow.
PATCHRAM_TRIES=3
_try=1
while :; do
    bt_wait 8 '[ -d /sys/class/bluetooth/hci0 ]' && break
    [ "$_try" -ge "$PATCHRAM_TRIES" ] && break
    echo "bt_init: patchram stalled (try $_try), power-cycling the radio" >&2
    killall brcm_patchram_plus 2>/dev/null
    # The rail has to drop: a stalled BT core does not recover from a fresh
    # patchram alone.
    echo 0 > /sys/class/rfkill/rfkill0/state 2>/dev/null
    usleep 200000
    echo 1 > /sys/class/rfkill/rfkill0/state 2>/dev/null
    usleep 200000
    brcm_patchram_plus --enable_hci --baudrate 3000000 --no2bytes \\
        --patchram /lib/firmware/bt_bcm/$firmware /dev/ttyS0 \\
        --tosleep=50000 --use_baudrate_for_download --enable_lpm \\
        --bd_addr $bt_addr &
    _try=$(( _try + 1 ))
done
"""


def bt_patchram_retry(text):
    """Retry a stalled brcm_patchram_plus instead of waiting on it forever.

    brcm_patchram_plus has no read timeout. When the chip stops responding
    mid-handshake it blocks in read(/dev/ttyS0, buf, 3) forever, waiting for a
    3-byte HCI event header that never arrives -- caught live 2026-09-06:

        patchram pid=881 state=S wchan=wait_woken
        syscall args: fd=0x4 (=/dev/ttyS0) buf=0x412980 count=0x3

    hci0 is only created once that handshake finishes, so Bluetooth never
    appears at all. Measured across 60 boots: 4-5 of them, roughly 7%. Not
    something this project introduced -- it is upstream of every script change
    here, and stock hits the same stall but hides it: its blind "sleep 5" just
    proceeds, and bt_init finishes at ~16s with no adapter and no complaint.
    The ~32s boots recorded earlier were this, not slow boots -- 15+5+5+5 is
    exactly bt_init's timeout cascade with no hci0.

    Only the hci0 wait is replaced; the launch line and the D-Bus block that
    now sits between them are left alone. Returns the new text, or None.
    Must run AFTER patch_bt_init_timing(), which creates the bt_wait this
    depends on, and inside the same in-memory chain -- writing the file
    directly here would be clobbered by the single write at the end.
    """
    if "PATCHRAM_TRIES" in text:
        return None                      # already done
    if text.count(BT_HCI0_WAIT) != 1:
        return None
    return text.replace(BT_HCI0_WAIT, BT_RETRY_BLOCK, 1)




BT_POWERED_ON_LINE = 'bt-adapter --set "Powered" "On"\n'

# Start the connect the moment bluez has the adapter powered, rather than at
# the end of the script. Measured 2026-09-07 with the working bluetoothctl
# connect: the connect itself costs 0.35s and succeeds on the first attempt
# every time (how=ours tries=1), so all remaining latency was the 8.4s of
# bt_init ahead of it. Between this line and bt_init_ok sit the Alias, a D-Bus
# ping wait, sdptool add HIBYLINK_SP and the Discoverable call -- about 1.4s
# the connect does not depend on. Backgrounded, so it overlaps them.
#
# An earlier attempt to move it here (LEAN_4u) appeared to make things worse,
# 15s -> 19s. That result was void: the connect command in use at the time
# (bt-device --connect) never worked at all, so what was being timed was the
# headset's own inbound paging, which the btconn.log shows varying between 24s
# and 70s. Nothing was learned from it either way.
BT_CONNECT_BLOCK = BT_POWERED_ON_LINE + """
(
  if grep -qE "^[[:space:]]*bt_enabled[[:space:]]*=[[:space:]]*1" /usr/data/music.conf 2>/dev/null; then
    _mac=$(grep -oE "([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" /usr/data/bt_lastused.txt 2>/dev/null | tail -1)
    if [ -n "$_mac" ]; then
      # Use the EXACT command the Settings tap runs -- bt_pair() in
      # app/status.c pipes an agent registration and a connect into
      # bluetoothctl. That path is known to work every time by hand, so the
      # boot path should not invent its own mechanism; two earlier attempts
      # did, and both were wrong:
      #   bt-device --connect  -- legacy bluez API, fails instantly (rc=1, 0s)
      #                           with org.bluez.Error.AlreadyExists on an
      #                           already-paired device, attempting nothing.
      #                           Every "retry" was a no-op plus a sleep, which
      #                           is why tuning the cadence never moved anything.
      #   Device1.Connect      -- works when invoked by hand, but did not
      #                           connect at boot.
      # The likely difference is the agent: bluetoothctl registers one inside
      # its own session. Verified on hardware 2026-09-07: cleared ACL, ran this
      # pipe, live ACL back in under 1s, direction "<" (outgoing, i.e. ours).
      # Self-measuring: record when the link came up, how many attempts it
      # took, and -- crucially -- WHO established it. "incoming" means the
      # headset paged us before our attempt landed, "ours" means Device1.Connect
      # won. Without this the two are indistinguishable after the fact: there is
      # no logread on this device, nothing in /var/log, and the bt_lastused
      # recorder only writes when the MAC changes, so a reconnect to the same
      # headset leaves no trace at all. Inferring it from hcitool's </> column
      # is not good enough to build on.
      _t0=$(cut -d" " -f1 /proc/uptime)
      _try=0; _how=timeout
      # Window widened to ~60s: 6 tries over ~21s was giving up while the
      # headset was still coming up.
      for _gap in 1 2 3 5 5 5 10 10 10 10; do
        if hcitool con 2>/dev/null | grep -qi "$_mac"; then _how=incoming; break; fi
        _try=$((_try + 1))
        printf "agent NoInputNoOutput\ndefault-agent\nconnect $_mac\nquit\n" \
          | bluetoothctl >/dev/null 2>&1
        if hcitool con 2>/dev/null | grep -qi "$_mac"; then _how=ours; break; fi
        sleep $_gap
      done
      _t1=$(cut -d" " -f1 /proc/uptime)
      echo "$(date -u "+%Y-%m-%d %H:%M:%S") mac=$_mac how=$_how tries=$_try from=${_t0}s to=${_t1}s" \
        >> /usr/data/btconn.log
      if [ "$(wc -l < /usr/data/btconn.log 2>/dev/null || echo 0)" -gt 200 ]; then
        tail -100 /usr/data/btconn.log > /usr/data/btconn.log.tmp \
          && mv /usr/data/btconn.log.tmp /usr/data/btconn.log
      fi
    fi
  fi
) &
"""

BT_OK_LINE = "echo   > /tmp/bt_init_ok\n"

BT_RECONNECT_BLOCK = """echo   > /tmp/bt_init_ok

# --- keep bt_lastused.txt current, and reconnect to what it names ------------
# Two problems, one fix.
#
# 1. bt_lastused.txt is HiBy's own record of which headset was last used, and
#    it is written by hiby_player -- which RP1 replaced with
#    library_standalone. So it froze: observed newest entry 2026-08-23 while a
#    different headset was actually connected. Anything reading it has been
#    working from stale data ever since. The recorder below keeps it current,
#    in HiBy's own "MAC YYYY-MM-DD HH:MM:SS GMT" format, so the file means
#    again what it claims to mean.
#
# 2. The R1 never pages out. PSCAN is on and every paired device is
#    Trusted=true, so an incoming page is accepted, but bluez's [Policy]
#    section is empty and its reconnect logic only re-establishes a link that
#    just dropped. A headset switched on before or during boot pages, gets no
#    answer, exhausts its attempts and gives up -- which is why the R1 had to
#    be turned on first. The reconnect below pages the last-used device
#    instead of waiting to be found.
BT_LASTUSED=/usr/data/bt_lastused.txt

(
  while :; do
    _cur=$(hcitool con 2>/dev/null | grep -oE "([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" | head -1)
    if [ -n "$_cur" ]; then
      # 3. Keep alsa.conf pointed at the device that is actually connected.
      #    Stock hiby_player rewrote this per device; library_standalone does
      #    not reference the file at all, so it froze holding whichever MAC was
      #    connected when hiby_player last ran. ALSA then routes to a headset
      #    that is not there -- silent failure, no error anywhere. Only the MAC
      #    is substituted, so the codec/eqmid settings in the file are kept.
      if [ -f /usr/data/alsa.conf ]; then
        _old=$(grep -oE "([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" /usr/data/alsa.conf 2>/dev/null | head -1)
        if [ -n "$_old" ] && [ "$_old" != "$_cur" ]; then
          sed "s/$_old/$_cur/g" /usr/data/alsa.conf > /usr/data/alsa.conf.tmp 2>/dev/null \
            && mv /usr/data/alsa.conf.tmp /usr/data/alsa.conf
        fi
      fi
      _prev=$(grep -oE "([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" $BT_LASTUSED 2>/dev/null | tail -1)
      if [ "$_cur" != "$_prev" ]; then
        echo "$_cur $(date -u "+%Y-%m-%d %H:%M:%S") GMT" >> $BT_LASTUSED
        # keep it bounded; it is a log, not an archive
        if [ "$(wc -l < $BT_LASTUSED 2>/dev/null || echo 0)" -gt 200 ]; then
          tail -100 $BT_LASTUSED > $BT_LASTUSED.tmp && mv $BT_LASTUSED.tmp $BT_LASTUSED
        fi
      fi
    fi
    sleep 10
  done
) &


"""


def bt_reconnect_last_device(text):
    """Keep bt_lastused.txt current, and page that device at boot.

    Fixes HiBy's own record rather than inventing a parallel one, so anything
    else reading it works again too. Returns the new text, or None.
    """
    if "BT_LASTUSED=" in text:
        return None
    if text.count(BT_OK_LINE) != 1 or text.count(BT_POWERED_ON_LINE) != 1:
        return None
    text = text.replace(BT_POWERED_ON_LINE, BT_CONNECT_BLOCK, 1)
    return text.replace(BT_OK_LINE, BT_RECONNECT_BLOCK, 1)



def trace_bt_steps(text):
    """Stamp /proc/uptime after each bring-up milestone in bt_init.

    The connect is now 0.35s and lands first try, and patchram's 4.34s plus
    the 0.86s rfkill are chip-side and fixed. What is left unaccounted for is
    the ~1.1s between hci0 appearing and the adapter being powered -- eight
    steps, each followed by a bt_wait poll. Some of that is real daemon
    startup and some is poll granularity waiting on something already ready,
    and guessing which has gone badly twice today (the "1.4s" between Powered
    On and bt_init_ok turned out to be 0.35s). So measure it rather than
    reason about it: one echo plus a /proc/uptime read per step, ~35ms total.

    Returns the new text, or None if the anchors do not match exactly.
    """
    if "bt_stamp" in text:
        return None

    helper = ('BTSTEPS=/usr/data/btsteps.log\n'
              'bt_stamp() { echo "$1 $(cut -d\' \' -f1 /proc/uptime)" >> $BTSTEPS; }\n')

    # Anchor the helper right after bt_wait's definition, so every later call
    # is in scope.
    marker = "\nhciconfig hci0 up\n"
    if text.count(marker) != 1:
        return None
    text = text.replace(marker, "\n" + helper + "\n: > $BTSTEPS\nbt_stamp hci0_present\n"
                        + "hciconfig hci0 up\n", 1)

    # (line, occurrence index, label) -- the UP RUNNING wait appears twice, so
    # occurrences are counted rather than replaced blindly.
    stamps = [
        ("bt_wait 5 'hciconfig hci0 | grep -q \"UP RUNNING\"'", 0, "hci0_up"),
        ("/usr/libexec/bluetooth/bluetoothd -E -C &", 0, "bluetoothd_spawned"),
        ("bt_wait 5 'dbus-send --system --print-reply --dest=org.bluez / "
         "org.freedesktop.DBus.Peer.Ping'", 0, "bluez_dbus_ready"),
        ("hciconfig hci0 reset", 0, "reset_issued"),
        ("bt_wait 5 'hciconfig hci0 | grep -q \"UP RUNNING\"'", 1, "reset_done"),
        ("bt_wait 5 'pidof bt-agent'", 0, "agent_ready"),
        ("bt_wait 5 'pidof bluealsa'", 0, "bluealsa_ready"),
    ]

    # Keyed by (anchor, occurrence): the "UP RUNNING" wait appears twice and
    # both occurrences are stamped, so a flat scan that stops at the first
    # matching anchor silently loses the second one.
    want = {(a, i): label for a, i, label in stamps}
    seen = {}
    out = []
    for ln in text.split("\n"):
        out.append(ln)
        key = ln.strip()
        n = seen.get(key)
        n = 0 if n is None else n
        label = want.get((key, n))
        if label is not None:
            out.append("bt_stamp " + label)
        if any(key == a for a, _, _ in stamps):
            seen[key] = n + 1
    text = "\n".join(out)

    if text.count("bt_stamp ") != len(stamps) + 1:
        return None
    return text


RTC32K_SCRIPT = "module_driver/soc_utils.sh"


def enable_rtc32k_at_boot(root):
    """Turn the chip's 32kHz LPO on when soc_utils loads, not later.

    The combo chip needs this clock to run. There are TWO rtc32k
    implementations on this device and only one works:

      * built-in  rtc32k_enable()          -- from ingenic_sdio.c, acts only
                                              if wifi_data.pctrl is set, and
                                              nothing ever sets it. A no-op.
      * soc_utils ingenic_rtc32k_enable()  -- the real one. cywdhd calls this
                                              (confirmed in its strings).

    bt_power_bluesleep.c declares `extern void rtc32k_enable(void)`, so as a
    built-in driver it links to the NO-OP. That is why BTPWR_1 and BTPWR_3
    both left the chip half-dead: hci0 at ~32s and a 6s HCI reset, the same
    signature as the userspace-GPIO attempt in bc4ecfe. A built-in cannot link
    to a module's export, so calling the working one would mean building
    bt_power as a module too.

    soc_utils takes a module parameter instead, and the vendor's own script
    passes it as 0. Setting it to 1 enables the clock at module load (~1.16s),
    which is before bt_init writes rfkill0, so the question of who calls
    rtc32k_enable stops mattering.
    """
    path = os.path.join(root, RTC32K_SCRIPT)
    if not os.path.exists(path):
        return False
    with open(path) as fh:
        t = fh.read()
    if "rtc32k_init_on=0" not in t:
        return False
    t = t.replace("rtc32k_init_on=0", "rtc32k_init_on=1", 1)
    with open(path, "w") as fh:
        fh.write(t)
    return True


RAIL_BLOCK = """# --- Power WL_REG_ON before soc_msc probes mmc0 --------------------------
# mmc0 is non-removable, so the MMC core scans it ONCE at host probe and never
# rescans. If the combo chip is not powered by then, the SDIO card never
# enumerates and brcmfmac has nothing to bind to -- measured on BRCM_5:
# soc_msc accepted wifi_reg_on=PB03 but never claimed the pin (gpio35 stayed
# exportable) and mmc0 enumerated no card at all, while mmc1 (the SD card) was
# fine. cywdhd used to do this itself, which is why removing it broke
# enumeration rather than just the driver binding.
#
# Placed after soc_gpio.sh so the GPIO banks exist, and before soc_msc.sh.
_wl_power_on() {
    W=35                      # WL_REG_ON = PB03, bank B starts at 32
    [ -d /sys/class/gpio/gpio$W ] || echo $W > /sys/class/gpio/export 2>/dev/null
    [ -d /sys/class/gpio/gpio$W ] || return 1
    echo out > /sys/class/gpio/gpio$W/direction 2>/dev/null
    echo 0 > /sys/class/gpio/gpio$W/value 2>/dev/null
    usleep 200000
    echo 1 > /sys/class/gpio/gpio$W/value 2>/dev/null
    usleep 300000
}
_wl_power_on
"""


BRCM_MODULES = ("brcmutil.ko", "brcmfmac.ko")
BRCM_FIRMWARE = ("brcmfmac43430-sdio.bin", "brcmfmac43430-sdio.txt")


def switch_to_brcmfmac(root, modules_dir, firmware_dir):
    """Replace cywdhd with mainline brcmfmac. docs/10 step 3.

    Four changes, all in the rootfs -- no kernel rebuild:

    1. soc_msc gets wifi_reg_on=PB03. This is the crux. cywdhd owns WL_REG_ON
       today (its own gpio_wlan_reg_on=PB03), which is why brcmfmac probes
       -1 the moment cywdhd is gone: nothing powers the chip. soc_msc -- the
       SDIO glue, not the blob -- exposes wifi_reg_on/wifi_power_on module
       parameters, both -1 (unset) as shipped. Setting it makes the glue own
       the rail, which is the mainline mmc-pwrseq-simple arrangement in all
       but name.

       mmc-pwrseq-simple itself cannot be used: it attaches via the host's
       of_node, and module_drivers registers its devices from its own parsed
       tree with platform_data instead (md_ingenic,mmc.0 has NO of_node --
       checked on hardware; the msc@ DT nodes are status="disable"). So the
       DT-only route recorded earlier in docs/10 does not exist.

    2. cywdhd is not loaded.
    3. brcmutil.ko + brcmfmac.ko are installed and loaded in its place.
    4. Firmware goes to /lib/firmware/brcm/ under the names mainline asks for
       -- a rename of the vendor blobs, verified compatible on hardware.

    Bluetooth then depends on the bt_power driver in the BTPWR kernels, which
    could not power the chip while cywdhd was present. Whether it can once
    cywdhd is gone and soc_msc holds WL_REG_ON is exactly what this build
    tests; it is not a promise.
    """
    init = os.path.join(root, MODULE_INIT_SCRIPT)
    msc = os.path.join(root, "module_driver/soc_msc.sh")
    if not (os.path.exists(init) and os.path.exists(msc)):
        return "missing module_driver scripts"

    # 1. soc_msc owns WL_REG_ON
    with open(msc) as fh:
        t = fh.read()
    # The script already passes these, defaulted to -1 (unset), so EDIT them
    # rather than appending. An earlier version guarded on "wifi_reg_on=" being
    # absent and therefore silently did nothing -- the shipped default matched.
    if "wifi_reg_on=PB03" not in t:
        if "wifi_reg_on=-1" not in t:
            return "no wifi_reg_on=-1 to set (soc_msc.sh has changed shape)"
        t = t.replace("wifi_reg_on=-1", "wifi_reg_on=PB03", 1)
        t = t.replace("wifi_reg_on_level=0", "wifi_reg_on_level=1", 1)
        with open(msc, "w") as fh:
            fh.write(t)

    # 1b. Power the rail before soc_msc probes mmc0.
    with open(init) as fh:
        t = fh.read()
    if "_wl_power_on" not in t:
        anchor = "sh soc_gpio.sh\n"
        if t.count(anchor) != 1:
            return "no 'sh soc_gpio.sh' line to anchor the rail power-up"
        t = t.replace(anchor, anchor + RAIL_BLOCK, 1)
        with open(init, "w") as fh:
            fh.write(t)

    # 2 + 3. cywdhd out, brcmfmac in
    with open(init) as fh:
        t = fh.read()
    if "sh cywdhd.sh" not in t:
        return "no cywdhd load line found"
    t = t.replace("sh cywdhd.sh &",
                  "# cywdhd REPLACED by mainline brcmfmac -- docs/10 step 3\n"
                  "sh brcmfmac.sh &", 1)
    t = t.replace("sh cywdhd.sh",
                  "# cywdhd REPLACED by mainline brcmfmac -- docs/10 step 3\n"
                  "sh brcmfmac.sh", 1)
    with open(init, "w") as fh:
        fh.write(t)

    loader = os.path.join(root, "module_driver/brcmfmac.sh")
    with open(loader, "w") as fh:
        fh.write("insmod brcmutil.ko\n"
                 "insmod brcmfmac.ko\n")
    os.chmod(loader, 0o755)

    # modules alongside the vendor ones
    for m in BRCM_MODULES:
        src = os.path.join(modules_dir, m)
        if not os.path.exists(src):
            return f"missing {src}"
        shutil.copy2(src, os.path.join(root, "module_driver", m))

    # 4. firmware under mainline's names
    fwdir = os.path.join(root, "lib/firmware/brcm")
    os.makedirs(fwdir, exist_ok=True)
    for f in BRCM_FIRMWARE:
        src = os.path.join(firmware_dir, f)
        if not os.path.exists(src):
            return f"missing {src}"
        shutil.copy2(src, os.path.join(fwdir, f))
    return None


def bt_enable_ssp(text):
    """Assert Simple Pairing Mode before anything can pair.

    Reported against an earlier base: pairing with SSP-only peripherals (a FiiO
    BTR17) failed with Auth Complete: Pairing Not Allowed (0x18) because the
    controller had SSP disabled and bluez fell back to legacy PIN pairing.

    NOT reproducible on this base, checked 2026-09-10 on hardware:
      * Read Simple Pairing Mode (0x03/0x0055) returns 01 on a cold boot.
      * It stays 01 across `hciconfig hci0 reset` -- bluez 5.54 re-applies it.
      * The BTR17's stored record is [LinkKey] Type=4, PINLength=0, i.e. an
        authenticated SSP key, so it did pair over SSP.
    Most likely already fixed as a side effect of keeping the radio powered
    (bt_keep_powered_when_enabled) instead of powering it off and back on.

    Added anyway because it is idempotent, costs one HCI command, and makes the
    guarantee explicit rather than dependent on bluez's timing. Placed AFTER the
    HCI reset -- a reset returns the controller to defaults, so writing it
    earlier would be undone -- and before bt-agent, so nothing can pair first.

    Returns the new text, or None if the anchor is missing.
    """
    if "0x0056" in text:
        return None
    anchor = "bt-agent -c NoInputNoOutput &\n"
    if text.count(anchor) != 1:
        return None
    block = ("# Write Simple Pairing Mode = enabled (OGF 0x03, OCF 0x0056).\n"
             "# After the reset above, which would clear it; before bt-agent,\n"
             "# so no pairing can be attempted with SSP still off.\n"
             "hcitool cmd 0x03 0x0056 0x01 >/dev/null 2>&1\n"
             + anchor)
    return text.replace(anchor, block, 1)

def hasten_bt_init(root):
    """Move bt_init from S80 to S22, so its fixed cost overlaps the rest of boot.

    Measured on hardware (LEAN_4, 2026-09-02): the dominant cost in bt_init is
    brcm_patchram_plus, which takes 4.26s before hci0 appears. That figure is
    *fixed* -- it was measured at --tosleep=50000, 20000, 10000 and 5000 and
    came back 4.25-4.26s every time, and the .hcd is only 46KB, so it is
    neither the per-command sleep nor the 3Mbaud transfer. It is the chip's own
    bring-up plus hardcoded delays inside brcm_patchram_plus, and we have no
    source for that binary. So it cannot be made shorter -- only started sooner.

    S80_bt_init already runs the script backgrounded ("$PL01 &"), so nothing
    downstream waits on it. Its real dependencies are:

      - /usr/data mounted, for bt_macaddr.txt and alsa.conf   -> S21mount_ubifs
      - rfkill0, i.e. cywdhd loaded                           -> S11, and the
        patched script now waits for the node rather than assuming it
      - /dev/ttyS0                                            -> kernel, always
      - its own dbus-daemon, which it starts itself           -> not S30dbus

    So S22 is the first slot where it can run, and moving it there overlaps
    those 4.26s with S30/S40/S43/S50 and the player start instead of paying
    them after all of it.

    Returns the new filename, or None if there is nothing to do.
    """
    initd = os.path.join(root, "etc/init.d")
    src = os.path.join(initd, "S80_bt_init")
    dst = os.path.join(initd, "S22_bt_init")
    if os.path.exists(dst):
        return None                      # already hastened
    if not os.path.exists(src):
        return None
    os.rename(src, dst)
    return "S22_bt_init"


def install_boot_adb(root):
    """Add the boot-time ADB wrapper if this rootfs doesn't already have one."""
    dest = os.path.join(root, "etc/init.d/S90adb")
    if os.path.exists(dest):
        return False                     # mod-based image already carries it
    with open(S90ADB_SRC, "rb") as fh:
        data = fh.read()
    with open(dest, "wb") as fh:
        fh.write(data)
    os.chmod(dest, 0o755)
    return True


def patch_script(text):
    """Return the patched supervisor, or None if it is already patched."""
    if "DEV_HOOK" in text:
        return None
    for name, anchor in (("config", ANCHOR_CONFIG),
                         ("launch block", ANCHOR_LAUNCH),
                         ("crash counter", ANCHOR_COUNT)):
        if text.count(anchor) != 1:
            die(f"{SCRIPT}: expected exactly one {name} anchor, found "
                f"{text.count(anchor)}. This firmware's supervisor differs from "
                f"the 2.0.25/2.0.26 releases this was written against; patch it "
                f"by hand, using ANCHOR_CONFIG/ANCHOR_LAUNCH/ANCHOR_COUNT above "
                f"as the reference for what each anchor expects.")
    # STARTED has to be set before wait, so the counter anchor absorbs both.
    text = text.replace(ANCHOR_CONFIG, INSERT_CONFIG)
    text = text.replace(ANCHOR_LAUNCH, INSERT_LAUNCH)
    text = text.replace(ANCHOR_COUNT, INSERT_COUNT)
    return text


def detect_format(iso):
    """'mod' (the D0000001/... chunk layout every prior release used) or
    'stock' (the older OTA_V0/... layout vanilla 1.6 uses -- confirmed by
    extracting a real stock 1.6 image directly, not assumed)."""
    for path, name in (("/D0000001", "mod"), ("/OTA_V0", "stock")):
        try:
            next(iso.list_children(iso_path=path))
            return name
        except Exception:
            continue
    die("unrecognised .upt layout -- neither /D0000001 (mod) nor /OTA_V0 "
        "(stock) found. Is this really an R1 firmware image?")


def read_images_ota_v0(iso):
    """Stock/vanilla layout: OTA_V0/OTA_UPDA.IN manifest, OTA_V0/OTA_MD5_.<xxx>
    per-image ordered chunk-digest lists (xxx = the image's own md5, first 3
    hex chars, uppercase -- same convention read_images() already documents
    for the mod format's F0000002/3.BIN, just keyed by name here instead of
    position), ROOTFS_S.<xxx>/XIMAGE_0.<xxx> chunk files.

    xxx in a chunk's own filename is NOT its own digest -- confirmed
    empirically against a real stock 1.6 image: chunk 0's suffix is the whole
    image's own digest prefix, and chunk N>0's is chunk N-1's -- the same
    verification-chain idea write_upt() already implements for the other
    format, just discovered independently here rather than assumed to carry
    over. Chunks are matched to the ordered digest list by each chunk's own
    real md5, same as read_images() does; names are for the chain, not
    lookup.
    """
    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    def list_dir(path):
        return sorted(c.file_identifier().decode()
                     for c in iso.list_children(iso_path=path)
                     if c.file_identifier() not in (b'.', b'..'))

    manifest = rd("/OTA_V0/OTA_UPDA.IN;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    if not entries:
        die("could not parse the image manifest — is this an R1 .upt?")

    names = list_dir("/OTA_V0")
    chunk_files = [n for n in names if n.startswith("ROOTFS_S.") or n.startswith("XIMAGE_0.")]
    by_own_md5 = {}
    for n in chunk_files:
        data = rd(f"/OTA_V0/{n}")
        by_own_md5[hashlib.md5(data).hexdigest()] = data

    images = []
    for img_type, name, size, md5 in entries:
        prefix = md5[:3].upper()
        dfile = next((n for n in names if n.upper().startswith(f"OTA_MD5_.{prefix}")), None)
        if dfile is None:
            die(f"no OTA_MD5_ digest list found for {name} (prefix {prefix}) "
                f"— image is corrupt or the layout has changed")
        digest_list = rd(f"/OTA_V0/{dfile}").decode().split()
        try:
            data = b"".join(by_own_md5[d] for d in digest_list)
        except KeyError as e:
            die(f"{name}: a chunk digest in {dfile} matches no chunk file "
                f"on disk ({e}) — image is corrupt or the layout has changed")
        got = hashlib.md5(data).hexdigest()
        if got != md5 or len(data) != int(size):
            die(f"{name}: reassembled {len(data)} bytes md5={got}, "
                f"manifest says {size} bytes md5={md5}")
        images.append({"type": img_type, "name": name, "data": data})
    return manifest, entries, images


# 8.3 base name per image type -- fixed strings, not derived from img_name,
# confirmed against the real stock image (every rootfs chunk is ROOTFS_S.xxx
# regardless of index, every kernel chunk XIMAGE_0.xxx).
OTA_V0_BASE_NAME = {"rootfs": "ROOTFS_S", "kernel": "XIMAGE_0"}


def write_upt_ota_v0(out_path, images):
    """Rebuild a stock-format .upt. See read_images_ota_v0() for the naming
    scheme this reproduces. Same Joliet(3)/Rock Ridge(1.09) requirement as
    the mod format's write_upt() -- confirmed present on the stock image too,
    by direct inspection (iso.has_joliet()/has_rock_ridge()) -- and the same
    reasoning applies: this is not cosmetic, the device's updater will not
    navigate an image missing them. Real names below (ota_update.in,
    ota_md5_..., ota_config.in, ...) are likewise taken from the stock
    image's own Rock Ridge records, not guessed.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="CDROM",
            joliet=3, rock_ridge="1.09")
    iso.add_directory("/OTA_V0", joliet_path="/ota_v0", rr_name="ota_v0")

    used_iso_paths = set()

    def add(data, iso_path, real, joliet_dir="/ota_v0/"):
        # The chunk suffix is 3 hex chars of a content digest -- only 4096
        # slots, and a real rootfs runs ~70 chunks, so a same-directory
        # collision is a real birthday-bound risk, not a hypothetical: it hit
        # on the very first build of a rootfs whose content differs from the
        # vendor's own (pycdlib doesn't error on a duplicate 8.3 name, it
        # silently drops the earlier record, corrupting the image). ISO9660
        # version numbers exist precisely to let two entries share a short
        # name, so bump the version on a collision rather than invent a
        # naming scheme of our own -- the Rock Ridge real name, which is what
        # actually carries the chain semantics, is untouched either way.
        base, _, version = iso_path.rpartition(";")
        v = int(version)
        while iso_path in used_iso_paths:
            v += 1
            iso_path = f"{base};{v}"
        used_iso_paths.add(iso_path)
        iso.add_fp(io.BytesIO(data), len(data), iso_path,
                   joliet_path=joliet_dir + real if joliet_dir else "/" + real,
                   rr_name=real)

    def chunks(d):
        return [d[i:i + CHUNK] for i in range(0, len(d), CHUNK)]

    lines = ["ota_version=0", ""]
    for img in images:
        lines.append(f"img_type={img['type']}")
        lines.append(f"img_name={img['name']}")
        lines.append(f"img_size={len(img['data'])}")
        lines.append(f"img_md5={hashlib.md5(img['data']).hexdigest()}")
        lines.append("")
    new_manifest = "\n".join(lines).rstrip("\n") + "\n"
    add(new_manifest.encode(), "/OTA_V0/OTA_UPDA.IN;1", "ota_update.in")

    for img in images:
        base = OTA_V0_BASE_NAME.get(img["type"])
        if base is None:
            die(f"unknown image type {img['type']!r} — no stock chunk-name "
                f"convention known for it; this tool has only ever seen "
                f"'rootfs' and 'kernel'")
        whole = hashlib.md5(img["data"]).hexdigest()
        pieces = chunks(img["data"])
        per_chunk = [hashlib.md5(c).hexdigest() for c in pieces]

        digest_list_text = "".join(d + "\n" for d in per_chunk)
        add(digest_list_text.encode(), f"/OTA_V0/OTA_MD5_.{whole[:3].upper()};1",
            f"ota_md5_{img['name']}.{whole}")

        for k, c in enumerate(pieces):
            chain_digest = whole if k == 0 else per_chunk[k - 1]
            suffix = chain_digest[:3].upper()
            add(c, f"/OTA_V0/{base}.{suffix};1",
                f"{img['name']}.{k:04d}.{chain_digest}")

    add(b"\n", "/OTA_V0/OTA_V0.OK;1", "ota_v0.ok")
    add(b"current_version=0\n", "/OTA_CONF.IN;1", "ota_config.in", joliet_dir="/")
    iso.write(out_path)
    iso.close()


def read_images(iso):
    """Pull the manifest and reassemble each chunked image, verifying digests.

    Chunk order is *not* manifest order — 2.0.26 lists the kernel first but
    stores the rootfs first — so the layout is taken from the per-chunk digest
    lists, whose lengths give each image's chunk count, and each assembled
    image is then matched to its manifest entry by digest rather than position.
    """
    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    manifest = rd("/D0000001/F0000004.TXT;1").decode()
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    if not entries:
        die("could not parse the image manifest — is this an R1 .upt?")

    counts = [len(rd(f"/D0000001/F{i:07d}.BIN;1").decode().split())
              for i in (2, 3)]
    by_md5 = {md5: (t, name, int(size)) for t, name, size, md5 in entries}

    images, n = [], 6
    for count in counts:
        data = b"".join(rd(f"/D0000001/F{i:07d}.BIN;1") for i in range(n, n + count))
        got = hashlib.md5(data).hexdigest()
        if got not in by_md5:
            die(f"chunks at F{n:07d} (md5 {got}) match no manifest entry — "
                f"image is corrupt or the layout is one this tool does not know")
        img_type, name, size = by_md5[got]
        if len(data) != size:
            die(f"{name}: reassembled {len(data)} bytes, manifest says {size}")
        images.append({"type": img_type, "name": name, "data": data, "first": n})
        n += count
    return manifest, entries, images, n


def write_upt(out_path, images, manifest, meta, version_blob, version_num):
    """Rebuild the .upt, matching the original's directory extensions exactly.

    The stock images carry both Joliet (level 3) and Rock Ridge 1.09. Omitting
    them still produces a valid ISO 9660 image that reads back perfectly on a
    host — every digest verifies — but the device's updater cannot navigate it:
    it displays "Upgrading..." and never progresses. The extensions shift where
    the payload starts (first chunk at extent 51 rather than 33), so this is not
    cosmetic. Anything changed here must be checked against a stock image with
    verify_firmware.py --against, not merely by confirming checksums.
    """
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="CDROM",
            joliet=3, rock_ridge="1.09")
    iso.add_directory("/D0000001", joliet_path="/ota_v0", rr_name="ota_v0")

    def add(data, iso_path, real):
        """Every file carries an 8.3 ISO name plus its real name in Joliet/RR."""
        iso.add_fp(io.BytesIO(data), len(data), iso_path,
                   joliet_path="/ota_v0/" + real if "/D0000001/" in iso_path
                   else "/" + real,
                   rr_name=real)

    def chunks(d):
        return [d[i:i + CHUNK] for i in range(0, len(d), CHUNK)]

    per_chunk, whole = [], []
    for img in images:
        per_chunk.append([hashlib.md5(c).hexdigest() for c in chunks(img["data"])])
        whole.append(hashlib.md5(img["data"]).hexdigest())

    add("".join(h + "\n" for h in per_chunk[0]).encode(), "/D0000001/F0000002.BIN;1",
        f"ota_md5_{images[0]['name']}.{whole[0]}")
    add("".join(h + "\n" for h in per_chunk[1]).encode(), "/D0000001/F0000003.BIN;1",
        f"ota_md5_{images[1]['name']}.{whole[1]}")
    add(manifest.encode(), "/D0000001/F0000004.TXT;1", "ota_update.in")
    add(meta, "/D0000001/F0000005.TXT;1", "ota_v0.ok")

    n = 6
    for i, img in enumerate(images):
        for k, c in enumerate(chunks(img["data"])):
            # The chunk names form a verification chain: index 0 carries the
            # digest of the whole image, and every later index carries the
            # digest of the chunk before it. Getting this wrong is invisible to
            # a checksum test and leaves the updater stuck on "Upgrading...".
            digest = whole[i] if k == 0 else per_chunk[i][k - 1]
            add(c, f"/D0000001/F{n:07d}.BIN;1", f"{img['name']}.{k:04d}.{digest}")
            n += 1
    add(version_blob, f"/F{n:07d}.TXT;1", "ota_config.in")
    iso.write(out_path)
    iso.close()
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="the mod's .upt, e.g. r1-audiobooks-2.0.26.upt")
    ap.add_argument("output", help="where to write the patched .upt")
    ap.add_argument("--rom-version", default=DEFAULT_ROM_VERSION,
                    help=f"full build string recorded in "
                         f"etc/r1_audiobook_version (default "
                         f"{DEFAULT_ROM_VERSION})")
    ap.add_argument("--no-radio", action="store_true",
                    help="do not surface the Internet radio tile on the "
                         "Stream media screen")
    ap.add_argument("--rom-rev", default="a", metavar="LETTER",
                    help="one character appended to the version shown in "
                         "System -> About; the field is cut to 7 chars and "
                         "'2.0.26' already uses 6 (default: a)")
    ap.add_argument("--kernel", metavar="XIMAGE",
                    help="replace the kernel image (uImage/xImage, u-boot "
                         "legacy header + payload) with this file. Swapped "
                         "in before rootfs patching, so a custom kernel and "
                         "the usual rootfs patches can be applied in one "
                         "pass. The stock (OTA_V0) manifest always rebuilds "
                         "fresh from each image's live data, so no extra "
                         "size/md5 bookkeeping is needed there; the mod "
                         "format's manifest is a text template that has to "
                         "be patched in place, same as the rootfs entry "
                         "already is below.")
    ap.add_argument("--standalone", metavar="DEVICE_PATH", nargs="?",
                    const="/usr/data/library_standalone",
                    help="RP1: replace hiby_player.sh entirely with a loop "
                         "that launches DEVICE_PATH (an on-device path) "
                         "directly, no hiby_player and no DEV_HOOK/LD_PRELOAD "
                         "at all. Defaults to /usr/data/library_standalone "
                         "if given with no value. Only works against a bare "
                         "vanilla hiby_player.sh -- see "
                         "build_standalone_supervisor()'s own comment. Pair "
                         "with --embed-binary to also ship a matching binary "
                         "inside the image itself, rather than assuming "
                         "DEVICE_PATH is already populated some other way.")
    ap.add_argument("--embed-binary", metavar="LOCAL_PATH",
                    help="bake a local library_standalone build into the "
                         "rootfs itself (under usr/lib/libra/), so flashing "
                         "this .upt alone -- no separate adb push -- gets a "
                         "fresh or version-mismatched device to a working "
                         "install. Requires --standalone and --embed-version. "
                         "The DEVICE_PATH from --standalone is still what "
                         "actually runs; this only seeds it on first boot "
                         "after a version change, so an existing device "
                         "mid-session with a newer adb-pushed dev build is "
                         "never overwritten by flashing an older release.")
    ap.add_argument("--embed-version", metavar="VERSION",
                    help="version string for --embed-binary (e.g. '0.45', "
                         "matching LIBRARY_VERSION in music_hook.c) -- "
                         "stamped alongside the embedded binary and compared "
                         "against /usr/data's own marker at boot to decide "
                         "whether to copy it in.")
    ap.add_argument("--brcmfmac-switch", metavar="MODULES_DIR",
                    help="replace cywdhd with mainline brcmfmac (docs/10 step 3). "
                         "MODULES_DIR must hold brcmutil.ko and brcmfmac.ko; "
                         "firmware is taken from --brcm-firmware.")
    ap.add_argument("--brcm-firmware", metavar="DIR",
                    help="directory holding brcmfmac43430-sdio.bin/.txt")
    ap.add_argument("--bt-gpio-test", action="store_true",
                    help="EXPERIMENT: power the BT radio by driving PB04 from "
                         "userspace instead of waiting for cywdhd's rfkill0, "
                         "and defer cywdhd to the end of the module script. "
                         "See bt_gpio_power_test().")
    ap.add_argument("--background-touch", action="store_true",
                    help="background the touchscreen module load (~0.3s off "
                         "boot). OFF by default: it reorders input device "
                         "enumeration and music_hook.c hardcodes event1/event2, "
                         "so it currently breaks touch. See "
                         "background_touch_module().")
    ap.add_argument("--kernel-build-id", metavar="ID",
                    help="stamp usr/resource/kernel_build_id with this "
                         "string (e.g. '4.4.94_r1') -- read by the app's "
                         "About screen in preference to uname(), since a "
                         "custom kernel's real uname()/vermagic has to stay "
                         "exactly stock for the closed-source modules to "
                         "keep loading. Meaningless without --kernel, but "
                         "not required by it.")
    args = ap.parse_args()

    try:
        import pycdlib
    except ImportError:
        die("pycdlib not installed — pip install pycdlib")
    need("unsquashfs")
    need("mksquashfs")

    if not os.path.exists(args.input):
        die(f"{args.input} not found")
    if os.path.exists(args.output):
        die(f"{args.output} already exists — refusing to overwrite")
    if args.embed_binary:
        if not args.standalone:
            die("--embed-binary requires --standalone (it seeds that DEVICE_PATH)")
        if not args.embed_version:
            die("--embed-binary requires --embed-version")
        if not os.path.exists(args.embed_binary):
            die(f"{args.embed_binary} not found")
    elif args.embed_version:
        die("--embed-version has no effect without --embed-binary")

    iso = pycdlib.PyCdlib()
    iso.open(args.input)
    fmt = detect_format(iso)

    def rd(path):
        b = io.BytesIO()
        iso.get_file_from_iso_fp(b, iso_path=path)
        return b.getvalue()

    if fmt == "mod":
        manifest, entries, images, last = read_images(iso)
        meta = rd("/D0000001/F0000005.TXT;1")
        version_blob = rd(f"/F{last:07d}.TXT;1")
    else:
        manifest, entries, images = read_images_ota_v0(iso)
        last = None          # the mod format's own F{last}.TXT numbering doesn't apply
        meta = version_blob = None    # written directly by write_upt_ota_v0() instead
    iso.close()
    print(f"detected layout: {fmt}")

    print(f"{args.input}:")
    for img in images:
        print(f"  {img['type']:8s} {img['name']:18s} {len(img['data']):>9} bytes  verified")

    rootfs = next((i for i in images if i["type"] == "rootfs"), None)
    if rootfs is None:
        die("no rootfs image in this .upt")

    if args.kernel:
        kernel = next((i for i in images if i["type"] == "kernel"), None)
        if kernel is None:
            die("no kernel image in this .upt -- --kernel has nothing to replace")
        if not os.path.exists(args.kernel):
            die(f"{args.kernel} not found")
        with open(args.kernel, "rb") as fh:
            new_kernel = fh.read()
        if new_kernel[:4] != b"\x27\x05\x19\x56":   # u-boot legacy image magic
            die(f"{args.kernel} does not start with the u-boot legacy image "
                f"magic (0x27051956) -- this doesn't look like a real xImage/"
                f"uImage. Refusing to write something the device's bootloader "
                f"would reject.")
        print(f"replacing kernel: {len(kernel['data'])} -> {len(new_kernel)} bytes")
        kernel["data"] = new_kernel

    with tempfile.TemporaryDirectory(prefix="r1patch-") as tmp:
        sqfs = os.path.join(tmp, "rootfs.squashfs")
        root = os.path.join(tmp, "root")
        with open(sqfs, "wb") as fh:
            fh.write(rootfs["data"])

        print("\nunpacking rootfs...")
        subprocess.run(["unsquashfs", "-d", root, "-q", sqfs],
                       check=True, stdout=subprocess.DEVNULL)

        target = os.path.join(root, SCRIPT)
        if not os.path.exists(target):
            die(f"{SCRIPT} not found — this does not look like a real R1 "
                f"rootfs at all.")

        with open(target, "r") as fh:
            original = fh.read()

        already_standalone = False
        if args.standalone:
            # A full replacement, not an incremental patch -- DEV_HOOK/
            # MAX_CRASHES already being present says nothing about whether
            # this can proceed, so that check is skipped entirely here.
            patched = build_standalone_supervisor(
                original, args.standalone, seed_version=args.embed_version)
            kind = f"standalone ({args.standalone})"
            if patched is None:
                die(f"{SCRIPT} does not match the bare vanilla shape "
                    f"--standalone was written against (missing the batd "
                    f"preamble anchor) -- see build_standalone_supervisor()'s "
                    f"own comment.")
            if args.embed_binary:
                embed_dir_abs = os.path.join(root, EMBED_DIR)
                os.makedirs(embed_dir_abs, exist_ok=True)
                embed_target = os.path.join(embed_dir_abs, EMBED_BINARY_NAME)
                shutil.copy2(args.embed_binary, embed_target)
                os.chmod(embed_target, 0o755)
                print(f"embedded {args.embed_binary} -> "
                      f"/{EMBED_DIR}/{EMBED_BINARY_NAME} "
                      f"(version {args.embed_version}, seeds {args.standalone} "
                      f"on first boot after a version change)")
        elif 'BINARY="' in original and "CRASH_COUNT=0" in original:
            # Already one of our own --standalone images being re-patched (to
            # swap a kernel, or pick up a rootfs fix like the bt_init timing
            # work). The supervisor is exactly what we would write, so leave
            # it alone rather than trying to apply the incremental mod-based
            # patch to it -- it has MAX_CRASHES, so that path would otherwise
            # be taken and would die on the launch-block anchor it does not
            # have. Everything else in this pass still applies normally.
            patched = None
            already_standalone = True
            kind = "standalone (already present, left as-is)"
        else:
            if "DEV_HOOK" in original:
                die(f"{SCRIPT} already contains DEV_HOOK — this image is already patched")

            # Two supervisor shapes, routed on whether this is a mod-based
            # image (has the mod's own MAX_CRASHES supervisor to patch
            # incrementally) or a vanilla one (bare stock script, no anchor
            # to patch -- build_vanilla_supervisor() writes the whole
            # replacement instead). Neither function guesses silently: each
            # returns None if the input does not match what it was written
            # against, and that is treated as a hard stop rather than a
            # best-effort patch, for the same reason patch_script() always
            # has -- a half-applied patch to init would be a brick rather
            # than a bug.
            if "MAX_CRASHES" in original:
                patched = patch_script(original)
                kind = "mod-based"
            else:
                patched = build_vanilla_supervisor(original)
                kind = "vanilla"
            if patched is None:
                die(f"{SCRIPT} matches neither the mod's supervisor shape nor "
                    f"vanilla's bare one — this firmware's {SCRIPT} has changed "
                    f"from what this tool knows. Patch it by hand, using "
                    f"patch_script() (mod-based) or build_vanilla_supervisor() "
                    f"(vanilla) above as the reference.")
        if patched is None:
            print(f"note: {SCRIPT} left unchanged — {kind}")
        else:
            with open(target, "w") as fh:
                fh.write(patched)
            print(f"patched {SCRIPT} ({kind} base, "
                  f"{len(original.splitlines())} -> {len(patched.splitlines())} lines)")

        # These apply to any standalone image, including one of our own being
        # re-patched -- they are rootfs tweaks, not supervisor surgery.
        if args.standalone or already_standalone:
            if disable_hgl(root):
                print(f"disabled {HGL_SCRIPT} (no consumer in a standalone build)")
            else:
                print(f"note: {HGL_SCRIPT} not found — nothing to disable")
            nfw = install_brcmfmac_firmware(root)
            if nfw:
                print(f"installed {nfw} brcmfmac firmware file(s) under lib/firmware/brcm/")
            sn, sbytes = strip_unused_resources(root)
            if sn:
                print(f"stripped {sn} unused stock file(s) "
                      f"({sbytes / 1024 / 1024:.1f}MB) -- hiby_player, dmrd, "
                      f"stock-only fonts, unused litegui assets")
            if args.background_touch:
                if background_touch_module(root):
                    print("backgrounded the touchscreen module load (~0.3s) — "
                          "NOTE: breaks touch unless music_hook.c resolves input "
                          "nodes by name; see background_touch_module()")
                else:
                    print("note: touchscreen module line not found, left alone")

            sdsafe = protect_sd_during_storage(root)
            if sdsafe:
                print(f"patched {', '.join(sdsafe)} (release the SD card "
                      f"before exporting it to the host)")
            else:
                print("note: adboff/adbon not patched — already safe, missing, "
                      "or they differ from the vendor scripts this expects")

            usbfix = fix_usb_mode_switch(root)
            if usbfix:
                print(f"patched {ADB_INIT_SCRIPT} (ADB<->storage switch: "
                      f"{'; '.join(usbfix)})")
            else:
                print(f"note: {ADB_INIT_SCRIPT} not patched — already fixed, "
                      f"missing, or the anchors have moved")

            if bt_mac_before_rfkill_wait(root):
                print(f"patched {BT_INIT} (MAC lookup moved ahead of the "
                      f"rfkill0 wait: ~0.8s)")

            # NOT enabled by default: trace_bt_init() inserts a stamp between
            # "bt-agent ... &" and its "sleep 2", which breaks the anchor
            # patch_bt_init_timing() needs -- the sleep then survives, costing
            # 2s. Diagnostic use only.
            nt = trace_bt_init(root) if os.environ.get("R1_TRACE_BT") else None
            if args.bt_gpio_test and bt_gpio_power_test(root):
                print("EXPERIMENT: bt_init drives PB04 directly; cywdhd deferred "
                      "to the end of the module script")

            if nt:
                print(f"instrumented {BT_INIT} with {nt} boot-time stamps "
                      f"-> /usr/data/btboot.log")

            if args.brcmfmac_switch:
                err = switch_to_brcmfmac(root, args.brcmfmac_switch,
                                         args.brcm_firmware or args.brcmfmac_switch)
                if err:
                    die(f"--brcmfmac-switch: {err}")
                print("patched module_driver (cywdhd -> mainline brcmfmac; "
                      "soc_msc wifi_reg_on=PB03)")

            if enable_rtc32k_at_boot(root):
                print("patched module_driver/soc_utils.sh (rtc32k_init_on=1: "
                      "enable the 32kHz LPO at module load)")

            if bt_keep_powered_when_enabled(root):
                print(f"patched {BT_INIT} (keep the radio powered when "
                      f"bt_enabled=1: ~2.1s of off-then-on removed)")

            early = start_patchram_earlier(root)
            if early:
                print("reordered init so patchram starts before the module "
                      "script: " + "; ".join(early))

            # After the reorder: this rewrites "sh foo.sh" to ". ./foo.sh",
            # which would otherwise break start_patchram_earlier()'s anchors.
            # DISABLED: sourcing broke boot. The 28 scripts then share one
            # shell, and while none contains "exit" or "$0" (checked), that was
            # the wrong risk list -- a "cd" or "set -e" in any of them changes
            # the environment every later ". ./foo.sh" depends on, and
            # soc_msc.sh alone is 57 lines. Worth ~110ms (0.120s -> 0.010s for
            # 28 spawns, measured); not worth an unbootable device.
            if os.environ.get("R1_SOURCE_MODULES"):
                ns = source_module_scripts(root)
                if ns:
                    print(f"module init: {ns} scripts sourced instead of "
                          f"spawned (~{ns * 4.3:.0f}ms) -- EXPERIMENTAL, "
                          f"has broken boot before")


            hb = hasten_bt_init(root)
            if hb:
                print(f"moved etc/init.d/S80_bt_init -> {hb} "
                      f"(BT bring-up overlaps the rest of boot)")
            else:
                print("note: bt_init not moved — already at S22, or S80_bt_init "
                      "missing")

            if defer_wifi_module(root):
                print(f"backgrounded the WiFi module load in {MODULE_INIT_SCRIPT} (kept in place, so Bluetooth is not delayed)")
            else:
                print(f"note: could not defer the WiFi module in {MODULE_INIT_SCRIPT}")

        mtarget = os.path.join(root, MOUNT_SCRIPT)
        if os.path.exists(mtarget):
            with open(mtarget, "r") as fh:
                mtext = fh.read()
            mpatched = patch_mount_script(mtext)
            if mpatched is None:
                print(f"note: {MOUNT_SCRIPT} not patched — no '-o sync' mount "
                      f"found, leaving it alone")
            else:
                with open(mtarget, "w") as fh:
                    fh.write(mpatched)
                print(f"patched {MOUNT_SCRIPT} (/usr/data: sync -> noatime)")

        btarget = os.path.join(root, BT_INIT)
        if os.path.exists(btarget):
            with open(btarget, "r") as fh:
                btext = fh.read()
            bpatched = patch_bt_init(btext)
            if bpatched is None:
                print(f"note: {BT_INIT} not patched — HFP already on, or the "
                      f"bluealsa line has moved")
            else:
                btext = bpatched
                print(f"patched {BT_INIT} (bluealsa: +hfp-ag for battery reporting)")

            # Independent of the HFP tweak: take the ~12s of fixed sleeps out
            # of this script. Measured 12.4s -> 5.79s on hardware.
            btiming = patch_bt_init_timing(btext)
            if btiming is None:
                print(f"note: {BT_INIT} sleeps not replaced — already polling, "
                      f"or none of the sleep anchors matched")
            else:
                btext, applied = btiming
                print(f"patched {BT_INIT} (sleep -> poll: {', '.join(applied)})")
                bpatched = btext

            # Maintain bt_lastused.txt and page that headset once the
            # adapter is powered (BG119).
            recon = bt_reconnect_last_device(btext)
            if recon is None:
                print(f"note: {BT_INIT} reconnect block not added — already "
                      f"present, or the bt_init_ok line has moved")
            else:
                btext = recon
                bpatched = btext
                print(f"patched {BT_INIT} (reconnect to the last-used headset, "
                      f"6 tries over ~30s, backgrounded)")

            ssp = bt_enable_ssp(btext)
            if ssp is None:
                print(f"note: {BT_INIT} SSP write not added — already present, "
                      f"or the bt-agent line has moved")
            else:
                btext = ssp
                bpatched = btext
                print(f"patched {BT_INIT} (Write Simple Pairing Mode = on, "
                      f"after the HCI reset)")

            retried = bt_patchram_retry(btext)
            if retried is None:
                print(f"note: {BT_INIT} patchram watchdog not added — already "
                      f"present, or the hci0 wait has moved")
            else:
                btext = retried
                bpatched = btext
                print(f"patched {BT_INIT} (patchram watchdog: retry with an "
                      f"rfkill power-cycle if hci0 does not appear in 8s)")

            # Must run after the watchdog: that patch rewrites the hci0 wait
            # this one anchors just below.
            traced = trace_bt_steps(btext)
            if traced is None:
                print(f"note: {BT_INIT} step timing not added — already "
                      f"present, or a bring-up anchor did not match")
            else:
                btext = traced
                bpatched = btext
                print(f"patched {BT_INIT} (step timing -> /usr/data/btsteps.log)")

            reordered = reorder_bt_init_radio_first(btext)
            if reordered is None:
                print(f"note: {BT_INIT} radio/D-Bus order unchanged — already "
                      f"reordered, or the anchors did not match")
            else:
                btext = reordered
                bpatched = btext
                print(f"patched {BT_INIT} (patchram now starts ~0.23s earlier, "
                      f"before the D-Bus preamble)")

            if bpatched is not None:
                with open(btarget, "w") as fh:
                    fh.write(btext)

        about = enable_about_tile(root)
        if about:
            print(f"enabled Settings -> About in {len(about)} file(s)")
        else:
            print("note: About already enabled, or set_functions.json missing")

        if install_boot_adb(root):
            print("installed etc/init.d/S90adb (boot ADB — stock rcS never "
                  "runs T90adb, so a vanilla base boots with no ADB otherwise)")
        else:
            print("note: boot ADB already present (mod base carries its own)")

        # Version stamp, so the build is identifiable from the device itself.
        if not args.no_radio:
            radio = enable_internet_radio(root)
            if radio:
                print(f"enabled Internet radio in {len(radio)} theme layout(s)")
            else:
                print("note: Internet radio already enabled, or layouts missing")

        ctarget = os.path.join(root, CONFIG_JSON)
        if os.path.exists(ctarget):
            with open(ctarget) as fh:
                ctext = fh.read()
            stamped = stamp_config_json(ctext, args.rom_rev[:1])
            if stamped is None:
                print(f"note: {CONFIG_JSON} version not stamped (already "
                      f"stamped, or no room in {ABOUT_VERSION_MAX} chars)")
            else:
                with open(ctarget, "w") as fh:
                    fh.write(stamped)
                shown = re.search(r'"version"\s*:\s*"([^"]+)"', stamped).group(1)
                print(f"stamped About version -> '{shown}'")

        vtarget = os.path.join(root, VERSION_FILE)
        if os.path.exists(vtarget):
            with open(vtarget) as fh:
                vtext = fh.read()
            vnew = stamp_version_file(vtext, args.rom_version)
            if vnew is not None:
                with open(vtarget, "w") as fh:
                    fh.write(vnew)
                print(f"stamped {VERSION_FILE} with podcast_rom={args.rom_version}")

        if args.kernel_build_id:
            kbtarget = os.path.join(root, "usr/resource/kernel_build_id")
            with open(kbtarget, "w") as fh:
                fh.write(args.kernel_build_id + "\n")
            print(f"stamped usr/resource/kernel_build_id -> "
                  f"'{args.kernel_build_id}'")

        print("repacking rootfs...")
        newsq = os.path.join(tmp, "rootfs.new.squashfs")
        subprocess.run(["mksquashfs", root, newsq, "-comp", "lzo", "-b", "131072",
                        "-no-exports", "-all-root", "-noappend", "-no-progress"],
                       check=True, stdout=subprocess.DEVNULL)
        with open(newsq, "rb") as fh:
            rootfs["data"] = fh.read()

    print("building .upt...")
    if fmt == "mod":
        # The rootfs almost always changes size, so the manifest text has to
        # follow it -- write_upt() takes the manifest as text and patches it
        # in place, matching how it was read. A replaced kernel (--kernel)
        # needs exactly the same treatment, for the same reason.
        changed = {"rootfs": rootfs}
        if args.kernel:
            changed["kernel"] = kernel
        for img_type, name, size, md5 in entries:
            img = changed.get(img_type)
            if img is not None:
                manifest = manifest.replace(f"img_size={size}",
                                            f"img_size={len(img['data'])}")
                manifest = manifest.replace(f"img_md5={md5}",
                                            f"img_md5={hashlib.md5(img['data']).hexdigest()}")
        last = write_upt(args.output, images, manifest, meta, version_blob, last)
        print(f"\nwrote {args.output} ({os.path.getsize(args.output)} bytes, "
              f"last entry F{last:07d}.TXT)")
    else:
        # write_upt_ota_v0() rebuilds OTA_UPDA.IN fresh from each image's own
        # (possibly just-changed) data, so there is no old manifest text to
        # patch in place here -- unlike the mod format, size/md5 are never
        # stale to begin with.
        write_upt_ota_v0(args.output, images)
        print(f"\nwrote {args.output} ({os.path.getsize(args.output)} bytes)")
    print("\nVerify it before flashing:  ./tools/verify_firmware.py " + args.output)


if __name__ == "__main__":
    main()
