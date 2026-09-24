#!/usr/bin/env bash
# PostToolUse hook: validate an edited file against the checks that file can break.
#
# Reads the hook's JSON payload on stdin, pulls out file_path and cwd, and runs:
#
#   *.wgsl                  -> scripts/check_shaders.sh   (tint --validate)
#   any "agree in two places" file -> scripts/check_invariants.py
#   *.wgsl / pass_table.* / simulation.cpp -> scripts/check_pass_table.py
#   assets/{editor/*.js,mobs,biomes,trees} -> scripts/generator_parity.mjs
#                          (test_mobgen / test_anatomy / test_environment.mjs)
#
# Lives in a script rather than inline in settings.json because the inline
# version was already an unreadable one-liner with three nested seds, and the
# hook is exactly the place where a silent quoting bug means the check stops
# running and nobody notices.
#
# Always exits 0 for anything it does not recognise. A failing check exits
# non-zero so its stderr is surfaced back into the session.

set -uo pipefail

payload=$(cat)

field() {  # field <name> — pull a top-level string out of the hook JSON
  printf '%s' "$payload" |
    sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1
}

f=$(field file_path)
d=$(field cwd)
[ -n "$f" ] || exit 0

cd "${d:-$CLAUDE_PROJECT_DIR}" 2>/dev/null || exit 0

rc=0

case "$f" in
  *.wgsl)
    [ -f scripts/check_shaders.sh ] && { bash scripts/check_shaders.sh "$f" || rc=1; }
    ;;
esac

# check_invariants.py decides for itself whether the edited file participates in
# one of the documented pairs, and exits 0 when it does not.
if [ -f scripts/check_invariants.py ]; then
  python scripts/check_invariants.py "$f" || rc=1
fi

# The pass table vs the WGSL bindings its rows describe. Same contract: it takes
# the edited file, decides whether that file can break the pair, and exits 0 when
# it cannot. Kept separate from check_invariants.py because it parses WGSL call
# graphs rather than regexing two lists, and because a Vulkan-port check that
# fails should say so in those terms.
if [ -f scripts/check_pass_table.py ]; then
  python scripts/check_pass_table.py "$f" || rc=1
fi

# The generator data gates (test_mobgen / test_anatomy / test_environment.mjs),
# which nothing ran before 2026-09-24. generator_parity.mjs owns the routing:
# it runs only the tests whose import closure or data directories contain the
# edited file and exits 0 without running anything otherwise, so a UI module or
# an unrelated asset costs one node start. Needs node; silently skipped without
# it (the `generator-parity` selftest gate SKIPS the same way).
# The payload's path is JSON-escaped (doubled backslashes on Windows), so fold
# every run of backslashes into one forward slash before matching.
fs=$(printf '%s' "$f" | sed 's#\\\\*#/#g')
case "$fs" in
  */assets/editor/*.js|*/assets/mobs/*|*/assets/biomes/*|*/assets/trees/*|\
  assets/editor/*.js|assets/mobs/*|assets/biomes/*|assets/trees/*|\
  *scripts/test_mobgen.mjs|*scripts/test_anatomy.mjs|*scripts/test_environment.mjs|\
  *scripts/generator_parity.mjs)
    if [ -f scripts/generator_parity.mjs ] && command -v node >/dev/null 2>&1; then
      node scripts/generator_parity.mjs --changed "$fs" >&2 || rc=1
    fi
    ;;
esac

exit $rc
