"""Generate assets/items/{round_shield,buckler,heater_shield}.{vox,json}.

WHAT A SHIELD IS MECHANICALLY. Nothing in a sidecar says how much a shield
covers. `MobSystem::FindShieldBlock` tests an attacker's swept blade against
the shield's OWN LATTICE in its live pose, so the art below IS the block: a
buckler stops a blade across a fist's width because it is that small, a heater
shield covers a torso because it is that big, and a shield hacked or burnt
through has a hole a blade goes through. What each one COSTS to hold up is its
`shield` row in items.json (raise time, walk and turn while raised, how hard a
blow it holds against) -- see src/game/item.h ShieldSpec.

So the materials are mechanical too, and every one is an existing material:
  * the round shield's boards are wood (staff_wood and wood, alternating so the
    planks read): a blade notches them (Mob::CutLimb's hardness rule: wood is
    34-40 against skin's 8), and fire takes them like any wood. Its rim is
    rawhide (`leather`), the first thing a fire finds.
  * the buckler is steel, small and quick: it does not burn and barely chips,
    and it covers almost nothing unless it is put where the blade is.
  * the heater shield is a wooden board FACED with iron and bound in steel: the
    face does not burn and a blade scratches it, and the boards behind it will
    still catch from the back and the edges.

THE FRAME (the same traps gen_mace_item.py documents, plus one).
  * Scene (the .vox, Z-up) -> engine (Y-up) is (x, z, -y). Everything below is
    written in ENGINE item coordinates and mapped once, in `to_scene`, so the
    shape code never has to think about the flip.
  * Engine item +X is the shield's FACE NORMAL (the boss stands toward +X),
    item +Y is UP (a heater's point is at low Y), item +Z is across.
  * THE FOREARM LIES ACROSS THE BACK, so the shield's UP is the fist's GRIP
    AXIS (hand +Z, the way a sword leaves the fist) and its face is the back
    of the hand: raised with the forearm level across the chest, the shield
    stands upright facing forward, which is the one pose that matters. The
    handle bar therefore runs along item Y. Hanging at rest the same grip
    carries the shield on the outside of the leg, face out, top forward.
  * A shield is NOT symmetric front to back, so it cannot use the engine's
    derived left grip (Mob::EquipItem mirrors a `held_right` grip through the
    item's thinnest axis, which would turn the face inward). Both hands are
    authored: `held_left` = Rx(90) (face +X = the body's left, outward for a
    left fist; up = hand +Z), `held_right` = Rz(180) Rx(90) (face -X, outward
    for the right). The two are each other's reflection through the item's
    own Z mid-plane, which a round shield and a heater are both symmetric
    about, so a left shield is exactly the mirror image of a right one -- and
    the raise clip is authored for the right hand, the left playing its
    derived mirror (scripts/gen_shield_clips.py solves it with this grip).
  * The HILT BOX is the fist, behind the middle of the shield. The runtime puts
    its centre on the hand's socket, so the shield hangs from the fist with no
    tuned constant. A centre-grip shield needs the fist INSIDE the boss: the
    boards have a hole at the middle and the boss is a hollow dome, which is
    how a real one is built and why the fist does not clip through the wood.

Run: python scripts/gen_shield_items.py
"""

import json
import math
import os
import struct

# ---- palette (assets/prefabs -- palette index == material ID) --------------
# Asserted against materials.json in main(): a renumbered material silently
# turns a shield into whatever took its slot.
WOOD_A = 54        # staff_wood: the TINTED wood, placed only by props and items
WOOD_B = 2         # wood
LEATHER = 53       # rawhide rim
GRIP = 58          # grip_leather: the handle
STEEL = 57
IRON = 121
MATS = {WOOD_A: "staff_wood", WOOD_B: "wood", LEATHER: "leather",
        GRIP: "grip_leather", STEEL: "steel", IRON: "iron"}

# 80 art voxels per metre, 1.25 cm a cell: the resolution of the human body and
# every item that has to sit in a fist (gen_mace_item.py SCALE).
SCALE = 8


# ---- .vox writing (gen_mace_item.py's helpers) ------------------------------
def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def dict_bytes(d):
    out = struct.pack("<i", len(d))
    for k, v in d.items():
        out += struct.pack("<i", len(k)) + k.encode()
        out += struct.pack("<i", len(v)) + v.encode()
    return out


