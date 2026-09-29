#!/bin/bash
# HARROWBY 00 -- the ground, before any house goes on it.
#
# 1. THE CLEARING. map.json has a site `harrowby_clearing`, kind "pad"
#    (Environment -> World map, the pad tool: Shift+drag a box): no tree,
#    bush or ground cover grows in the box x 345..780, z 3080..3560, so the
#    green, the lanes and the field stay open and the forest stands round it.
#
# 2. LEVEL IT. The home area rolls about 2 m from the low west (the alehouse
#    side) to the high east (the smithy side). Bring the whole clearing to a
#    ground top of y 200 (house floors at 201), fading back into the
#    untouched forest over 9.6 m. This writes the map's SCULPT layer
#    (assets/worldmap/default/sculpt.svsculpt) -- the same file the World map
#    page's sculpt brush edits, so keep brushing there if you like.
#    What is left is the ground's fine grain (a hand's breadth up and down).
#
# Run from the repo root, after a build (it asks the engine for heights).
set -e
node scripts/sculpt_flatten.mjs default --rect 360,3095,765,3545 --y 200 --feather 96 --strength 1 --passes 4
