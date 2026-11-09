#!/usr/bin/env python3
"""Ask the board for the frame it is showing and save it as a PNG.

    python3 tools/shot.py /dev/ttyACM0 out.png [entry]

Sends GOTO (optional), waits for the frame to settle, then SHOT. The payload is
RLE (length, RGB565) words, decoded here. This is how the layout gets checked
without a camera pointed at the panel.
"""

from __future__ import annotations

import struct
import sys
import time

import serial
from PIL import Image


def read_line(port: serial.Serial, deadline: float) -> str:
    line = b""
    while time.time() < deadline:
        ch = port.read(1)
        if not ch:
            continue
        if ch == b"\n":
            return line.decode("utf-8", "replace").strip()
        line += ch
    raise TimeoutError("no line from board")


def grab(port: serial.Serial) -> Image.Image:
    port.reset_input_buffer()
    port.write(b"SHOT\n")
    header = ""
    deadline = time.time() + 5
    while time.time() < deadline:
        header = read_line(port, deadline)
        if header.startswith("SHOT "):
            break
    else:
        raise RuntimeError("board did not answer SHOT")
    _, runs, w, h = header.split()
    runs, w, h = int(runs), int(w), int(h)

    payload = b""
    port.timeout = 5
    while len(payload) < runs * 4:
        chunk = port.read(runs * 4 - len(payload))
        if not chunk:
            raise RuntimeError(f"short read: {len(payload)}/{runs * 4} bytes")
        payload += chunk

    img = Image.new("RGB", (w, h))
    px = img.load()
    x = y = 0
    for i in range(runs):
        length, colour = struct.unpack_from("<HH", payload, i * 4)
        r = (colour >> 11) & 0x1F
        g = (colour >> 5) & 0x3F
        b = colour & 0x1F
        rgb = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
        for _ in range(length):
            px[x, y] = rgb
            x += 1
            if x >= w:
                x = 0
                y += 1
    return img


def main() -> None:
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    out = sys.argv[2] if len(sys.argv) > 2 else "shot.png"
    entry = int(sys.argv[3]) if len(sys.argv) > 3 else None

    # No DTR/RTS toggling: this board enumerates as native USB CDC, and pulling
    # DTR is a reset request. The port closes and re-enumerates underneath you,
    # which is what "write failed: [Errno 5]" was. Opening the port is enough;
    # the sketch is already running.
    port = serial.Serial(dev, 115200, timeout=0.2)
    time.sleep(0.4)
    port.reset_input_buffer()
    if entry:
        port.write(f"GOTO {entry}\n".encode())
        time.sleep(2.6)          # past the landing and the reaction
    img = grab(port)
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(out)
    port.close()
    print(f"{out}: {img.width}x{img.height}")


if __name__ == "__main__":
    main()
