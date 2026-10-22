# Sourced by run_tests.sh and onboarding-tutorials/run_tutorial_tests.sh, which point
# miint's ENA requests at test/scripts/ena_webin_mock.py (docs/testing.md, "Guards and
# skipped tests").

# start_ena_mock: start the mock on a free port, set ENA_MOCK_PID, and export
# ENA_WEBIN_MOCK_URL and the base URLs miint reads from the environment, which send these
# requests to the mock (Webin submissions take their URL from the ENA secret instead):
# - MIINT_ENA_PORTAL_URL_BASE: every Portal API request (ResolvePortalBaseFromEnv);
# - MIINT_ENA_CHECKLIST_URL_BASE: the checklist XMLs INSERT INTO ena.samples validates
#   attributes against;
# - MIINT_ENA_REPORTS_URL_BASE: the Reports API, which the lifecycle functions and
#   DELETE-by-alias use to turn an alias into an accession.
# Returns 1, with the mock's output on stderr, when it has not started within 5 s.
start_ena_mock() {
    local boot
    boot=$(mktemp)
    python3 "$(dirname "${BASH_SOURCE[0]}")/ena_webin_mock.py" 0 > "$boot" 2>&1 &
    ENA_MOCK_PID=$!
    ENA_WEBIN_MOCK_URL=""
    for _ in $(seq 1 50); do
        ENA_WEBIN_MOCK_URL=$(sed -n 's/^ENA_WEBIN_MOCK_URL=//p' "$boot")
        [ -n "$ENA_WEBIN_MOCK_URL" ] && break
        sleep 0.1
    done
    if [ -z "$ENA_WEBIN_MOCK_URL" ]; then
        cat "$boot" >&2
        rm -f "$boot"
        kill "$ENA_MOCK_PID" 2>/dev/null || true
        return 1
    fi
    rm -f "$boot"
    export ENA_WEBIN_MOCK_URL
    export MIINT_ENA_PORTAL_URL_BASE="$ENA_WEBIN_MOCK_URL/portal/api"
    export MIINT_ENA_CHECKLIST_URL_BASE="$ENA_WEBIN_MOCK_URL/browser/api/xml"
    export MIINT_ENA_REPORTS_URL_BASE="$ENA_WEBIN_MOCK_URL/submit/report"
}

# unset_ena_mock_env: send miint's ENA requests to ENA itself again.
unset_ena_mock_env() {
    unset MIINT_ENA_PORTAL_URL_BASE MIINT_ENA_CHECKLIST_URL_BASE MIINT_ENA_REPORTS_URL_BASE
}
