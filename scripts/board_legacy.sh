#!/usr/bin/env bash
# Shared coordination board for the parallel Claude sessions on this repo.
#
# The board is append-only so that two agents writing at the same moment cannot
# lose each other's lines: a single short `>>` write is atomic in practice,
# whereas the Edit/Write tools rewrite the file whole and would drop a
# concurrent append. Everything here goes through one `emit`.
#
# Usage:
#   board.sh read [n]              last n entries (default 40)
#   board.sh active                open claims (claimed, not yet done/released)
#   board.sh claim <files> <text>  announce files you are about to edit
#   board.sh done <text>           release your claims, say what landed
#   board.sh note <text>           heads-up with no claim
#   board.sh whoami                print this session's agent name
#
# Agent name: $SANDVOX_AGENT if set, else derived from $CLAUDE_CODE_SESSION_ID.
# It MUST be stable across tool calls within one session, or `done` cannot
# release the claim that same session posted — PPID is not, since every Bash
# tool call gets a fresh shell under a different parent.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="$ROOT/AGENTS_BOARD.md"

agent_name() {
  if [ -n "${SANDVOX_AGENT:-}" ]; then printf '%s' "$SANDVOX_AGENT"; return; fi
  local sid="${CLAUDE_CODE_SESSION_ID:-}"
  if [ -n "$sid" ]; then
    # First 6 chars of the session uuid: short, readable, collision-free enough.
    printf 'agent-%s' "$(printf '%s' "$sid" | tr -d '-' | cut -c1-6)"
    return
  fi
  printf 'agent-unknown'
}

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# Strip the field separator and newlines so one entry stays one line.
clean() { printf '%s' "$*" | tr '\n|' '  ' | sed 's/[[:space:]]\+/ /g; s/^ //; s/ $//'; }

emit() { # kind files text
  local kind="$1" files="$2" text="$3"
  printf '%s | %s | %s | %s | %s\n' \
    "$(now)" "$(agent_name)" "$kind" "$(clean "$files")" "$(clean "$text")" >> "$BOARD"
}

entries() { grep -E '^[0-9]{4}-[0-9]{2}-[0-9]{2}T' "$BOARD" 2>/dev/null; }

cmd="${1:-read}"
shift 2>/dev/null || true

case "$cmd" in
  read)
    n="${1:-40}"
    echo "== agent board (last $n) =="
    entries | tail -n "$n"
    ;;

  active)
    # A claim is open until the same agent posts a later done. Walk in order and
    # keep the last state per agent.
    echo "== open claims =="
    entries | awk -F' \\| ' '
      $3=="claim" { open[$2]=$0 }
      $3=="done"  { delete open[$2] }
      END { for (a in open) print open[a] }
    ' | sort
    ;;

  claim)
    files="${1:-}"; text="${2:-}"
    [ -z "$files" ] && { echo "usage: board.sh claim <files> <text>" >&2; exit 2; }
    # Warn about anyone else already holding these paths.
    conflicts=$("$0" active | grep -F -f <(printf '%s\n' $files) 2>/dev/null \
                | grep -v "| $(agent_name) |")
    emit claim "$files" "$text"
    if [ -n "$conflicts" ]; then
      echo "WARNING: another agent has an open claim overlapping these files:"
      echo "$conflicts"
      echo "Re-read those files before editing; coordinate via board.sh note."
    else
      echo "claimed: $files"
    fi
    ;;

  done)
    text="${1:-released}"
    emit done "" "$text"
    echo "released."
    ;;

  note)
    text="${1:-}"
    [ -z "$text" ] && { echo "usage: board.sh note <text>" >&2; exit 2; }
    emit note "" "$text"
    echo "noted."
    ;;

  whoami) agent_name; echo ;;

  *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
