#!/usr/bin/env python3
"""Turn Elite Battle DX art into the two kinds of data the panel can use.

Source: art/Battle&animations/EBDX.zip (the archive ships a 384x308 painted
battleback per environment, ground plates and scenery elements, and the move
effect sheets used by its 54 move scripts).

Two outputs, both baked so the sketch only moves pixels:

1. **Battle scenes** — ten 320x138 RGB565 images, one per biome. Each is a
   painted battleback cropped around its horizon, with EBDX scenery layered in
   and a ground plate from the archive feathering the stand line. The pixels are
   written already byte-swapped for the ST7789 frame buffer, so drawing one is a
   row memcpy rather than a per-pixel conversion.

2. **Move textures** — the effect sheets trimmed to one cell, scaled down and
   packed as 4bpp indexed art (0 transparent, 1..15 palette). The palette carries
   a few brightness steps, which is what lets the animation fade an effect by
   changing color rather than compositing alpha.

Usage: python3 tools/make_battle_art.py [--preview out.png]
"""

from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ZIP = ROOT / "art" / "Battle&animations" / "EBDX.zip"
BG_HDR = ROOT / "pokedex" / "bg_art.h"
BG_ASM = ROOT / "pokedex" / "bg_art.S"
ATK_HDR = ROOT / "pokedex" / "atk_art.h"
ATK_ASM = ROOT / "pokedex" / "atk_art.S"

SCENE_W = 320
SCENE_H = 138          # the panel above the HUD
GROUND_ROW = 124       # where the creature's feet are


# --------------------------------------------------------------------- tools ---

def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def swap16(c: int) -> int:
    return ((c >> 8) | (c << 8)) & 0xFFFF


class Art:
    def __init__(self, path: Path):
        self.z = zipfile.ZipFile(path)

    def image(self, name: str) -> Image.Image:
        data = self.z.read(name)
        return Image.open(io.BytesIO(data)).convert("RGBA")

    def bg(self, name: str, dx: int = 32, dy: int = 0) -> Image.Image:
        im = self.image(f"Graphics/EBDX/Battlebacks/battlebg/{name}.png")
        return im.crop((dx, dy, dx + SCENE_W, dy + SCENE_H))

    def element(self, name: str) -> Image.Image:
        return self.image(f"Graphics/EBDX/Battlebacks/elements/{name}.png")

    def move(self, name: str) -> Image.Image:
        return self.image(f"Graphics/EBDX/Animations/Moves/{name}.png")


