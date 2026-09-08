#!/usr/bin/env bash
# Validate every WGSL shader the way the engine actually compiles it.
#
# LoadShader() (src/gpu/resources.cpp) prepends common.wgsl to each shader before
# handing it to Dawn, so common.wgsl is NOT a standalone module and the others do
# not compile alone. We reproduce that concatenation, run tint --validate over the
# result, and remap reported line numbers back to the real file.
#
# Usage: bash scripts/check_shaders.sh [file.wgsl ...]
#   With no arguments, validates all shaders in assets/shaders.
#   Exits non-zero if any shader fails.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHADER_DIR="$ROOT/assets/shaders"
COMMON="$SHADER_DIR/common.wgsl"
WORLD_H="$ROOT/src/sim/world.h"

# Locate the tint CLI. Built by the `tint_cmd_tint_cmd` target once
# TINT_BUILD_CMD_TOOLS is ON; may land in a few places depending on generator.
find_tint() {
  if [ -n "${TINT:-}" ] && [ -x "$TINT" ]; then echo "$TINT"; return 0; fi
  local c
  for c in \
    "$ROOT/build/_deps/dawn-build/Release/tint.exe" \
    "$ROOT/build/_deps/dawn-build/src/tint/Release/tint.exe" \
    "$ROOT/build/_deps/dawn-build/src/tint/cmd/tint/Release/tint.exe" \
    "$ROOT/build/Release/tint.exe" \
    "${SANDVOX_DEPS:-C:/sv-deps}/ninja/dawn-build/Release/tint.exe" \
    "${SANDVOX_DEPS:-C:/sv-deps}/dawn-build/Release/tint.exe"; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done
  c="$(find "$ROOT/build" -name 'tint.exe' -type f 2>/dev/null | head -1)"
  [ -n "$c" ] && { echo "$c"; return 0; }
  return 1
}

TINT_BIN="$(find_tint)" || {
  cat >&2 <<'EOF'
check_shaders: tint CLI not found.

Build it once (TINT_BUILD_CMD_TOOLS is ON in CMakeLists.txt):
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release --target tint_cmd_tint_cmd

Or point TINT=/path/to/tint.exe at an existing binary.
EOF
  exit 127
}

[ -f "$COMMON" ] || { echo "check_shaders: missing $COMMON" >&2; exit 1; }
[ -f "$WORLD_H" ] || { echo "check_shaders: missing $WORLD_H" >&2; exit 1; }

# Reproduce ShaderConstantPrelude() (src/gpu/resources.cpp): the world constants
# are generated from world.h, not declared in common.wgsl. Values are scraped
# from world.h rather than duplicated here, so this script cannot drift from the
# engine the way common.wgsl used to drift from world.h.
cpp_const() {  # cpp_const <name> -> literal, minus any type suffix
  sed -n "s/.*constexpr[a-z0-9_ ]* $1 = \([0-9.]*\)f\?;.*/\1/p" "$WORLD_H" | head -1
}
cpp_const_hex() {  # cpp_const_hex <name> -> hex literal, minus the u suffix
  sed -n "s/.*constexpr[a-z0-9_ ]* $1 = \(0x[0-9A-Fa-f]*\)u\?;.*/\1/p" "$WORLD_H" | head -1
}
W_N="$(cpp_const kWorldN)"
W_CHUNK="$(cpp_const kChunk)"
W_VOX="$(cpp_const kVoxelMeters)"
W_IFAIR="$(cpp_const_hex kCellOpIfAir)"
W_FAR="$(cpp_const kFarLevels)"
if [ -z "$W_N" ] || [ -z "$W_CHUNK" ] || [ -z "$W_VOX" ] || [ -z "$W_IFAIR" ] \
   || [ -z "$W_FAR" ]; then
  echo "check_shaders: cannot parse kWorldN/kChunk/kVoxelMeters/kCellOpIfAir/kFarLevels from $WORLD_H" >&2
  exit 1
fi
W_NCHUNK=$((W_N / W_CHUNK))

# Sub-chunk occupancy bitmask (world.h kSubOccShift block). kSubOccDim and
# kSubOccStride are expressions there, so scrape the two literals and redo the
# arithmetic the same way world.h does.
W_SUBSHIFT="$(cpp_const kSubOccShift)"
W_SUBWORDS="$(cpp_const kSubOccWords)"
if [ -z "$W_SUBSHIFT" ] || [ -z "$W_SUBWORDS" ]; then
  echo "check_shaders: cannot parse kSubOccShift/kSubOccWords from $WORLD_H" >&2
  exit 1
fi
W_SUBDIM=$((W_CHUNK >> W_SUBSHIFT))
W_SUBSTRIDE=$((W_SUBWORDS * 2))

# Openness grid (world.h kOpenFaces block): its own buffer, so what the
# shaders need is the face count and the per-chunk WORD stride. Both derive
# from kSubOccDim in world.h, so redo that arithmetic here rather than parse
# the expression -- the same treatment kSubOccStride gets above.
W_OPENFACES=6
W_OPENWORDS=$(( (W_SUBDIM * W_SUBDIM * W_SUBDIM * W_OPENFACES) / 4 ))

