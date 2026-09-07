#!/usr/bin/env python3
"""Minimal ISO9660 reader for the R1 .upt, then reassemble + unpack the rootfs.

Only what's needed: walk the directory tree, pull files out by path. The .upt
is a plain ISO9660 image whose OTA_V0 directory holds a manifest plus the
rootfs/kernel split into 512 KiB chunks (see patch_firmware.py's
read_images_ota_v0 for the format, which this mirrors).
"""
import hashlib
import re
import struct
import sys

SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        assert pvd[1:6] == b"CD001", "not an ISO9660 image"
        self.root = self._parse_record(pvd[156:156 + 34], 0)

    def _read_sectors(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    @staticmethod
    def _parse_record(rec, _):
        if len(rec) < 33 or rec[0] == 0:
            return None
        lba = struct.unpack("<I", rec[2:6])[0]
        size = struct.unpack("<I", rec[10:14])[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen].decode("latin-1")
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, entry):
        data = self._read_sectors(entry["lba"], entry["size"])
        out, off = [], 0
        while off < len(data):
            rlen = data[off]
            if rlen == 0:
                off = (off // SECTOR + 1) * SECTOR
                continue
            r = self._parse_record(data[off:off + rlen], 0)
            if r and r["name"] not in ("\x00", "\x01"):
                out.append(r)
            off += rlen
        return out

    def find(self, path):
        cur = self.root
        for part in [p for p in path.strip("/").split("/") if p]:
            kids = self.listdir(cur)
            # Match the version suffix exactly when the caller supplied one.
            # Two chunks can share an 8.3 base name and be distinguished only
            # by ;1 / ;2; matching on the base alone always returned the first
            # record, so the second chunk was unreachable.
            if ";" in part:
                match = next((k for k in kids
                              if k["name"].upper() == part.upper()), None)
            else:
                match = next((k for k in kids
                              if k["name"].split(";")[0].upper() == part.upper()), None)
            if match is None:
                raise FileNotFoundError(f"{path} (at {part})")
            cur = match
        return cur

    def read(self, path):
        e = self.find(path)
        return self._read_sectors(e["lba"], e["size"])


def main():
    iso = Iso(sys.argv[1] if len(sys.argv) > 1 else "r1.upt")
    root_kids = iso.listdir(iso.root)
    print("top level:", [k["name"] for k in root_kids])

    ota = iso.find("/OTA_V0")
    # Keep the ISO version suffix (;1, ;2, ...). patch_firmware bumps it when
    # two chunks collide on an 8.3 name -- only 3 hex chars of suffix, ~70
    # chunks, so collisions are routine rather than rare. Stripping the version
    # collapsed both records onto one name, so the second chunk was never read
    # and its digest went missing from the index.
    names = [k["name"] for k in iso.listdir(ota)]
    print(f"OTA_V0 holds {len(names)} entries")

    manifest = iso.read("/OTA_V0/OTA_UPDA.IN").decode("latin-1")
    entries = re.findall(r"img_type=(\S+)\s+img_name=(\S+)\s+"
                         r"img_size=(\d+)\s+img_md5=([0-9a-f]+)", manifest)
    print("manifest:", [(t, n, s) for t, n, s, _ in entries])

    # Index every chunk by its own md5 -- the digest lists reference chunks by
    # content, not by filename (the names encode a verification chain instead).
    chunks = {}
    for n in names:
        if n.split(";")[0].startswith(("ROOTFS_S.", "XIMAGE_0.")):
            d = iso.read(f"/OTA_V0/{n}")
            chunks[hashlib.md5(d).hexdigest()] = d
    print(f"indexed {len(chunks)} chunks")

    for img_type, name, size, md5 in entries:
        if img_type != "rootfs":
            continue
        prefix = md5[:3].upper()
        dfile = next(n for n in names if n.upper().startswith(f"OTA_MD5_.{prefix}"))
        digests = iso.read(f"/OTA_V0/{dfile}").decode().split()
        data = b"".join(chunks[d] for d in digests)
        got = hashlib.md5(data).hexdigest()
        ok = (got == md5 and len(data) == int(size))
        print(f"{name}: {len(data)} bytes md5={got} {'OK' if ok else 'MISMATCH'}")
        assert ok, "rootfs failed verification"
        open("rootfs.squashfs", "wb").write(data)
        print("wrote rootfs.squashfs")


if __name__ == "__main__":
    main()