def tint(im: Image.Image, colour: tuple[int, int, int]) -> Image.Image:
    """Multiply RGB, leave alpha alone. Grades the painted battlebacks."""
    if colour is None:
        return im
    r, g, b = colour
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            pr, pg, pb, pa = px[x, y]
            px[x, y] = (pr * r // 255, pg * g // 255, pb * b // 255, pa)
    return im


def paint(im: Image.Image, colour: tuple[int, int, int], strength: float = 0.45) -> Image.Image:
    """Blend toward a colour instead of multiplying.

    The EBDX trees and clouds are near-black silhouettes, so a multiply can
ever lift them into the scene's palette; a blend can.
    """
    if colour is None:
        return im
    r, g, b = colour
    px = im.load()
    inv = 1.0 - strength
    for y in range(im.height):
        for x in range(im.width):
            pr, pg, pb, pa = px[x, y]
            px[x, y] = (int(pr * inv + r * strength), int(pg * inv + g * strength),
                        int(pb * inv + b * strength), pa)
    return im


def place(img: Image.Image, elem: Image.Image, x: int, y: int, scale: float = 1.0,
          colour=None, flip: bool = False, alpha: float = 1.0) -> None:
    x, y = int(x), int(y)
    if scale != 1.0:
        elem = elem.resize((max(1, int(elem.width * scale)),
                            max(1, int(elem.height * scale))), Image.LANCZOS)
    if flip:
        elem = elem.transpose(Image.FLIP_LEFT_RIGHT)
    if colour:
        paint(elem, colour)
    if alpha < 1.0:
        a = elem.getchannel("A").point(lambda v: int(v * alpha))
        elem.putalpha(a)
    img.alpha_composite(elem, (x, y))


def feather_top(im: Image.Image, rows: int = 14) -> Image.Image:
    a = im.getchannel("A").copy()
    px = a.load()
    for y in range(min(rows, im.height)):
        f = y / rows
        for x in range(im.width):
            px[x, y] = int(px[x, y] * f)
    out = im.copy()
    out.putalpha(a)
    return out


def stars(img: Image.Image, count: int, seed: int, ymax: int = 100) -> None:
    import random
    random.seed(seed)
    d = ImageDraw.Draw(img)
    for _ in range(count):
        x = random.randrange(SCENE_W)
        y = random.randrange(ymax)
        b = random.randrange(170, 256)
        s = 1 if random.random() < 0.75 else 2
        d.rectangle([x, y, x + s, y + s], fill=(b, b, 255, 230))


# -------------------------------------------------------------------- scenes ---

def scene_forest(a: Art) -> Image.Image:
    im = a.bg("Forest")
    place(im, a.element("treePine"), -6, 22, 1.05, (54, 92, 70))
    place(im, a.element("treePine"), 92, 14, 0.82, (40, 74, 60))
    place(im, a.element("treePine"), 184, 20, 1.10, (64, 108, 80))
    place(im, a.element("treePine"), 276, 16, 0.92, (46, 86, 66), flip=True)
    for x, s in ((18, 0.9), (112, 0.75), (216, 0.85), (292, 0.7)):
        place(im, a.element("tallGrass"), x, GROUND_ROW - 18 * s, s, (66, 118, 74))
    return im


def scene_cave(a: Art) -> Image.Image:
    im = a.bg("CaveDark")
    place(im, a.element("pillars"), 4, -14, 0.84, (86, 74, 116))
    place(im, a.element("fog"), -30, GROUND_ROW - 34, 0.62, (66, 58, 92), alpha=0.45)
    place(im, a.element("rocks"), 38, GROUND_ROW - 12, 1.1, (86, 74, 110))
    place(im, a.element("rocks"), 268, GROUND_ROW - 10, 0.8, (74, 64, 96))
    return im


def scene_volcano(a: Art) -> Image.Image:
    im = a.bg("Cave")
    tint(im, (255, 118, 92))
    # A hot band where the floor meets the smoke.
    glow = Image.new("RGBA", im.size, (0, 0, 0, 0))
    px = glow.load()
    for y in range(SCENE_H):
        f = max(0.0, (y - 74) / 64.0)
        c = (int(150 * f), int(38 * f), 0, int(110 * f))
        for x in range(SCENE_W):
            px[x, y] = c
    im.alpha_composite(glow)
    place(im, a.element("pillars002"), -18, -20, 0.86, (112, 42, 36))
    place(im, a.element("darkFog"), -40, 82, 0.62, (190, 88, 64), alpha=0.55)
    place(im, a.element("rocks"), 30, GROUND_ROW - 12, 1.0, (104, 50, 42))
    place(im, a.element("rocks"), 272, GROUND_ROW - 10, 0.8, (88, 42, 36))
    return im


def scene_ocean(a: Art) -> Image.Image:
    im = a.bg("Water")
    place(im, a.element("cloud1"), -40, 4, 1.0, (196, 218, 240))
    place(im, a.element("cloud2"), 156, 15, 1.0, (178, 204, 230))
    place(im, a.element("cloud1"), 250, 26, 0.8, (206, 226, 244), alpha=0.8)
    return im


def fluffy_cloud(im: Image.Image, cx: float, cy: float, w: float, h: float,
                 colour=(244, 250, 255), alpha=210) -> None:
    """A soft cloud bank for the sky scene (the archive only has black clouds)."""
    from PIL import ImageFilter
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    puffs = 9
    for i in range(puffs):
        f = i / (puffs - 1)
        px = cx - w / 2 + w * f
        py = cy - h * 0.45 * (0.35 + 0.65 * (1 - abs(f - 0.5) * 2))
        r = h * (0.35 + 0.35 * (1 - abs(f - 0.5) * 2))
        d.ellipse([px - r, py - r, px + r, py + r], fill=colour + (alpha,))
    layer = layer.filter(ImageFilter.GaussianBlur(4))
    im.alpha_composite(layer)


def scene_sky(a: Art) -> Image.Image:
    im = Image.new("RGBA", (SCENE_W, SCENE_H), (188, 226, 246, 255))
    sky = a.element("skyDay").resize((SCENE_W, SCENE_H), Image.LANCZOS)
    im.alpha_composite(sky, (0, 0))
    place(im, a.element("sun"), 26, 6, 1.0)
    place(im, a.element("cloud1"), -34, 34, 1.0, (250, 253, 255))
    place(im, a.element("cloud2"), 138, 50, 1.0, (236, 246, 255))
    place(im, a.element("cloud1"), 246, 22, 0.7, (246, 251, 255), alpha=0.85)
    fluffy_cloud(im, 80, 112, 220, 32)
    fluffy_cloud(im, 250, 124, 190, 30, (228, 242, 255), 235)
    fluffy_cloud(im, 160, 134, 260, 26, (250, 253, 255), 245)
    return im


def scene_city(a: Art) -> Image.Image:
    im = a.bg("City")
    tint(im, (188, 196, 226))
    skyline = a.element("decor004")
    strip = skyline.crop((0, max(0, skyline.height - 22), skyline.width, skyline.height))
    strip = strip.resize((SCENE_W + 40, 38), Image.LANCZOS)
    place(im, strip, -18, 86, 1.0, (58, 66, 104))
    place(im, a.element("decor001"), -34, 66, 0.8, (255, 214, 140), alpha=0.5)
    place(im, a.element("decor002"), 146, 58, 0.8, (150, 190, 255), alpha=0.4)
    place(im, a.element("decor006"), 26, 76, 0.7, (230, 240, 255), alpha=0.5)
    return im


def scene_snow(a: Art) -> Image.Image:
    im = a.bg("Snow")
    place(im, a.element("treePine"), -4, 18, 1.0, (126, 156, 178))
    place(im, a.element("treePine"), 96, 26, 0.8, (108, 138, 162))
    place(im, a.element("treePine"), 250, 16, 1.05, (132, 162, 184), flip=True)
    place(im, a.element("snow"), 30, 100, 1.0, (235, 246, 255), alpha=0.7)
    return im


def scene_storm(a: Art) -> Image.Image:
    im = a.bg("Water")
    tint(im, (78, 92, 130))
    place(im, a.element("cloud1"), -50, 8, 1.0, (52, 62, 94))
    place(im, a.element("cloud2"), 110, 20, 1.0, (42, 52, 82))
    place(im, a.element("darkFog"), -30, 78, 0.7, (46, 58, 88), alpha=0.5)
    return im


def scene_void(a: Art) -> Image.Image:
    im = a.bg("Darkness")
    place(im, a.element("dark001"), 182, 4, 1.25, (144, 124, 224), alpha=0.8)
    place(im, a.element("dark002"), 96, -22, 1.0, (118, 98, 210), alpha=0.7)
    place(im, a.element("dark003"), 118, 36, 0.8, (168, 140, 240), alpha=0.5)
    stars(im, 90, 11, 104)
    return im


def scene_desert(a: Art) -> Image.Image:
    im = a.bg("Sand")
    place(im, a.element("mountain"), -10, 62, 0.7, (196, 168, 120), alpha=0.7)
    place(im, a.element("mountainC"), 170, 68, 0.58, (176, 146, 100), alpha=0.6)
    place(im, a.element("treeC"), 34, 88, 0.6, (122, 94, 54))
    place(im, a.element("rocks"), 250, GROUND_ROW - 11, 1.0, (122, 90, 58))
    place(im, a.element("rocks"), 66, GROUND_ROW - 9, 0.7, (108, 80, 52))
    return im


SCENES = [
    ("FOREST", scene_forest),
    ("CAVE", scene_cave),
    ("VOLCANO", scene_volcano),
    ("OCEAN", scene_ocean),
    ("SKY", scene_sky),
    ("CITY", scene_city),
    ("SNOW", scene_snow),
    ("STORM", scene_storm),
    ("VOID", scene_void),
    ("DESERT", scene_desert),
]


# -------------------------------------------------------------- move textures ---
# name, source, cell (x0,y0,x1,y1) or None, target max side.
# Cells are the frame boxes the EBDX scripts slice for themselves.
MOVES = [
    ("ORB",      "eb010", None, 28),
    ("SHADOW",   "ebShadow", (0, 0, 21, 31), 24),
    ("RIPPLE",   "ebRipple", None, 56),
    ("BURST",    "eb069", None, 56),
    ("SPARK",    "eb069_3", None, 40),
    ("WATER",    "eb156", None, 40),
    ("ICEBEAM",  "eb159", None, 48),
    ("LIGHT",    "eb195", None, 28),
    ("GREEN",    "eb200", None, 26),
    ("JET",      "eb243", None, 56),
    ("RING",     "eb057", None, 40),
    ("BOLT",     "eb081_2", None, 56),
    ("SHURIKEN", "eb008", (0, 0, 32, 32), 28),
    ("SLASH",    "eb027", None, 56),
    ("CLAW",     "eb028", None, 52),
    ("SWORD",    "eb137", None, 48),
    ("PSYRING",  "eb175", None, 52),
    ("SHADOWORB","eb175_2", None, 28),
    ("DARKCLAW", "eb176", None, 44),
    ("SWIRL",    "eb191", None, 52),
    ("SMOKE",    "eb250", None, 48),
    ("SUN",      "eb303", (0, 0, 32, 32), 28),
    ("DARK",     "eb427_1", None, 48),
    ("BLOB",     "eb429", None, 48),
    ("STREAK",   "eb430", None, 48),
    ("WAVE",     "eb452", None, 56),
    ("PSYRIPPLE","eb458", None, 52),
    ("ROCK",     "eb504", None, 40),
    ("GEAR",     "eb520", (0, 0, 32, 32), 26),
    ("SPARKLE",  "ebStar", None, 20),
    ("ICESTAR",  "eb540", (0, 0, 32, 32), 26),
    ("STRIKE",   "eb551", None, 52),
    ("POISON",   "eb59", None, 44),
    ("FLAME",    "eb136", (0, 0, 53, 101), 30),
    ("FLAME2",   "eb224", None, 22),
    ("CRYSTAL",  "eb248", (0, 0, 16, 25), 22),
    ("NOVA",     "eb263", None, 56),
    ("SWIRL2",   "eb093", None, 52),
    # Exact sheets used by the move scripts that have a distinctive animation.
    # Close Combat is a barrage of these fists, Thunderbolt is a strike down
    # from above, Vine Whip is the green lash, and Shadow Ball is the purple orb.
    ("FIST",     "eb086", None, 32),
    ("FISTSPARK","eb086_2", None, 12),
    ("LIGHTNING","eb069_2", (0, 0, 98, 430), 64),
    ("WHIP",     "eb208", None, 44),
    ("SHADOWBALL","eb175_3", None, 30),
    ("AURA",     "eb093", None, 48),
    ("AURASHOT", "eb093_3", None, 44),
]


def prep_move(art: Art, src: str, cell, target: int) -> Image.Image:
    im = art.move(src)
    if cell:
        im = im.crop(cell)
    bb = im.getchannel("A").getbbox()
    if bb:
        im = im.crop(bb)
    if max(im.size) > target:
        s = target / max(im.size)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS)
    return im


