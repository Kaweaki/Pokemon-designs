#!/usr/bin/env python3
"""Check the board without a camera: sweep every entry, then measure the frame.

    python3 tools/verify.py [/dev/ttyACM0] [--shots DIR]

Three things happen, in this order:

1. `SCAN` — the firmware draws all 649 entries once and reports the average and
   worst frame cost, plus free heap. A crash, an out-of-bounds read or a blank
   entry shows up here as a panic or a short count.
2. `PERF` — the frame budget, broken into push / sky / biome / creature / HUD.
   This is where a claim about fluidity has to come from; do not infer it.
3. `--shots DIR` — a frame grab per requested entry, decoded to PNG. Pass
   entries as `--shots shots 1 94 649` or leave it off.

Exit status is non-zero if the board does not answer or if the sweep comes back
short, so this can gate a flashing script.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shot import grab  # noqa: E402
from PIL import Image


def read_until(port: serial.Serial, token: str, deadline: float = 300.0) -> str:
    end = time.time() + deadline
    buf = b""
    while time.time() < end:
        ch = port.read(1)
        if not ch:
            continue
        if ch == b"\n":
            line = buf.decode("utf-8", "replace").strip()
            buf = b""
            if token in line:
                return line
            if line.startswith("scan "):
                print(line)
        else:
            buf += ch
    raise TimeoutError(f"board never said {token!r}")


def main() -> int:
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    args = sys.argv[2:]
    out_dir: Path | None = None
    entries: list[int] = []
    if args and args[0] == "--shots":
        out_dir = Path(args[1])
        out_dir.mkdir(parents=True, exist_ok=True)
        entries = [int(a) for a in args[2:]]

    # No DTR/RTS toggling: this board is native USB CDC and pulling DTR resets it,
    # which closes the port underneath the write.
    port = serial.Serial(dev, 115200, timeout=0.3)
    time.sleep(1.0)
    port.reset_input_buffer()

    port.write(b"SCAN\n")
    scan = read_until(port, "scan ok")
    print(scan)

    # One whole cycle, so a transition happens inside the window PERF averages:
    # the melt is 820 ms of every 6 s and is invisible in a frame average.
    time.sleep(7.0)
    port.reset_input_buffer()
    port.write(b"PERF\n")
    print(read_until(port, "headroom"))
    print(read_until(port, "hud"))
    print(read_until(port, "melt", deadline=5.0))

    for n in entries:
        port.write(f"GOTO {n}\n".encode())
        time.sleep(2.4)                     # past the landing and the reaction
        img = grab(port)
        path = out_dir / f"{n:03d}.png"
        # NEAREST: the default filter is bilinear, which turns a 320-pixel panel
        # into a blurry approximation of itself and hides exactly the one-pixel
        # problems a shot exists to find.
        img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(path)
        print(f"shot {path}")

    port.close()

    if "649/649" not in scan:
        print("FAIL: the sweep did not cover every entry", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
