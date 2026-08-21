#!/usr/bin/env bash
# Worktree overview + merge-back helper. Run from the MAIN checkout.
#
# Worktrees isolate the editing; the cost moves to integration, and that is
# where the friction actually is (CLAUDE.md "Parallel sessions"). This exists so
# reviewing and landing N parallel branches is one command each rather than a
# hand-assembled git incantation per agent.
#
#   worktrees.sh list            what exists, branch, dirty, ahead/behind
#   worktrees.sh diff <name>     what that worktree changed vs main
#   worktrees.sh land <name>     merge its branch into main, then offer removal
#   worktrees.sh gc              remove worktrees that are clean AND merged
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
WT_DIR=".claude/worktrees"

# git prints worktree paths as C:/... while MSYS `pwd` gives /c/... — comparing
# those two strings never matches, which would let `gc`/`land` treat the main
# checkout as just another worktree. Ask git for the main path in git's own
# spelling and compare that instead.
MAIN="$(git worktree list --porcelain | awk '/^worktree /{print substr($0,10); exit}')"
same_path() { [ "${1%/}" = "${2%/}" ]; }

# Resolve a short name to a path: "items-tab" -> .claude/worktrees/items-tab
resolve() {
  local n="$1"
  [ -d "$n" ] && { printf '%s' "$n"; return; }
  [ -d "$WT_DIR/$n" ] && { printf '%s' "$WT_DIR/$n"; return; }
  echo "no such worktree: $n" >&2
  echo "try: bash scripts/worktrees.sh list" >&2
  exit 1
}

# Refuse to operate from inside a worktree — merging into main from a worktree
# that has main checked out elsewhere is exactly the confusion we removed.
if [ ! -d "$ROOT/.git" ]; then
  echo "You are inside a worktree. Run this from the main checkout:" >&2
  git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's/\.git$//' >&2
  exit 1
fi

cmd="${1:-list}"

case "$cmd" in
  list)
    printf '%-24s %-26s %-14s %s\n' WORKTREE BRANCH STATE VS-MAIN
    git worktree list --porcelain | awk '/^worktree /{print substr($0,10)}' | while IFS= read -r p; do
      [ -d "$p" ] || continue
      name="$(basename "$p")"
      same_path "$p" "$MAIN" && name="(main)"
      b="$(git -C "$p" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
      d="$(git -C "$p" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
      case "$d" in 0) st="clean";; *) st="$d changed";; esac
      if same_path "$p" "$MAIN"; then vs="-"; else
        a="$(git -C "$p" rev-list --count main..HEAD 2>/dev/null || echo 0)"
        bh="$(git -C "$p" rev-list --count HEAD..main 2>/dev/null || echo 0)"
        vs="+$a/-$bh"
      fi
      printf '%-24s %-26s %-14s %s\n' "$name" "$b" "$st" "$vs"
    done
    echo ""
    echo "+N = commits to land, -N = commits behind main (rebase before landing)."
    ;;

  diff)
    p="$(resolve "${2:-}")"
    echo "== commits in $(basename "$p") not in main =="
    git -C "$p" log --oneline main..HEAD 2>/dev/null || echo "(none)"
    echo ""
    echo "== files changed vs main (committed) =="
    git -C "$p" diff --stat main...HEAD 2>/dev/null || true
    echo ""
    u="$(git -C "$p" status --porcelain 2>/dev/null)"
    [ -n "$u" ] && { echo "== UNCOMMITTED in that worktree (will NOT merge) =="; printf '%s\n' "$u"; }
    ;;

  land)
    p="$(resolve "${2:-}")"
    b="$(git -C "$p" rev-parse --abbrev-ref HEAD)"
    u="$(git -C "$p" status --porcelain 2>/dev/null)"
    if [ -n "$u" ]; then
      echo "REFUSING: $(basename "$p") has uncommitted work — it would not be merged:" >&2
      printf '%s\n' "$u" >&2
      echo "Commit it in that worktree first." >&2
      exit 1
    fi
    n="$(git -C "$p" rev-list --count main..HEAD 2>/dev/null || echo 0)"
    [ "$n" = 0 ] && { echo "$b has nothing to land."; exit 0; }
    echo "Merging $b ($n commit(s)) into main..."
    git merge --no-ff "$b" || {
      echo "" >&2
      echo "Conflict. Resolve here in the main checkout, then: git merge --continue" >&2
      exit 1
    }
    echo ""
    echo "Landed. Remove the worktree when you're done with it:"
    echo "  git worktree remove $p && git branch -d $b"
    ;;

  gc)
    # `while` in a pipeline runs in a subshell, so a counter set inside it does
    # not survive. Collect the removable ones first, then act in this shell.
    victims=""
    while IFS= read -r p; do
      same_path "$p" "$MAIN" && continue
      [ -d "$p" ] || continue
      [ -n "$(git -C "$p" status --porcelain 2>/dev/null)" ] && continue
      # `|| echo 1` matters: if rev-list fails we must NOT treat it as "0 ahead"
      # and delete a worktree whose commits we failed to count.
      a="$(git -C "$p" rev-list --count main..HEAD 2>/dev/null || echo 1)"
      [ "$a" != 0 ] && continue
      victims="$victims$p"$'\n'
    done <<EOF
$(git worktree list --porcelain | awk '/^worktree /{print substr($0,10)}')
EOF

    if [ -z "$(printf '%s' "$victims" | tr -d '[:space:]')" ]; then
      echo "nothing to collect (worktrees with work or unmerged commits are kept)"
    else
      printf '%s' "$victims" | while IFS= read -r p; do
        [ -n "$p" ] || continue
        b="$(git -C "$p" rev-parse --abbrev-ref HEAD 2>/dev/null)"
        echo "removing $(basename "$p") (clean, fully merged)"
        git worktree remove "$p" && case "$b" in
          worktree-*) git branch -d "$b" 2>/dev/null || true ;;
        esac
      done
    fi
    git worktree prune
    ;;

  *)
    sed -n '1,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