# Shadow cache (world.h kShadowCacheBuckets block). Same treatment as the
# sub-occupancy pair above: the two SIZES are shifts in world.h precisely so
# they can be scraped as literals here, with the shift redone rather than the
# expression parsed.
W_SHADOWSHIFT="$(cpp_const kShadowCacheShift)"
W_SHADOWREQSHIFT="$(cpp_const kShadowReqCapShift)"
W_SHADOWREQHDR="$(cpp_const kShadowReqHeaderWords)"
W_SHADOWREQW="$(cpp_const kShadowReqWords)"
W_SHADOWSUBMAX="$(cpp_const kShadowSubdivMax)"
W_RSSLOTS="$(cpp_const kRenderStatSlots)"
W_RSSTRIPES="$(cpp_const kRenderStatStripes)"
if [ -z "$W_SHADOWSHIFT" ] || [ -z "$W_SHADOWREQSHIFT" ] ||
   [ -z "$W_SHADOWREQHDR" ] || [ -z "$W_SHADOWREQW" ] ||
   [ -z "$W_SHADOWSUBMAX" ]; then
  echo "check_shaders: cannot parse the kShadow* constants from $WORLD_H" >&2
  exit 1
fi
W_SHADOWBUCKETS=$((1 << W_SHADOWSHIFT))
W_SHADOWREQCAP=$((1 << W_SHADOWREQSHIFT))
W_WORLDSHIFT=0
while [ $((1 << W_WORLDSHIFT)) -lt "$W_N" ]; do W_WORLDSHIFT=$((W_WORLDSHIFT + 1)); done

# Voxels per metre (world.h kVoxelsPerMetre -> prelude VOXELS_PER_M). Integer,
# and DERIVED from kVoxelMeters on both sides -- worldgen's every metre-authored
# size converts through it, and the hand-written duplicate it replaced had
# drifted to 16 against a world running at 10.
W_VPM="$(awk -v v="$W_VOX" 'BEGIN{ printf "%d", (1.0/v) + 0.5 }')"
[ -n "$W_VPM" ] && [ "$W_VPM" != "0" ] || {
  echo "check_shaders: cannot derive kVoxelsPerMetre from kVoxelMeters" >&2
  exit 1; }

# Fluid-lab flat-slab height (world.h kLabSlabY -> prelude LAB_SLAB_Y).
W_LABY="$(cpp_const kLabSlabY)"
[ -n "$W_LABY" ] || {
  echo "check_shaders: cannot parse kLabSlabY from $WORLD_H" >&2; exit 1; }

# Software page table (docs/PLAN_page_table.md §2.2). Same rule as every other
# world constant: world.h is the source, this script scrapes it. PT_EMPTY is
# PT_SENTINEL_BIT | kMatAir and kMatAir is 0, so it needs no separate scrape —
# EMPTY being UNIFORM(air) is the design, not a coincidence.
W_PTSENT="$(cpp_const_hex kPtSentinelBit)"
W_PTJIT="$(cpp_const_hex kPtJitterBit)"
W_PTMAT="$(cpp_const_hex kPtMatMask)"
W_PTPAGE="$(cpp_const_hex kPtPageMask)"
W_PTUNRES="$(cpp_const_hex kPtUnresident)"
W_PTNOWORD="$(cpp_const_hex kPtNoWord)"
if [ -z "$W_PTSENT" ] || [ -z "$W_PTMAT" ] || [ -z "$W_PTPAGE" ] \
   || [ -z "$W_PTUNRES" ] || [ -z "$W_PTNOWORD" ] || [ -z "$W_PTJIT" ]; then
  echo "check_shaders: cannot parse kPt* page-table constants from $WORLD_H" >&2
  exit 1
fi

# Stain palette base — kStainPaletteBase is defined as an expression in world.h
# (kMaterialSlots - 8), so scrape the slot count and redo the arithmetic here
# rather than trying to parse the expression.
W_MATSLOTS="$(cpp_const kMaterialSlots)"
[ -n "$W_MATSLOTS" ] || {
  echo "check_shaders: cannot parse kMaterialSlots from $WORLD_H" >&2; exit 1; }
W_STAINBASE=$((W_MATSLOTS - 8))

# Art palette — same shape as the stain palette: a run of reserved material
# slots holding per-voxel mob SKIN colours (world.h kArtPaletteBaseGpu).
#
# There is no ART_SLOT_MIN any more. Shaders index this run with a 1-BASED
# MERGED art index, and the .vox palette slot is converted to one on the CPU at
# load (MicroBodyMergeArt), so voxload.h's kArtPaletteBase no longer reaches
# WGSL and is deliberately not scraped here.
W_ARTSLOTS="$(cpp_const kArtPaletteSlotsGpu)"
[ -n "$W_ARTSLOTS" ] || {
  echo "check_shaders: cannot parse kArtPaletteSlotsGpu from $WORLD_H" >&2; exit 1; }
W_ARTBASE=$((W_STAINBASE - W_ARTSLOTS))

