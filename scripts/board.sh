#!/usr/bin/env bash
# DEPRECATED — the agent board has been replaced by git worktrees.
#
# The board was an advisory lock: a session appended "I am editing world.h" and
# every other session was trusted to read it and stay away. That is not a mutex,
# and the failures it was meant to prevent (silent overwrites, swept-up commits,
# stash/pop conflict markers) kept happening anyway.
#
# Worktrees enforce what the board only asked for: each session edits its own
# checkout, and Claude Code blocks a worktree session from writing into the main
# one. See CLAUDE.md "Parallel sessions".
#
#   claude --worktree <name>      # or ask Claude to "work in a worktree"
#
# This shim exists so a session mid-task that still calls board.sh gets told
# what changed instead of a "command not found". `note` still appends, since a
# heads-up is worth keeping; the claim/done verbs are gone because nothing
# reads them now.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmd="${1:-read}"

case "$cmd" in
  note)
    shift 2>/dev/null || true
    printf '%s | %s | note | %s\n' \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
      "${SANDVOX_AGENT:-agent-$(printf '%s' "${CLAUDE_CODE_SESSION_ID:-unknown}" | tr -d '-' | cut -c1-6)}" \
      "$*" >> "$ROOT/AGENTS_BOARD.md"
    echo "noted (board is deprecated; see CLAUDE.md 'Parallel sessions')" >&2
    ;;
  read|active)
    bash "$ROOT/scripts/board_legacy.sh" "$cmd" "${2:-}" 2>/dev/null
    echo "" >&2
    echo "NOTE: the board is retired. Live sessions are worktrees now:" >&2
    git -C "$ROOT" worktree list 2>/dev/null >&2
    ;;
  *)
    cat >&2 <<'EOF'
board.sh is deprecated — worktrees replaced it.

  Don't claim files; take a worktree instead, and you cannot collide:
      claude --worktree <name>       (or ask Claude to "work in a worktree")

  To see who else is live:
      git worktree list

  Full workflow: CLAUDE.md, "Parallel sessions".
EOF
    exit 2
    ;;
esac
