#!/usr/bin/env bash
# SessionStart brief: where am I, and who else is in this repo right now?
#
# Replaces the old board_brief.sh. The agent board tried to be an advisory
# mutex over one shared checkout and could not be one — a claim is a line of
# text, and nothing enforces it. Worktrees make the isolation real (Claude Code
# blocks writes from a worktree session into the main checkout), so all this
# needs to report is the layout.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" 2>/dev/null || exit 0
command -v git >/dev/null 2>&1 || exit 0

# Is this session in the main checkout or a worktree? --show-toplevel is the
# worktree root either way; the main checkout is the one whose .git is a dir.
here="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
if [ -d "$here/.git" ]; then where="MAIN CHECKOUT"; else where="WORKTREE"; fi

echo "== sandvox: $where — $(git rev-parse --abbrev-ref HEAD 2>/dev/null) =="

if [ "$where" = "MAIN CHECKOUT" ]; then
  cat <<'EOF'
You are in the shared main checkout. If you are about to EDIT anything, work in
a worktree instead so you cannot collide with another session:

  ask me to "work in a worktree", or start with:  claude --worktree <name>

Reading, reviewing, and merging finished branches are fine to do right here.
EOF
fi

# Other worktrees = other agents. Show each one's branch and whether it is dirty.
n=0
while IFS= read -r line; do
  p="${line%% *}"
  [ "$p" = "$here" ] && continue
  [ -d "$p" ] || continue
  n=$((n + 1))
  [ "$n" = 1 ] && echo "" && echo "Other worktrees (other sessions may be live in these):"
  b="$(git -C "$p" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
  d="$(git -C "$p" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
  case "$d" in 0) s="clean";; *) s="$d changed";; esac
  # Ahead of main? That is work waiting to be merged.
  a="$(git -C "$p" rev-list --count main..HEAD 2>/dev/null || echo 0)"
  case "$a" in 0|"") ac="";; *) ac=", $a commit(s) ahead of main";; esac
  printf '  %-28s %s (%s%s)\n' "$(basename "$p")" "$b" "$s" "$ac"
done <<EOF
$(git worktree list 2>/dev/null)
EOF

[ "$n" = 0 ] && [ "$where" = "MAIN CHECKOUT" ] && echo "" && echo "No other worktrees right now — you are alone in this repo."
exit 0
