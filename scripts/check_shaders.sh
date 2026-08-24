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
    "$ROOT/build/Release/tint.exe"; do
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
# slots holding per-voxel mob SKIN colours (world.h kArtPaletteBaseGpu). The
# lowest .vox palette index that counts as art comes from sim/voxload.h, which
# is the other half of the same convention.
W_ARTSLOTS="$(cpp_const kArtPaletteSlotsGpu)"
[ -n "$W_ARTSLOTS" ] || {
  echo "check_shaders: cannot parse kArtPaletteSlotsGpu from $WORLD_H" >&2; exit 1; }
W_ARTBASE=$((W_STAINBASE - W_ARTSLOTS))
VOXLOAD_H="$ROOT/src/sim/voxload.h"
W_ARTSLOTMIN="$(sed -n 's/^constexpr int kArtPaletteBase = \([0-9]*\);.*/\1/p' "$VOXLOAD_H" | head -1)"
[ -n "$W_ARTSLOTMIN" ] || {
  echo "check_shaders: cannot parse kArtPaletteBase from $VOXLOAD_H" >&2; exit 1; }

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

# far-field grid (decoupled from the window — see world.h kFarN/kFarShiftBase)
W_FARN="$(cpp_const kFarN)"
[ -n "$W_FARN" ] || { echo "check_shaders: cannot parse kFarN from $WORLD_H" >&2; exit 1; }
W_FARNCHUNK=$((W_FARN / W_CHUNK))
W_FARSHIFT=0
while [ $((W_FARN << W_FARSHIFT)) -lt "$W_N" ]; do W_FARSHIFT=$((W_FARSHIFT + 1)); done

# Fill-queue packing + cascade geometry (world.h kFarSlotShift/kFarSlotMask,
# kWindowHalfExtentMeters, kFarCellVox(1)) — derived exactly as world.h derives
# them, since the constexpr lambdas cannot be scraped as literals.
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
  "const ART_SLOT_MIN : u32 = ${W_ARTSLOTMIN}u;" \
  "const MICRO_POOL_WORDS : u32 = ${W_MICROPOOL}u;" \
  "const MICRO_BODY_POOL_WORDS : u32 = ${W_MBPOOL}u;" \
  "const MATERIAL_SLOTS : u32 = ${W_MATSLOTS}u;" \
  "const FAR_LEVELS : u32 = ${W_FAR}u;" \
  "const FAR_N : u32 = ${W_FARN}u;" \
  "const FAR_NCHUNK : u32 = ${W_FARNCHUNK}u;" \
  "const FAR_NUM_CHUNKS : u32 = $((W_FARNCHUNK * W_FARNCHUNK * W_FARNCHUNK))u;" \
  "const FAR_VOX : u32 = $((W_FARN * W_FARN * W_FARN))u;" \
  "const FAR_MASK : i32 = $((W_FARN - 1));" \
  "const FAR_NCHUNK_MASK : i32 = $((W_FARNCHUNK - 1));" \
  "const FAR_SHIFT_BASE : u32 = ${W_FARSHIFT}u;" \
  "const FAR_SLOT_SHIFT : u32 = ${W_FARSLOTSHIFT}u;" \
  "const FAR_SLOT_MASK : u32 = ${W_FARSLOTMASK}u;" \
  "const WINDOW_HALF_EXTENT_METERS : f32 = ${W_WINHALF};" \
  "const FAR_CELL1_VOX : f32 = ${W_FARCELL1}.0;" \
  "const VOXEL_METERS : f32 = ${W_VOX};")"

# LoadShader() also prepends the tuning constants (TuningWgslBlock, from
# assets/materials/tuning.json). Generated by a helper rather than re-parsed
# here, so bash never has to know the schema.
TUNING_TEXT="$(python "$ROOT/scripts/tuning_prelude.py")" || {
  echo "check_shaders: scripts/tuning_prelude.py failed" >&2
  exit 1
}
PRELUDE_TEXT="$(printf '%s\n%s' "$PRELUDE_TEXT" "$TUNING_TEXT")"

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
  commonSrc="$COMMON"
  stripRead=0; stripWrite=0
  grep -q '> voxels' "$f" || { stripRead=1; stripWrite=1; }
  grep -q 'read_write> voxels' "$f" || stripWrite=1
  if [ "$stripRead" -eq 1 ] || [ "$stripWrite" -eq 1 ]; then
    commonSrc="$TMP/common_${name}"
    awk -v sr="$stripRead" -v sw="$stripWrite" '
      /PAGE_TABLE_WRITE_BEGIN/ { print; s = sw; next }
      /PAGE_TABLE_WRITE_END/   { print; s = 0;  next }
      /PAGE_TABLE_BEGIN/       { print; s = sr; next }
      /PAGE_TABLE_END/         { print; s = 0;  next }
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
fn ptOrigin() -> vec3<i32> { return ${u}.origin; }"
    ptseedLines=2
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

if [ "$failed" -eq 0 ]; then
  echo "check_shaders: ${checked} shader(s) OK"
fi
exit "$failed"
