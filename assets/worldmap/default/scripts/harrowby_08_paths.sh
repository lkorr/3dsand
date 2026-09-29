#!/bin/bash
# HARROWBY 08 -- the paths: packed gravel on the green, dirt lanes behind.
#
# Painted into the map's ground layer (assets/worldedits/default_ground.svedit,
# named by map.json `editLayer`) with scripts/paint_ground.mjs. Each stroke is
# a polyline of world x,z columns; the layer is ground-relative, so a path lies
# on the grass wherever the ground is. A later stroke wins where two cross.
# Coordinates: the doors' outside waynodes (refs/harrowby.json and the houses'
# `..._out` slots) and the well's flagstone apron (x 544..566, z 3448..3470).
#
# Run from the repo root. Re-running is harmless (it repaints the same cells);
# to take a path out, run its line again with --erase.
set -e
P="node scripts/paint_ground.mjs default"

# the green: each door to the well's apron
$P --mat gravel --width 11 --ragged 2 --path 555,3379 555,3420 553,3447    # longhouse front door, south to the well
$P --mat gravel --width 11 --ragged 2 --path 455,3456 500,3458 543,3459    # the alehouse door, east to the well
$P --mat gravel --width 11 --ragged 2 --path 567,3459 610,3458 657,3457    # the well, east to the smithy door
$P --mat gravel --width 11 --ragged 2 --path 557,3471 560,3494             # the well, south onto the open green
# the smithy yard: hard standing before the open forge bay
$P --mat gravel --width 26 --ragged 3 --path 646,3424 646,3446             # stops short of the house: its floor is the house's
# the lanes: dirt round the byre end to the back door, and out to the field gate
$P --mat dirt --width 12 --ragged 3 --path 515,3456 470,3400 482,3342      # the green to the byre door
$P --mat dirt --width 12 --ragged 3 --path 470,3400 470,3292 535,3301      # ...on round to the back door
$P --mat dirt --width 14 --ragged 3 --path 535,3301 545,3240 560,3212      # the back door to the field gate
