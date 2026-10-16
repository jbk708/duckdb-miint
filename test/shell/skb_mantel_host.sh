#!/bin/bash
# skb_mantel against fake skb-arrow hosts: how each failure surfaces, and what an
# interrupt leaves behind. Fake hosts are bash scripts speaking skb-arrow's
# protocol, so this needs skb_mantel compiled in, not skb-arrow installed.

set -u

DUCKDB=./build/release/duckdb
if ! echo "SELECT 1 FROM duckdb_functions() WHERE function_name = 'skb_mantel';" | $DUCKDB -csv -noheader 2>/dev/null | grep -q 1; then
    echo "skb_mantel not compiled in; skipping"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/miint_skb_mantel_host.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
SEGDIR="$WORK/segments"
mkdir "$SEGDIR"
FAILED=0

READY='{"type":"ready","protocol_version":1,"host_version":"9.9.9","capabilities":{"ancombc":1,"mantel":1},"id":null,"warnings":[]}'

# A fake host: the real host's startup check, the handshake, one call read,
# then `body`.
fake_host() {
    local path="$WORK/$1" body="$2"
    cat > "$path" <<EOF
#!/bin/bash
[ "\$1" = --segment-dir ] && [ -d "\$2" ] && [ -z "\$(ls -A "\$2")" ] || exit 2
IFS= read -r line
printf '%s\n' '$READY'
IFS= read -r line
$body
EOF
    chmod +x "$path"
    echo "$path"
}

# A body answering the call with an error of `kind`, then idling until stdin closes.
error_body() {
    printf "printf '%%s\\\\n' '{\"type\":\"error\",\"id\":1,\"kind\":\"%s\",\"message\":\"fake %s failure\",\"traceback\":\"Traceback: fake frame\",\"warnings\":[]}'\nwhile IFS= read -r line; do :; done" "$1" "$1"
}

SETUP="CREATE TABLE x AS SELECT * FROM (VALUES ('a','b',1.0::DOUBLE),('a','c',2.0),('b','c',3.0)) t(sample_a, sample_b, distance);"

# Run skb_mantel against `host`, then a second query in the same session.
run_with_host() {
    local host="$1"
    printf '%s\nSELECT * FROM skb_mantel('"'"'x'"'"', '"'"'x'"'"');\nSELECT 42 AS still_alive;\n' "$SETUP" |
        MIINT_SKB_ARROW_PATH="$host" MIINT_SKB_ARROW_SEGMENT_DIR="$SEGDIR" timeout 60 $DUCKDB -no-agent -csv 2>&1
}

expect() {
    local name="$1" out="$2" pattern="$3"
    if echo "$out" | grep -qE -- "$pattern"; then
        echo "  ✓ $name"
    else
        echo "  ✗ $name: expected /$pattern/ in:"
        echo "$out" | sed 's/^/      /'
        FAILED=1
    fi
}

reject() {
    local name="$1" out="$2" pattern="$3"
    if echo "$out" | grep -qE -- "$pattern"; then
        echo "  ✗ $name: unexpected /$pattern/ in:"
        echo "$out" | sed 's/^/      /'
        FAILED=1
    else
        echo "  ✓ $name"
    fi
}

# The session survives every failure: an InternalException would invalidate
# the database, and the follow-up query would fail.
survives() {
    expect "$1: the session still answers" "$2" "^42$"
    reject "$1: never an internal error" "$2" "INTERNAL Error|invalidated"
}

echo "Running: error kinds map to DuckDB exceptions"
for case in "invalid_input|Invalid Input Error: skb_mantel: fake invalid_input failure" \
    "invalid_param|Invalid Input Error: skb_mantel: fake invalid_param failure" \
    "unsupported|Not implemented Error: skb_mantel: fake unsupported failure" \
    "resource|IO Error: skb_mantel: fake resource failure.*MIINT_SKB_ARROW_SEGMENT_DIR" \
    "host_incompatible|IO Error: skb_mantel: .*fake host_incompatible failure" \
    "internal|IO Error: skb_mantel: skb-arrow internal error \(please report\): fake internal failure" \
    "no_such_kind|IO Error: skb_mantel: skb-arrow internal error \(please report\)"; do
    kind="${case%%|*}"
    out=$(run_with_host "$(fake_host "host_$kind" "$(error_body "$kind")")")
    expect "$kind" "$out" "${case#*|}"
    survives "$kind" "$out"
done

out=$(run_with_host "$(fake_host host_internal_tb "$(error_body internal)")")
expect "internal carries the host's traceback" "$out" "Traceback: fake frame"

echo "Running: a host that dies mid-call"
out=$(run_with_host "$(fake_host host_dies 'echo "about to die" >&2; kill -9 $$')")
expect "reported as an internal error" "$out" "IO Error: skb_mantel: skb-arrow internal error \(please report\)"
expect "with how it died" "$out" "killed by signal 9"
expect "and its last words" "$out" "about to die"
survives "host death" "$out"

echo "Running: no skb-arrow at the configured path"
out=$(run_with_host "$WORK/no-such-skb-arrow")
expect "says how to install it" "$out" "IO Error: skb_mantel: .*SELECT install_skb_arrow\(\)"
survives "missing binary" "$out"

expect "no session directory is left behind" "$(ls -A "$SEGDIR")" "^$"

echo "Running: SIGINT kills the host and everything it started"
host=$(fake_host host_hangs 'sleep 300 & wait')
printf '%s\nSELECT * FROM skb_mantel('"'"'x'"'"', '"'"'x'"'"');\n' "$SETUP" > "$WORK/hang.sql"
MIINT_SKB_ARROW_PATH="$host" MIINT_SKB_ARROW_SEGMENT_DIR="$SEGDIR" $DUCKDB -no-agent -csv -f "$WORK/hang.sql" > "$WORK/hang.out" 2>&1 &
duck=$!
hostpid=""
for _ in $(seq 1 200); do
    hostpid=$(pgrep -f -- "$host --segment-dir" | head -1)
    [ -n "$hostpid" ] && [ "$(pgrep -g "$hostpid" | wc -l)" -ge 2 ] && break
    sleep 0.05
done
if [ -z "$hostpid" ]; then
    echo "  ✗ the fake host never started"
    cat "$WORK/hang.out"
    kill -9 "$duck" 2>/dev/null
    exit 1
fi
kill -INT "$duck"
for _ in $(seq 1 100); do kill -0 "$duck" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$duck" 2>/dev/null; then
    echo "  ✗ duckdb still running 10 s after SIGINT"
    kill -9 "$duck"
    FAILED=1
else
    echo "  ✓ the query stops"
fi
wait "$duck" 2>/dev/null
for _ in $(seq 1 50); do [ -z "$(pgrep -g "$hostpid")" ] && break; sleep 0.1; done
expect "no process of the host's group survives" "$(pgrep -g "$hostpid" || true)" "^$"
expect "its session directory is removed" "$(ls -A "$SEGDIR")" "^$"

if [ "$FAILED" -ne 0 ]; then
    echo "skb_mantel_host.sh: FAILED"
    exit 1
fi
echo "All tests passed!"