# Tint palette — the third reserved run (world.h kTintPaletteBaseGpu), holding
# per-material GRID colours indexed by a MATF_TINTED material's state nibble.
W_TINTSLOTS="$(cpp_const kTintPaletteSlotsGpu)"
[ -n "$W_TINTSLOTS" ] || {
  echo "check_shaders: cannot parse kTintPaletteSlotsGpu from $WORLD_H" >&2; exit 1; }
W_TINTBASE=$((W_ARTBASE - W_TINTSLOTS))

# Static micro-detail brick pool (render-only). kMicroPoolWordsWorld is written
# as a shift expression in world.h, so scrape the shift and redo the arithmetic
# rather than trying to parse `1u << 20`.
W_MICROSHIFT="$(sed -n 's/.*constexpr[a-z0-9_ ]* kMicroPoolWordsWorld = 1u << \([0-9]*\);.*/\1/p' "$WORLD_H" | head -1)"
[ -n "$W_MICROSHIFT" ] || {
  echo "check_shaders: cannot parse kMicroPoolWordsWorld from $WORLD_H" >&2; exit 1; }
W_MICROPOOL=$((1 << W_MICROSHIFT))

# Dynamic microvoxel body pool (same shift-expression problem as above).
W_MBSHIFT="$(sed -n 's/.*constexpr[a-z0-9_ ]* kMicroBodyPoolWordsWorld = 1u << \([0-9]*\);.*/\1/p' "$WORLD_H" | head -1)"
[ -n "$W_MBSHIFT" ] || {
  echo "check_shaders: cannot parse kMicroBodyPoolWordsWorld from $WORLD_H" >&2; exit 1; }
W_MBPOOL=$((1 << W_MBSHIFT))

# Water bodies (docs/PLAN_water_master.md M2). Plain literals in world.h, so a
# straight scrape — same rule as everything else here: world.h is the source.
W_WBCAP="$(cpp_const kWaterBodyCap)"
W_WBWORDS="$(cpp_const kWaterBodyWords)"
W_WBSTATE="$(cpp_const kWaterBodyStateWords)"
# Present in ShaderConstantPrelude() but missing here, so every check_shaders.sh
# run reported sim_waterbody.wgsl as broken while the real build compiled it
# fine -- exactly the "add it to BOTH" gap CLAUDE.md names.
W_WDRAINOPS="$(cpp_const kWaterDrainOpsPerBody)"
W_WCHUNKCAP="$(cpp_const kWaterChunkCap)"
# The sweep/curve block (world.h kWaterCurveBase). These landed in
# ShaderConstantPrelude() without landing here, which left common.wgsl unable to
# resolve WATER_SWEEP_HEADER and failed ALL 17 shaders — the checker was dead for
# every caller, not just water. CLAUDE.md's rule: a new generated constant goes
# in BOTH places in the same commit.
W_WSPLITGRID="$(cpp_const kWaterSplitGrid)"
W_WSPLITCELLS="$(cpp_const kWaterSplitCells)"
W_WSPLITWORDS="$(cpp_const kWaterSplitWords)"
W_WCURVEMAXY="$(cpp_const kWaterCurveMaxY)"
W_WSWEEPHDR="$(cpp_const kWaterSweepHeaderWords)"
W_WCURVEWORDS="$(cpp_const kWaterCurveWords)"
W_WSCRATCHWORDS="$(cpp_const kWaterSweepScratchWords)"
# Derived in world.h from the caps above, so derive them the same way rather
# than scraping a comment.
W_WCURVEBASE=$((W_WBCAP * W_WBSTATE))
W_WSCRATCHBASE=$((W_WCURVEBASE + W_WBCAP * W_WCURVEWORDS))
if [ -z "$W_WBCAP" ] || [ -z "$W_WBWORDS" ] || [ -z "$W_WBSTATE" ] \
   || [ -z "$W_WCHUNKCAP" ]; then
  echo "check_shaders: cannot parse kWaterBody*/kWaterChunkCap from $WORLD_H" >&2
  exit 1
fi

# far-field grid (decoupled from the window — see world.h kFarN/kFarShiftBase)
W_FARN="$(cpp_const kFarN)"
[ -n "$W_FARN" ] || { echo "check_shaders: cannot parse kFarN from $WORLD_H" >&2; exit 1; }
W_FARNCHUNK=$((W_FARN / W_CHUNK))
W_FARSHIFT=0
while [ $((W_FARN << W_FARSHIFT)) -lt "$W_N" ]; do W_FARSHIFT=$((W_FARSHIFT + 1)); done

# Fill-queue packing + cascade geometry (world.h kFarSlotShift/kFarSlotMask,
# kWindowHalfExtentMeters, kFarCellVox(1)) — derived exactly as world.h derives
# them, since the constexpr lambdas cannot be scraped as literals.
# Far-field patch buffer base (world.h kFarPatchBase = kFarListCap * 2).
W_FARLISTCAP="$(cpp_const kFarListCap)"
[ -n "$W_FARLISTCAP" ] || {
  echo "check_shaders: cannot parse kFarListCap from $WORLD_H" >&2; exit 1; }
W_FARPATCHBASE=$((W_FARLISTCAP * 2))

