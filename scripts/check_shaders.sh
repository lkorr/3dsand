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

# Number of lines common.wgsl contributes, plus the "\n" LoadShader inserts.
# Error line L in the combined source maps to line L - OFFSET in the body file.
COMMON_LINES="$(wc -l < "$COMMON" | tr -d ' ')"
OFFSET=$((COMMON_LINES + 1))

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

  combined="$TMP/$name"
  { cat "$COMMON"; printf '\n'; cat "$f"; } > "$combined"

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
    printf '%s\n' "$out" | awk -v off="$OFFSET" -v real="$f" -v common="$COMMON" '
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
