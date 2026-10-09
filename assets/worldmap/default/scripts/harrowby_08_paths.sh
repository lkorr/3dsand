#!/bin/bash
# HARROWBY 08 -- the paths: packed gravel on the green, packed-earth lanes behind.
#
# SINCE MAP V2 R1 (2026-10-08) THE PATHS ARE ROADS, not painted ground: the
# eight strokes this script used to paint into the edit layer with
# scripts/paint_ground.mjs are `roads[]` entries in ../map.json (ids
# harrowby_*; kind "path"), so the engine GRADES them (a baked profile, a cut
# and a fill, a shoulder easing back into the ground), keeps trunks, cover and
# tarns off them, and draws them in the far field. Edit them there (or on the
# World map page's vector layer, E3).
#
# Two things changed with the move, on purpose:
#  * the surface is a SOLID look-alike -- packed_gravel for the green's
#    gravel, packed_earth for the dirt lanes -- because a road is graded and a
#    powder (gravel, dirt: repose 40) on a grade never sleeps (CLAUDE.md rule
#    2); the loader refuses a powder road surface;
#  * the edge is the engine's ragged edge (half-width +-1 by a counter hash of
#    the column's 2x2 block), not paint_ground's --ragged 2-3 band.
#
# Running this script ERASES the old painted paths from the map's ground
# layer (assets/worldedits/default_ground.svedit) so they do not double up
# with the roads: the same strokes, same width and ragged band (so the same
# columns), `--erase --only <mat>` so only a column that is wholly that path
# material goes -- the well's flagstone apron, the cellar and the yard under
# them stay. Re-running is harmless (nothing left to erase). Run from the repo
# root; --layer <name> as an extra argument targets another layer (the locale
# layer locales/harrowby/ground.svedit after L1, which keeps world columns).
set -e
P="node scripts/paint_ground.mjs default --erase $*"

# the green: each door to the well's apron
$P --only gravel --width 11 --ragged 2 --path 555,3379 555,3420 553,3447
$P --only gravel --width 11 --ragged 2 --path 455,3456 500,3458 543,3459
$P --only gravel --width 11 --ragged 2 --path 567,3459 610,3458 657,3457
$P --only gravel --width 11 --ragged 2 --path 557,3471 560,3494
# the smithy yard
$P --only gravel --width 26 --ragged 3 --path 646,3424 646,3446
# the lanes
$P --only dirt --width 12 --ragged 3 --path 515,3456 470,3400 482,3342
$P --only dirt --width 12 --ragged 3 --path 470,3400 470,3292 535,3301
$P --only dirt --width 14 --ragged 3 --path 535,3301 545,3240 560,3212
