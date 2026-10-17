#!/usr/bin/env bash
# spv_sizes.sh -- per-entry SPIR-V instruction counts for the two shaders whose
# compile cost matters (docs/PLAN_shader_compile.md, "Slimming").
#
#   bash scripts/spv_sizes.sh [outfile]
#   SPV_EPS="far cols" SPV_RAY=0 bash scripts/spv_sizes.sh   # a subset (iteration)
#
# For every worldgen.wgsl entry point: Tint's raw count (what check_shaders.sh's
# ceiling reads) and the count after the engine's worldgen recipe
# (vk_spirv.cpp OptRecipe::kLegalization: inline-exhaustive + SSA + DCE), which
# is the single inlined function the driver actually compiles. For
# raymarch.wgsl:fs, raw and after the performance recipe (-O). Needs tint (as
# check_shaders.sh finds it) and spirv-opt from the Vulkan SDK on PATH.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEEP="$(mktemp -d)"
trap 'rm -rf "$KEEP"' EXIT
SPV_RAY="${SPV_RAY:-1}"
SHADERS=("$ROOT/assets/shaders/worldgen.wgsl")
[ "$SPV_RAY" = 1 ] && SHADERS+=("$ROOT/assets/shaders/raymarch.wgsl")
CHECK_SHADERS_KEEP="$KEEP" bash "$ROOT/scripts/check_shaders.sh" "${SHADERS[@]}" >/dev/null 2>&1
TINT_BIN=""
for c in "${TINT:-}" "$ROOT/build/_deps/dawn-build/Release/tint.exe" \
         "${SANDVOX_DEPS:-C:/sv-deps}/ninja/dawn-build/Release/tint.exe" \
         "${SANDVOX_DEPS:-C:/sv-deps}/dawn-build/Release/tint.exe"; do
  [ -n "$c" ] && [ -x "$c" ] && { TINT_BIN="$c"; break; }
done
[ -n "$TINT_BIN" ] || TINT_BIN="$(find "$ROOT/build" "$ROOT/../../../build" -name tint.exe -type f 2>/dev/null | head -1)"
[ -n "$TINT_BIN" ] || { echo "spv_sizes: tint not found" >&2; exit 127; }
count() { python -c "
import struct,sys
b=open(sys.argv[1],'rb').read()
w=struct.unpack('<%dI'%(len(b)//4),b)
i,n=5,0
while i<len(w):
    l=w[i]>>16
    if l==0: break
    i+=l; n+=1
print(n)" "$1"; }
LEGAL="--inline-entry-points-exhaustive --private-to-local --scalar-replacement \
--eliminate-local-single-block --convert-local-access-chains --ssa-rewrite \
--simplify-instructions --eliminate-dead-branches --merge-blocks \
--eliminate-dead-code-aggressive"
out="${1:-$KEEP/sizes.txt}"
{
  eps="${SPV_EPS:-$(grep -A1 '^@compute' "$ROOT/assets/shaders/worldgen.wgsl" | sed -n 's/^fn \([A-Za-z0-9_]*\)(.*//p')}"
  one() {
    local ep="$1" spv="$KEEP/wg_$1.spv" raw opt
    "$TINT_BIN" -f spirv -ep "$ep" -o "$spv" "$KEEP/worldgen.wgsl" >/dev/null 2>&1 || { echo "worldgen:$ep tint FAILED" > "$KEEP/res_$ep"; return; }
    raw="$(count "$spv")"
    spirv-opt $LEGAL --preserve-interface --target-env=vulkan1.1 "$spv" -o "$KEEP/wg_${ep}_o.spv" >/dev/null 2>&1       && opt="$(count "$KEEP/wg_${ep}_o.spv")" || opt="opt-failed"
    printf 'worldgen:%-14s raw %7s  legal %8s
' "$ep" "$raw" "$opt" > "$KEEP/res_$ep"
  }
  # spirv-opt is the slow part; four entries at a time
  n=0
  for ep in $eps; do one "$ep" & n=$((n + 1)); [ $((n % 4)) -eq 0 ] && wait; done
  wait
  for ep in $eps; do cat "$KEEP/res_$ep"; done
  spv="$KEEP/ray_fs.spv"
  if [ "$SPV_RAY" != 1 ]; then :; elif "$TINT_BIN" -f spirv -ep fs -o "$spv" "$KEEP/raymarch.wgsl" >/dev/null 2>&1; then
    raw="$(count "$spv")"
    spirv-opt -O --preserve-interface --target-env=vulkan1.1 "$spv" -o "$KEEP/ray_fs_o.spv" >/dev/null 2>&1 \
      && opt="$(count "$KEEP/ray_fs_o.spv")" || opt="opt-failed"
    printf 'raymarch:%-14s raw %7s  perf  %8s\n' fs "$raw" "$opt"
  else echo "raymarch:fs tint FAILED"; fi
} > "$out"
[ -n "${1:-}" ] || cat "$out"
