#!/usr/bin/env python3
"""Generate the character screen's pixel-art chrome.

Writes assets/ui/chrome.bmp (32-bit BGRA, alpha preserved) and
assets/ui/chrome.json (a sprite name -> rect table, plus 9-slice borders).
Both are COMMITTED: the engine reads the BMP at startup and packs each sprite
into ImGui's font atlas as a custom rect, so the UI needs no texture-upload
plumbing of its own beyond the one sampler the avatar portrait already needs.

WHY A SCRIPT AND NOT A PNG SOMEBODY DREW. Every other piece of art in this
repo is generated (gen_mina.py, gen_sword_item.py, gen_palette.py) for the same
two reasons that apply here: the palette is shared with the game's own
materials and has to move with them, and a diff of a generated image is a diff
of the code that made it. Re-run this and commit both outputs.

    python scripts/gen_ui_chrome.py

PALETTE. Derived from mina's robe (deep indigo #48, gold emissive trim #49,
near-black indigo #50, leather #53) so the UI and the character it frames are
the same object. Everything is authored at 1x and drawn at an integer 2x, which
is what keeps it crisp: the sampler is NEAREST and every sprite lands on a
whole-pixel boundary.
"""

import json
import os
import struct

# ---- palette ---------------------------------------------------------------
INK      = (18, 16, 30)      # near-black indigo: outlines, the hard edge
DEEP     = (34, 30, 56)      # panel field
MID      = (48, 42, 78)      # riveted metal band
HI       = (68, 60, 104)     # top/left bevel
LOW      = (26, 23, 44)      # bottom/right bevel
GOLD     = (201, 164, 74)    # inlay, the accent that says "this is worn"
GOLD_HI  = (240, 214, 140)
GOLD_DIM = (120, 96, 42)
PARCH    = (216, 205, 180)   # engraving strokes
PARCH_D  = (150, 140, 118)
BLOOD    = (168, 46, 46)
CLEAR    = None              # transparent


