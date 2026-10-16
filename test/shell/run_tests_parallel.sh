#!/bin/bash
# Scenario tests for test/scripts/run_tests_parallel.py, the runner behind
# `MIINT_TEST_JOBS=N bash run_tests.sh`. It must run exactly the files the serial pass runs,
# fail the run on any failure it can see, and report what the serial pass reports; each
# scenario says which part of that it pins and why.
#
# Scenarios run the real unittest binary against a throwaway test root (unittest's
# --test-dir) holding generated .test files, so no fixture sits under this repository's
# test/ for the serial pass to pick up. Three use a unittest stub for what the real one
# cannot be made to do: die partway through its listing, and report the environment its
# job saw. The C++ test binary is always a stub. The root is removed when all scenarios
# pass and kept otherwise.
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
# SIGKILL rather than SIGSEGV: also a negative exit status, and no core file in the caller's
# working directory.
CPP_KILLED="$TMP_ROOT/bin/cpp_killed"
printf '#!/bin/sh\nkill -KILL $$\n' > "$CPP_KILLED"
# A unittest whose listing dies after one name, as a crash partway through would.
UNITTEST_DIES="$TMP_ROOT/bin/unittest_dies"
printf '#!/bin/sh\necho test/sql/listed.test\nkill -KILL $$\n' > "$UNITTEST_DIES"
# A C++ binary that hangs, as a deadlocked test does; it records its pid so the scenario can
# check it was killed.
CPP_HANGS="$TMP_ROOT/bin/cpp_hangs"
printf '#!/bin/sh\necho $$ > "%s"\nexec sleep 30\n' "$TMP_ROOT/cpp_hangs.pid" > "$CPP_HANGS"
chmod +x "$CPP_HANGS"
# A unittest that lists one file and passes it only if the job sees EXPECT_RUST_MIN_STACK.
UNITTEST_STACK="$TMP_ROOT/bin/unittest_stack"
printf '#!/bin/sh\ncase "$*" in *--list-test-names-only*) echo test/sql/stack.test; exit 1 ;; esac\necho "RUST_MIN_STACK=${RUST_MIN_STACK-unset}"\n[ "${RUST_MIN_STACK-}" = "$EXPECT_RUST_MIN_STACK" ]\n' \
    > "$UNITTEST_STACK"
chmod +x "$CPP_OK" "$CPP_KILLED" "$UNITTEST_DIES" "$UNITTEST_STACK"

pass_test() { printf 'query I\nSELECT 1;\n----\n1\n'; }
fail_test() { printf 'query I\nSELECT 1;\n----\n2\n'; }
skip_test() { printf 'require-env %s\n\nquery I\nSELECT 1;\n----\n1\n' "$1"; }

# root NAME: a fresh test root; fixtures go in $ROOT/test/sql.
root() {
    ROOT="$TMP_ROOT/$1"
    mkdir -p "$ROOT/test/sql"
}

# invoke CPP_TESTS [ARG...]: run the script on $ROOT with two workers. A later --jobs or
# --unittest in ARG overrides the default, and a bare ARG is the spec.
invoke() {
    EXIT=0
    OUT="$(python3 "$SCRIPT" --jobs 2 --unittest "$UNITTEST" --cpp-tests "$1" --test-dir "$ROOT" \
        "${@:2}" 2>&1)" || EXIT=$?
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
report "a failing file fails the run and prints its output" \
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

# One worker: the C++ job is queued first, so these files start only after it has failed.
# The worker takes the next job the moment one ends, so a run that stopped on the failure
# could still start the first of them; the other two show whether it went on.
root cpp_killed
for f in a b c; do pass_test > "$ROOT/test/sql/$f.test"; done
invoke "$CPP_KILLED" --jobs 1
ok=0
[[ $EXIT -ne 0 ]] && grep -qE '^\[[0-9]+/[0-9]+\] +FAIL .*C\+\+ tests' <<< "$OUT" &&
    ran_once ok a.test && ran_once ok b.test && ran_once ok c.test && ok=1
report "a C++ binary killed by a signal fails the run, and the queued files still run" \
    "a signal is a negative exit status in Python, which a check for a positive one passes; and one failure must not hide the rest" "$ok"

root hang
for f in a b; do pass_test > "$ROOT/test/sql/$f.test"; done
started=$SECONDS
invoke "$CPP_HANGS" --timeout 2
elapsed=$((SECONDS - started))
ok=0
[[ $EXIT -ne 0 && $elapsed -lt 20 && "$OUT" == *"C++ tests (timed out after 2s)"* ]] &&
    grep -qE '^\[[0-9]+/[0-9]+\] +FAIL .*C\+\+ tests' <<< "$OUT" && ran_once ok a.test && ran_once ok b.test &&
    ! kill -0 "$(cat "$TMP_ROOT/cpp_hangs.pid")" 2> /dev/null && ok=1
report "a job past the timeout is killed and fails the run, and the others still run" \
    "a deadlocked test otherwise stalls the run with no output, and a job left running holds the box" "$ok"

root spill_dir
printf "query I\nSELECT current_setting('temp_directory') = '__TEST_DIR__/spill';\n----\ntrue\n" \
    > "$ROOT/test/sql/spill.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -eq 0 ]] && ran_once ok spill.test && ok=1
report "each SQL process spills into its own test directory" \
    "workers share a working directory, and two DuckDB processes spilling to its .tmp overwrite and delete each other's files" "$ok"

root rust_stack
unset RUST_MIN_STACK
export EXPECT_RUST_MIN_STACK=4194304
invoke "$CPP_OK" --unittest "$UNITTEST_STACK"
ok=0
[[ $EXIT -eq 0 ]] && ran_once ok stack.test && ok=1
report "SQL jobs run with a 4 MiB Rust thread stack" \
    "the load these workers make is enough for rype's rayon workers to overflow Rust's default 2 MiB stack (the-miint/RYpe#28)" "$ok"

export RUST_MIN_STACK=8388608 EXPECT_RUST_MIN_STACK=8388608
invoke "$CPP_OK" --unittest "$UNITTEST_STACK"
unset RUST_MIN_STACK EXPECT_RUST_MIN_STACK
ok=0
[[ $EXIT -eq 0 ]] && ran_once ok stack.test && ok=1
report "a caller's own RUST_MIN_STACK wins" \
    "a run probing the stack size, such as one checking whether RYpe#28 is fixed, must get the value it set" "$ok"

root odd_name
pass_test > "$ROOT/test/sql/fine.test"
pass_test > "$ROOT/test/sql/fine.test,odd.test"
invoke "$CPP_OK"
ok=0
[[ $EXIT -ne 0 ]] && ran_once FAIL 'fine.test,odd.test' && ran_once ok fine.test && ok=1
report "a file name that selects another file fails the run" \
    "Catch2 reads a comma in a name as 'or': this name runs fine.test, and only -w NoTests turns the miss into a failure" "$ok"

root listing_dies
invoke "$CPP_OK" --unittest "$UNITTEST_DIES"
ok=0
[[ $EXIT -ne 0 && "$OUT" == *"exited -9 after 1 names"* ]] && ! grep -qE '^\[[0-9]+/' <<< "$OUT" && ok=1
report "a listing that dies partway fails the run before any file runs" \
    "the names it printed are a subset of the suite, and running only those would pass as a full run" "$ok"

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