def model_chunks(size, voxels):
    sx, sy, sz = size
    xyzi = struct.pack("<i", len(voxels)) + b"".join(
        struct.pack("4B", x, y, z, c) for (x, y, z, c) in voxels)
    return chunk(b"SIZE", struct.pack("<iii", sx, sy, sz)) + chunk(b"XYZI", xyzi)


def ntrn(node_id, name, child, t):
    attrs = {"_name": name} if name else {}
    frame = {"_t": f"{t[0]} {t[1]} {t[2]}"}
    c = struct.pack("<i", node_id) + dict_bytes(attrs)
    c += struct.pack("<iiii", child, -1, 0, 1) + dict_bytes(frame)
    return chunk(b"nTRN", c)


def ngrp(node_id, children):
    c = struct.pack("<i", node_id) + dict_bytes({})
    c += struct.pack("<i", len(children))
    for ch in children:
        c += struct.pack("<i", ch)
    return chunk(b"nGRP", c)


def nshp(node_id, model_id):
    c = struct.pack("<i", node_id) + dict_bytes({})
    c += struct.pack("<ii", 1, model_id) + dict_bytes({})
    return chunk(b"nSHP", c)


def write_vox(path, name, esize, cells):
    """`cells` is {(ex, ey, ez): material} in ENGINE item coordinates inside
    the engine box `esize`. Mapped to the scene frame here and nowhere else:
    scene x = ex, scene z = ey, scene y = (depth - 1) - ez (the flip, rebased
    so the scene box starts at 0 like the engine one)."""
    ex, ey, ez = esize
    ssize = (ex, ez, ey)            # scene (x, y, z) extents
    vox = []
    for (x, y, z), m in sorted(cells.items()):
        sx, sy, sz = x, (ez - 1) - z, y
        assert 0 <= sx < ssize[0] and 0 <= sy < ssize[1] and 0 <= sz < ssize[2]
        vox.append((sx, sy, sz, m))
    assert max(ssize) <= 120, f"{name}: a part is at most 120 micro on an axis"
    body = model_chunks(ssize, vox)
    pivot = (ssize[0] // 2, ssize[1] // 2, ssize[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2])
    graph += ntrn(2, name, 3, pivot) + nshp(3, 0)
    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(path, "wb") as f:
        f.write(data)
    return len(vox)


def hilt_scene(esize, lo, hi):
    """An ENGINE box [lo, hi) -> the sidecar's scene `min`/`size`."""
    ez = esize[2]
    return {"min": [lo[0], ez - hi[2], lo[1]],
            "size": [hi[0] - lo[0], hi[2] - lo[2], hi[1] - lo[1]]}


# ---- shapes -------------------------------------------------------------------
def disc_shield(radius, plank_x, plank_t, rim_w, rim_mat, face_mats, boss_r,
                boss_t, boss_mat, hole_r, handle_len, plank_w=None):
    """A round centre-grip shield. Engine X is depth (front = +X), the disc in
    (Y, Z). Returns (size, cells, hilt (lo, hi), centre (y, z))."""
    d = int(math.ceil(2 * radius))
    boss_front = plank_x + plank_t + int(math.ceil(boss_r))
    sx = boss_front + 1
    size = (sx, d, d)
    cy = cz = d / 2.0
    cells = {}

    def put(p, m, force=False):
        if force or p not in cells:
            cells[p] = m

    # THE BOSS FIRST (first write wins, gen_mace_item.py's convention): a hollow
    # steel dome over the hole, its open mouth on the boards' front face.
    bx0 = plank_x + plank_t   # the dome's base plane
    for x in range(bx0, sx):
        for y in range(d):
            for z in range(d):
                dx = x + 0.5 - bx0
                r = math.sqrt((y + 0.5 - cy) ** 2 + (z + 0.5 - cz) ** 2 + dx * dx)
                if boss_r - boss_t <= r <= boss_r:
                    put((x, y, z), boss_mat)
    # ...and its flange, one cell proud of the boards round the dome.
    for y in range(d):
        for z in range(d):
            r = math.sqrt((y + 0.5 - cy) ** 2 + (z + 0.5 - cz) ** 2)
            if boss_r - 0.5 <= r <= boss_r + 1.6:
                put((bx0, y, z), boss_mat)
    # THE RIM, wrapped round the edge and a cell proud front and back.
    for x in range(plank_x - 1, plank_x + plank_t + 1):
        for y in range(d):
            for z in range(d):
                r = math.sqrt((y + 0.5 - cy) ** 2 + (z + 0.5 - cz) ** 2)
                if radius - rim_w <= r <= radius:
                    put((x, y, z), rim_mat)
    # THE BOARDS, holed for the fist. Planks run vertically (along Y), each
    # `plank_w` across, alternating the face materials so they read as planks.
    for x in range(plank_x, plank_x + plank_t):
        for y in range(d):
            for z in range(d):
                r = math.sqrt((y + 0.5 - cy) ** 2 + (z + 0.5 - cz) ** 2)
                if hole_r <= r <= radius:
                    k = int(z // plank_w) if plank_w else 0
                    put((x, y, z), face_mats[k % len(face_mats)])
    # THE HANDLE: a leather-wrapped bar across the back of the hole, running
    # along Y (the fist's grip axis -- see THE FRAME), its ends seated under
    # the boards.
    hz0 = int(cz) - 1
    hy0 = int(round(cy - handle_len / 2.0))
    lo = (plank_x - 3, hy0, hz0)
    hi = (plank_x - 1, hy0 + handle_len, hz0 + 2)
    for x in range(lo[0], hi[0]):
        for y in range(lo[1], hi[1]):
            for z in range(lo[2], hi[2]):
                put((x, y, z), GRIP)
    # ...and a block under each end, so the bar stands off the boards.
    for y in (hy0, hy0 + handle_len - 1):
        for z in range(lo[2], hi[2]):
            put((plank_x - 1, y, z), GRIP)
    return size, cells, (lo, hi), (cy, cz)


def heater_shield(width, height, back_t, face_t, rim_w):
    """A heater: flat top, straight sides, curving to a point below. Engine X
    depth (front = +X): a grip and two straps, then `back_t` of boards, then
    `face_t` of iron, the outline bound in steel. Returns (size, cells, hilt)."""
    grip_x = 0
    board_x = 5          # the fist's room between the grip and the boards
    face_x = board_x + back_t
    sx = face_x + face_t + 1
    size = (sx, height, width)
    cz = width / 2.0
    cells = {}

    def put(p, m):
        if p not in cells:
            cells[p] = m

    def inside(y, z, shrink=0.0):
        u = abs(z + 0.5 - cz) / (width / 2.0 - shrink)
        v = (y + 0.5) / height
        if u > 1.0 or v > 1.0 - shrink / height or v < shrink / height:
            return False
        knee = 0.45
        if v >= knee:
            return True
        t = v / knee
        return u <= math.sin(t * math.pi / 2.0) ** 0.8

    for y in range(height):
        for z in range(width):
            if not inside(y, z):
                continue
            edge = not inside(y, z, rim_w)
            for x in range(board_x, face_x + face_t + (1 if edge else 0)):
                if edge:
                    put((x, y, z), STEEL)
                elif x >= face_x:
                    put((x, y, z), IRON)
                else:
                    put((x, y, z), WOOD_A if (z // 6) % 2 == 0 else WOOD_B)
            if edge:
                put((board_x - 1, y, z), STEEL)
    # THE GRIP: an upright bar on standoffs, centred a little above the middle
    # (where a heater's weight balances), along Y like the round shield's
    # handle, so the forearm lies level across the back when it is raised.
    gy = int(height * 0.55)
    hz0 = int(cz) - 1
    lo = (grip_x, gy - 6, hz0)
    hi = (grip_x + 2, gy + 6, hz0 + 2)
    for x in range(lo[0], hi[0]):
        for y in range(lo[1], hi[1]):
            for z in range(lo[2], hi[2]):
                put((x, y, z), GRIP)
    for y in (gy - 6, gy + 5):
        for x in range(grip_x + 2, board_x):
            for z in range(hz0, hz0 + 2):
                put((x, y, z), GRIP)
    return size, cells, (lo, hi)


# ---- the three shields --------------------------------------------------------
GRIPS = {
    # Euler X then Y then Z (anim.cpp QuatFromEulerDeg). held_left = Rx(90):
    # face +X stays +X (outward for a left fist), up (item +Y) onto the
    # fist's grip axis (hand +Z). held_right = Rz(180) Rx(90): face -X,
    # outward for the right, up onto the same grip axis.
    "held_left": {"translation": [0, 0, 0], "rotation": [90, 0, 0], "scale": 1.0},
    "held_right": {"translation": [0, 0, 0], "rotation": [90, 0, 180], "scale": 1.0},
}


def sidecar(name, model, hp, comment, hilt):
    return {
        "comment": comment + " Regenerate with scripts/gen_shield_items.py; the "
                   "hilt box is emitted from the same constants that build the "
                   "mesh, so the fist cannot drift off the grip.",
        "name": name,
        "model": model,
        "artVoxelsPerMetre": SCALE * 10,
        "hp": hp,
        # SEVERABLE, deliberately: a non-severable slot at hp 0 routes to
        # Mob::Sever -> Die() (vital/non-severable is "this is a death"). A
        # shield's hp 0 is handled by the block (Mob::BreakHeld) instead.
        "severable": True,
        # 0 = never knocked from the hand by speed: a shield is braced, and
        # what beats it is the stagger (items.json `stability`).
        "severImpactSpeed": 0,
        "grip": GRIPS,
        "hilt": hilt,
    }


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    with open(os.path.join(root, "assets", "materials", "materials.json")) as mf:
        mats = json.load(mf)["materials"]
    for idx, want in MATS.items():
        assert idx <= len(mats) and mats[idx - 1]["id"] == want, (
            f"materials.json[{idx - 1}] is not {want!r}: the palette index this "
            f"shield is authored against has moved")

    # ---- ROUND SHIELD: 0.6 m of boards, a steel boss, a rawhide rim ---------
    # 48 cells across. Boards 3 cells (3.75 cm) -- thicker than the real
    # 1 cm, because a board one cell thick is holed by the first chip
    # (CutLimb floors every chip at one lattice cell).
    size, cells, (lo, hi), _ = disc_shield(
        radius=24.0, plank_x=4, plank_t=3, rim_w=1.6, rim_mat=LEATHER,
        face_mats=[WOOD_A, WOOD_B], boss_r=6.5, boss_t=1.6, boss_mat=STEEL,
        hole_r=5.0, handle_len=12, plank_w=6)
    n = write_vox(os.path.join(out_dir, "round_shield.vox"), "round_shield", size, cells)
    sc = sidecar(
        "round_shield", "round_shield", 60,
        "A round centre-grip shield, 0.6 m across: vertical boards of wood "
        "under a hollow steel boss, bound at the rim in rawhide. What it stops "
        "is its own voxels (MobSystem::FindShieldBlock): a blade notches the "
        "boards, fire takes them, and a shield hacked through has a hole.",
        hilt_scene(size, lo, hi))
    with open(os.path.join(out_dir, "round_shield.json"), "w") as f:
        json.dump(sc, f, indent=2)
        f.write("\n")
    print(f"round_shield: {n} voxels, engine box {size}")

    # ---- BUCKLER: 0.32 m of steel with a deep boss ---------------------------
    size, cells, (lo, hi), _ = disc_shield(
        radius=13.0, plank_x=4, plank_t=2, rim_w=1.4, rim_mat=STEEL,
        face_mats=[STEEL], boss_r=5.5, boss_t=1.5, boss_mat=STEEL,
        hole_r=4.0, handle_len=8)
    n = write_vox(os.path.join(out_dir, "buckler.vox"), "buckler", size, cells)
    sc = sidecar(
        "buckler", "buckler", 45,
        "A steel buckler, 0.32 m across, gripped in the fist behind a deep "
        "boss. It covers a hand's width and no more: it is a thing you put "
        "where the blade is going, not a wall to stand behind.",
        hilt_scene(size, lo, hi))
    with open(os.path.join(out_dir, "buckler.json"), "w") as f:
        json.dump(sc, f, indent=2)
        f.write("\n")
    print(f"buckler: {n} voxels, engine box {size}")

    # ---- HEATER SHIELD: 0.48 x 0.62 m, iron over boards, steel bound ---------
    size, cells, (lo, hi) = heater_shield(width=38, height=50, back_t=2,
                                          face_t=1, rim_w=1.5)
    n = write_vox(os.path.join(out_dir, "heater_shield.vox"), "heater_shield", size, cells)
    sc = sidecar(
        "heater_shield", "heater_shield", 110,
        "A heater shield, 0.48 m by 0.62 m: wooden boards faced with a sheet "
        "of iron and bound at the edge in steel, carried on a grip and a "
        "forearm strap. The face does not burn and a blade only scores it; "
        "the boards behind it will still catch.",
        hilt_scene(size, lo, hi))
    with open(os.path.join(out_dir, "heater_shield.json"), "w") as f:
        json.dump(sc, f, indent=2)
        f.write("\n")
    print(f"heater_shield: {n} voxels, engine box {size}")


if __name__ == "__main__":
    main()
