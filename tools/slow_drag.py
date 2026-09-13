#!/usr/bin/env python3
"""Inject a touch drag on the R1 over ADB, spread over real wall-clock time.

swipe.py sends an entire gesture -- press, every intermediate move, release
-- as one atomic write with no delay between events. That works fine for
gestures this app resolves from the release position alone (a tap, or a
scrub bar that jumps to wherever you lifted), but any gesture built on the
polling-loop `touch_down` idiom (radio's own waveform scrubber, the volume
slider, dragging a long title) needs the press to actually persist across
more than one ~33ms tick before the drag code ever sees it move -- which an
atomic burst of events, arriving before the app polls even once, does not
do. A real finger drag is naturally spread over real time and works fine;
this script exists to make a scripted one behave the same way.

Usage:  slow_drag.py X1 Y1 X2 Y2 [steps]
"""
import base64
import os
import struct
import subprocess
import sys
import time

ADB = os.environ.get(
    "ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb"
)
STEP_DELAY_S = 0.08


def ev(etype: int, code: int, value: int) -> bytes:
    return struct.pack("<llHHl", 0, 0, etype, code, value)


def frame(x: int, y: int, press) -> list[bytes]:
    out = [
        ev(3, 57, 0),    # ABS_MT_TRACKING_ID
        ev(3, 58, 63),   # ABS_MT_PRESSURE
        ev(3, 48, 9),    # ABS_MT_TOUCH_MAJOR
        ev(3, 53, x),    # ABS_MT_POSITION_X
        ev(3, 54, y),    # ABS_MT_POSITION_Y
        ev(0, 2, 0),     # SYN_MT_REPORT
    ]
    if press is True:
        out.append(ev(1, 330, 1))   # BTN_TOUCH down
    elif press is False:
        out.append(ev(1, 330, 0))   # BTN_TOUCH up
    out.append(ev(0, 0, 0))         # SYN_REPORT
    return out


def send(events: list[bytes]) -> None:
    blob = base64.b64encode(b"".join(events)).decode()
    cmd = f"echo '{blob}' | base64 -d > /tmp/.slow_drag.bin && cat /tmp/.slow_drag.bin > /dev/input/event1"
    subprocess.run([ADB, "shell", cmd], capture_output=True, text=True)


def main() -> None:
    x1, y1, x2, y2 = (int(a) for a in sys.argv[1:5])
    steps = int(sys.argv[5]) if len(sys.argv) > 5 else 12

    send(frame(x1, y1, press=True))
    time.sleep(STEP_DELAY_S)
    for i in range(1, steps + 1):
        x = x1 + (x2 - x1) * i // steps
        y = y1 + (y2 - y1) * i // steps
        send(frame(x, y, press=None))
        time.sleep(STEP_DELAY_S)
    send(frame(x2, y2, press=False))
    print(f"slow drag ({x1},{y1}) -> ({x2},{y2}) over {steps} steps")


if __name__ == "__main__":
    main()
