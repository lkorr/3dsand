"""Generate assets/items/back_quiver.{vox,json} -- THE QUIVER (2026-10-15,
docs/PLAN_poison_arrows.md, game/quiver.h).

A quiver (ItemKind::Quiver) is worn on the BACK (EquipSlotId::Back) and is the
only thing the bow draws from. It is never a rig slot: game/quiver.cpp
PresentQuiver draws it render-only on the rig's `back` socket, with the tails
of the arrows it holds standing out of its mouth. So the art is only a LOOK --
nothing collides with it, nothing carves it (owner: not damageable, for now).

A stitched leather tube, open at the top, a turned wooden base, two darker
strap bands and a rolled rim. Authored along +x like every item in the
library: the BASE at low x, the MOUTH at high x (PresentQuiver stands the
arrows in from the high end). Hollow -- the arrows are drawn inside it.

80 art voxels per metre (the library's held-item pitch). The tube is 44 cells
(0.55 m) long: the 0.65 m arrow stands 12-22 cm proud of the mouth with its
point clear of the base.

Run: python scripts/gen_quiver_item.py
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bladesmith as bs  # noqa: E402  (shared .vox writer + colour table)

ART = bs.ART_VOXELS_PER_METRE      # 80

# palette index == material ID (bladesmith's convention)
STAFF_WOOD = 54
GRIP_LEATHER = 58

# THE BOW'S OWN COLOURS (gen_bow_items.py), by value: the merged art palette is
# nearly full (255 entries across every creature and item), and a quiver that
# matches the bow it serves costs it nothing.
bs.COLOURS.update({
    "quiver":     0x6B4626,   # = bow "stave": oiled brown leather
    "quiver_dk":  0x553C22,   # = arrow "shaft_dk": the shadowed side
    "quiver_band": 0x3A2619,  # = bow "grip": the strap bands
    "quiver_rim": 0x9A6A3C,   # = bow "stave_hi": the rolled rim, worn pale
    "quiver_base": 0x7A5A36,  # = arrow "shaft": the turned wooden base
    "quiver_stitch": 0xCFC6A8,  # = bow "string": waxed linen stitching
})

LEN = 44          # 0.55 m
X = 9             # cross-section, odd (11 cm across)
R_OUT = 4.45
R_IN = 3.2
BASE = 2          # the wooden base's cells
BANDS = ((11, 13), (30, 32))
RIM = LEN - 2


def quiver_cells():
    f = bs.Forge(LEN, X, X)

    def fn(x, dy, dz):
        r = math.hypot(dy, dz)
        if r > R_OUT:
            return None
        if x < BASE:
            return (STAFF_WOOD, "quiver_base")
        if r < R_IN:
            return None   # hollow: the arrows stand in here
        if x >= RIM:
            return (GRIP_LEATHER, "quiver_rim")
        for a, b in BANDS:
            if a <= x < b:
                return (GRIP_LEATHER, "quiver_band")
        # The seam down the back (-y), stitched every third cell.
        if dy <= -3 and abs(dz) <= 1:
            return (GRIP_LEATHER, "quiver_stitch" if x % 3 == 0 else "quiver_dk")
        return (GRIP_LEATHER, "quiver_dk" if dz < -2 else "quiver")

    f.stamp(fn)
    return f


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "items")
    q = quiver_cells()
    sx, sy, sz = q.size
    side = {
        "comment": "A back quiver: a stitched leather tube with a turned wooden "
                   "base, worn on the BACK (kind quiver, EquipSlotId::Back). The "
                   "bow draws a random arrow out of the worn one (game/quiver.h). "
                   "Drawn render-only on the rig's `back` socket by its `back` "
                   "grip (the tube's +x is its mouth; the arrows stand in from "
                   "there); the hilt box is the strap's middle, the point that "
                   "sits on the socket. Regenerate with scripts/gen_quiver_item.py.",
        "name": "back_quiver",
        "model": "back_quiver",
        "artVoxelsPerMetre": ART,
        "hp": 12,
        "severable": False,
        "severImpactSpeed": 0.0,
        # Mouth up over the RIGHT shoulder (the rig faces +z, its right is -x):
        # +x onto up, tipped 20 degrees toward the right.
        "grip": {
            "back": {"translation": [0, 0, 0], "rotation": [0, 0, 110], "scale": 1},
            "held_right": {"translation": [0, 0, 0], "rotation": [90, -90, 0],
                           "scale": 1},
        },
        "hilt": {"min": [(sx - 6) // 2, 0, 0], "size": [6, sy, sz]},
    }
    bs.emit(out, "back_quiver", q, side)


if __name__ == "__main__":
    main()
