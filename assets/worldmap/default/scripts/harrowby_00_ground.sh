#!/bin/bash
# HARROWBY 00 -- the ground, before any house goes on it.
#
# Nothing to RUN any more: the ground is two things written in map.json and
# the refs, both edited by hand on the pages (docs/EDITOR_GUIDE.md section 14).
#
# 1. SOFTENED GROUND. map.json has a site `harrowby_ground`, kind "soften"
#    (Environment -> World map, the Soften tool: drag a box; its corners,
#    feather and scales are in the Sites panel): over x 330..790, z 3090..3650 (the spawn included)
#    the forest floor keeps 25 % of its rolling swells (hills), none of its
#    3 m bumps or sub-metre grain, and the waynode routes between the doors
#    keep only 10 % (paths) -- easing back to the untouched forest over 12 m
#    (feather 120). The home area is centred on y 200, so the village rolls
#    gently round 200 and every house floor is 201.
#
# 2. NO CLEARING. The forest stands among the houses: worldgen leaves out
#    every tree whose crown would come within 1 m of a building (the per-tree
#    building rule), keeps trunks off each house's pad and ramp, and off the
#    villagers' walking lines (every waynode link, 1.5 m either side). The
#    `clearing` kind is still there for an open field or a glade.
#
# 3. EACH HOUSE levels its own floor. A structure ref's props: `padApron`
#    (the ring round the walls kept dead level with the floor: the doorsteps)
#    and `padMargin` (the smooth S-curve from there back into the softened
#    ground; it widens itself if the ground is further from the floor than it
#    can ease without a 2-voxel step). Both are sliders in the in-game
#    inspector (F8 / References -> the house -> "ground round the house").
#
# Until 2026-09-29 this script ran `scripts/sculpt_flatten.mjs` over a clearing
# box, which planed the village flat and left a plane of one-voxel pits with
# dirt on every riser; the map's sculpt layer (sculpt.svsculpt) was deleted
# with it. The sculpt brush and sculpt_flatten.mjs are still there for
# shaping ground by hand.
echo "harrowby_00: the ground is map.json harrowby_ground (kind soften) + the houses' pads; nothing to run"
