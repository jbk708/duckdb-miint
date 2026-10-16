#!/usr/bin/env python3
"""Run the SQL test files and the C++ tests on parallel workers.

`MIINT_TEST_JOBS=N bash run_tests.sh` runs this in place of `make test` followed by the
C++ test binary. The SQL files are the ones `make test` runs: unittest lists them from
the spec `make test` passes it, test/*, this script's default. Each runs in its own
unittest process, and so gets its own __TEST_DIR__, which unittest names by process id.
The C++ binary is one more job, queued first because it is the longest.

Every job runs to completion. The run fails if any job failed, after printing each failed
job's output, and it reports skips per reason as the serial pass does.
"""

import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys
import time

CPP_JOB = "C++ tests"
SKIP_HEADER = "Skipped tests for the following reasons:"


def run(name, cmd, env, timeout):
    """(name, exit status or None if the job timed out, output, seconds)."""
    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=timeout
        )
        code, output = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as expired:
        # run() has already killed the job. A deadlocked test (#318) would otherwise stall
        # the whole run with nothing printed.
        code, output = None, expired.stdout or b""
    return name, code, output.decode(errors="replace"), time.monotonic() - start


def skip_reasons(output):
    """The `reason: count` lines unittest prints under its skip header."""
    reasons = collections.Counter()
    lines = output.splitlines()
    if SKIP_HEADER not in lines:
        return reasons
    for line in lines[lines.index(SKIP_HEADER) + 1 :]:
        match = re.fullmatch(r"(.+): (\d+)", line)
        if not match:
            break
        reasons[match.group(1)] += int(match.group(2))
    return reasons


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--jobs", type=int, required=True, help="worker processes")
    parser.add_argument("--unittest", required=True, help="DuckDB's unittest binary")
    parser.add_argument("--cpp-tests", required=True, help="miint's Catch2 test binary")
    parser.add_argument(
        "--timeout", type=int, default=600, help="seconds a job may run before it is killed and fails (default 600)"
    )
    parser.add_argument("--test-dir", help="passed to unittest, which registers the .test files under DIR/test")
    parser.add_argument("spec", nargs="?", default="test/*", help="unittest test spec (default: test/*, as make test)")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be at least 1")

    unittest = [args.unittest] + (["--test-dir", args.test_dir] if args.test_dir else [])
    listing = subprocess.run(
        unittest + ["--list-test-names-only", args.spec],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        text=True,
    )
    files = [line for line in listing.stdout.splitlines() if line.strip()]
    # Catch2 exits a listing with the number of names it listed, truncated to a byte, so
    # anything else, a signal included, means the listing did not finish.
    if listing.returncode != len(files) % 256:
        sys.exit(f"error: listing {args.spec} exited {listing.returncode} after {len(files)} names")
    if not files:
        # unittest itself exits 0 on "No tests ran".
        sys.exit(f"error: {args.spec} matches no test file")

    # Every worker shares the working directory, and so the .tmp an in-memory database spills
    # to, where processes corrupt each other's spill files (docs/internals/duckdb-engine-notes.md,
    # "Storage"). unittest runs on_init whenever it opens a database, with __TEST_DIR__
    # expanded to the process's own directory; temp_directory is database-wide. It also runs
    # it on `reconnect`, so a test that spills and then reconnects fails here alone: DuckDB
    # refuses to move a temp directory in use.
    sql_env = dict(os.environ)
    sql_env["DUCKDB_TEST_ON_INIT"] = ";".join(
        filter(None, [os.environ.get("DUCKDB_TEST_ON_INIT"), "SET temp_directory='__TEST_DIR__/spill'"])
    )
    # The load these workers make can overflow rype's rayon stacks (docs/testing.md,
    # the-miint/RYpe#28). A caller's own value wins. Drop this once RYpe#28 is fixed.
    sql_env.setdefault("RUST_MIN_STACK", "4194304")

    # -w NoTests: a name that matches nothing fails instead of passing as "No tests ran".
    jobs = [(CPP_JOB, [args.cpp_tests], None)] + [(f, unittest + ["-w", "NoTests", f], sql_env) for f in files]
    print(f"Running {len(files)} SQL test files and the C++ tests on {args.jobs} workers...", flush=True)

    failed = []
    statuses = collections.Counter()
    reasons = collections.Counter()
    assertions = 0
    cpp_summary = ""
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run, name, cmd, env, args.timeout) for name, cmd, env in jobs]
        try:
            for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
                name, code, output, seconds = future.result()
                # A signal is a negative return code and a timeout is None, so test for
                # nonzero, not positive.
                if code != 0:
                    status = "FAIL"
                    failed.append((name, code, output))
                elif "All tests were skipped" in output:
                    status = "skip"
                else:
                    status = "ok"
                if name == CPP_JOB:
                    cpp_summary = output.strip().splitlines()[-1] if output.strip() else "(no output)"
                else:
                    statuses[status] += 1
                    reasons.update(skip_reasons(output))
                    match = re.search(r"(\d+) assertions? in \d+ test case", output)
                    assertions += int(match.group(1)) if match else 0
                print(f"[{done}/{len(jobs)}] {status:<4} {seconds:6.1f}s  {name}", flush=True)
        except KeyboardInterrupt:
            # The running jobs got the terminal's SIGINT too; without this the pool would
            # go on starting every queued one before the interrupt took effect.
            for future in futures:
                future.cancel()
            raise

    for name, code, output in failed:
        reason = f"exit {code}" if code is not None else f"timed out after {args.timeout}s"
        print(f"\n==== FAILED: {name} ({reason}) ====\n{output}")
    print(
        f"\nSQL test files: {statuses['ok']} passed, {statuses['skip']} skipped, {statuses['FAIL']} failed"
        f" ({assertions} assertions)"
    )
    if reasons:
        print(SKIP_HEADER)
        for reason, count in sorted(reasons.items()):
            print(f"{reason}: {count}")
    print(f"C++ tests: {'FAILED, see above' if any(f[0] == CPP_JOB for f in failed) else cpp_summary}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
