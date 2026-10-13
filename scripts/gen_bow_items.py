"""Generate assets/items/{bow,arrow}.{vox,json} -- the BOW AND ARROWS
(docs/PLAN_projectiles.md package C, game/arrows.h).

THE ARROW is ammunition (ItemKind::Ammo): carried in the pack, never held to
fight with. Its art is what flies, what sticks out of a body it embedded in
(an appended slot on the struck limb, Mob::EmbedArrow) and what lies on the
ground. Authored along +x like every held item in the library: the nock and
fletching at low x, the steel head at HIGH x. The loader takes the arrow's
POINT from the art (the foremost cell's centre, ItemDef::ArrowSpec::tip) and
the point's matter from that cell's material, so the head IS what the
penetrator's hardness is read from (steel, materials.json hardness 200).

THE BOW is a held item (kind melee, weaponClass "bow"): a self longbow, braced,
the stave bending away from a straight string. Stave along x, the string offset
in y (the scene axis a sword's edge leads in), the leather grip in the middle
where the hilt box closes the fist on it.

Both at 80 art voxels per metre (8 micro per world voxel at 10 vox/m), the
library's held-item pitch, and inside the DebrisVoxel int8 range (<= 120 micro).

Run: python scripts/gen_bow_items.py
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bladesmith as bs  # noqa: E402  (shared .vox writer + colour table)

ART = bs.ART_VOXELS_PER_METRE      # 80
SCALE = bs.SCALE                   # micro per world voxel

# palette index == material ID (bladesmith's convention)
STAFF_WOOD = 54
STEEL = 57
GRIP_LEATHER = 58
LINEN = 111

bs.COLOURS.update({
    "shaft":     0x7A5A36,   # a spruce shaft, oiled
    "shaft_dk":  0x553C22,
    "nock":      0x2A1E14,   # the horn nock
    "fletch_a":  0xA8241C,   # the cock feather: red
    "fletch_b":  0xD8D2C0,   # the hen feathers: grey-white goose
    "head":      0x5A626C,   # the bodkin
    "head_hi":   0x8C96A1,
    "stave":     0x6B4626,   # yew heartwood
    "stave_hi":  0x9A6A3C,   # sapwood back
    "string":    0xCFC6A8,   # waxed linen
    "grip":      0x3A2619,
    "grip_dk":   0x1E130C,
})


# ---- THE ARROW ---------------------------------------------------------------
# 2026-10-13 (owner: "a little shorter/smaller"): 58 -> 48 cells, 0.725 m ->
# 0.60 m, the head's socket a cell shorter and the vanes two out only at their
# middle -- it read as a javelin stuck in a man. The point is still the
# foremost cell, so every placement that reads ItemDef::ArrowSpec::tip (the
# flight, the embed, the stuck item) follows the new length by itself.
ARROW_LEN = 48          # 0.60 m
ARROW_X = 5             # cross-section, odd: the axis through the middle cell
HEAD_FROM = ARROW_LEN - 7   # socket + point from here on (7 cells, 8.75 cm)


def arrow_cells():
    f = bs.Forge(ARROW_LEN, ARROW_X, ARROW_X)

    def fn(x, dy, dz):
        on_axis = dy == 0 and dz == 0
        # The head: a socket (a 3-cell cross round the shaft's end), then a
        # four-cell bodkin point. The foremost cell is the arrow's POINT
        # (ItemDef::ArrowSpec::tip).
        if x >= HEAD_FROM:
            k = x - HEAD_FROM
            if k <= 2:
                if (abs(dy) + abs(dz)) <= 1:
                    return (STEEL, "head")
                return None
            return (STEEL, "head_hi") if on_axis else None
        # The nock.
        if x <= 1:
            return (STAFF_WOOD, "nock") if on_axis else None
        # Fletching: four vanes, 9 cells long, 2 out from the shaft at their
        # middle, 1 at the ends (a trailing taper read at a glance).
        if 2 <= x <= 10 and (dy == 0) != (dz == 0):
            r = max(abs(dy), abs(dz))
            full = 5 <= x <= 8
            if r == 1 or (r == 2 and full):
                cock = dz > 0   # the up vane is the cock feather
                return (LINEN, "fletch_a" if cock else "fletch_b")
            return None
        if on_axis:
            return (STAFF_WOOD, "shaft_dk" if (x % 9) == 0 else "shaft")
        return None

    f.stamp(fn)
    return f


# ---- THE BOW -----------------------------------------------------------------
BOW_LEN = 97            # 1.21 m tip to tip
BOW_Y = 15              # the bend + brace height (string to belly)
BOW_Z = 3               # the stave's width
GRIP_HALF = 6           # the wrapped grip: 13 cells (16 cm) at the middle


def bow_cells():
    f = bs.Forge(BOW_LEN, BOW_Y, BOW_Z)
    mid = (BOW_LEN - 1) / 2.0
    half = mid
    # The string runs along the +y face; the stave's centre line bends away
    # from it, touching it at the tips (the nocks) and `brace` cells off it at
    # the grip. dy is centred on 0.
    string_dy = (BOW_Y - 1) // 2      # +7
    brace = 9.0

    def stave_dy(x):
        u = (x - mid) / half
        return (string_dy - 1) - brace * (1.0 - u * u)

    def thick(x):
        # Depth of the stave (in y) from the grip out to the nocks.
        u = abs(x - mid) / half
        if abs(x - mid) <= GRIP_HALF:
            return 3
        return 3 if u < 0.55 else (2 if u < 0.9 else 1)

    def fn(x, dy, dz):
        c = stave_dy(x)
        t = thick(x)
        lo = math.floor(c - (t - 1) / 2.0 + 0.5)
        if lo <= dy < lo + t:
            if abs(x - mid) <= GRIP_HALF:
                wrap = ((x + dy) % 3) == 0
                return (GRIP_LEATHER, "grip_dk" if wrap else "grip")
            # Width tapers to 1 at the nocks.
            if abs(x - mid) / half > 0.9 and dz != 0:
                return None
            back = dy == lo + t - 1  # the back (string side) is sapwood
            return (STAFF_WOOD, "stave_hi" if back else "stave")
        # The string: tip to tip on the +y face, one cell, centre width.
        if dy == string_dy and dz == 0 and 1 <= x <= BOW_LEN - 2:
            return (LINEN, "string")
        return None

    f.stamp(fn)
    return f


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "items")

    arrow = arrow_cells()
    ax, ay, az = arrow.size
    # A grip context so the loader is content (every non-worn item declares
    # one); an arrow is never put in a fist by the game.
    arrow_side = {
        "comment": "A war arrow: a spruce shaft, goose fletching (the red cock "
                   "feather up), a steel bodkin. Ammunition (kind ammo): carried "
                   "in the pack, shot from a bow, and what sticks out of whatever "
                   "it embeds in. Regenerate with scripts/gen_bow_items.py. The "
                   "point is the art's foremost cell (its material is the head's "
                   "hardness); flight numbers are items.json's `arrow` block.",
        "name": "arrow",
        "model": "arrow",
        "artVoxelsPerMetre": ART,
        "hp": 6,
        "severable": True,
        "severImpactSpeed": 3.0,
        "grip": {"held_right": {"translation": [0, 0, 0],
                                "rotation": [90, -90, 0], "scale": 1}},
        "hilt": {"min": [(ax - 10) // 2, 0, 0], "size": [10, ay, az]},
    }
    bs.emit(out, "arrow", arrow, arrow_side)

    bow = bow_cells()
    bx, by, bz = bow.size
    mid = (bx - 1) // 2
    # The hilt is the wrapped grip: its centre is what the hand socket holds.
    # Its y range is the stave's at the middle (dy -4..-2, i.e. y 3..5).
    bow_side = {
        "comment": "A braced self longbow of yew, a waxed linen string, a "
                   "leather grip. Held like any weapon (kind melee, weaponClass "
                   "bow), but LMB DRAWS it: hold to draw, release to shoot "
                   "(game/arrows.h). Draw speed and time are items.json's `bow` "
                   "block. Regenerate with scripts/gen_bow_items.py.",
        "name": "bow",
        "model": "bow",
        "artVoxelsPerMetre": ART,
        "hp": 25,
        "severable": True,
        "severImpactSpeed": 6.0,
        "grip": {"held_right": {"translation": [0, 0, 0],
                                "rotation": [90, -90, 0], "scale": 1}},
        "hilt": {"min": [mid - GRIP_HALF, 3, 0],
                 "size": [2 * GRIP_HALF + 1, 3, bz]},
    }
    bs.emit(out, "bow", bow, bow_side)


if __name__ == "__main__":
    main()
