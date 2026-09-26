#!/usr/bin/env bash
# sar-34's plist/executable coherence guard (runtime/juce_build.mk's
# $(APP_BUNDLE) rule), exercised end-to-end via scripted make invocations.
#
# A stub "binary" (an arbitrary executable file -- the bundle rule only
# copies it, never runs it) plus a tiny Info.plist fixture drive the real
# $(APP_BUNDLE) rule directly, with the JUCE-module and app-source
# prerequisite variables overridden to empty so make treats the stub as
# already up to date and never attempts a real JUCE compile.
#
# Two paths, both required by sar-34's spec:
#   1. CFBundleExecutable != APP_NAME -> the bundle rule must fail, its
#      message naming both the plist value and APP_NAME, and no bundle
#      (complete or partial) may be left behind.
#   2. CFBundleExecutable == APP_NAME -> the bundle rule must succeed and
#      produce the usual bundle layout.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
SYNTH_ROOT="$(pwd)"
JUCE_BUILD_MK="$SYNTH_ROOT/runtime/juce_build.mk"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

APP_NAME="StubApp"
BUILD_DIR="$TMP_DIR/build"
mkdir -p "$BUILD_DIR"

# Stub "binary": no real JUCE build needed -- the bundle rule only copies it.
STUB_APP="$BUILD_DIR/$APP_NAME"
printf '#!/bin/sh\necho stub\n' > "$STUB_APP"
chmod +x "$STUB_APP"

failed=0
fail() {
    printf '%s\n' "$1" >&2
    failed=1
}

# Drive just the $(APP_BUNDLE) rule: every variable that would otherwise
# pull in real app sources / JUCE modules is overridden to empty, so $(APP)'s
# prerequisite list is empty and the pre-created stub above is already up to
# date -- no compiler invocation happens.
make_bundle() {
    local plist="$1"
    make -f "$JUCE_BUILD_MK" "$BUILD_DIR/$APP_NAME.app" \
        APP_NAME="$APP_NAME" \
        APP_BUILD_DIR="$BUILD_DIR" \
        APP_INFO_PLIST="$plist" \
        APP_SOURCES= \
        SYNTH_SRC= \
        SYNTH_RUNTIME_SRC= \
        SYNTH_HEADERS= \
        SYNTH_JUCE_HEADERS= \
        JUCE_MODULE_SRC= \
        JUCE_MODULE_OBJ= \
        JUCE_C_MODULE_SRC= \
        JUCE_C_MODULE_OBJ=
}

write_plist() {
    local path="$1" executable="$2"
    cat > "$path" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>$executable</string>
</dict>
</plist>
EOF
}

# --- Path 1: mismatched plist must fail the bundle rule, naming both values,
# and leave no bundle behind. ---
MISMATCH_PLIST="$TMP_DIR/mismatch.plist"
write_plist "$MISMATCH_PLIST" "WrongName"

if output="$(make_bundle "$MISMATCH_PLIST" 2>&1)"; then
    fail "check_app_bundle_plist: mismatched plist unexpectedly built a bundle:
$output"
else
    if ! printf '%s' "$output" | grep -q "WrongName" \
        || ! printf '%s' "$output" | grep -q "$APP_NAME"; then
        fail "check_app_bundle_plist: failure message did not name both the plist value and APP_NAME:
$output"
    fi
fi
if [ -e "$BUILD_DIR/$APP_NAME.app" ]; then
    fail "check_app_bundle_plist: mismatched plist left a bundle behind at $BUILD_DIR/$APP_NAME.app"
fi

# --- Path 2: matching plist must build exactly as before. ---
MATCH_PLIST="$TMP_DIR/match.plist"
write_plist "$MATCH_PLIST" "$APP_NAME"

if ! output="$(make_bundle "$MATCH_PLIST" 2>&1)"; then
    fail "check_app_bundle_plist: matching plist failed to build:
$output"
fi
if [ ! -e "$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME" ]; then
    fail "check_app_bundle_plist: matching plist did not produce the bundle binary"
fi
if [ ! -e "$BUILD_DIR/$APP_NAME.app/Contents/Info.plist" ]; then
    fail "check_app_bundle_plist: matching plist did not produce Info.plist"
fi

if [ "$failed" -ne 0 ]; then
    exit 1
fi
printf 'check_app_bundle_plist: OK (mismatch rejected naming both values; match built)\n'
