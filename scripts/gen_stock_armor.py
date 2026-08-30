#!/usr/bin/env python3
"""Generate the STOCK ARMOUR SET — assets/items/{hood,robe,sash,boots}.{vox,json}.

WHAT A WORN PIECE IS. Not a mesh draped over the body: a LIST OF SHELLS, one
per body part it covers, each of which becomes a real rig slot on the wearer
(src/game/item.h ItemCover, src/game/mob.cpp Mob::WearItem). So this file's job
is to emit, per piece, one .vox model per covered limb plus the sidecar that
says which limb each model goes on and where its corner sits relative to that
limb's corner.

THE GEOMETRY IS DERIVED, NOT DRAWN. Every shell is the stock human's own
silhouette dilated outward by one authored micro and with the body subtracted
back off — a garment that fits by construction rather than by somebody eyeing
it against a screenshot. Three things fall out of that and all three are the
reason it is done this way:

  * The shell is STRICTLY OUTSIDE the body. Nothing of the robe is inside the
    torso, so "degraded armour shows the body underneath" needs no code: burn a
    hole in the cloth and the skin is simply visible through it.
  * It cannot go stale. The limb boxes and the shape builders are IMPORTED from
    gen_human.py, never restated here, so re-proportioning the human
    re-proportions the coat. CLAUDE.md's rule against a second source of truth,
    applied to art.
  * `offset` and `fitBox` in the sidecar are MEASURED off that same import
    rather than typed in. They are the two numbers a wrong value in would put
    the sleeve a quarter-voxel off the arm, which reads as a rendering bug.

MATERIALS ARE THE MECHANIC. There is no armour-value field anywhere and there
is not going to be one: cloth burns because it IS `robe_cloth` (tag:flammable,
tag:dissolvable) and steel would resist acid because `steel` carries no
`tag:dissolvable`. Protection is geometric occlusion plus material identity.
See docs/PLAN_items_equipment.md §2.2.

COLOUR IS ART, NOT MATERIAL. Black cloth is palette slots in a parallel
"<name>.col" layer, exactly as the human's skin tones are (gen_human.py's
one-material-many-colours note). Painting with materials instead — which is
what mina does — would mean the black hem burned on a different schedule from
the purple sleeve.

Run:  python scripts/gen_stock_armor.py
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# THE BODY THIS IS CUT FOR. Imported rather than restated — see the note above.
# gen_human's main() is guarded, so this costs nothing but the module.
import gen_human as H

# ---- materials --------------------------------------------------------------
# PALETTE CONVENTION (as everywhere): .vox palette index i+1 == materials.json[i].
# Asserted against materials.json in main() rather than trusted, because a
# renumbered material turns a robe into whatever took its slot.
CLOTH_MAT, CLOTH_ID = 48, "robe_cloth"
TRIM_MAT, TRIM_ID = 49, "robe_trim"
LEATHER_MAT, LEATHER_ID = 53, "leather"

# ---- art palette ------------------------------------------------------------
# Allocated DOWNWARD from 255 like gen_human's, and FILE-LOCAL: palette indices
# never mean the same thing in two .vox files, which is why voxels are generated
# here rather than copied from mina (memory: art palette indices are file-local).
BLACK = 255
BLACK_SHADE = 254
GREY = 253
GREY_SHADE = 252
GOLD = 251
GOLD_SHADE = 250
HIDE = 249
HIDE_SHADE = 248

ART_RGB = {
    BLACK:       0x2A2A2E,
    BLACK_SHADE: 0x1B1B1F,
    GREY:        0x4E4E56,
    GREY_SHADE:  0x3A3A41,
    GOLD:        0xB89230,
    GOLD_SHADE:  0x8C6E22,
    HIDE:        0x4A3220,
    HIDE_SHADE:  0x332215,
}

# Authored at gen_human's ART_SCALE and shipped at its SCALE, for the same
# reason the body is: the two lattices have to agree cell for cell or a garment
# cut to fit sits half a micro proud of the limb it was cut for.
ART_SCALE = H.ART_SCALE
UPSCALE = H.SKIN_UPSCALE
SCALE = H.SCALE          # 8 micro per world voxel, as shipped


# ---- the body, as occupancy -------------------------------------------------

def body_cells():
    """Every filled cell of the stock human, in SCENE coordinates at ART_SCALE.

    The shape builders already flip front-to-back (gen_human's flip_y), so this
    is the final reading of the figure: the FACE and the TOES are at LOW scene
    y, which is what makes the hood's face void a low-y window rather than a
    high-y one."""
    per_limb = {}
    occupied = set()
    for name, (size, mn) in H.ART_LIMBS.items():
        cells = set()
        for (x, y, z, _c) in H.SHAPES[name](size):
            c = (x + mn[0], y + mn[1], z + mn[2])
            cells.add(c)
        per_limb[name] = cells
        occupied |= cells
    return per_limb, occupied


def ring_xy(cells, occupied, r):
    """Cells within Chebyshev distance `r` in the xy-plane of `cells`, at the
    SAME z, minus everything the body occupies and minus `cells` itself.

    Per-z rather than in 3D on purpose: a garment is a tube around a limb, and
    a 3D dilation would also cap the top and bottom of every box — which on the
    torso means a plate across the shoulders whether or not that is wanted. The
    caps that ARE wanted are added explicitly by the piece builders below."""
    out = set()
    for (x, y, z) in cells:
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if dx == 0 and dy == 0:
                    continue
                if max(abs(dx), abs(dy)) != r:
                    continue
                n = (x + dx, y + dy, z)
                if n in occupied or n in cells:
                    continue
                out.add(n)
    return out


def tube(limb, occupied, r=1, zlo=None, zhi=None):
    """A one-micro sleeve around a limb, optionally trimmed in z."""
    src = {c for c in limb
           if (zlo is None or c[2] >= zlo) and (zhi is None or c[2] <= zhi)}
    inner = src
    shell = set()
    for k in range(1, r + 1):
        shell |= ring_xy(inner, occupied, k)
    return shell


def dilate26(cells, occupied):
    """Every empty 26-neighbour of `cells` that the body does not occupy — a
    closed skin, top and bottom included.

    The per-z `tube` cannot close a boot. A foot is capped at z1+1, the shin
    stands in the middle of that cap, and subtracting the body therefore
    removes exactly the part of the lid the shin passes through — leaving the
    instep bare between the toe cap and the ankle. Nothing about the tube's
    geometry can fix that, because the hole is on a face the tube does not
    generate. A full dilation has no faces it does not generate."""
    out = set()
    for (x, y, z) in cells:
        for dz in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0 and dz == 0:
                        continue
                    n = (x + dx, y + dy, z + dz)
                    if n in occupied or n in cells:
                        continue
                    out.add(n)
    return out


def cap(limb, occupied, z_from, z_to, grow=0):
    """A horizontal plate over a limb: the limb's silhouette at `z_from`,
    dilated by `grow`, laid at every z in `z_to` (a range), minus the body.

    This is how the shoulders get a collar and the hood gets a crown — the
    per-z tube above cannot produce either, because at the top of a box there
    is no silhouette above to ring."""
    sil = {(x, y) for (x, y, z) in limb if z == z_from}
    if grow:
        grown = set(sil)
        for (x, y) in sil:
            for dx in range(-grow, grow + 1):
                for dy in range(-grow, grow + 1):
                    grown.add((x + dx, y + dy))
        sil = grown
    out = set()
    for z in z_to:
        for (x, y) in sil:
            if (x, y, z) not in occupied:
                out.add((x, y, z))
    return out


# ---- the pieces -------------------------------------------------------------

def build_hood(per_limb, occ):
    """A cowl over the skull with the face cut out of it.

    The face void is the one place a purely derived garment cannot be derived:
    a hood that hugs the head is a helmet with no eyeholes. Cut against the EYE
    ROW gen_human derives from tuning.json rather than a literal, so the window
    stays on the face if the head is ever re-proportioned."""
    head = per_limb["head"]
    z0 = min(c[2] for c in head)
    z1 = max(c[2] for c in head)
    y0 = min(c[1] for c in head)
    shell = tube(head, occ, 1, zlo=z0 + 2)      # bare neck below the cowl
    shell |= cap(head, occ, z1, range(z1 + 1, z1 + 2))
    # The peak. Mina's hood has one and it is most of what makes a cowl read as
    # a cowl rather than as a swim cap; two tapering rows is the cheapest thing
    # that says it at this resolution.
    crown = {(x, y) for (x, y, z) in head if z == z1}
    cx = sum(x for (x, _y) in crown) / max(1, len(crown))
    cy = sum(y for (_x, y) in crown) / max(1, len(crown))
    for i, z in enumerate(range(z1 + 2, z1 + 4)):
        keep = {(x, y) for (x, y) in crown
                if abs(x + 0.5 - cx) <= 2.5 - i and abs(y + 0.5 - cy) <= 2.5 - i}
        shell |= {(x, y, z) for (x, y) in keep if (x, y, z) not in occ}
    # The face window: everything on the FRONT face (low y) between the brow and
    # the chin. `EYE_Z` is scene-absolute; the head box's own z0 is not.
    eye_z = H.EYE_Z
    shell = {(x, y, z) for (x, y, z) in shell
             if not (y <= y0 and eye_z - 4 <= z <= eye_z + 3)}
    return shell


def build_robe(per_limb, occ):
    """Torso panel, four sleeve segments, and a skirt over the hips.

    Returned per PART, because each one becomes its own shell on its own rig
    slot: the sleeve has to swing with the forearm, and a single welded robe
    model could only ever be nailed to one of them."""
    out = {}
    torso = per_limb["torso"]
    tz1 = max(c[2] for c in torso)
    out["torso"] = tube(torso, occ, 1) | cap(torso, occ, tz1, range(tz1 + 1, tz1 + 2), grow=1)
    for arm in ("armU.L", "armU.R", "armL.L", "armL.R"):
        out[arm] = tube(per_limb[arm], occ, 1)

    # THE SKIRT. A tube around the hips, then flaring outward as it falls past
    # them — which is the only shell here that is not simply the body plus one,
    # because a skirt that hugged the thighs would not be a skirt.
    hips = per_limb["hips"]
    hz0 = min(c[2] for c in hips)
    skirt = tube(hips, occ, 1)
    hem = {(x, y) for (x, y, z) in hips if z == hz0}
    for i, z in enumerate(range(hz0 - 1, hz0 - 7, -1)):
        r = 1 + i // 2
        row = set()
        for (x, y) in hem:
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) != r:
                        continue
                    row.add((x + dx, y + dy, z))
        skirt |= {c for c in row if c not in occ}
    out["hips"] = skirt
    return out


def build_sash(per_limb, occ, taken):
    """A band at the waist, sitting OUTSIDE the robe rather than inside it.

    DILATED FROM THE ROBE'S OWN SURFACE, not from the body at a bigger radius.
    The obvious version — a ring at Chebyshev distance 2 from the hips — is
    what this was first, and it produced a band that existed and could not be
    seen: distance 2 from a rounded hip is only reached at the flat faces, so
    the "belt" came out as 28 cells per row of disconnected edge rather than a
    ring, sitting one micro proud of a skirt it never covered. Dilating the
    UNION of the body and whatever the robe put there gives a complete band on
    the outermost surface, which is what a belt is.

    `taken` is every cell the robe already claimed; subtracting it is what
    keeps two shells over one limb from occupying the same lattice cells and
    z-fighting — a thing that can happen here precisely because a piece is not
    one welded model."""
    hips = per_limb["hips"]
    z1 = max(c[2] for c in hips)
    lo = z1 - 3
    base = {c for c in (hips | taken) if lo <= c[2] <= z1}
    band = ring_xy(base, occ | taken, 1)
    return {c for c in band if lo <= c[2] <= z1}


def build_boots(per_limb, occ):
    """Foot, plus a short cuff up the ankle.

    THE CUFF IS NOT DECORATION. Capping the foot at z1+1 alone leaves a hole:
    the shin stands in the middle of that cap, so subtracting the body removes
    exactly the part of the lid the shin passes through, and the foot's own
    top surface is then visible around it — bare skin between the boot and the
    trouser, which is what the first render showed. Ringing the shin for two
    micro closes it.

    The cuff cells belong to the FOOT's shell and therefore rotate with the
    ankle rather than with the shin. At two authored micro — half a world
    voxel — the flex is smaller than one collider cell, and putting them on
    the shin instead would mean the boots covered a limb their cover entry
    does not name."""
    out = {}
    shins = {"foot.L": "legL.L", "foot.R": "legL.R"}
    for foot, shin in shins.items():
        cells = per_limb[foot]
        z1 = max(c[2] for c in cells)
        # A closed skin over the whole foot, then the cuff: the dilation seals
        # the instep, the cuff seals the seam where the leg leaves the boot.
        shell = dilate26(cells, occ)
        shell |= tube(per_limb[shin], occ, 1, zlo=z1 + 1, zhi=z1 + 2)
        # Nothing below the sole. A shell under z 0 hangs through the floor,
        # and the foot's own bottom row is already standing on it.
        out[foot] = {c for c in shell if c[2] >= 0}
    return out


# ---- colour -----------------------------------------------------------------

def shade(cells, base, dark):
    """Two tones, split front/back the way every gen_human builder does: the
    away-facing half of a surface takes the darker slot. With the face at LOW y
    (see body_cells), "away" is HIGH y."""
    if not cells:
        return {}
    ys = [c[1] for c in cells]
    mid = (min(ys) + max(ys)) * 0.5
    return {c: (dark if c[1] > mid else base) for c in cells}


# ---- .vox emission ----------------------------------------------------------

def rgba_chunk(mat_rgb):
    """Palette. ENTRY i IS INDEX i+1 — the same off-by-one gen_human documents,
    and getting it wrong shifts every colour by one slot, which looks almost
    right."""
    pal = bytearray(1024)
    for idx, rgb in mat_rgb.items():
        o = (idx - 1) * 4
        pal[o:o + 4] = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))
    for slot, rgb in ART_RGB.items():
        o = (slot - 1) * 4
        pal[o:o + 4] = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))
    return H.chunk(b"RGBA", bytes(pal))


def write_piece(path, shells, mat, mat_rgb):
    """One .vox holding two models per shell: "<part>" carrying the MATERIAL in
    every cell and "<part>.col" carrying the art slot over the SAME cells, at
    the same size and the same translation. The loader matches the two by
    ABSOLUTE CELL, so identical placement is the join condition, not a tidiness
    preference (voxload.cpp's art-layer fold)."""
    body = b""
    graph = b""
    children = []
    for i, (part, info) in enumerate(shells.items()):
        cells, cols, size, mn = info
        local = [(x - mn[0], y - mn[1], z - mn[2]) for (x, y, z) in cells]
        body += H.model_chunks(size, [(x, y, z, mat) for (x, y, z) in local])
        body += H.model_chunks(
            size, [(x, y, z, cols[(x + mn[0], y + mn[1], z + mn[2])])
                   for (x, y, z) in local])
        pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
        t = (mn[0] + pivot[0], mn[1] + pivot[1], mn[2] + pivot[2])
        trn, shp, ctrn, cshp = 2 + 4 * i, 3 + 4 * i, 4 + 4 * i, 5 + 4 * i
        graph += H.ntrn(trn, part, shp, t) + H.nshp(shp, 2 * i)
        graph += H.ntrn(ctrn, part + ".col", cshp, t) + H.nshp(cshp, 2 * i + 1)
        children += [trn, ctrn]
    graph = H.ntrn(0, "", 1, (0, 0, 0)) + H.ngrp(1, children) + graph
    payload = body + rgba_chunk(mat_rgb) + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(path, "wb") as f:
        f.write(data)


def upscale_set(cells):
    """Every cell becomes a UPSCALE^3 BLOCK — the same operation gen_human's
    upscale_voxels performs on the body, and it has to be the same one here.

    Multiplying the COORDINATES alone (the obvious shortcut) leaves a sparse
    lattice of every other cell, whose extents come out 2n-1 instead of 2n and
    whose max corner is a cell short. On the two positive axes that is merely a
    wrong `fitBox`; on the NEGATED axis, where the engine minimum is derived
    from the scene MAXIMUM, it is a one-cell offset error that puts every shell
    a sixteenth of a world voxel into the limb it covers."""
    out = set()
    for (x, y, z) in cells:
        bx, by, bz = x * UPSCALE, y * UPSCALE, z * UPSCALE
        for dz in range(UPSCALE):
            for dy in range(UPSCALE):
                for dx in range(UPSCALE):
                    out.add((bx + dx, by + dy, bz + dz))
    return out


def engine_min(cells):
    """The min corner of a scene-space cell set in ENGINE axes.

    Scene(Z-up) -> engine(Y-up) is (x, z, -y). THE NEGATED AXIS IS THE TRAP:
    "min maps to min" is false there, because cells y0..y1 map to -y1..-y0. Get
    this backwards and every shell is a full model-depth out along one axis —
    the same mistake that once put a sword's hilt on the far side of its own
    blade (see PrefabModel::sceneMin's note in sim/voxload.h)."""
    return (min(c[0] for c in cells),
            min(c[2] for c in cells),
            -max(c[1] for c in cells))


def engine_size(cells):
    return (max(c[0] for c in cells) - min(c[0] for c in cells) + 1,
            max(c[2] for c in cells) - min(c[2] for c in cells) + 1,
            max(c[1] for c in cells) - min(c[1] for c in cells) + 1)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(root, "assets", "materials", "materials.json")) as mf:
        mats = json.load(mf)["materials"]
    for idx, want in ((CLOTH_MAT, CLOTH_ID), (TRIM_MAT, TRIM_ID),
                      (LEATHER_MAT, LEATHER_ID)):
        assert mats[idx - 1]["id"] == want, (
            f"materials.json[{idx - 1}] is {mats[idx - 1]['id']!r}, not "
            f"{want!r} — the palette index this set is authored against has "
            f"moved, and every piece would load as another material")
    mat_rgb = {i + 1: int(m["colors"][0].lstrip("#"), 16)
               for i, m in enumerate(mats) if m.get("colors")}

    per_limb, occ = body_cells()

    robe = build_robe(per_limb, occ)
    taken = set()
    for s in robe.values():
        taken |= s
    pieces = [
        ("hood", "armor_head", CLOTH_MAT, (BLACK, BLACK_SHADE), 14.0,
         {"head": build_hood(per_limb, occ)}),
        ("robe", "armor_chest", CLOTH_MAT, (BLACK, BLACK_SHADE), 16.0, robe),
        ("sash", "armor_belt", TRIM_MAT, (GOLD, GOLD_SHADE), 8.0,
         {"hips": build_sash(per_limb, occ, taken)}),
        ("boots", "armor_boots", LEATHER_MAT, (HIDE, HIDE_SHADE), 12.0,
         build_boots(per_limb, occ)),
    ]

    rows = []
    for name, kind, mat, (base, dark), hp, shells in pieces:
        models = {}
        cover = []
        for part, cells in shells.items():
            assert cells, f"{name}: shell over {part} came out empty"
            # A SHELL MAY NOT TOUCH THE BODY IT COVERS. Checked rather than
            # trusted: everything above subtracts `occ`, so a violation means a
            # builder reached for a cell set that was not the whole body — and
            # the symptom would be armour rendering inside flesh, which reads
            # as a z-fighting bug rather than as the geometry error it is.
            assert not (cells & occ), f"{name}/{part} intersects the body"

            up = set()
            cols = {}
            painted = shade(cells, base, dark)
            for (x, y, z) in cells:
                c = painted[(x, y, z)]
                for dz in range(UPSCALE):
                    for dy in range(UPSCALE):
                        for dx in range(UPSCALE):
                            q = (x * UPSCALE + dx, y * UPSCALE + dy,
                                 z * UPSCALE + dz)
                            up.add(q)
                            cols[q] = c
            mn = (min(c[0] for c in up), min(c[1] for c in up),
                  min(c[2] for c in up))
            size = (max(c[0] for c in up) - mn[0] + 1,
                    max(c[1] for c in up) - mn[1] + 1,
                    max(c[2] for c in up) - mn[2] + 1)
            models[part] = (sorted(up), cols, size, mn)

            # ---- the two measured numbers -----------------------------------
            # `offset` is the shell's engine min corner MINUS the limb's, which
            # is exactly what Mob::AppendWornShell adds to the covered limb's
            # own restOffset. Measured off the limb's FILLED CELLS, not its
            # declared box: the loader rebases every model onto its own voxels
            # (voxload.cpp `em.cells[i] - em.mn`), so the box's empty margin is
            # not part of the frame either side lives in.
            limb_up = upscale_set(per_limb[part])
            sm, lm = engine_min(up), engine_min(limb_up)
            cover.append({
                "part": part,
                "model": part,
                "offset": [sm[0] - lm[0], sm[1] - lm[1], sm[2] - lm[2]],
                # The box this shell was CUT FOR, so the fit resample can tell
                # how much bigger the wearer is than the mannequin.
                "fitBox": list(engine_size(limb_up)),
                "hp": hp,
            })

        write_piece(os.path.join(out_dir, name + ".vox"), models, mat, mat_rgb)
        sidecar = {
            "comment": (
                f"The {name}, a WORN piece: one shell per covered limb, each of "
                f"which becomes a real rig slot on the wearer and therefore "
                f"burns, dissolves, carves, severs and drops exactly as a limb "
                f"does. Geometry is the stock human's own silhouette dilated "
                f"outward by one authored micro, so it fits by construction. "
                f"Regenerate with scripts/gen_stock_armor.py — `offset` and "
                f"`fitBox` are MEASURED off gen_human.py's limb table, so "
                f"editing them by hand puts the garment off the body."),
            "name": name,
            "scale": SCALE,
            "hp": hp * len(cover),
            "severable": True,
            "cover": sorted(cover, key=lambda c: c["part"]),
        }
        with open(os.path.join(out_dir, name + ".json"), "w") as f:
            json.dump(sidecar, f, indent=2)
            f.write("\n")

        vox = sum(len(m[0]) for m in models.values())
        rows.append({"id": name, "kind": kind,
                     "desc": ITEM_DESC[name]})
        print(f"  {name:6s} {len(cover)} shells, {vox} voxels, "
              f"{', '.join(c['part'] for c in sidecar['cover'])}")

    # ---- items.json ---------------------------------------------------------
    # Merged, not overwritten: the sword's row is hand-authored content that
    # this generator has no business owning. Rows it DOES own are replaced in
    # place so a re-run is idempotent.
    items_path = os.path.join(out_dir, "items.json")
    with open(items_path) as f:
        doc = json.load(f)
    mine = {r["id"] for r in rows}
    doc["items"] = [r for r in doc["items"] if r["id"] not in mine] + rows
    with open(items_path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"wrote {len(rows)} pieces to assets/items/ and merged their rows "
          f"into items.json ({len(doc['items'])} items total)")


ITEM_DESC = {
    "hood": "A deep cloth cowl. It keeps the rain off and catches fire "
            "readily; what it will not do is stop a blade.",
    "robe": "A heavy black robe, sleeves to the wrist and a skirt to the "
            "knee. Cloth over skin: fire reaches the cloth first.",
    "sash": "A gold-shot band worn at the waist, over the robe.",
    "boots": "Cut leather, slow to catch. Better against a spill than "
             "against a fire.",
}

if __name__ == "__main__":
    main()