W_FARNUM=$((W_FARNCHUNK * W_FARNCHUNK * W_FARNCHUNK))
W_FARSLOTSHIFT=0
while [ $((1 << W_FARSLOTSHIFT)) -lt "$W_FARNUM" ]; do W_FARSLOTSHIFT=$((W_FARSLOTSHIFT + 1)); done
W_FARSLOTMASK=$(( (1 << W_FARSLOTSHIFT) - 1 ))
W_WINHALF="$(awk "BEGIN{print ($W_N / 2) * $W_VOX}")"
W_FARCELL1=$((1 << (1 + W_FARSHIFT)))

W_SHIFT=0
while [ $((1 << W_SHIFT)) -lt "$W_CHUNK" ]; do W_SHIFT=$((W_SHIFT + 1)); done
PRELUDE_TEXT="$(printf '%s\n' \
  "const WORLD_N : u32 = ${W_N}u;" \
  "const CHUNK : u32 = ${W_CHUNK}u;" \
  "const NCHUNK : u32 = ${W_NCHUNK}u;" \
  "const NUM_CHUNKS : u32 = $((W_NCHUNK * W_NCHUNK * W_NCHUNK))u;" \
  "const CHUNK_VOL : u32 = $((W_CHUNK * W_CHUNK * W_CHUNK))u;" \
  "const CHUNK_SHIFT : u32 = ${W_SHIFT}u;" \
  "const CHUNK_MASK : i32 = $((W_CHUNK - 1));" \
  "const WORLD_MASK : i32 = $((W_N - 1));" \
  "const NCHUNK_MASK : i32 = $((W_NCHUNK - 1));" \
  "const CELLOP_IF_AIR : u32 = ${W_IFAIR}u;" \
  "const SUBOCC_SHIFT : u32 = ${W_SUBSHIFT}u;" \
  "const SUBOCC_DIM : u32 = ${W_SUBDIM}u;" \
  "const SUBOCC_WORDS : u32 = ${W_SUBWORDS}u;" \
  "const SUBOCC_STRIDE : u32 = ${W_SUBSTRIDE}u;" \
  "const SUBOCC_BASE : u32 = $((W_NCHUNK * W_NCHUNK * W_NCHUNK))u;" \
  "const OPEN_FACES : u32 = ${W_OPENFACES}u;" \
  "const OPEN_WORDS_PER_CHUNK : u32 = ${W_OPENWORDS}u;" \
  "const WORLD_SHIFT : u32 = ${W_WORLDSHIFT}u;" \
  "const SHADOW_CACHE_AVAILABLE : bool = true;" \
  "const SHADOW_CACHE_BUCKETS : u32 = ${W_SHADOWBUCKETS}u;" \
  "const SHADOW_REQ_HEADER : u32 = ${W_SHADOWREQHDR}u;" \
  "const SHADOW_REQ_WORDS : u32 = ${W_SHADOWREQW}u;" \
  "const SHADOW_REQ_CAP : u32 = ${W_SHADOWREQCAP}u;" \
  "const SHADOW_SUBDIV_MAX : u32 = ${W_SHADOWSUBMAX}u;" \
  "const RENDER_STATS : bool = true;" \
  "const RENDER_STATS_SLOTS : u32 = ${W_RSSLOTS}u;" \
  "const RENDER_STATS_STRIPES : u32 = ${W_RSSTRIPES}u;" \
  "const LAB_SLAB_Y : i32 = ${W_LABY};" \
  "const PT_SENTINEL_BIT : u32 = ${W_PTSENT}u;" \
  "const PT_JITTER_BIT : u32 = ${W_PTJIT}u;" \
  "const PT_MAT_MASK : u32 = ${W_PTMAT}u;" \
  "const PT_EMPTY : u32 = ${W_PTSENT}u;" \
  "const PT_PAGE_MASK : u32 = ${W_PTPAGE}u;" \
  "const PT_UNRESIDENT : u32 = ${W_PTUNRES}u;" \
  "const PT_NO_WORD : u32 = ${W_PTNOWORD}u;" \
  "const STAIN_PALETTE_BASE : u32 = ${W_STAINBASE}u;" \
  "const ART_PALETTE_BASE : u32 = ${W_ARTBASE}u;" \
  "const TINT_PALETTE_BASE : u32 = ${W_TINTBASE}u;" \
  "const MICRO_POOL_WORDS : u32 = ${W_MICROPOOL}u;" \
  "const MICRO_BODY_POOL_WORDS : u32 = ${W_MBPOOL}u;" \
  "const MATERIAL_SLOTS : u32 = ${W_MATSLOTS}u;" \
  "const WATERBODY_CAP : u32 = ${W_WBCAP}u;" \
  "const WATERBODY_WORDS : u32 = ${W_WBWORDS}u;" \
  "const WATERBODY_STATE_WORDS : u32 = ${W_WBSTATE}u;" \
  "const WATER_DRAIN_OPS : u32 = ${W_WDRAINOPS}u;" \
  "const WATER_CHUNK_CAP : u32 = ${W_WCHUNKCAP}u;" \
  "const WATER_SPLIT_GRID : u32 = ${W_WSPLITGRID}u;" \
  "const WATER_SPLIT_CELLS : u32 = ${W_WSPLITCELLS}u;" \
  "const WATER_SPLIT_WORDS : u32 = ${W_WSPLITWORDS}u;" \
  "const WATER_CURVE_MAXY : u32 = ${W_WCURVEMAXY}u;" \
  "const WATER_SWEEP_HEADER : u32 = ${W_WSWEEPHDR}u;" \
  "const WATER_CURVE_WORDS : u32 = ${W_WCURVEWORDS}u;" \
  "const WATER_CURVE_BASE : u32 = ${W_WCURVEBASE}u;" \
  "const WATER_SCRATCH_BASE : u32 = ${W_WSCRATCHBASE}u;" \
  "const FAR_LEVELS : u32 = ${W_FAR}u;" \
  "const FAR_N : u32 = ${W_FARN}u;" \
  "const FAR_NCHUNK : u32 = ${W_FARNCHUNK}u;" \
  "const FAR_NUM_CHUNKS : u32 = $((W_FARNCHUNK * W_FARNCHUNK * W_FARNCHUNK))u;" \
  "const FAR_VOX : u32 = $((W_FARN * W_FARN * W_FARN))u;" \
  "const FAR_MASK : i32 = $((W_FARN - 1));" \
  "const FAR_NCHUNK_MASK : i32 = $((W_FARNCHUNK - 1));" \
  "const FAR_PATCH_BASE : u32 = ${W_FARPATCHBASE}u;" \
  "const FAR_SHIFT_BASE : u32 = ${W_FARSHIFT}u;" \
  "const FAR_SLOT_SHIFT : u32 = ${W_FARSLOTSHIFT}u;" \
  "const FAR_SLOT_MASK : u32 = ${W_FARSLOTMASK}u;" \
  "const WINDOW_HALF_EXTENT_METERS : f32 = ${W_WINHALF};" \
  "const FAR_CELL1_VOX : f32 = ${W_FARCELL1}.0;" \
  "const VOXEL_METERS : f32 = ${W_VOX};" \
  "const VOXELS_PER_M : i32 = ${W_VPM};")"

