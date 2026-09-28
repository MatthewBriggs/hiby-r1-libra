# Bluetooth base-image upgrade (BlueALSA 5, BlueZ 5.87)

Since 0.59, Libra's firmware images replace HiBy's BlueALSA 4.1.1 and
bluetoothd 5.54 with **BlueALSA 5.0.0** and **BlueZ 5.87**, plus the libraries
they need (D-Bus 1.16.2, GLib 2.84.4, ALSA 1.2.16 with the speexdsp rate
converter, SBC 2.2, fdk-aac 2.0.3, libopenaptx, zlib 1.3.2, Expat 2.8.4).

## Where it comes from

The upgrade is **compas-player's**
(<https://github.com/Starnished66/compas-player>, GPLv3), built with their own
scripts and patches from `scripts/` and `scripts/base_image/`, **unmodified**,
at compas-player commit `a7b2b0014438f5cca0241dce13e7b5cd7d36af22`. Their
BlueALSA and BlueZ patches are part of what is built. Every upstream source is
fetched at the commit or checksum those scripts pin. `build.sh` lists the Git
checkouts; the release tarballs are pinned by SHA-256 inside compas-player's
own scripts.

Deliberately left out of what Libra installs:

- **libldacdec** (compas-player's rebuilt LDAC decoder). Upstream
  (anonymix007/libldacdec) publishes no licence. It is used only when the R1
  receives LDAC from a phone as a Bluetooth DAC, which Libra does not do, so
  the stock decoder stays.
- Their `bt_init` / `bt_resume` / `bluealsa_profile`. `patch_firmware.py`
  applies the same rename to Libra's own patched scripts instead.

The LDAC *encoder* and ABR libraries are HiBy's own, from the stock firmware,
as compas-player also does.

Licence notices for everything installed ship in the image under
`/usr/share/licenses/base-upgrade/`.

## Building it

```sh
docker build --platform linux/amd64 -t r1-bt-stack .
docker volume create r1-bt-build
docker run --platform linux/amd64 --rm \
  -v /path/to/compas-player:/src:ro -v $PWD/stock:/stock:ro \
  -v $PWD/build.sh:/build.sh:ro -v r1-bt-build:/build \
  r1-bt-stack bash /build.sh                 # all nine components
# then: bash /build.sh overlay test          # overlay + qemu smoke test
```

- `stock/rootfs.squashfs` is the stock R1 v1.6 root filesystem
  (`tools/upt_extract.py` on HiBy's `r1.upt`), which the scripts compare every
  library's ABI against.
- It builds inside a Docker volume, not a macOS folder: the Ingenic toolchain
  has headers whose names differ only in case.
- On Apple Silicon, Rosetta crashes the x86-64 Ingenic gcc 5.2 at random
  (about 1 compile in 30), and sometimes qemu-mipsel's address-space setup.
  `build.sh` wraps both in retries.
- Copy the overlay and `bluetoothd`/tools out of the volume into `out/overlay`
  and `out/bluez`, then pass that `out/` to
  `tools/patch_firmware.py --bluealsa5`.
