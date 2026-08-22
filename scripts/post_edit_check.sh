#!/usr/bin/env bash
# PostToolUse hook: validate an edited file against the checks that file can break.
#
# Reads the hook's JSON payload on stdin, pulls out file_path and cwd, and runs:
#
#   *.wgsl                  -> scripts/check_shaders.sh   (tint --validate)
#   any "agree in two places" file -> scripts/check_invariants.py
#   *.wgsl / pass_table.* / simulation.cpp -> scripts/check_pass_table.py
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

exit $rc
