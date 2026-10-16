#!/bin/bash
# Scenario tests for test/scripts/run_tests_parallel.py, the runner behind
# `MIINT_TEST_JOBS=N bash run_tests.sh`: that it runs every SQL test file once, that a
# failure or a crash anywhere fails the run, and that skips are reported per reason as
# the serial pass reports them.
#
# Each scenario runs the real unittest binary against a throwaway test root (unittest's
# --test-dir) holding generated .test files, so no fixture sits under this repository's
# test/ for the main pass to pick up. The C++ test binary is a stub. The root is removed
# when all scenarios pass and kept otherwise.
#
# Usage: bash test/shell/run_tests_parallel.sh [script-under-test]

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
SCRIPT="${1:-$REPO_ROOT/test/scripts/run_tests_parallel.py}"
UNITTEST="$REPO_ROOT/build/release/test/unittest"
FAILED=0
ALL_PASSED=0

# The skip scenario relies on these being unset.
unset MIINT_RTP_UNSET_A MIINT_RTP_UNSET_B

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/run_tests_parallel.XXXXXX")"
trap 'if [ "$ALL_PASSED" = 1 ]; then rm -rf "$TMP_ROOT"; fi' EXIT

mkdir "$TMP_ROOT/bin"
CPP_OK="$TMP_ROOT/bin/cpp_ok"
printf '#!/bin/sh\necho "All tests passed (1 assertion in 1 test case)"\n' > "$CPP_OK"
CPP_CRASH="$TMP_ROOT/bin/cpp_crash"
printf '#!/bin/sh\nkill -SEGV $$\n' > "$CPP_CRASH"
chmod +x "$CPP_OK" "$CPP_CRASH"

pass_test() { printf 'query I\nSELECT 1;\n----\n1\n'; }
fail_test() { printf 'query I\nSELECT 1;\n----\n2\n'; }
skip_test() { printf 'require-env %s\n\nquery I\nSELECT 1;\n----\n1\n' "$1"; }

# root NAME: a fresh test root; fixtures go in $ROOT/test/sql.
root() {
    ROOT="$TMP_ROOT/$1"
    mkdir -p "$ROOT/test/sql"
}

# invoke CPP_TESTS [SPEC]: run the script on $ROOT with two workers.
invoke() {
    EXIT=0
    OUT="$(python3 "$SCRIPT" --jobs 2 --unittest "$UNITTEST" --cpp-tests "$1" --test-dir "$ROOT" \
        ${2:+"$2"} 2>&1)" || EXIT=$?
}

# ran_once STATUS FILE: FILE's progress line appears exactly once, with STATUS.
ran_once() {
    [ "$(grep -cE "^\[[0-9]+/[0-9]+\] +$1 +[0-9.]+s +test/sql/$2\$" <<< "$OUT")" = 1 ]
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
        echo "  Output: $OUT"
        FAILED=1
    fi
}

root all_pass
for f in a b c; do pass_test > "$ROOT/test/sql/$f.test"; done
invoke "$CPP_OK"
ok=0
[[ $EXIT -eq 0 ]] && ran_once ok a.test && ran_once ok b.test && ran_once ok c.test &&
    [[ "$OUT" == *"SQL test files: 3 passed, 0 skipped, 0 failed"* ]] && ok=1
report "every listed file runs exactly once" \
    "a file the parallel run drops or repeats is coverage the serial pass had and this one reports wrongly" "$ok"

root one_fails
pass_test > "$ROOT/test/sql/good.test"
fail_test > "$ROOT/test/sql/bad.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -ne 0 ]] && ran_once FAIL bad.test && ran_once ok good.test &&
    [[ "$OUT" == *"Wrong result"* ]] && ok=1
report "a failing file fails the run, prints its output, and the rest still run" \
    "a lost exit status leaves the run green over a broken test; the output is the only diagnosis" "$ok"

root skips
pass_test > "$ROOT/test/sql/runs.test"
skip_test MIINT_RTP_UNSET_A > "$ROOT/test/sql/skip_a1.test"
skip_test MIINT_RTP_UNSET_A > "$ROOT/test/sql/skip_a2.test"
skip_test MIINT_RTP_UNSET_B > "$ROOT/test/sql/skip_b.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -eq 0 ]] && ran_once skip skip_a1.test && ran_once ok runs.test &&
    [[ "$OUT" == *"SQL test files: 1 passed, 3 skipped, 0 failed"* ]] &&
    grep -qx 'require-env MIINT_RTP_UNSET_A: 2' <<< "$OUT" &&
    grep -qx 'require-env MIINT_RTP_UNSET_B: 1' <<< "$OUT" && ok=1
report "skips are counted per reason across files" \
    "a change in the skip count is how a reviewer sees coverage move, so it must match the serial pass's report" "$ok"

root cpp_crash
pass_test > "$ROOT/test/sql/fine.test"
invoke "$CPP_CRASH"
ok=0
[[ $EXIT -ne 0 ]] && grep -qE '^\[[0-9]+/[0-9]+\] +FAIL .*C\+\+ tests' <<< "$OUT" && ok=1
report "a crashing C++ test binary fails the run" \
    "a signal is a negative exit status in Python, and a check for a positive one would pass a segfault" "$ok"

root spill_dir
printf "query I\nSELECT current_setting('temp_directory') = '__TEST_DIR__/spill';\n----\ntrue\n" \
    > "$ROOT/test/sql/spill.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -eq 0 ]] && ran_once ok spill.test && ok=1
report "each SQL process spills into its own test directory" \
    "workers share a working directory, and two DuckDB processes spilling to its .tmp overwrite and delete each other's files" "$ok"

root odd_name
pass_test > "$ROOT/test/sql/fine.test"
pass_test > "$ROOT/test/sql/fine.test,odd.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -ne 0 && "$OUT" == *"test/sql/fine.test,odd.test"* ]] && ok=1
report "a file name that selects another file fails the run" \
    "Catch2 reads a comma in a name as 'or': this name runs fine.test, and only -w NoTests turns the miss into a failure" "$ok"

root no_match
pass_test > "$ROOT/test/sql/present.test"
invoke "$CPP_OK" "test/sql/absent*"
ok=0
[[ $EXIT -ne 0 && "$OUT" == *"test/sql/absent* matches no test file"* ]] && ok=1
report "a spec that matches no file fails the run" \
    "unittest exits 0 on 'No tests ran', so an empty selection would otherwise pass" "$ok"

echo ""
if [ $FAILED -eq 0 ]; then
    ALL_PASSED=1
    echo "All tests passed!"
    exit 0
else
    echo "Some tests failed! Scenario roots kept in $TMP_ROOT"
    exit 1
fi
