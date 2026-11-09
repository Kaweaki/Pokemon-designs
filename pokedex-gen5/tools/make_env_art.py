#!/usr/bin/env python3
"""Turn the battle art into headers the sketch can blit.

Two kinds of art, two encodings, both baked at build time so the sketch only
moves pixels.

1. **Battle platforms** — the ground patch each creature stands on, lifted from
   `art/Enviroments/Battleplatforms.webp`. The sheet is the 800x700 reference
   chart of Gen 5 platform art: one row per terrain, a front-facing ellipse and a
   flattened back-sprite patch. The flattened patch is the one used here, because
   a creature standing on the ground line needs ground, not a disc seen edge-on.

   These are stored as plain RGB565 with a per-row span, not as indexed art.
   They are photographic-ish gradients (a rock patch has fifty browns) and 15
   palette slots would band them; and because a platform is an ellipse, every
   row of it is one contiguous run, so the blit is one `memcpy` per row and the
   gradient costs nothing at run time.

2. **Elemental frames** — flame, bubble and bolt, authored here and baked. Four
   frames each at 1:1, indexed 4bpp with 15 colours, because these are small,
   flat, and drawn every frame in a handful of runs.

Usage:  python3 tools/make_env_art.py [--preview out.png]
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

from PIL import Image

SHEET = Path("art/Enviroments/Battleplatforms.webp")
HDR = Path("pokedex/env_art.h")
ASM = Path("pokedex/env_art.S")

# Bounding boxes of the flattened "backsprite" platform patches on the sheet,
# found by flood-filling the alpha channel rather than by eye. Every one is 192
# px wide; the heights differ by up to five pixels because the art does.
PATCHES = [
    ("cave_pool", 204, 85, 395, 101),      # dark water
    ("rocks_a", 204, 144, 395, 165),       # grey stone
    ("light_path", 204, 213, 395, 229),    # pale sand
    ("green_grass", 204, 274, 395, 293),
    ("rocks_b", 204, 336, 395, 357),       # warm rock
    ("brown_path", 204, 405, 395, 421),    # packed dirt
    ("river", 204, 469, 395, 485),         # light water
    ("snow", 204, 530, 395, 549),
    ("snowy_grass", 204, 594, 395, 613),
    ("ice", 204, 661, 395, 677),
    ("forest", 604, 82, 795, 101),         # dark undergrowth
    ("tall_grass", 604, 146, 795, 165),
    ("wood", 604, 213, 795, 229),          # planks
    ("man_made", 604, 341, 795, 357),      # paving
]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# --------------------------------------------------------------- platforms ---


def extract(name: str, box: tuple[int, int, int, int], sheet: Image.Image):
    """Crop one patch to its opaque pixels and give each row its span.

    The sheet has a real alpha channel with hard edges (0 or 255), so there is no
    white to key and no halo to clean up. Each row of an ellipse is contiguous,
    but a row can still have a detached speck from the art (a grass blade), so the
    span is the first-to-last opaque pixel and the interior is copied whole.
    """
    x0, y0, x1, y1 = box
    crop = sheet.crop((x0, y0, x1 + 1, y1 + 1))
    w, h = crop.size
    px = crop.load()

    rows: list[list[int]] = []
    spans: list[tuple[int, int]] = []
    for y in range(h):
        lit = [x for x in range(w) if px[x, y][3] > 0]
        if not lit:
            rows.append([])
            spans.append((0, 0))
            continue
        a, b = min(lit), max(lit)
        rows.append([rgb565(*px[x, y][:3]) for x in range(a, b + 1)])
        spans.append((a, b + 1))

    top = min(y for y, (a, b) in enumerate(spans) if b > a)
    bot = max(y for y, (a, b) in enumerate(spans) if b > a)
    rows = rows[top:bot + 1]
    spans = spans[top:bot + 1]

    # Left-trim: every row is stored starting at its span, so the row data is
    # ragged. Storing the full width instead keeps the index arithmetic to
    # `r * w + x` and costs (192 - span) words per row, which is a few hundred
    # bytes of flash for a lot less run-time work.
    full: list[int] = []
    for y, (a, b) in enumerate(spans):
        band = [0] * a + rows[y] + [0] * (w - b)
        full.extend(band)
    flat_spans = [v for pair in spans for v in pair]
    return name, w, len(rows), full, flat_spans


# ------------------------------------------------------------------- frames ---
#
# Hand-tuned, not sampled: there is no effect sheet in the folder (the links in
# Enviroments/ point at a site that needs a browser), and a flame drawn as three
# concentric circles is a bubble, not fire. These are shapes, not formulas, which
# is why they are here and not in the sketch.

FLAME_RAMP = [
    (255, 246, 214),   # core
    (255, 226, 130),
    (255, 190, 66),
    (247, 140, 40),
    (214, 84, 26),
    (150, 44, 22),     # the smoke-dark edge
]


def flame(w: int, h: int, frame: int) -> list[list[int]]:
    """A fire, four frames of it.

    The silhouette is a set of tongues with different periods, so the top edge
    never repeats; the fill is a ramp from a white core out to a dark red edge.
    `frame` shifts the phases by a quarter turn, which is what makes a loop.
    """
    grid = [[0] * w for _ in range(h)]
    cx = (w - 1) / 2.0
    ph = frame * math.pi / 2.0
    for x in range(w):
        d = abs(x - cx) / (cx + 0.5)
        if d > 1.0:
            continue
        # Three tongues at different pitches: one wave reads as a candle, three
        # read as a fire that is breathing.
        tongue = (math.sin(ph + x * 0.75) * 0.16
                  + math.sin(ph * 1.7 + x * 1.35 + 1.1) * 0.11
                  + math.sin(ph * 0.6 + x * 2.4) * 0.05)
        height = (h - 3) * (1.0 - d * d) * (1.0 + tongue)
        height = max(2.0, min(float(h), height + (1.0 if abs(x - cx) < 1.5 else 0.0)))
        top = h - int(round(height))
        for y in range(top, h):
            # Vertical position in the flame is the temperature: the base is
            # hottest, the tip is the colour of the edge.
            t = (y - top) / max(1.0, float(h - top))
            core = 1.0 - d * 1.5 - t * 0.35
            idx = 0 if core > 0.62 else 1 if core > 0.40 else 2 if core > 0.16 \
                else 3 if core > -0.10 else 4 if core > -0.40 else 5
            grid[y][x] = idx + 1
    return grid


def bubble(w: int, h: int, frame: int) -> list[list[int]]:
    """A soap bubble: a thin ring, a bright rim, and one specular dot."""
    grid = [[0] * w for _ in range(h)]
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    r = min(cx, cy) - (1.0 if frame in (1, 3) else 0.0)
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, (y - cy) * 1.08)
            if d > r + 0.6:
                continue
            if d > r - 0.9:                      # the ring itself
                # Top-left of the ring catches the light, bottom-right is water.
                lit = (x - cx) + (y - cy) < 0
                grid[y][x] = 1 if lit else 2
            elif (x - cx) + 0.9 * (y - cy) < -r * 0.45:
                grid[y][x] = 3                   # pale interior wash
            if math.hypot(x - (cx - r * 0.42), y - (cy - r * 0.44)) < 1.1:
                grid[y][x] = 4                   # specular
        # a little trail under it: what stops it reading as a ring on the panel
    if frame % 2:
        grid[h - 1][int(cx)] = 2
    return grid


def bolt(w: int, h: int, frame: int) -> list[list[int]]:
    """A zigzag, rasterised with a glow behind the core.

    The four frames have to differ or the animation is a still: the span of the
    zigzag grows and the whole bolt leans a little further each frame, so it
    crackles rather than blinks.
    """
    grid = [[0] * w for _ in range(h)]
    cx = (w - 1) / 2.0
    amp = 3.2 + (frame % 4) * 0.75
    lean = (frame - 1.5) * 0.7
    pts = []
    y = 0
    step = 0
    while y <= h - 1:
        swing = (cx + (amp if step % 2 else -amp)) + lean if step else cx
        pts.append((int(round(swing)), y))
        y += 4
        step += 1
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for k in range(n + 1):
            x = int(round(x0 + (x1 - x0) * k / n))
            yy = int(round(y0 + (y1 - y0) * k / n))
            for dx, dy, v in ((0, 0, 1), (1, 0, 2), (-1, 0, 2), (0, 1, 2), (2, 0, 3), (-2, 0, 3)):
                nx, ny = x + dx, yy + dy
                if 0 <= nx < w and 0 <= ny < h and grid[ny][nx] < v:
                    grid[ny][nx] = v
    return grid


FX = [
    # name,        w,  h,  frames, builder, colours
    ("flame", 13, 17, 4, flame,
     [(255, 246, 214), (255, 226, 130), (255, 190, 66), (247, 140, 40),
      (214, 84, 26), (150, 44, 22)]),
    ("bubble", 11, 11, 4, bubble,
     [(206, 242, 252), (150, 214, 238), (96, 160, 210), (176, 226, 244),
      (255, 255, 255)]),
    ("bolt", 10, 17, 4, bolt,
     [(255, 255, 255), (214, 236, 255), (140, 186, 255), (74, 116, 210)]),
]


def pack_frames(grids: list[list[list[int]]]):
    """4bpp, two pixels a byte, frames stacked vertically."""
    w = len(grids[0][0])
    h = len(grids[0])
    data: list[int] = []
    for g in grids:
        for row in g:
            for x in range(0, w, 2):
                hi = row[x]
                lo = row[x + 1] if x + 1 < w else 0
                data.append((hi << 4) | lo)
    return w, h, data


def main() -> None:
    sheet = Image.open(SHEET).convert("RGBA")

    plat_data: list[int] = []
    plat_spans: list[int] = []
    plat_entries = []
    for name, x0, y0, x1, y1 in PATCHES:
        name, w, h, pixels, spans = extract(name, (x0, y0, x1, y1), sheet)
        plat_entries.append((name, w, h, len(plat_data), len(plat_spans)))
        plat_data.extend(pixels)
        plat_spans.extend(spans)

    fx_data: list[int] = []
    fx_entries = []
    for name, w, h, frames, builder, colours in FX:
        grids = [builder(w, h, f) for f in range(frames)]
        gw, gh, data = pack_frames(grids)
        pal = [rgb565(*c) for c in colours]
        pal += [0] * (15 - len(pal))
        fx_entries.append((name, gw, gh, frames, len(fx_data), pal))
        fx_data.extend(data)

    asm = [
        f"# Generated by tools/make_env_art.py — {len(plat_data)} platform words,",
        f"# {len(fx_data)} effect bytes. Do not edit by hand.",
        ".section .rodata",
        ".balign 4",
        ".global PLAT_DATA",
        ".type PLAT_DATA, @object",
        "PLAT_DATA:",
    ]
    for i in range(0, len(plat_data), 12):
        asm.append(".short " + ",".join(str(v) for v in plat_data[i:i + 12]))
    asm += [".global PLAT_DATA_END", "PLAT_DATA_END:", ".balign 4",
            ".global FX_DATA", ".type FX_DATA, @object", "FX_DATA:"]
    for i in range(0, len(fx_data), 32):
        asm.append(".byte " + ",".join(str(b) for b in fx_data[i:i + 32]))
    asm += [".global FX_DATA_END", "FX_DATA_END:", ""]
    ASM.write_text("\n".join(asm), encoding="utf-8")

    lines = [
        "// Generated by tools/make_env_art.py — do not edit by hand.",
        "//",
        "// Battle platforms: RGB565, one row per entry in PLAT_SPANS holding the",
        "// first and last opaque column. An ellipse has one run per row, so the",
        "// blit is a memcpy per row and the gradients stay gradients.",
        "//",
        "// Elemental frames: 4 bpp, two pixels per byte, nibble 1..15 palette,",
        "// 0 transparent. Frames are stacked vertically; row stride is",
        "// (w + 1) / 2 bytes.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct PlatInfo {",
        "  uint32_t off;    // index into PLAT_DATA, first word of the top row",
        "  uint32_t span;   // index into PLAT_SPANS, two bytes per row",
        "  uint8_t w, h;",
        "};",
        "",
        "struct FxInfo {",
        "  uint32_t off;    // index into FX_DATA, first byte of the top row of frame 0",
        "  uint16_t pal[15];",
        "  uint8_t w, h, frames;",
        "};",
        "",
        "extern \"C\" const uint16_t PLAT_DATA[];",
        "extern \"C\" const uint8_t FX_DATA[];",
        "",
        f"#define PLAT_COUNT {len(plat_entries)}",
        f"#define FX_COUNT {len(fx_entries)}",
        "",
        "static const uint8_t PLAT_SPANS[] = {",
    ]
    for i in range(0, len(plat_spans), 24):
        lines.append("  " + ",".join(str(v) for v in plat_spans[i:i + 24]) + ",")
    lines += ["};", "",
              "// Indexed by the Plat enum in the sketch, in this order:", "",
              "//   " + ", ".join(name for name, *_ in plat_entries), "",
              "static const PlatInfo PLATFORMS[PLAT_COUNT] = {"]
    for name, w, h, off, span in plat_entries:
        lines.append(f"  {{{off},{span},{w},{h}}},   // {name}")
    lines += ["};", "",
              "static const FxInfo FXART[FX_COUNT] = {"]
    for name, w, h, frames, off, pal in fx_entries:
        p = ",".join(f"0x{c:04x}" for c in pal)
        lines.append(f"  {{{off},{{{p}}},{w},{h},{frames}}},   // {name}")
    lines += ["};", ""]
    HDR.write_text("\n".join(lines), encoding="utf-8")

    print(f"{HDR} + {ASM.name}: {len(plat_entries)} platforms "
          f"({len(plat_data) * 2 / 1024:.0f} KB), {len(fx_entries)} effects "
          f"({len(fx_data)} B)")
    for name, w, h, off, span in plat_entries:
        print(f"  {name:<12} {w}x{h}")

    if "--preview" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--preview") + 1])
        preview(plat_entries, plat_data, plat_spans, fx_entries, fx_data, out)


def preview(plat_entries, plat_data, plat_spans, fx_entries, fx_data, out: Path) -> None:
    """Draw the generated art on a dark background so it can be looked at."""
    pad = 4
    wp = max(w for _, w, _, _, _ in plat_entries)
    hp = sum(h + pad for _, _, h, _, _ in plat_entries) + pad
    img = Image.new("RGB", (wp + 8, hp), (28, 26, 34))
    px = img.load()
    y = pad
    for name, w, h, off, spanidx in plat_entries:
        for r in range(h):
            a, b = plat_spans[spanidx + r * 2], plat_spans[spanidx + r * 2 + 1]
            for x in range(a, b):
                c = plat_data[off + r * w + x]
                px[x + 4, y + r] = ((c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3)
        y += h + pad

    fw = sum((e[1] + 2) * e[3] + 12 for e in fx_entries) + 12
    fh = max(e[2] for e in fx_entries) + 12
    fimg = Image.new("RGB", (fw, fh), (16, 14, 20))
    fpx = fimg.load()
    x0 = 6
    for name, w, h, frames, off, pal in fx_entries:
        stride = (w + 1) // 2
        for f in range(frames):
            for r in range(h):
                for x in range(w):
                    byte = fx_data[off + (f * h + r) * stride + (x >> 1)]
                    v = (byte & 0x0F) if (x & 1) else (byte >> 4)
                    if v:
                        c = pal[v - 1]
                        fpx[x0 + x, 6 + r] = ((c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3)
            x0 += w + 2
        x0 += 8
    combo = Image.new("RGB", (max(img.width, fimg.width), img.height + fimg.height + 6),
                      (12, 12, 16))
    combo.paste(img, (0, 0))
    combo.paste(fimg, (0, img.height + 6))
    combo = combo.resize((combo.width * 3, combo.height * 3), Image.NEAREST)
    combo.save(out)
    print(f"preview {out}")


if __name__ == "__main__":
    main()