class Img:
    """A tiny RGBA raster. No PIL: the repo's art scripts are stdlib-only."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [(0, 0, 0, 0)] * (w * h)

    def put(self, x, y, c, a=255):
        if c is CLEAR or x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        self.px[y * self.w + x] = (c[0], c[1], c[2], a)

    def get(self, x, y):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return (0, 0, 0, 0)
        return self.px[y * self.w + x]

    def rect(self, x, y, w, h, c, a=255):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.put(i, j, c, a)

    def frame(self, x, y, w, h, c, a=255):
        for i in range(x, x + w):
            self.put(i, y, c, a)
            self.put(i, y + h - 1, c, a)
        for j in range(y, y + h):
            self.put(x, j, c, a)
            self.put(x + w - 1, j, c, a)

    def blit(self, src, dx, dy):
        for j in range(src.h):
            for i in range(src.w):
                p = src.get(i, j)
                if p[3]:
                    self.put(dx + i, dy + j, (p[0], p[1], p[2]), p[3])


# ---- sprite builders -------------------------------------------------------

def panel(size=24, border=8, gold_corners=True, hollow=False):
    """The 9-slice panel frame: riveted dark metal with a gold inlay line.

    The border must be at least 2 px narrower than half the tile or the middle
    slice degenerates and the stretch has nothing to repeat.

    `hollow` clears the MIDDLE SLICE to transparent, which is the difference
    between a panel and a FRAME. A 9-slice stretches its centre pixel across
    the whole interior, so an opaque centre paints over everything the frame
    was supposed to be framing — which is exactly what a solid `panel_inner`
    did to the avatar portrait: the picture rendered correctly and was then
    covered by its own border sprite.
    """
    im = Img(size, size)
    im.rect(0, 0, size, size, DEEP)
    im.frame(0, 0, size, size, INK)                       # hard outer edge
    im.rect(1, 1, size - 2, border - 3, MID)              # top band
    im.rect(1, size - border + 2, size - 2, border - 3, MID)
    im.rect(1, 1, border - 3, size - 2, MID)              # side bands
    im.rect(size - border + 2, 1, border - 3, size - 2, MID)
    # bevel: light from the top-left, exactly one pixel, no gradients
    for i in range(1, size - 1):
        im.put(i, 1, HI)
        im.put(i, size - 2, LOW)
    for j in range(1, size - 1):
        im.put(1, j, HI)
        im.put(size - 2, j, LOW)
    # gold inlay: a single line just inside the metal band
    g = border - 2
    im.frame(g, g, size - 2 * g, size - 2 * g, GOLD_DIM)
    for i in range(g, size - g):
        im.put(i, g, GOLD)
    # rivets in the corners of the band
    for (rx, ry) in ((3, 3), (size - 4, 3), (3, size - 4), (size - 4, size - 4)):
        im.put(rx, ry, GOLD_HI)
        im.put(rx, ry + 1, GOLD_DIM)
    if gold_corners:
        # a two-pixel gold notch pointing in from each corner: the whole
        # "authored object" read comes from these, and they must be INSIDE the
        # corner slice or the 9-slice stretch would smear them
        for (cx, cy, sx, sy) in ((g, g, 1, 1), (size - g - 1, g, -1, 1),
                                 (g, size - g - 1, 1, -1),
                                 (size - g - 1, size - g - 1, -1, -1)):
            im.put(cx + sx, cy + sy, GOLD_HI)
            im.put(cx + 2 * sx, cy + sy, GOLD)
            im.put(cx + sx, cy + 2 * sy, GOLD)
    if hollow:
        for j in range(border, size - border):
            for i in range(border, size - border):
                im.px[j * im.w + i] = (0, 0, 0, 0)
    return im


def slot(size=22, edge=INK, face=None, accent=None, inset=True):
    """One item slot: a sunken square with a bevel and an optional accent."""
    im = Img(size, size)
    im.rect(1, 1, size - 2, size - 2, face if face else (24, 21, 40))
    im.frame(0, 0, size, size, edge)
    if inset:
        for i in range(1, size - 1):          # sunken: dark top, light bottom
            im.put(i, 1, LOW)
            im.put(i, size - 2, HI)
        for j in range(1, size - 1):
            im.put(1, j, LOW)
            im.put(size - 2, j, HI)
    if accent:
        im.frame(2, 2, size - 4, size - 4, accent)
        for (cx, cy) in ((2, 2), (size - 3, 2), (2, size - 3),
                         (size - 3, size - 3)):
            im.put(cx, cy, GOLD_HI if accent is GOLD else accent)
    return im


def stroke(im, pts, c):
    for (x, y) in pts:
        im.put(x, y, c)


def engraving(kind, size=16):
    """A single silhouette that says what an empty slot is FOR.

    Deliberately one solid shape with a highlight edge rather than a detailed
    icon: it is drawn dim behind whatever is dropped on it, so it has to read
    at a glance and disappear politely.
    """
    im = Img(size, size)
    c, h = PARCH_D, PARCH

    def fill(rows, col=None):
        col = col or c
        for (y, x0, x1) in rows:
            for x in range(x0, x1 + 1):
                im.put(x, y, col)

    if kind == "head":            # a helm: dome + brow bar + cheek guards
        fill([(3, 6, 9), (4, 4, 11), (5, 3, 12), (6, 3, 12), (7, 3, 12)])
        fill([(8, 3, 5), (8, 10, 12), (9, 3, 5), (9, 10, 12),
              (10, 3, 5), (10, 10, 12), (11, 4, 5), (11, 10, 11)])
        fill([(8, 6, 9)], GOLD_DIM)
        stroke(im, [(x, 3) for x in range(6, 10)], h)
    elif kind == "chest":         # a cuirass
        fill([(3, 4, 11), (4, 3, 12), (5, 3, 12), (6, 3, 12), (7, 3, 12),
              (8, 3, 12), (9, 4, 11), (10, 4, 11), (11, 5, 10)])
        fill([(4, 7, 8), (5, 7, 8), (6, 7, 8), (7, 7, 8)], GOLD_DIM)
        stroke(im, [(x, 3) for x in range(4, 12)], h)
    elif kind == "legs":          # greaves: two tapering columns
        fill([(3, 4, 11), (4, 4, 11)])
        for y in range(5, 13):
            fill([(y, 4, 6), (y, 9, 11)])
        fill([(3, 7, 8)], GOLD_DIM)
        stroke(im, [(x, 3) for x in range(4, 12)], h)
    elif kind == "boots":         # a pair of boots, toes out
        for y in range(4, 10):
            fill([(y, 4, 6), (y, 9, 11)])
        fill([(10, 2, 6), (11, 2, 6), (10, 9, 13), (11, 9, 13)])
        stroke(im, [(4, 4), (5, 4), (9, 4), (10, 4)], h)
    elif kind == "shoulders":     # pauldrons: two overlapping plates
        fill([(5, 2, 6), (6, 1, 7), (7, 1, 7), (8, 2, 6)])
        fill([(5, 9, 13), (6, 8, 14), (7, 8, 14), (8, 9, 13)])
        stroke(im, [(x, 5) for x in range(2, 7)], h)
        stroke(im, [(x, 5) for x in range(9, 14)], h)
    elif kind == "hands":         # a gauntlet
        fill([(3, 5, 6), (3, 8, 9), (4, 4, 10), (5, 4, 10), (6, 4, 11),
              (7, 3, 11), (8, 3, 11), (9, 4, 10), (10, 5, 9), (11, 6, 9)])
        fill([(9, 5, 9)], GOLD_DIM)
    elif kind == "belt":          # a girdle with a buckle
        fill([(6, 2, 13), (7, 2, 13), (8, 2, 13)])
        fill([(6, 6, 9), (7, 6, 9), (8, 6, 9)], GOLD_DIM)
        fill([(7, 7, 8)], GOLD)
        stroke(im, [(x, 6) for x in range(2, 14)], h)
    elif kind == "trinket":       # an amulet on a cord
        stroke(im, [(4, 3), (5, 2), (10, 2), (11, 3)], c)
        fill([(4, 6, 9), (5, 5, 10), (6, 5, 10), (7, 5, 10), (8, 6, 9)])
        fill([(6, 7, 8)], GOLD)
        stroke(im, [(x, 4) for x in range(6, 10)], h)
    elif kind == "sheath":        # a scabbard, hilt up
        fill([(2, 7, 8), (3, 6, 9), (4, 7, 8)])
        fill([(3, 6, 9)], GOLD_DIM)
        for y in range(5, 14):
            fill([(y, 6, 9)])
        fill([(5, 6, 9), (9, 6, 9)], GOLD_DIM)
        stroke(im, [(6, y) for y in range(5, 14)], h)
    elif kind == "quick":         # a pouch
        fill([(4, 6, 9), (5, 4, 11), (6, 3, 12), (7, 3, 12), (8, 3, 12),
              (9, 3, 12), (10, 4, 11), (11, 5, 10)])
        fill([(6, 7, 8), (7, 7, 8)], GOLD_DIM)
        stroke(im, [(x, 5) for x in range(4, 12)], h)
    elif kind == "element":       # a filled droplet: matter
        fill([(3, 7, 8), (4, 6, 9), (5, 5, 10), (6, 4, 11), (7, 4, 11),
              (8, 4, 11), (9, 5, 10), (10, 6, 9), (11, 7, 8)])
    elif kind == "form":          # an arrow: what the spell BECOMES
        fill([(7, 2, 12), (8, 2, 12)])
        fill([(4, 9, 10), (5, 10, 11), (6, 11, 12),
              (9, 11, 12), (10, 10, 11), (11, 9, 10)])
    elif kind == "modifier":      # a spiral tick: decoration
        fill([(4, 5, 10), (5, 5, 6), (6, 5, 6), (7, 5, 10), (8, 9, 10),
              (9, 9, 10), (10, 5, 10)])
    elif kind == "melee":         # a sword, point up: what a melee item IS
        fill([(1, 7, 8), (2, 7, 8), (3, 7, 8), (4, 7, 8), (5, 7, 8),
              (6, 7, 8), (7, 7, 8), (8, 7, 8), (9, 7, 8)])
        stroke(im, [(7, y) for y in range(1, 10)], h)     # lit edge
        fill([(10, 4, 11)], GOLD_DIM)                      # crossguard
        fill([(10, 7, 8)], GOLD)
        fill([(11, 7, 8), (12, 7, 8), (13, 6, 9)])         # grip + pommel
        fill([(13, 7, 8)], GOLD)
    elif kind == "unknown":       # anything the panel has no icon for
        fill([(4, 5, 10), (5, 4, 11), (6, 4, 11), (7, 4, 11), (8, 4, 11),
              (9, 4, 11), (10, 5, 10)])
        fill([(6, 6, 9), (7, 6, 9), (8, 6, 9)], (24, 21, 40))
    elif kind == "bag":           # the general storage mark
        fill([(4, 6, 9), (5, 4, 11), (6, 3, 12), (7, 3, 12), (8, 3, 12),
              (9, 3, 12), (10, 3, 12), (11, 4, 11)])
        fill([(3, 5, 6), (3, 9, 10)], h)
    else:
        raise SystemExit("unknown engraving kind: " + kind)
    return im


# ---- the sheet -------------------------------------------------------------

def build():
    sprites = {}
    order = []

    def add(name, im, border=None):
        sprites[name] = (im, border)
        order.append(name)

    add("panel", panel(24, 8), [8, 8, 8, 8])
    # HOLLOW: this one frames a picture (the avatar portrait), so its middle
    # slice must not exist. See panel()'s docstring.
    add("panel_inner", panel(16, 5, gold_corners=False, hollow=True),
        [5, 5, 5, 5])
    add("slot", slot(22))
    add("slot_hover", slot(22, edge=GOLD_DIM, face=(38, 33, 60), accent=GOLD))
    add("slot_filled", slot(22, face=(30, 27, 50), accent=GOLD_DIM))
    add("slot_refuse", slot(22, edge=BLOOD, face=(48, 22, 26), accent=BLOOD))
    for k in ("head", "chest", "legs", "boots", "shoulders", "hands", "belt",
              "trinket", "sheath", "quick"):
        add("slot_" + k, engraving(k))
    for k in ("element", "form", "modifier"):
        add("glyph_" + k, engraving(k))
    # Item icons are keyed by ItemKind, not by item name: there is no
    # per-asset art here, and inventing a naming convention for art that does
    # not exist would be a schema nobody can author against. "unknown" is what
    # every kind without its own mark falls back to.
    for k in ("melee", "unknown"):
        add("item_" + k, engraving(k))
    add("icon_bag", engraving("bag"))

    # Pack into a sheet, one row per 24 px band. Simple shelf packing — the
    # sheet is 20-odd small sprites and an optimal packer would be more code
    # than the pixels it saves.
    W = 128
    pad = 1
    x, y, rowH = pad, pad, 0
    rects = {}
    for name in order:
        im, border = sprites[name]
        if x + im.w + pad > W:
            x = pad
            y += rowH + pad
            rowH = 0
        rects[name] = (x, y, im.w, im.h, border)
        x += im.w + pad
        rowH = max(rowH, im.h)
    H = y + rowH + pad
    # Round up to a power of two so the file is a tidy texture-shaped thing.
    H = 1 << (H - 1).bit_length()

    sheet = Img(W, H)
    for name in order:
        rx, ry, _, _, _ = rects[name]
        sheet.blit(sprites[name][0], rx, ry)
    return sheet, rects


def write_bmp(path, im):
    """32-bit BGRA, BITMAPV4HEADER with explicit channel masks.

    V4 rather than the 24-bit BITMAPINFOHEADER the screenshot writer uses
    (test/support.cpp WriteBmpFile), for one reason: chrome needs ALPHA. A
    32 bpp BI_RGB bitmap leaves the fourth byte formally undefined and editors
    disagree about it; V4 with masks is the format that reliably round-trips,
    so the committed file can be opened and repainted by hand.
    """
    row = im.w * 4
    img = bytearray(row * im.h)
    for j in range(im.h):                      # bottom-up, as BMP wants
        src = (im.h - 1 - j) * im.w
        off = j * row
        for i in range(im.w):
            r, g, b, a = im.px[src + i]
            img[off + i * 4 + 0] = b
            img[off + i * 4 + 1] = g
            img[off + i * 4 + 2] = r
            img[off + i * 4 + 3] = a
    hdr_size = 108                             # BITMAPV4HEADER
    offset = 14 + hdr_size
    v4 = struct.pack(
        "<IiiHHIIiiII" + "IIII" + "I" + "9i" + "III",
        hdr_size, im.w, im.h, 1, 32, 3,        # 3 = BI_BITFIELDS
        len(img), 2835, 2835, 0, 0,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
        0x73524742,                            # 'sRGB' colour space
        *([0] * 9),                            # CIEXYZTRIPLE endpoints, unused
        0, 0, 0)                               # gamma R/G/B, unused for sRGB
    assert len(v4) == hdr_size, len(v4)
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", offset + len(img), 0, 0, offset))
        f.write(v4)
        f.write(img)


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(here, "assets", "ui")
    os.makedirs(out, exist_ok=True)
    sheet, rects = build()
    write_bmp(os.path.join(out, "chrome.bmp"), sheet)
    table = {
        "version": 1,
        "width": sheet.w,
        "height": sheet.h,
        "sprites": {
            name: ({"x": r[0], "y": r[1], "w": r[2], "h": r[3]}
                   | ({"border": r[4]} if r[4] else {}))
            for name, r in rects.items()
        },
    }
    with open(os.path.join(out, "chrome.json"), "w") as f:
        json.dump(table, f, indent=1, sort_keys=True)
        f.write("\n")
    print("chrome: %d sprites, %dx%d -> assets/ui/chrome.{bmp,json}"
          % (len(rects), sheet.w, sheet.h))


if __name__ == "__main__":
    main()
