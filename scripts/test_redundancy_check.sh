#!/usr/bin/env bash
# test_redundancy_check.sh — PreToolUse/Bash advisory: catch PROVABLY redundant
# sandvox verification runs.
#
# WHY THIS EXISTS. The suites here are slow (full --selftest ~50 s per residency
# mode, --vk-smoke-loud ~40 s) and this repo's determinism culture makes "run it
# again" feel free. It is not. A measured session spent ~2.5 h on ~45 binary
# invocations for ~20 min of code, and roughly a dozen of those runs could not
# have produced new information: same exe, same tree, same argv as a run minutes
# earlier. See CLAUDE.md "When to run what — verification is a BUDGET".
#
# WHAT IT IS NOT. This NEVER blocks. It prints a note and exits 0 always.
# Blocking would be wrong: re-running the same command as instrumentation
# changes is exactly how the JITTER page-fill bug was found, and a hard gate
# would have obstructed that. The goal is to put evidence at the decision point,
# not to overrule the judgement.
#
# THREE CHECKS, each catching a class that showed up in the measured session:
#   1. IDENTICAL RE-RUN  — same argv + same exe mtime+size + same tree hash as a
#      run in the last 30 min. Catches the double acceptance block (once in the
#      worktree, once on main), re-verifying after a no-op revert, and
#      recomputing the invariant control arm of an A/B.
#   2. GATE-THEN-SUITE   — a full --selftest when --selftest --gate ran against
#      this same exe+tree recently. The suite re-runs the gate; pick one.
#   3. STALE EXE         — a suite invoked while a tracked source file is newer
#      than the binary. That is gotcha-verify-the-binary-you-measure: the number
#      you get back is the OLD build's.
#
# The ledger is per-repo, capped, and gitignored. Absence of a ledger entry is
# never evidence of anything — a cold start simply says nothing.

set -uo pipefail

INPUT="$(cat 2>/dev/null || true)"
CMD="$(printf '%s' "$INPUT" | jq -r '.tool_input.command // empty' 2>/dev/null || true)"
[ -z "$CMD" ] && exit 0

# Only look at invocations of the game binary. Everything else is none of our
# business — git, grep, python checkers, builds all pass straight through.
case "$CMD" in
  *sandvox.exe*) ;;
  *) exit 0 ;;
esac

# ...and only at the VERIFICATION subcommands. --frames/--shot are interactive
# or measurement runs whose value is not a pass/fail, so repeats of those are
# legitimate and unremarkable.
case "$CMD" in
  *--selftest*|*--vk-smoke*) ;;
  *) exit 0 ;;
esac

ROOT="${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel 2>/dev/null)}"
[ -z "$ROOT" ] && exit 0
cd "$ROOT" 2>/dev/null || exit 0

LEDGER="$ROOT/.claude/.test-ledger"
NOW=$(date +%s)
WINDOW=1800   # 30 min: long enough to span a build+verify cycle, short enough
              # that a genuinely stale entry ages out rather than nagging.

# --- identity of THIS run ---------------------------------------------------
# argv normalised: collapse whitespace, drop the leading path so the same
# command from a worktree and from the main checkout compare equal.
ARGV="$(printf '%s' "$CMD" | sed 's#[^ ]*sandvox\.exe#sandvox.exe#' | tr -s ' ' | sed 's/^ *//;s/ *$//')"

EXE="$ROOT/build/Release/sandvox.exe"
[ -f "$EXE" ] || exit 0
# mtime+size rather than a hash: an 8 MB hash on every invocation is a cost the
# hook has no right to impose, and mtime+size is sufficient to detect a rebuild.
EXE_ID="$(stat -c '%Y-%s' "$EXE" 2>/dev/null || echo unknown)"