# LoadShader() also prepends the tuning constants (TuningWgslBlock, from
# assets/materials/tuning.json). Generated by a helper rather than re-parsed
# here, so bash never has to know the schema.
TUNING_TEXT="$(python "$ROOT/scripts/tuning_prelude.py")" || {
  echo "check_shaders: scripts/tuning_prelude.py failed" >&2
  exit 1
}
PRELUDE_TEXT="$(printf '%s\n%s' "$PRELUDE_TEXT" "$TUNING_TEXT")"

# The tree lattice (ShaderConstantPrelude's TREE_TILE / TREE_SCAN /
# TREE_CAND_MAX). Load-time asset data in the engine -- the finest biome tile
# and the atlas's widest reach -- so it is derived from the same assets here
# (scripts/tree_lattice.py mirrors treeatlas.h TreeLatticeFor) rather than
# scraped from a header. TREE_CAND_MAX sizes an array in worldgen.wgsl.
TREE_TEXT="$(python "$ROOT/scripts/tree_lattice.py" --vpm "$W_VPM" --assets "$ROOT/assets")" || {
  echo "check_shaders: scripts/tree_lattice.py failed" >&2
  exit 1
}
PRELUDE_TEXT="$(printf '%s\n%s' "$PRELUDE_TEXT" "$TREE_TEXT")"

# The pond lattice (ShaderConstantPrelude's POND_TILE; P-F): the finest live
# water tile of any biome, derived from the assets by scripts/pond_lattice.py
# exactly as worldmap.cpp PondLatticeVox derives it at load.
POND_TEXT="$(python "$ROOT/scripts/pond_lattice.py" --vpm "$W_VPM" --assets "$ROOT/assets")" || {
  echo "check_shaders: scripts/pond_lattice.py failed" >&2
  exit 1
}
PRELUDE_TEXT="$(printf '%s\n%s' "$PRELUDE_TEXT" "$POND_TEXT")"

# The map's reference scale (ShaderConstantPrelude's REF_VOXELS_PER_METRE;
# P-G): map.json terrain.refVoxelsPerMetre of the map tuning.json names,
# derived from the assets by scripts/map_terrain.py exactly as LoadWorldMap
# reads it at load.
MAP_TEXT="$(python "$ROOT/scripts/map_terrain.py" --assets "$ROOT/assets")" || {
  echo "check_shaders: scripts/map_terrain.py failed" >&2
  exit 1
}
PRELUDE_TEXT="$(printf '%s\n%s' "$PRELUDE_TEXT" "$MAP_TEXT")"

# Lines contributed ahead of the body: prelude + its "\n" + common + its "\n".
# Error line L in the combined source maps to line L - OFFSET in the body file.
COMMON_LINES="$(wc -l < "$COMMON" | tr -d ' ')"
PRELUDE_LINES="$(printf '%s\n' "$PRELUDE_TEXT" | wc -l | tr -d ' ')"
OFFSET=$((PRELUDE_LINES + 1 + COMMON_LINES + 1))