def pack_4bpp(im: Image.Image):
    """RGBA -> 4bpp nibbles and a 15-colour palette, transparent as nibble 0."""
    rgb = im.convert("RGB")
    pal_img = rgb.quantize(colors=15, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = pal_img.getpalette()[:45]
    colours = [rgb565(pal[i], pal[i + 1], pal[i + 2]) for i in range(0, len(pal) - 2, 3)]
    colours += [0] * (15 - len(colours))
    idx = pal_img.load()
    alpha = im.getchannel("A").load()
    w, h = im.size
    data: list[int] = []
    for y in range(h):
        row = [0] * w
        for x in range(w):
            row[x] = 0 if alpha[x, y] < 100 else min(15, idx[x, y] + 1)
        for x in range(0, w, 2):
            hi = row[x]
            lo = row[x + 1] if x + 1 < w else 0
            data.append((hi << 4) | lo)
    return w, h, data, colours


# ----------------------------------------------------------------------- main ---

def write_bg(a: Art) -> tuple[list[Image.Image], list[int]]:
    images, floors = [], []
    for name, builder in SCENES:
        im = builder(a).convert("RGB")
        images.append(im)
        # Floor colour around the feet, for the reflection and contact shadow.
        px = im.load()
        values = []
        for y in range(GROUND_ROW + 2, min(GROUND_ROW + 8, SCENE_H)):
            for x in range(80, 240, 4):
                r, g, b = px[x, y]
                values.append(rgb565(r, g, b))
        values.sort()
        floors.append(values[len(values) // 2])
    return images, floors


def write_backgrounds(images: list[Image.Image], floors: list[int]) -> None:
    data: list[int] = []
    entries = []
    for im, floor in zip(images, floors):
        entries.append((len(data), floor))
        px = im.load()
        for y in range(SCENE_H):
            for x in range(SCENE_W):
                data.append(swap16(rgb565(*px[x, y])))

    asm = [
        f"# Generated by tools/make_battle_art.py — {len(data)} scene words.",
        "# Stored in panel byte order: the sketch memcpys rows into the frame buffer.",
        ".section .rodata",
        ".balign 4",
        ".global BG_DATA",
        ".type BG_DATA, @object",
        "BG_DATA:",
    ]
    for i in range(0, len(data), 12):
        asm.append(".short " + ",".join(str(v) for v in data[i:i + 12]))
    asm += [".global BG_DATA_END", "BG_DATA_END:", ""]
    BG_ASM.write_text("\n".join(asm), encoding="utf-8")

    lines = [
        "// Generated by tools/make_battle_art.py — do not edit by hand.",
        "//",
        "// One painted 320x138 battle scene per biome, from the EBDX battlebacks,",
        "// scenery elements and ground plates. Byte-swapped for the panel buffer.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct BgInfo {",
        "  uint32_t off;    // first word of the top row in BG_DATA",
        "  uint16_t floor;  // RGB565 of the ground at the feet, for the reflection",
        "};",
        "",
        'extern "C" const uint16_t BG_DATA[];',
        "",
        f"#define BG_COUNT {len(entries)}",
        f"#define BG_W {SCENE_W}",
        f"#define BG_H {SCENE_H}",
        "",
        "static const BgInfo BGS[BG_COUNT] = {",
    ]
    for (name, _), (off, floor) in zip(SCENES, entries):
        lines.append(f"  {{{off},0x{floor:04x}}},   // {name}")
    lines += ["};", ""]
    BG_HDR.write_text("\n".join(lines), encoding="utf-8")
    print(f"{BG_HDR} + {BG_ASM.name}: {len(entries)} scenes, {len(data) * 2 / 1024:.0f} KB")


def write_moves(a: Art) -> tuple[list[tuple[str, int, int]], list[Image.Image]]:
    data: list[int] = []
    entries = []
    previews = []
    for name, src, cell, target in MOVES:
        im = prep_move(a, src, cell, target)
        if name == "SPARKLE":
            # The source star is yellow; a neutral white star can be tinted to the
            # move's type at run time and still read as a sparkle.
            px = im.load()
            for yy in range(im.height):
                for xx in range(im.width):
                    r, g, b, al = px[xx, yy]
                    v = max(r, g, b)
                    px[xx, yy] = (v, v, v, al)
        w, h, packed, colours = pack_4bpp(im)
        entries.append((name, w, h, len(data), colours))
        data.extend(packed)
        previews.append((name, im, colours))

    asm = [
        f"# Generated by tools/make_battle_art.py — {len(data)} effect bytes.",
        ".section .rodata",
        ".balign 4",
        ".global ATK_DATA",
        ".type ATK_DATA, @object",
        "ATK_DATA:",
    ]
    for i in range(0, len(data), 32):
        asm.append(".byte " + ",".join(str(b) for b in data[i:i + 32]))
    asm += [".global ATK_DATA_END", "ATK_DATA_END:", ""]
    ATK_ASM.write_text("\n".join(asm), encoding="utf-8")

    lines = [
        "// Generated by tools/make_battle_art.py — do not edit by hand.",
        "//",
        "// EBDX move effects, one cell per texture, 4 bpp (0 transparent, 1..15",
        "// palette). Palette entry 1 is the brightest, which the attack code uses",
        "// as the flash/core colour.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct AtkTex {",
        "  uint32_t off;",
        "  uint16_t pal[15];",
        "  uint8_t w, h;",
        "};",
        "",
        'extern "C" const uint8_t ATK_DATA[];',
        "",
        f"#define ATK_COUNT {len(entries)}",
        "",
        "enum AtkId : uint8_t {",
    ]
    for i, (name, *_rest) in enumerate(entries):
        lines.append(f"  ATK_{name},")
    lines += ["};", "",
              "static const AtkTex ATK_TEX[ATK_COUNT] = {"]
    for name, w, h, off, colours in entries:
        pal = ",".join(f"0x{c:04x}" for c in colours)
        lines.append(f"  {{{off},{{{pal}}},{w},{h}}},   // {name}")
    lines += ["};", ""]
    ATK_HDR.write_text("\n".join(lines), encoding="utf-8")
    print(f"{ATK_HDR} + {ATK_ASM.name}: {len(entries)} textures, {len(data) / 1024:.1f} KB")
    return entries, previews


def preview(images: list[Image.Image], moves_previews) -> None:
    path = Path(sys.argv[sys.argv.index("--preview") + 1])
    sheet = Image.new("RGB", (SCENE_W * 2, SCENE_H * 5), (20, 20, 24))
    for i, im in enumerate(images):
        sheet.paste(im, ((i % 2) * SCENE_W, (i // 2) * SCENE_H))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(path)
    sheet2 = Image.new("RGB", (640, 640), (30, 30, 36))
    x = y = 0
    rowh = 0
    d = ImageDraw.Draw(sheet2)
    for name, im, colours in moves_previews:
        if x + im.width > 640:
            x, y = 0, y + rowh + 4
            rowh = 0
        bg = Image.new("RGB", im.size, (60, 60, 70))
        m = im.getchannel("A").point(lambda v: 255 if v > 100 else 0)
        bg.paste(im.convert("RGB"), (0, 0), m)
        sheet2.paste(bg, (x, y))
        d.text((x, y + im.height + 1), name, fill=(255, 255, 0))
        x += im.width + 6
        rowh = max(rowh, im.height + 12)
    sheet2 = sheet2.resize((sheet2.width * 2, sheet2.height * 2), Image.NEAREST)
    sheet2.save(path.with_name(path.stem + "-moves.png"))
    print(f"preview {path}")


def main() -> None:
    a = Art(ZIP)
    images, floors = write_bg(a)
    write_backgrounds(images, floors)
    entries, previews = write_moves(a)
    for name, w, h, off, colours in entries:
        print(f"  {name:<10} {w}x{h}")
    if "--preview" in sys.argv:
        preview(images, previews)


if __name__ == "__main__":
    main()
