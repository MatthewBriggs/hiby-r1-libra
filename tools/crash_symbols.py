#!/usr/bin/env python3
"""Turn a Libra crash report from music.log into function names.

Libra's crash handler (music-app/app/crash.c) logs raw addresses: pc, ra, the
code addresses found on the stack, and the executable mappings. This resolves
the ones inside library_standalone against the unstripped build of the same
source (library_standalone.debug, which build_standalone.sh keeps beside the
shipped binary), and shows the rest as library+offset.

Usage:  crash_symbols.py REPORT.txt [library_standalone.debug]
REPORT.txt is the "[crash]" lines, e.g. `grep "\\[crash\\]" music.log`.
Uses the system `nm`, which reads MIPS ELF on macOS.
"""
import bisect
import os
import re
import subprocess
import sys


def symbols(binary):
    out = subprocess.run(["nm", "-n", binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    report = open(sys.argv[1]).read()
    binary = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..", "music-app", "app", "library_standalone.debug")
    addrs, names = symbols(binary)

    maps = []   # (lo, hi, path)
    for m in re.finditer(r"\[crash\] map ([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s*(\S*)", report):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), m.group(4) or "?"))
    main_lo = min((lo for lo, hi, p in maps if p.endswith("library_standalone")), default=None)

    def where(a):
        for lo, hi, path in maps:
            if lo <= a < hi and not path.endswith("library_standalone"):
                return "%s+0x%x" % (os.path.basename(path), a - lo)
        if main_lo is None or a >= main_lo:
            i = bisect.bisect_right(addrs, a) - 1
            if i >= 0:
                return "%s+0x%x" % (names[i], a - addrs[i])
        return "?"

    for line in report.splitlines():
        if "[crash] pc " in line or "[crash] stack" in line:
            print(line.split("[crash] ", 1)[1].split()[0])
            found = re.findall(r"0x[0-9a-f]+", line)
            if "[crash] pc " in line:
                found = found[:2]   # pc, ra; sp is a stack address, not code
            for a in found:
                a = int(a, 16)
                if a:
                    print("  0x%08x  %s" % (a, where(a)))
        elif "[crash]" in line and " map " not in line:
            print(line.split("[crash] ", 1)[1])


if __name__ == "__main__":
    main()
