#!/usr/bin/env python3
"""Turn the Gen 5 sprite folder into a header the sketch can blit.

Input:  0001.png .. 0649.png, 96x96, palette PNG, real alpha, 6..15 opaque
        colours. Output: pokedex/sprites.h, two pixels per byte.

Why 4 bpp and not a byte per pixel
----------------------------------
At one byte per pixel the whole national dex is 2.0 MB, which is 78% of the 3 MB
app slot: it compiles, but there is no room left and the blit reads twice the
flash it needs to. Two pixels per byte is 1.0 MB and roughly halves the byte
traffic in the inner loop, which is where fluidity actually comes from.

The nibble budget is 16 states: 0 transparent, 1..14 colours, 15 outline. The
archive's sprites use at most 15 colours, so at most one colour has to go. It is
never picked by eye — the least-used colour is merged into its nearest neighbour
in RGB, so what is lost is a handful of antialiasing pixels, not a shade that
carries the creature's shape.

Three decisions that matter, all learned the hard way
-----------------------------------------------------
1. **Alpha is the mask.** These sprites ship a real alpha channel, so unlike the
   Red/Green set there is no "is white background or highlight" question.
2. **Bake the outline** (nibble 15). One pixel around the silhouette, in the
   device's outline colour. Doing it at run time costs a second fillRect per run;
   baking also means the blit never has to look at a neighbour.
3. **Crop to the drawn pixels.** The canvas is 96x96 for every sprite, but the
   art is not: cropping is what makes them stand on the ground line and stops a
   legendary being scaled like a Caterpie.

Only numbered front sprites (NNNN.png) are converted. The archive also ships
0201-a.png style forms, shiny/ and back/ and female/, and letting those in would
silently take an entry's slot.

Usage:  python3 tools/make_sprites.py art/gen5 pokedex/sprites.h [count]
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

OUTLINE = 15
COLOURS_MAX = 14          # nibbles 1..14; 15 is the outline, 0 is transparent


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def distance2(a: tuple[int, int, int], b: tuple[int, int, int]) -> int:
    """Weighted RGB distance.

    Not plain Euclidean: the eye resolves green far better than blue, so a merge
    that only looks right in RGB space can be obvious on the panel. These are the
    usual weights (2, 4, 3).
    """
    dr, dg, db = a[0] - b[0], a[1] - b[1], a[2] - b[2]
    return 2 * dr * dr + 4 * dg * dg + 3 * db * db


def reduce_palette(colours: list[tuple[tuple[int, int, int], int]]):
    """Merge the least-used colour into its nearest neighbour until it fits.

    `colours` is (rgb, pixel count). Returns the surviving colours and a map from
    every original colour to its survivor, so the caller can repaint the pixels.
    """
    live = list(colours)
    merge: dict[tuple[int, int, int], tuple[int, int, int]] = {}
    while len(live) > COLOURS_MAX:
        live.sort(key=lambda c: c[1])                 # least used first
        victim, count = live[0]
        rest = live[1:]
        # Nearest is computed against what is left, not against the original set:
        # otherwise a chain of merges can walk a colour far from where it began.
        target = min(rest, key=lambda c: distance2(victim, c[0]))[0]
        live = [(target if rgb == target else rgb, n + (count if rgb == target else 0))
                for rgb, n in rest]
        merge[victim] = target
        # Anything already pointing at the victim has to follow it.
        for rgb, tgt in list(merge.items()):
            if tgt == victim:
                merge[rgb] = target
    return [rgb for rgb, _ in live], merge


def load(path: Path) -> tuple[int, int, list[int], list[list[int]]]:
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    px = img.load()

    counts: dict[tuple[int, int, int], int] = {}
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a:
                counts[(r, g, b)] = counts.get((r, g, b), 0) + 1

    palette, merge = reduce_palette(list(counts.items()))
    index = {rgb: i + 1 for i, rgb in enumerate(palette)}

    def value(rgb: tuple[int, int, int]) -> int:
        return index[merge.get(rgb, rgb)]

    rows = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a:
                rows[y][x] = value((r, g, b))

    # Outline: grow the canvas by one pixel and ring every transparent neighbour
    # of a drawn pixel. 8-connected, so diagonal steps are covered too.
    ew, eh = w + 2, h + 2
    grown = [[0] * ew for _ in range(eh)]
    for y in range(h):
        for x in range(w):
            grown[y + 1][x + 1] = rows[y][x]
    ring = [row[:] for row in grown]
    for y in range(eh):
        for x in range(ew):
            if grown[y][x]:
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < eh and 0 <= nx < ew and grown[ny][nx]:
                        ring[y][x] = OUTLINE
                        break
                if ring[y][x] == OUTLINE:
                    break

    xs = [x for y in range(eh) for x in range(ew) if ring[y][x]]
    ys = [y for y in range(eh) for x in range(ew) if ring[y][x]]
    if not xs:
        raise SystemExit(f"{path.name}: nothing left after masking")
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    cropped = [ring[y][x0:x1 + 1] for y in range(y0, y1 + 1)]

    # Two pixels per byte, high nibble first. An odd width leaves the padding
    # nibble 0 (transparent), which the blit skips anyway.
    packed: list[int] = []
    for row in cropped:
        for x in range(0, len(row), 2):
            hi = row[x]
            lo = row[x + 1] if x + 1 < len(row) else 0
            packed.append((hi << 4) | lo)

    pal565 = [rgb565(*c) for c in palette]
    while len(pal565) < 15:
        pal565.append(0)
    return len(cropped[0]), len(cropped), pal565, packed


def main() -> None:
    src = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("pokedex/sprites.h")
    asm_out = out.with_suffix(".S")
    count = int(sys.argv[3]) if len(sys.argv) > 3 else None
    if count is None:
        found = [p for p in src.iterdir() if p.stem.isdigit() and p.suffix == ".png"]
        count = max(int(p.stem) for p in found) if found else 0
        if count == 0:
            raise SystemExit(f"no NNNN.png files in {src}")

    data: list[int] = []
    entries: list[tuple[int, int, int, list[int]]] = []
    for n in range(1, count + 1):
        path = src / f"{n:04d}.png"
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        w, h, pal, rows = load(path)
        entries.append((w, h, len(data), pal))
        data.extend(rows)

    # The pixel payload goes into an assembler file, not a C array.
    #
    # 1 MB written as `0x1f,` is 3.3 MB of tokens for the C++ front end, and GCC
    # spent over twenty minutes on it (quadratic-ish in the initialiser). The
    # assembler eats .byte lists in under a second, and the symbol is the same
    # array. This is the difference between a build you can iterate on and one
    # you start and walk away from.
    asm = [
        f"# Generated by tools/make_sprites.py — {len(entries)} sprites, "
        f"{len(data)} bytes. Do not edit by hand.",
        ".section .rodata",
        ".balign 4",
        ".global SPR_DATA",
        ".type SPR_DATA, @object",
        "SPR_DATA:",
    ]
    for i in range(0, len(data), 32):
        asm.append(".byte " + ",".join(str(b) for b in data[i:i + 32]))
    asm.append(".global SPR_DATA_END")
    asm.append("SPR_DATA_END:")
    asm.append("")
    asm_out.write_text("\n".join(asm), encoding="utf-8")

    lines = [
        "// Generated by tools/make_sprites.py — do not edit by hand.",
        "//",
        "// Two pixels per byte (4 bpp): 0 transparent, 1..14 this sprite's",
        "// palette, 15 the outline ring baked around the silhouette. Sprites are",
        "// cropped to their drawn pixels, so w/h below is the artwork, not the",
        "// 96x96 canvas. Row stride is (w + 1) / 2 bytes, high nibble first.",
        "//",
        "// The pixels themselves live in sprites.S (assembled, not compiled: see",
        "// the note in make_sprites.py) and are linked in as this array.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define SPR_OUTLINE {OUTLINE}",
        "",
        "struct SprInfo {",
        "  uint32_t off;        // byte offset into SPR_DATA",
        "  uint16_t pal[15];    // RGB565, index 1..14",
        "  uint8_t w, h;",
        "};",
        "",
        f"#define SPR_COUNT {len(entries)}",
        "",
        "extern \"C\" const uint8_t SPR_DATA[];",
        "",
        "static const SprInfo SPRITES[SPR_COUNT] = {",
    ]
    for w, h, off, pal in entries:
        p = ",".join(f"0x{c:04x}" for c in pal)
        lines.append(f"  {{{off},{{{p}}},{w},{h}}},")
    lines.append("};")
    lines.append("")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"{out} + {asm_out.name}: {len(entries)} sprites, "
          f"{len(data)} bytes ({len(data) / 1024:.0f} KB)")
    hs = [h for _, h, _, _ in entries]
    ws = [w for w, _, _, _ in entries]
    print(f"cropped {min(ws)}..{max(ws)} wide, {min(hs)}..{max(hs)} tall")


if __name__ == "__main__":
    main()
