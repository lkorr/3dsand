#!/usr/bin/env bash
# SessionStart hook: inject the agent board into a new session's context.
# Stays SILENT when nothing is going on, so a solo session pays no noise cost.
set -u

ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 0
[ -f AGENTS_BOARD.md ] || exit 0

open=$(bash scripts/board.sh active 2>/dev/null | tail -n +2)
# Recent = entries from the last 12 hours, so a stale board doesn't shout.
cutoff=$(date -u -d '12 hours ago' +%Y-%m-%dT%H:%M:%SZ 2>/dev/null) || cutoff=""
if [ -n "$cutoff" ]; then
  recent=$(grep -E '^[0-9]{4}-' AGENTS_BOARD.md 2>/dev/null | awk -v c="$cutoff" -F' \\| ' '$1 > c' | tail -12)
else
  recent=$(grep -E '^[0-9]{4}-' AGENTS_BOARD.md 2>/dev/null | tail -12)
fi

[ -z "$open" ] && [ -z "$recent" ] && exit 0

echo "AGENT BOARD (AGENTS_BOARD.md) — other Claude sessions may be working this tree right now."
if [ -n "$open" ]; then
  echo
  echo "OPEN CLAIMS (files another agent is editing — re-read these before you touch them):"
  echo "$open"
fi
if [ -n "$recent" ]; then
  echo
  echo "RECENT ACTIVITY (last 12h):"
  echo "$recent"
fi
echo
echo "Before editing shared files, run: bash scripts/board.sh claim \"<files>\" \"<what>\""
echo "When you stop:                    bash scripts/board.sh done \"<what landed>\""
