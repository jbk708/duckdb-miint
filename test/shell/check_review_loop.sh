#!/bin/bash
# Scenario tests for scripts/check-review-loop.sh, the PR-description check behind
# the review-loop-check workflow: which descriptions it accepts, which it refuses,
# and with what message.
#
# Each scenario runs the script inside a throwaway repository holding a base branch
# (one commit), a PR branch two commits ahead of it, and a third branch with a commit
# on neither, so "is this sha one of the PR's own commits" is decided by real git
# ancestry. The repository is removed when all scenarios pass and kept otherwise.
#
# Usage: bash test/shell/check_review_loop.sh [script-under-test]

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${1:-$HERE/../../scripts/check-review-loop.sh}"
TEMPLATE="$HERE/../../.github/pull_request_template.md"
SKILL="$HERE/../../.claude/skills/miint-review/SKILL.md"
FAILED=0
ALL_PASSED=0

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/check_review_loop.XXXXXX")"
trap 'if [ "$ALL_PASSED" = 1 ]; then rm -rf "$TMP_ROOT"; fi' EXIT
REPO="$TMP_ROOT/repo"

# The developer's global hooks and signing settings must not reach the throwaway repo.
g() { git -C "$REPO" -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"; }

mkdir -p "$REPO"
g init -q -b base
g config user.email t@example.org
g config user.name t
commit() {
    printf '%s\n' "$1" > "$REPO/f"
    g add f
    g commit -q -m "$1"
    g rev-parse --short HEAD
}
SHA_base="$(commit base)"
g checkout -q -b pr
SHA_first="$(commit first)"
SHA_second="$(commit second)"
g checkout -q -b elsewhere base
SHA_elsewhere="$(commit elsewhere)"
g checkout -q pr

BLOCK='## Summary

Something.

## Reviewer loop

- Reviewed at: @SHA@ · rounds: 2 · stopped: nothing new
- Fixed: 3
- Declined:
  - none
- Deferred:
  - none
- Not probed:
  - none

## Notes
'
block() { printf '%s' "${BLOCK//@SHA@/$1}"; }

# invoke BODY [LABELS] [BASE]: run the script as the workflow does, with the PR head
# at the `pr` branch. BASE defaults to `base`; an empty BASE leaves BASE_SHA unset.
invoke() {
    local body="$1" labels="${2:-}" base="${3-base}"
    EXIT=0
    OUT="$(cd "$REPO" && env -u BASE_SHA ${base:+"BASE_SHA=$base"} \
        PR_BODY="$body" LABELS="$labels" HEAD_SHA=pr \
        bash "$SCRIPT" 2> "$TMP_ROOT/stderr")" || EXIT=$?
    ERR="$(cat "$TMP_ROOT/stderr")"
}

report() {
    local name="$1" why="$2" ok="$3"
    echo "Running: $name"
    if [ "$ok" = 1 ]; then
        echo "  PASS"
    else
        echo "  FAIL"
        echo "  Why it matters: $why"
        echo "  Exit: $EXIT"
        echo "  Stdout: $OUT"
        echo "  Stderr: $ERR"
        FAILED=1
    fi
}

# --- accepted ---

invoke "$(block "$SHA_first")"
ok=0
[[ $EXIT -eq 0 && "$OUT" == *"$SHA_first"* ]] && ok=1
report "a block naming one of the PR's commits passes" \
    "the check exists to accept exactly this, and its output names the commit the loop read" "$ok"

body="$(block "\`$SHA_second\`")"
invoke "${body//$'\n'/$'\r\n'}"
ok=0
[[ $EXIT -eq 0 ]] && ok=1
report "CRLF line endings and a backticked sha pass" \
    "GitHub stores descriptions with CRLF, and the skill's block may wrap the sha in backticks" "$ok"

invoke "" "ci-macos,no-agent-review"
ok=0
[[ $EXIT -eq 0 && "$OUT" == *"no-agent-review"* ]] && ok=1
report "the no-agent-review label passes without a block" \
    "the label is the only opt-out for a PR whose author cannot run the loop" "$ok"

# 100 KB is over a pipe buffer and under Linux's 128 KiB cap on one environment
# string. Against a piped `grep -q` this fails on most Linux runs but not every one,
# because whether the writer is killed is a race.
body="$(block "$SHA_first")"
long="$(printf '%999s\n' '' | tr ' ' x)"
for _ in $(seq 100); do body+=$'\n'"$long"; done
invoke "$body"
ok=0
[[ $EXIT -eq 0 ]] && ok=1
report "a 100 KB description does not hide the section" \
    "a grep -q that exits early can kill its writer with SIGPIPE, which pipefail reads as no match" "$ok"

# --- refused ---

invoke $'## Summary\n\nNo section here.\n'
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"no '## Reviewer loop' section"* ]] && ok=1
report "a description without the section is refused" \
    "a PR opened without running the loop has to fail the check" "$ok"

invoke "$(block '<sha>')"
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"does not name a commit sha"* ]] && ok=1
report "the template's unfilled placeholder is refused" \
    "the template carries the block's shape, so an untouched template must not pass" "$ok"

invoke "$(block 0123456789abcdef)"
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"is not a commit in this repository"* ]] && ok=1
report "a sha that names no commit is refused" \
    "a made-up sha cannot record which commit the loop read" "$ok"

invoke "$(block "$SHA_base")"
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"a commit of the base branch"* ]] && ok=1
report "a base-branch commit is refused" \
    "the base tip is an ancestor of the PR head, so ancestry alone would accept a review of none of the PR's code" "$ok"

invoke "$(block "$SHA_elsewhere")"
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"is not on this branch"* ]] && ok=1
report "a commit on another branch is refused" \
    "a block copied from another PR names a commit this branch does not contain" "$ok"

for heading in "Fixed:" "Declined:" "Deferred:" "Not probed:"; do
    body="$(block "$SHA_first")"
    invoke "${body/"- $heading"/- Other:}"
    ok=0
    [[ $EXIT -eq 1 && "$ERR" == *"no '- $heading' line"* ]] && ok=1
    report "a block without the '- $heading' line is refused" \
        "the four ledger headings are what hands the open findings to the human reviewer" "$ok"
done

body="$(block "$SHA_first")"
heading='## Reviewer loop'
other='## Something else'
invoke "${body/"$heading"/"$other"}"
ok=0
[[ $EXIT -eq 1 ]] && ok=1
report "the block's lines under another heading do not count" \
    "only the Reviewer loop section is read, so a stray copy elsewhere must not satisfy it" "$ok"

invoke $'## Summary\n\n## Reviewer loop\n\n\n## Notes\n'
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"does not name a commit sha"* ]] && ok=1
report "an empty section is reported as unfilled, not missing" \
    "the author added the heading and needs to be told what to put under it" "$ok"

body="$(block "$SHA_first")"
invoke $'## Reviewer loop\n\n'"${body/"$heading"/"$other"}"
ok=0
[[ $EXIT -eq 1 && "$ERR" == *"does not name a commit sha"* ]] && ok=1
report "a filled block under another heading does not fill an empty section" \
    "the ledger is read from the Reviewer loop section only, not from anywhere in the description" "$ok"

invoke "$(block "$SHA_base")" "" no-such-ref
ok=0
[[ $EXIT -eq 2 && "$ERR" == *"cannot resolve 'no-such-ref'"* ]] && ok=1
report "an unresolvable base is an error, not a pass" \
    "merge-base exits 128 on an unknown ref, which an if reads as not-an-ancestor and would let a base commit through" "$ok"

invoke "$(block "$SHA_base")" "" ""
ok=0
[[ $EXIT -eq 2 && "$ERR" == *"BASE_SHA"* ]] && ok=1
report "an unset BASE_SHA is an error, not a default" \
    "the base branch is renamed with each DuckDB version bump, so a default branch name would go stale" "$ok"

# --- the published block shape ---

EXIT=0 OUT="" ERR=""
if filled="$(sed "s/^- Reviewed at:.*/- Reviewed at: $SHA_second · rounds: 1 · stopped: nothing new/" "$TEMPLATE")"; then
    invoke "$filled"
fi
ok=0
[[ -n "${filled:-}" && $EXIT -eq 0 ]] && ok=1
report "the PR template passes once its sha is filled in" \
    "the template carries a copy of the block, and it has to be the shape the script accepts" "$ok"

# The fenced example under the skill's PR-body heading.
EXIT=0 OUT="" ERR=""
example="$(awk '/^## Then write the PR-body block/ {s = 1} s && /^```markdown$/ {on = 1; next} on && /^```$/ {exit} on' \
    "$SKILL" 2> /dev/null)" || true
if [ -n "$example" ]; then
    invoke "$(sed "s/^- Reviewed at:.*/- Reviewed at: $SHA_second · rounds: 1 · stopped: nothing new/" <<< "$example")"
fi
ok=0
[[ -n "$example" && $EXIT -eq 0 ]] && ok=1
report "the skill's example block passes once its sha is filled in" \
    "the skill prints the block authors paste, and it has to be the shape the script accepts" "$ok"

echo ""
if [ $FAILED -eq 0 ]; then
    ALL_PASSED=1
    echo "All tests passed!"
    exit 0
else
    echo "Some tests failed! Scenario repository kept in $TMP_ROOT"
    exit 1
fi
