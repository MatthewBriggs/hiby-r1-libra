#!/usr/bin/env bash
# Runs inside the container. /src = compas-player checkout (read-only),
# /stock = stock R1 rootfs.squashfs, /build = case-sensitive volume.
set -euo pipefail
cd /build
if [[ ! -d cp/.git ]]; then git clone -q /src cp; fi
git -C cp fetch -q /src HEAD && git -C cp checkout -q FETCH_HEAD
R=/build/cp
S=$R/scratch
B=$S/base-upgrade
mkdir -p "$B"
pin() { # dir url commit
    [[ -d $1/.git ]] || git clone -q "$2" "$1"
    git -C "$1" checkout -q "$3"
}
pin $S/ingenic-toolchain-v5.2 https://github.com/tobunto/ingenic-toolchain-v5.2 4de21963b0f10c19118045f78c3e853a5e2c0be6
pin $S/bluez-alsa-5.0.0 https://github.com/arkq/bluez-alsa 1935d6dcb8975f2d7a51aaafe61538d157224623
pin $B/fdk-aac https://github.com/mstorsjo/fdk-aac 716f4394641d53f0d79c9ddac3fa93b03a49f278
pin $B/libopenaptx https://github.com/pali/libopenaptx 2459ed4686eaef0a19dfa3f330a960813c5f60de
pin $B/ldacBT https://github.com/EHfive/ldacBT 6579bd585a618f2e1612b3c1650d2b7fcfb1d43f
git -C $B/ldacBT submodule -q update --init
pin $B/libldacdec-anonymix007 https://github.com/anonymix007/libldacdec c90094b15e25aef0e47c6d775fa94aceb36cabbc
pin $R/dbus https://gitlab.freedesktop.org/dbus/dbus.git 958bf9db2100553bcd2fe2a854e1ebb42e886054
git -C $R/dbus fetch -q --tags
# Rosetta runs this x86-64 toolchain with an occasional random segfault
# (about 1 compile in 30, measured). Wrap every tool in a retry on a crash;
# ordinary errors pass straight through. Stdin is buffered so a retried
# `gcc -E -` still sees its input.
REAL=$S/ingenic-toolchain-v5.2/toolchain/bin
W=/build/wrap
mkdir -p $W
for t in $REAL/mips-linux-gnu-*; do
    n=$(basename $t)
    cat > $W/$n <<WRAP
#!/bin/bash
in=""
if [ ! -t 0 ]; then in=\$(mktemp); cat > "\$in"; fi
err=\$(mktemp)
for i in 1 2 3 4 5 6 7 8; do
    if [ -n "\$in" ]; then "$t" "\$@" < "\$in" 2> "\$err"; else "$t" "\$@" 2> "\$err"; fi
    rc=\$?
    if [ \$rc -ge 128 ] || grep -q "internal compiler error\\|Segmentation fault\\|terminated with signal" "\$err"; then
        echo "[wrap] $n crashed (rc \$rc), retry \$i" >> /build/wrap-retries.log
        continue
    fi
    break
done
cat "\$err" >&2
rm -f "\$err" \${in:+"\$in"}
exit \$rc
WRAP
    chmod +x $W/$n
done
# qemu-user under Rosetta sometimes cannot place its guest address space
# ("Unable to find a guest_base"); a retry lands it. Same retry idea.
cat > $W/qemu-mipsel <<'WRAP'
#!/bin/bash
err=$(mktemp)
for i in 1 2 3 4 5 6 7 8; do
    /usr/bin/qemu-mipsel "$@" 2> "$err"; rc=$?
    grep -q "Unable to find a guest_base" "$err" || break
    echo "[wrap] qemu-mipsel guest_base retry $i" >> /build/wrap-retries.log
done
cat "$err" >&2; rm -f "$err"; exit $rc
WRAP
chmod +x $W/qemu-mipsel
export PATH=$W:$PATH
export BASE_CROSS_PREFIX=$W/mips-linux-gnu-
export MIPS_GCC=$W/mips-linux-gnu-gcc
export BASE_STOCK_ROOT=/build/stock-root
if [[ ! -d $BASE_STOCK_ROOT/usr/lib ]]; then
    rm -rf "$BASE_STOCK_ROOT"
    unsquashfs -q -no-xattrs -d "$BASE_STOCK_ROOT" /stock/rootfs.squashfs
fi
export STOCK_ZLIB=$(ls $BASE_STOCK_ROOT/usr/lib/libz.so.1.* | head -1)
export BASE_EXPAT_STOCK=$(ls $BASE_STOCK_ROOT/usr/lib/libexpat.so.1.* | head -1)
export JOBS=${JOBS:-6}
cd $R
steps=${*:-"audio_deps zlib expat bluez_library glib dbus bt_codecs bluealsa bluez"}
for s in $steps; do
    echo "=================== $s"
    case $s in
        overlay)
            # Their script also packs their player; we only want the base
            # overlay, so feed it a placeholder and drop it afterwards.
            echo placeholder > compas_player_target
            bash scripts/prepare_base_image_overlay.sh
            rm -f compas_player_target "$(readlink -f $B/overlay)/usr/bin/compas_player"
            ov=$(readlink -f $B/overlay); sed -i '\|/usr/bin/compas_player$|d' "$(dirname $ov)/files.txt" ;;
        test)
            rm -rf /build/candidate && cp -a "$BASE_STOCK_ROOT" /build/candidate
            ov=$(readlink -f $B/overlay)
            echo "overlay: $ov"
            cp -a "$ov/." /build/candidate/
            cp -a $B/bluez/stage/usr/libexec/bluetooth/bluetoothd /build/candidate/usr/libexec/bluetooth/
            for b in bluetoothctl hciconfig hcitool btmon; do cp -a $B/bluez/stage/usr/bin/$b /build/candidate/usr/bin/; done
            bash scripts/test_base_image.sh /build/candidate ;;
        *) bash scripts/build_base_$s.sh ;;
    esac
done
