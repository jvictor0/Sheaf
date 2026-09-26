#!/usr/bin/env bash
# sru-61's zero-length-discovery regression guard: a permanent, scripted
# rerun of the empty-discovery scenario task 1.3 verified by hand under
# literal /bin/bash 3.2.57 but never wired into the gate.
#
# check_ui_boundary.sh discovers app-producer headers with
# `rg --files apps -g '*.hpp' ...` into a bash array, then reads it back
# through `${arr[@]+"${arr[@]}"}` guards everywhere after (sru-61). Before
# that fix, macOS system bash 3.2's `set -u` treated a bare `"${arr[@]}"` on
# a zero-element array as an unbound-variable reference and aborted the
# script before the discovery-floor diagnostic -- the one that exists
# precisely for an empty discovery -- ever ran.
#
# This drives the real script end to end, with PATH prefixed by a stub `rg`
# that empties only the one discovery call the scenario needs and forwards
# every other invocation -- the JUCE boundary scan, backend discovery, the
# scanner self-tests -- to the real ripgrep, so the rest of the inspection
# runs unmodified and still has to pass. Run under literal /bin/bash (macOS
# system bash 3.2.57), the interpreter sru-61 was specific to, not whatever
# `bash` a dev machine's PATH happens to resolve first.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
SYNTH_ROOT="$(pwd)"

REAL_RG="$(command -v rg || true)"
if [ -z "$REAL_RG" ]; then
    printf 'check_ui_boundary_empty_discovery.sh requires ripgrep (rg)\n' >&2
    exit 2
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

STUB_DIR="$TMP_DIR/stub-bin"
mkdir -p "$STUB_DIR"
STUB_RG="$STUB_DIR/rg"
cat >"$STUB_RG" <<EOF
#!/bin/sh
# Empties exactly the app-producer discovery call
# ("rg --files apps -g '*.hpp' ...") and forwards every other invocation to
# the real ripgrep -- check_ui_boundary.sh's own JUCE-boundary scan, backend
# discovery, and scanner self-tests all still have to pass through this
# stub, which is what proves the empty case is isolated rather than the
# whole script being disabled.
if [ "\$1" = "--files" ] && [ "\$2" = "apps" ]; then
    exit 1
fi
exec "$REAL_RG" "\$@"
EOF
chmod +x "$STUB_RG"

failed=0
fail() {
    printf '%s\n' "$1" >&2
    failed=1
}

# Positive control: prove the stub actually empties the app-producer
# discovery before trusting anything check_ui_boundary.sh does with it --
# the exact command check_ui_boundary.sh runs at its own discovery site.
stub_discovery_count="$({ PATH="$STUB_DIR:$PATH" "$STUB_RG" --files apps -g '*.hpp' || true; } | wc -l | tr -d ' ')"
if [ "$stub_discovery_count" -ne 0 ]; then
    fail "check_ui_boundary_empty_discovery.sh: stub rg did not empty the apps discovery (found $stub_discovery_count entries)"
fi

output="$(PATH="$STUB_DIR:$PATH" /bin/bash "$SYNTH_ROOT/scripts/check_ui_boundary.sh" 2>&1)" && status=0 || status=$?

if [ "$status" -eq 0 ]; then
    fail "check_ui_boundary_empty_discovery.sh: check_ui_boundary.sh unexpectedly passed with app-producer discovery emptied:
$output"
fi

if printf '%s' "$output" | grep -qi "unbound variable"; then
    fail "check_ui_boundary_empty_discovery.sh: check_ui_boundary.sh aborted on an unbound-variable reference instead of reaching the discovery-floor diagnostic (sru-61 regression):
$output"
fi

if ! printf '%s' "$output" | grep -q "app producer discovery found only 0 headers; expected at least"; then
    fail "check_ui_boundary_empty_discovery.sh: check_ui_boundary.sh did not report the discovery-floor diagnostic for the emptied array:
$output"
fi

if [ "$failed" -ne 0 ]; then
    exit 1
fi
printf 'check_ui_boundary_empty_discovery.sh: OK (empty app-producer discovery reaches the floor diagnostic under /bin/bash 3.2, no unbound-variable abort)\n'
