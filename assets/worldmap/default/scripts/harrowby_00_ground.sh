#!/bin/bash
# HARROWBY 00 -- the ground, before any house goes on it.
#
# 1. THE CLEARING. map.json has a site `harrowby_clearing`, kind "clearing"
#    (Environment -> World map, the Clearing tool: drag a box; its corners and
#    feather are in the Sites panel; docs/EDITOR_GUIDE.md section 14.1): no
#    forest tree's CROWN reaches over the box x 375..741, z 3131..3518 (the
#    five structures plus 2 voxels), and the trees thin out over the 64 voxels
#    past their crowns, so the wood stands round the hamlet with a ragged edge.
#    Grass, flowers and the ground's shape are untouched. (It was a "pad" box
#    until 2026-09-29 -- the selftest's fixture ground, which bares the grass
#    too and calms the hills: a bald, jittered square. Do not use a pad for a
#    clearing.) The spawn is in the wood 10 m south of it, at (580, 3620).
#
# 2. LEVEL IT. The home area rolls about 2 m from the low west (the alehouse
#    side) to the high east (the smithy side). Bring the clearing's ground to
#    an AVERAGE top of y 200 (house floors at 201), fading back into the
#    untouched forest over 6.4 m. --keep-grain 24 levels the ground's average
#    over +-24 voxels rather than every 8-voxel sample: an exact flatten left
#    a plane of one- and two-voxel steps with dirt on every riser (it read as
#    churned ground from the spawn), and with it the clearing keeps the same
#    fine texture as the forest floor round it. Each house still levels its
#    own square to its floor. This writes the map's SCULPT layer
#    (assets/worldmap/default/sculpt.svsculpt) -- the same file the World map
#    page's sculpt brush edits, so keep brushing there if you like. Delete the
#    file first to redo it from scratch (the passes correct what is there).
#
# Run from the repo root, after a build (it asks the engine for heights).
set -e
node scripts/sculpt_flatten.mjs default --rect 375,3131,741,3518 --y 200 --feather 64 --strength 1 --passes 4 --keep-grain 24