# Tree identity: HEAD plus the hash of uncommitted tracked changes. Two trees
# with the same value are byte-identical in everything git tracks, which is the
# whole claim being made.
HEAD_ID="$(git rev-parse HEAD 2>/dev/null || echo nohead)"
DIRTY_ID="$(git diff HEAD 2>/dev/null | git hash-object --stdin 2>/dev/null || echo nodiff)"
TREE_ID="${HEAD_ID:0:12}-${DIRTY_ID:0:12}"

NOTES=""

# --- check 3: stale exe -----------------------------------------------------
# Cheapest and most valuable, so it runs first and is reported even if the run
# is otherwise novel. A newer source file than the binary means the result
# describes a build that no longer exists.
NEWER="$(find src assets/shaders -newer "$EXE" -type f \
          \( -name '*.cpp' -o -name '*.h' -o -name '*.def' \) 2>/dev/null | head -3)"
if [ -n "$NEWER" ]; then
  # WGSL is loaded from disk at launch, so a newer .wgsl is NOT stale — only
  # compiled sources are. Filtered above by extension, deliberately.
  COUNT="$(printf '%s\n' "$NEWER" | wc -l | tr -d ' ')"
  FIRST="$(printf '%s\n' "$NEWER" | head -1)"
  NOTES="${NOTES}STALE EXE: $FIRST (and $((COUNT-1)) more) is newer than build/Release/sandvox.exe. This run measures the PREVIOUS build — rebuild first, or the number is the old binary's (gotcha-verify-the-binary-you-measure). "
fi

# --- checks 1 and 2: consult the ledger -------------------------------------
if [ -f "$LEDGER" ]; then
  while IFS='|' read -r TS L_EXE L_TREE L_ARGV; do
    [ -z "${TS:-}" ] && continue
    AGE=$((NOW - TS))
    [ "$AGE" -gt "$WINDOW" ] && continue
    [ "$L_EXE" != "$EXE_ID" ] && continue
    [ "$L_TREE" != "$TREE_ID" ] && continue
    MINS=$((AGE / 60))

    if [ "$L_ARGV" = "$ARGV" ]; then
      NOTES="${NOTES}IDENTICAL RE-RUN: this exact command ran ${MINS} min ago against the same binary and the same tree. Nothing has changed since, so it cannot report anything new. If you are re-running to confirm a merge, \`git diff --stat <branch> HEAD\` answers that in one second. "
      break
    fi

    case "$ARGV" in
      *--selftest*)
        case "$L_ARGV" in
          *--gate*)
            NOTES="${NOTES}GATE-THEN-SUITE: \`$L_ARGV\` ran ${MINS} min ago on this same binary and tree, and the full suite re-runs that gate. Pick one — gate while iterating, suite once at the end. "
            break
            ;;
        esac
        ;;
    esac
  done < <(tac "$LEDGER" 2>/dev/null || cat "$LEDGER")
fi

# --- record this run --------------------------------------------------------
mkdir -p "$(dirname "$LEDGER")" 2>/dev/null
printf '%s|%s|%s|%s\n' "$NOW" "$EXE_ID" "$TREE_ID" "$ARGV" >> "$LEDGER" 2>/dev/null
# Cap it. This is a scratch ledger, not history.
if [ "$(wc -l < "$LEDGER" 2>/dev/null || echo 0)" -gt 200 ]; then
  tail -100 "$LEDGER" > "$LEDGER.tmp" 2>/dev/null && mv "$LEDGER.tmp" "$LEDGER" 2>/dev/null
fi

[ -z "$NOTES" ] && exit 0

# Advisory only: additionalContext reaches the model, systemMessage reaches the
# user. Neither blocks. permissionDecision is deliberately NOT set.
jq -n --arg n "$NOTES" '{
  systemMessage: ("verification budget — " + $n),
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    additionalContext: ("REDUNDANCY CHECK (advisory, not a block): " + $n
      + "See CLAUDE.md \"When to run what — verification is a BUDGET\". If you have a reason to run it anyway (adding instrumentation, chasing a heisenbug), do so.")
  }
}' 2>/dev/null || true
exit 0