if [ "$#" -gt 0 ]; then
  FILES=("$@")
else
  FILES=()
  for f in "$SHADER_DIR"/*.wgsl; do
    [ "$(basename "$f")" = "common.wgsl" ] && continue
    FILES+=("$f")
  done
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

failed=0
checked=0

for f in "${FILES[@]}"; do
  name="$(basename "$f")"

  # A bare common.wgsl argument (e.g. from the edit hook) means "revalidate
  # everything", since every shader embeds it.
  if [ "$name" = "common.wgsl" ]; then
    exec bash "$ROOT/scripts/check_shaders.sh"
  fi

  [ -f "$f" ] || { echo "check_shaders: no such file: $f" >&2; failed=1; continue; }

  # common.wgsl's page-table accessor block references voxels/pageTable/
  # pageFaults, which only the shaders that address voxels declare. LoadShader
  # (gpu/resources.cpp, StripPageBlock) blanks it for the others; do the same
  # here, blanking the lines rather than deleting them so OFFSET stays exact.
  # Two blocks: the READ half needs voxels + pageTable, the WRITE half also
  # needs voxels to be read_write and needs pageFaults. raymarch has the first
  # and not the second.
  # The support-loss block gets the same treatment: it references supportOut,
  # which only the kernels that can REMOVE a voxel declare (sim_step,
  # sim_mutate, sim_explode). Predicate read off the body, exactly as
  # LoadShader's BodyFlagsSupportLoss does.
  commonSrc="$COMMON"
  stripRead=0; stripWrite=0; stripSupport=0
  grep -q '> voxels' "$f" || { stripRead=1; stripWrite=1; }
  grep -q 'read_write> voxels' "$f" || stripWrite=1
  grep -q '> supportOut' "$f" || stripSupport=1
  if [ "$stripRead" -eq 1 ] || [ "$stripWrite" -eq 1 ] || [ "$stripSupport" -eq 1 ]; then
    commonSrc="$TMP/common_${name}"
    awk -v sr="$stripRead" -v sw="$stripWrite" -v ss="$stripSupport" '
      /PAGE_TABLE_WRITE_BEGIN/ { print; s = sw; next }
      /PAGE_TABLE_WRITE_END/   { print; s = 0;  next }
      /PAGE_TABLE_BEGIN/       { print; s = sr; next }
      /PAGE_TABLE_END/         { print; s = 0;  next }
      /SUPPORT_LOSS_BEGIN/     { print; s = ss; next }
      /SUPPORT_LOSS_END/       { print; s = 0;  next }
      s                        { print ""; next }
      { print }
    ' "$COMMON" > "$commonSrc"
  fi

  # LoadShader also GENERATES the ptSeed()/ptOrigin() accessors for shaders
  # that address voxels (PtSeedAccessor, gpu/resources.cpp): the page block's
  # JITTER synthesis calls them, and their body is `T.seed` in a sim kernel
  # but `R.seed` in the render pass — whichever uniform the shader declares.
  # A voxel-addressing shader with neither uniform is the same build error the
  # engine raises (a wrong seed is a synthesized word that differs from the
  # materialized page, i.e. a lost voxel).
  ptseed=""
  ptseedLines=0
  if [ "$stripRead" -eq 0 ]; then
    if grep -q 'uniform> T :' "$f"; then u=T
    elif grep -q 'uniform> R :' "$f"; then u=R
    else
      failed=1
      echo "FAIL $name (addresses voxels but declares neither T : TickParams nor R : RenderParams)"
      continue
    fi
    ptseed="fn ptSeed() -> u32 { return ${u}.seed; }
fn ptOrigin() -> vec3<i32> { return ${u}.origin; }
fn ptTick() -> u32 { return ${u}.tick; }"
    ptseedLines=3
  fi

  combined="$TMP/$name"
  { printf '%s\n\n' "$PRELUDE_TEXT"
    [ -n "$ptseed" ] && printf '%s\n' "$ptseed"
    cat "$commonSrc"; printf '\n'; cat "$f"; } > "$combined"

  # `-f wgsl` parses, resolves, and validates, then re-emits WGSL we discard.
  # (`-f none` is advertised in --help but rejected by this build.) A missing
  # entry point is a real error here: every shader in this project has one.
  if out="$("$TINT_BIN" -f wgsl "$combined" 2>&1 >/dev/null)" && [ -z "$out" ]; then
    checked=$((checked + 1))
  else
    failed=1
    echo "FAIL $name"
    # tint reports "<path>:LINE:COL error: msg" against the combined file. Rewrite
    # the path to the real shader and subtract the common.wgsl prologue so line
    # numbers point where the user can actually edit. Diagnostics at or above the
    # offset came from common.wgsl itself and are labelled as such.
    printf '%s\n' "$out" | awk -v off="$((OFFSET + ptseedLines))" -v real="$f" -v common="$COMMON" '
      # Match a trailing :LINE:COL after any path (handles C:/... drive letters).
      match($0, /:[0-9]+:[0-9]+/) {
        head = substr($0, 1, RSTART - 1)          # the path
        loc  = substr($0, RSTART + 1, RLENGTH - 1) # LINE:COL
        tail = substr($0, RSTART + RLENGTH)        # " error: msg"
        split(loc, p, ":")
        lineno = p[1] + 0
        if (lineno > off) printf "  %s:%d:%s%s\n", real, lineno - off, p[2], tail
        else              printf "  %s:%d:%s%s (via common.wgsl)\n", common, lineno, p[2], tail
        next
      }
      { print "  " $0 }
    '
  fi
done

# ---------------------------------------------------------------------------
# THE RAYMARCH'S SPECIALIZED VARIANT (W2-A).
#
# raymarch.wgsl declares three `const SPEC_* : bool = true;` lines and the
# engine compiles a SECOND pipeline from the same assembled source with those
# three flipped to `false` (Simulation::BuildRaymarchVariant). Two things have
# to be true and neither is checked by validating the shipped spelling alone:
#
#   1. THE LEAN SPELLING MUST COMPILE. Dead code is still type-checked, and a
#      `const false` arm that references a variable only the live arm declares
#      is a real error nobody would see until a cold pipeline build.
#   2. THE SUBSTITUTION MUST STILL MATCH. It is a string replacement on exact
#      source lines; reformatting one of them (`= true ;`, a line break) turns
#      the specialization silently into a second copy of the universal shader
#      that nothing would ever report. So the substitution is performed HERE by
#      the same three literals, and a miss is a hard failure.
#
# The SPIR-V instruction counts printed below are the size of what the
# specialization removes, measured without a GPU: they are the cheap standing
# answer to "is this variant worth a pipeline?".
RAY_COMBINED="$TMP/raymarch.wgsl"
if [ -f "$RAY_COMBINED" ]; then
  RAY_LEAN="$TMP/raymarch_lean.wgsl"
  cp "$RAY_COMBINED" "$RAY_LEAN"
  specMiss=0
  for k in SPEC_FLUID SPEC_DEBUG_VIZ SPEC_SHORT_RANGE; do
    if ! grep -qF "const $k : bool = true;" "$RAY_LEAN"; then
      failed=1; specMiss=1
      echo "FAIL raymarch.wgsl — the variant substitution target"
      echo "  \"const $k : bool = true;\" is not in the assembled source."
      echo "  Simulation::BuildRaymarchVariant flips that exact line; keep the"
      echo "  spelling or update BOTH it and scripts/check_shaders.sh."
    fi
  done
  if [ "$specMiss" -eq 0 ]; then
    sed -i \
      -e 's/^const SPEC_FLUID : bool = true;$/const SPEC_FLUID : bool = false;/' \
      -e 's/^const SPEC_DEBUG_VIZ : bool = true;$/const SPEC_DEBUG_VIZ : bool = false;/' \
      -e 's/^const SPEC_SHORT_RANGE : bool = true;$/const SPEC_SHORT_RANGE : bool = false;/' \
      "$RAY_LEAN"
    if out="$("$TINT_BIN" -f wgsl "$RAY_LEAN" 2>&1 >/dev/null)" && [ -z "$out" ]; then
      checked=$((checked + 1))
      echo "check_shaders: raymarch.wgsl lean variant OK (SPEC_* all false)"
    else
      failed=1
      echo "FAIL raymarch.wgsl (lean variant: SPEC_FLUID/DEBUG_VIZ/SHORT_RANGE false)"
      printf '%s\n' "$out" | sed 's/^/  /'
    fi
    # ...and what the specialization actually deletes, in SPIR-V instructions.
    # `fs` only: `vs` is three vertices of a fullscreen triangle and carries
    # none of this. Reported, never a ceiling — the number is the EVIDENCE for
    # the variant, and a shrinking one is good news, not a failure.
    #
    # AFTER spirv-opt, and that is the whole point of the extra invocation.
    # Tint's SPIR-V writer keeps functions no entry point can reach: with
    # SPEC_FLUID false, `fluidMarch`, `fluidMarchBlocky`, `traceRefraction` and
    # `shadeMpmFluid` are all still IN the module Tint emits, and the raw count
    # moves by 197 of 29,256 instructions (0.7%) — a number that reads as "the
    # specialization does nothing" and is simply measuring the wrong artifact.
    # The engine runs SPIRV-Tools' performance recipe over every module before
    # the driver sees it (gpu/vk_spirv.cpp, ON by default since 2026-09-07), and
    # THAT module is 156,482 -> 118,637 instructions, -24.2%. Same recipe here,
    # same target env, so the two numbers are comparable to the engine's.
    #
    # spirv-opt comes from the Vulkan SDK's Bin directory. Optional: without it
    # the pre-opt counts are printed with a note, because a checker that fails
    # on a missing diagnostic tool is a checker people stop running.
    if command -v spirv-opt >/dev/null 2>&1; then optSpv=1; else optSpv=0; fi
    for pair in "universal:$RAY_COMBINED" "lean:$RAY_LEAN"; do
      tag="${pair%%:*}"; src="${pair#*:}"
      spv="$TMP/raymarch_${tag}.spv"
      "$TINT_BIN" -f spirv -ep fs -o "$spv" "$src" >/dev/null 2>&1 || {
        echo "check_shaders: could not emit SPIR-V for raymarch.wgsl:fs ($tag)" >&2
        continue; }
      note="(pre-optimizer; spirv-opt not on PATH, so this UNDER-REPORTS)"
      if [ "$optSpv" -eq 1 ] &&
         spirv-opt -O --preserve-interface --target-env=vulkan1.1 \
                   "$spv" -o "$TMP/raymarch_${tag}_opt.spv" >/dev/null 2>&1; then
        spv="$TMP/raymarch_${tag}_opt.spv"
        note="(after the engine's spirv-opt recipe)"
      fi
      n="$(python -c "
import struct,sys
b=open(sys.argv[1],'rb').read()
w=struct.unpack('<%dI'%(len(b)//4),b)
i,n=5,0
while i<len(w):
    l=w[i]>>16
    if l==0: break
    i+=l; n+=1
print(n)
" "$spv")" || continue
      echo "check_shaders: raymarch.wgsl:fs $tag — $n SPIR-V instructions $note"
    done
  fi
fi

# ---------------------------------------------------------------------------
# SPIR-V SIZE CEILING for worldgen.wgsl.
#
# WHAT THIS DEFENDS AGAINST, precisely: on 2026-09-07 a cold
# vkCreateComputePipelines on worldgen's `far` entry took 746 s on the NVIDIA
# driver (docs/PLAN_shader_compile.md), and before that the pond work made `far`
# never finish compiling at all. Both shipped, because nothing in the repo
# measured how big the shader was — the cost only shows up on a cold cache, in a
# run nobody makes while iterating on WGSL. Driver front-end cost is superlinear
# in ENTRY POINT size, so the cheap deterministic proxy is the SPIR-V
# instruction count, and this is the one place that already assembles the exact
# source the engine compiles.
#
# Only worldgen.wgsl, and only its two biggest entries: every other shader in
# the tree compiles in milliseconds, and paying five extra tint invocations on
# every shader edit to prove that again is not worth the seconds.
#
# WHEN THIS FIRES: it is not "revert". It is "you just made the cold boot
# noticeably slower, say so in the commit message and raise the number, or split
# the entry point" (PLAN_shader_compile.md package C lists the splits). The
# ceilings are ~1.5x the count measured the day they were set, which is room for
# real work and not room for another 700 s regression.
WORLDGEN_COMBINED="$TMP/worldgen.wgsl"
if [ -f "$WORLDGEN_COMBINED" ]; then
  # entry point -> ceiling in SPIR-V instructions, ~1.5x the count measured
  # 2026-09-07 on the tree that carries the unrollFence: far 16,778 /
  # fardown 16,767. (main 16,552 and list 16,579 are within 1.5 % of those and
  # share the same inlined genColumn body, so guarding the two cascade entries
  # guards them too without two more tint invocations per shader edit.
  # `pagefill` is 745 and is not worth a line.)
  #
  # NOTE, so nobody over-reads this number: instruction count is a size proxy,
  # NOT a compile-time model. far and fardown are the same size and cost 746 s
  # and 98 s respectively — the driver's blow-up is about control flow and
  # aggregate copies inside the entry, not the raw count. What the ceiling
  # catches is the failure mode that actually happened: worldgen quietly
  # doubling in size and nobody noticing until a cold boot.
  for spec in "far:25000" "fardown:25000"; do
    ep="${spec%%:*}"; ceil="${spec##*:}"
    spv="$TMP/worldgen_${ep}.spv"
    if ! "$TINT_BIN" -f spirv -ep "$ep" -o "$spv" "$WORLDGEN_COMBINED" >/dev/null 2>&1; then
      echo "check_shaders: could not emit SPIR-V for worldgen.wgsl:$ep (size ceiling not checked)" >&2
      continue
    fi
    # SPIR-V is a word stream: a 5-word header, then instructions whose first
    # word carries its own length in its high 16 bits. Counting instructions
    # rather than bytes keeps the number comparable across constant-data
    # changes.
    n="$(python -c "
import struct,sys
b=open(sys.argv[1],'rb').read()
w=struct.unpack('<%dI'%(len(b)//4),b)
i,n=5,0
while i<len(w):
    l=w[i]>>16
    if l==0: break
    i+=l; n+=1
print(n)
" "$spv")" || { echo "check_shaders: SPIR-V instruction count failed for $ep" >&2; continue; }
    if [ "$n" -gt "$ceil" ]; then
      failed=1
      echo "FAIL worldgen.wgsl:$ep — $n SPIR-V instructions, ceiling $ceil"
      echo "  Driver pipeline-compile time is superlinear in entry-point size;"
      echo "  worldgen 'far' already costs ~750 s cold. Split the entry point"
      echo "  (docs/PLAN_shader_compile.md package C) or raise the ceiling in"
      echo "  scripts/check_shaders.sh and say why in the commit message."
    else
      echo "check_shaders: worldgen.wgsl:$ep $n SPIR-V instructions (ceiling $ceil)"
    fi
  done
fi

if [ "$failed" -eq 0 ]; then
  echo "check_shaders: ${checked} shader(s) OK"
fi
exit "$failed"
