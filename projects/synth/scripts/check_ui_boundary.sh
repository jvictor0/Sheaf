#!/usr/bin/env bash
# sru-51's layering inspection.
#
# Six checks, each of which exists because something real was separated or
# deleted and must not come back:
#
#   1. JUCE stays under projects/synth/juce and the named desktop host files.
#   2. A backend includes no component-library or producer header. A backend
#      that could call the builder or the resolver could make a layout decision,
#      which is the whole thing sru-51 separates.
#   3. A wire codec includes no library, page, or app header. The codec knows the
#      MODEL and nothing above it, which is why `Node` carries no LayoutOptions.
#   3a. The component library includes only model/library headers; no producer,
#      DSP-domain, wire, or backend header can sit in the authoring layer.
#   3b. A producer includes no wire codec and no backend -- the same boundary
#      looked at from above, which checks 2 and 3 both miss.
#   4. No deleted policy symbol reappears ANYWHERE in a backend tree: the
#      auto-flow cursor, the default-size table, BOTH coordinate classifier
#      families, and the per-variant colour table. These went in tasks 3.5-3.8
#      and 7.2, and a deletion that is only asserted is a deletion that comes
#      back -- under a new name, or in a new file beside the one being watched.
#   5. Every library and producer header compiles alone against `include/` with
#      no JUCE include path, so a stray JUCE include is a compile error rather
#      than a convention.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if ! command -v rg >/dev/null 2>&1; then
    printf 'check_ui_boundary.sh requires ripgrep (rg)\n' >&2
    exit 2
fi

failed=0

fail() {
    printf '%s\n' "$1" >&2
    failed=1
}

# ---------------------------------------------------------------------------
# The file sets this inspection is about.
#
# The backend set is DISCOVERED, not allowlisted, and that distinction is the
# whole point. An earlier version of this script named two files -- the JUCE
# renderer header and `ui.ts` -- so every deleted-symbol scan ran over those two
# alone. Reintroducing `FlowCursor` in a new `juce/PortableJuceFlow.hpp`, or a
# default-size table in a new `browser/src/ui-layout.ts`, and including it from
# the endpoint would have left the endpoint clean and this script green: "looks
# deleted, actually moved", inside the gate built to stop exactly that. It was
# green for the right reason only by accident of the JUCE backend being
# header-only and browser rendering being one file.
#
# So the backend is every source in its tree, and what is EXCLUDED is enumerated
# with a reason. A new file is scanned by default; removing it from coverage is
# a visible edit here rather than an omission nobody notices.
# ---------------------------------------------------------------------------

# Excluded from every backend scan.
#   *Tests.cpp                 test binaries: they build trees with the builder
#                              and name retired symbols in re-pin assertions
#   ControllersPageHarness.hpp developer harness, not shipped
# Discovery takes EVERY file under the backend roots and removes named
# non-backend files here. It is deliberately not a list of extensions to
# include: an allowlist of extensions is the same evasion as an allowlist of
# files, one level down, and this gate has now been caught by that shape twice.
# A new `.h`, `.cc`, `.mts` or `.tsx` holding reintroduced policy is discovered
# automatically; only the names below are trusted, and each says why.
BACKEND_EXCLUDED_FROM_ALL=(
    '-g!*Tests.cpp'
    '-g!ControllersPageHarness.hpp'
    # Build and publish tooling. These are Node scripts that run at build time,
    # never shipped runtime modules, and `check:generic-runtime` is the scan
    # that holds them to sbap-4. They legitimately mention identifiers the
    # policy scans look for, so scanning them here produces false failures.
    '-g!app-build-manifest.mjs'
    '-g!build-browser-apps.mjs'
    '-g!build-first-party-catalog.mjs'
    '-g!package-app.mjs'
    '-g!package-contract.mjs'
    '-g!publish-site.mjs'
    '-g!static-server.mjs'
    '-g!validate-deployed-catalog.mjs'
    '-g!wasm-exports.mjs'
)

# Additionally excluded from the PRODUCER-include scan only.
#   RuntimePagesJuce.hpp       the JUCE host wiring behind the runtime pages --
#                              audio device enumeration, file dialogs. It is a
#                              host, not a renderer, and knowing the pages is
#                              its job. Every other scan still covers it.
BACKEND_EXCLUDED_FROM_PRODUCER_SCAN=(
    '-g!RuntimePagesJuce.hpp'
)

# The NINE NAMED build and publish scripts above are out of the backend set:
# they are tooling rather than shipped runtime modules, and `check:generic-runtime`
# is the scan that holds them to sbap-4's generic-source rule. Any OTHER `.mjs`
# under `browser/src` is discovered and scanned like any backend file — that is
# the point of excluding by name rather than by extension.
discover_backend_sources() {
    # macOS still ships bash 3.2, so no `mapfile` and no array element prefixing.
    rg --files juce browser/src \
        "${BACKEND_EXCLUDED_FROM_ALL[@]}" "$@" | sort
}

BACKEND_SOURCES=()
while IFS= read -r discovered; do
    BACKEND_SOURCES+=("$discovered")
done < <(discover_backend_sources)

BACKEND_SOURCES_FOR_PRODUCER_SCAN=()
while IFS= read -r discovered; do
    BACKEND_SOURCES_FOR_PRODUCER_SCAN+=("$discovered")
done < <(discover_backend_sources "${BACKEND_EXCLUDED_FROM_PRODUCER_SCAN[@]}")

# The two ends of the version-2 wire.
CODEC_SOURCES=(
    include/synth/browser/BrowserCommandBuffer.hpp
    browser/src/protocol.ts
)

# The JUCE-free component library.
LIBRARY_HEADERS=(
    include/synth/PortableUI.hpp
    include/synth/PortableUIBuilders.hpp
    include/synth/PortableUILayout.hpp
    include/synth/PortableUIMetrics.hpp
    include/synth/PortableUIStandardLayout.hpp
)

# The page producers built on it: the page style table, the runtime pages, the
# Controllers page, the wizard, and the SHELL that composes them.
PAGE_PRODUCER_HEADERS=(
    include/synth/RuntimePageStyle.hpp
    include/synth/RuntimePages.hpp
    include/synth/ControllersPageUI.hpp
    include/synth/ControllerWizard.hpp
    # The wizard's tree is built here, not in its header: this is a producer in
    # every sense the scan cares about, and it was omitted while its header was
    # listed.
    src/ControllerWizard.cpp
    include/synth/RuntimeMainComponent.hpp
    # Producer-side scope drawing adapter: it intentionally depends on DSP
    # scope types while emitting portable UI draw commands.
    include/synth/PortableScopeVisualizer.hpp
)

# App producer headers are discovered by convention. The previous hand-lists
# missed `src/ControllerWizard.cpp`, then the app UI headers, then the draw
# headers. All first-party app headers are scanned by default; only named
# non-producer or host-only headers are excluded, and every exclusion carries its
# reason here.
# Paths, not bare basenames: an unanchored `!Launcher.hpp` would also skip a
# future `apps/newapp/Launcher.hpp`, which is the same evasion shape this scan
# was widened to close, one size down.
APP_PRODUCER_EXCLUDED=(
    # JUCE-touching, so it cannot be a JUCE-free producer at all: scanning it
    # would fail rule 5's standalone compile rather than tighten anything. Note
    # this is NOT "it builds no UI" -- it does include PortableScopeVisualizer.
    '-g!apps/miniapp/MiniAppCore.hpp'
    # No JUCE, no portable include, no builder use -- nothing for any rule to
    # find. Excluded to keep the list symmetric with its sibling; scanning it
    # would simply pass.
    '-g!apps/braid-4/Braid4Core.hpp'
    # Registration manifests; they expose app metadata and launch glue, not UI.
    '-g!apps/braid-4/Braid4Registration.hpp'
    '-g!apps/miniapp/MiniAppRegistration.hpp'
    # Desktop launcher host; it is explicitly JUCE-owned, not a portable producer.
    '-g!apps/sheaf-patch/Launcher.hpp'
)
APP_PRODUCER_HEADERS=()
while IFS= read -r discovered; do
    APP_PRODUCER_HEADERS+=("$discovered")
done < <(rg --files apps -g '*.hpp' "${APP_PRODUCER_EXCLUDED[@]}" | sort)

# The discovery-floor diagnostic runs before any expansion of the discovered
# set below (the line-179 concatenation and everything after it): an empty
# discovery must reach this `fail` rather than abort the shell. macOS bash 3.2
# treats `"${arr[@]}"` on a zero-element array as unbound under `set -u`, so
# every later `${APP_PRODUCER_HEADERS[@]}` use is guarded with
# `${arr[@]+"${arr[@]}"}`, but the floor check itself only takes the count
# (`${#arr[@]}`), which bash 3.2 evaluates to 0 without complaint.
APP_PRODUCER_DISCOVERY_FLOOR=8
if [ "${#APP_PRODUCER_HEADERS[@]}" -lt "$APP_PRODUCER_DISCOVERY_FLOOR" ]; then
    fail "check_ui_boundary.sh: app producer discovery found only ${#APP_PRODUCER_HEADERS[@]} headers; expected at least $APP_PRODUCER_DISCOVERY_FLOOR"
fi

PRODUCER_HEADERS=("${PAGE_PRODUCER_HEADERS[@]}" ${APP_PRODUCER_HEADERS[@]+"${APP_PRODUCER_HEADERS[@]}"})

# Every enumerated path must exist: a renamed or moved file must break this
# script loudly rather than quietly reduce its coverage to nothing.
for path in "${CODEC_SOURCES[@]}" "${LIBRARY_HEADERS[@]}" "${PAGE_PRODUCER_HEADERS[@]}"; do
    if [ ! -f "$path" ]; then
        fail "check_ui_boundary.sh: inspected file is missing: $path"
    fi
done

for required in apps/braid-4/Braid4UI.hpp apps/miniapp/MiniAppUI.hpp apps/braid-4/Braid4Draw.hpp apps/miniapp/MiniAppDraw.hpp; do
    found=0
    for path in ${APP_PRODUCER_HEADERS[@]+"${APP_PRODUCER_HEADERS[@]}"}; do
        [ "$path" = "$required" ] && found=1
    done
    if [ "$found" -eq 0 ]; then
        fail "check_ui_boundary.sh: app producer discovery did not find $required"
    fi
done

# The discovered set gets the same treatment in the only form that makes sense
# for a discovered set: it must be non-trivial and must still contain the two
# renderers this change rewrote. Discovery that silently found nothing -- a moved
# directory, a changed glob -- would otherwise pass every scan below.
BACKEND_DISCOVERY_FLOOR=6
if [ "${#BACKEND_SOURCES[@]}" -lt "$BACKEND_DISCOVERY_FLOOR" ]; then
    fail "check_ui_boundary.sh: backend discovery found only ${#BACKEND_SOURCES[@]} sources; expected at least $BACKEND_DISCOVERY_FLOOR"
fi
for required in juce/PortableJuceBackend.hpp browser/src/ui.ts; do
    found=0
    for path in ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}; do
        [ "$path" = "$required" ] && found=1
    done
    if [ "$found" -eq 0 ]; then
        fail "check_ui_boundary.sh: backend discovery did not find $required"
    fi
done

if [ "$failed" -ne 0 ]; then
    printf '\nEvery inspected path must exist, backend discovery must reach both renderers,\nand app producer discovery must reach both first-party app UI surfaces.\nA moved or renamed file silently shrinks this inspection, so it is an error\nrather than a skipped check.\n' >&2
    exit 1
fi

# Drops whole-line comments so a note that NAMES a deleted symbol does not fail
# the scan. Deliberately conservative: a symbol in a TRAILING comment still
# fails, because the alternative is a stripper that could hide a real one.
strip_comment_lines() {
    rg -v '^[[:space:]]*(//|\*|/\*)' "$1" || true
}

# Every pattern this script scans with is registered here, so the self-test
# below can prove each one fires. A pattern that stopped matching -- a typo, a
# ripgrep syntax change, a renamed symbol -- would otherwise turn its check green
# while looking at nothing, which is the exact failure mode these checks exist to
# prevent in the code they scan.
PATTERNS=()

scan() {
    # scan <label> <pattern> <file>...
    local label="$1" pattern="$2"
    shift 2
    PATTERNS+=("$pattern")
    local path hits
    for path in "$@"; do
        hits="$(strip_comment_lines "$path" | rg -n "$pattern" || true)"
        if [ -n "$hits" ]; then
            fail "$label: $path"
            printf '%s\n' "$hits" | sed 's/^/    /' >&2
        fi
    done
}

# <pattern> <sample>: the sample must match, and the same sample written as a
# whole-line comment must not.
SELF_TEST_SAMPLES=()
register_sample() {
    SELF_TEST_SAMPLES+=("$1")
}

run_scanner_self_test() {
    local index=0 pattern sample hit commented
    if [ "${#PATTERNS[@]}" -ne "${#SELF_TEST_SAMPLES[@]}" ]; then
        fail "check_ui_boundary.sh: ${#PATTERNS[@]} patterns but ${#SELF_TEST_SAMPLES[@]} self-test samples; every scan needs one"
        return
    fi
    for pattern in ${PATTERNS[@]+"${PATTERNS[@]}"}; do
        sample="${SELF_TEST_SAMPLES[$index]}"
        index=$((index + 1))

        printf '%s\n' "$sample" >"$compile_dir/sample.txt"
        hit="$(strip_comment_lines "$compile_dir/sample.txt" | rg -n "$pattern" || true)"
        if [ -z "$hit" ]; then
            fail "check_ui_boundary.sh self-test: pattern matches nothing it is meant to catch"
            printf '    pattern: %s\n    sample:  %s\n' "$pattern" "$sample" >&2
        fi

        printf '    // %s\n' "$sample" >"$compile_dir/sample.txt"
        commented="$(strip_comment_lines "$compile_dir/sample.txt" | rg -n "$pattern" || true)"
        if [ -n "$commented" ]; then
            fail "check_ui_boundary.sh self-test: pattern fires on a whole-line comment"
            printf '    pattern: %s\n    sample:  // %s\n' "$pattern" "$sample" >&2
        fi
    done
}

# Library-header includes are an allowlist rather than a single banned-symbol
# regex, so this check cannot use scan() directly. Keep the scanner itself under
# the same self-test discipline: it must see allowed portable includes, reject a
# forbidden DSP include, and ignore a commented-out forbidden include.
# `.h` as well as `.hpp`: a library header including "synth/Foo.h" would
# otherwise escape a rule whose whole point is not having gaps.
LIBRARY_INCLUDE_PATTERN='^[[:space:]]*#include[[:space:]]*[<"][^">]+\.hp?p?[">]'

library_header_include_lines() {
    strip_comment_lines "$1" | rg -n "$LIBRARY_INCLUDE_PATTERN" || true
}

library_include_allowed() {
    case "$1" in
        Color.hpp|synth/Color.hpp|\
        PortableUI.hpp|synth/PortableUI.hpp|\
        PortableUIBuilders.hpp|synth/PortableUIBuilders.hpp|\
        PortableUILayout.hpp|synth/PortableUILayout.hpp|\
        PortableUIMetrics.hpp|synth/PortableUIMetrics.hpp|\
        PortableUIStandardLayout.hpp|synth/PortableUIStandardLayout.hpp)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

library_include_violations() {
    local path="$1" line match include
    while IFS=: read -r line match; do
        [ -n "${line:-}" ] || continue
        include="$(printf '%s\n' "$match" | sed -E 's/^[[:space:]]*#include[[:space:]]*[<"]([^">]+)[">].*/\1/')"
        if ! library_include_allowed "$include"; then
            printf '%s:%s:%s\n' "$path" "$line" "$match"
        fi
    done < <(library_header_include_lines "$path")
}

run_library_include_self_test() {
    local good bad commented hit violations

    good="$compile_dir/library-good.hpp"
    printf '#include "synth/PortableUI.hpp"\n#include <synth/PortableUILayout.hpp>\n' >"$good"
    hit="$(library_header_include_lines "$good")"
    if [ -z "$hit" ]; then
        fail "check_ui_boundary.sh self-test: library include scanner saw no allowed portable include"
        printf '    pattern: %s\n' "$LIBRARY_INCLUDE_PATTERN" >&2
    fi
    violations="$(library_include_violations "$good")"
    if [ -n "$violations" ]; then
        fail "check_ui_boundary.sh self-test: library include scanner rejected allowed portable includes"
        printf '%s\n' "$violations" | sed 's/^/    /' >&2
    fi

    # Every include FORM the pattern and allowlist were widened to accept, so
    # that widening cannot silently regress: quoted synth-prefixed, bare,
    # angle-bracketed, relative, and a `.h` rather than `.hpp`. Each must still
    # be rejected -- proving the allowlist resolves the name, not the spelling.
    bad="$compile_dir/library-bad.hpp"
    for forbidden in \
        '#include "synth/DspScope.hpp"' \
        '#include "DspScope.hpp"' \
        '#include <synth/DspScope.hpp>' \
        '#include "../DspScope.hpp"' \
        '#include "synth/DspScope.h"'
    do
        printf '%s\n' "$forbidden" >"$bad"
        violations="$(library_include_violations "$bad")"
        if [ -z "$violations" ]; then
            fail "check_ui_boundary.sh self-test: library include scanner missed a forbidden synth include"
            printf '    sample: %s\n' "$forbidden" >&2
        fi
    done

    commented="$compile_dir/library-commented.hpp"
    printf '// #include "synth/DspScope.hpp"\n' >"$commented"
    violations="$(library_include_violations "$commented")"
    if [ -n "$violations" ]; then
        fail "check_ui_boundary.sh self-test: library include scanner fired on a whole-line comment"
        printf '%s\n' "$violations" | sed 's/^/    /' >&2
    fi
}

# ---------------------------------------------------------------------------
# 1. JUCE boundary.
# ---------------------------------------------------------------------------

tmp="$(mktemp)"
compile_dir="$(mktemp -d)"
trap 'rm -f "$tmp"; rm -rf "$compile_dir"' EXIT

set +e
rg -n --glob '*.hpp' --glob '*.h' --glob '*.cpp' --glob '*.mm' \
    --glob '!build/**' --glob '!apps/miniapp/build/**' --glob '!apps/sheaf-patch/build/**' \
    '(#include[[:space:]]*<juce[^>]*>|(^|[^[:alnum:]_])juce::)' . >"$tmp"
rg_status=$?
set -e
if [ "$rg_status" -gt 1 ]; then
    printf 'check_ui_boundary.sh: ripgrep scan failed with status %s\n' "$rg_status" >&2
    exit "$rg_status"
fi

juce_failed=0
while IFS=: read -r path line match; do
    [ -n "${path:-}" ] || continue
    path="${path#./}"
    if [[ "$match" =~ ^[[:space:]]*// ]]; then
        continue
    fi

    case "$path" in
        juce/*|\
        runtime/AudioConfigPage.hpp|\
        runtime/FilePage.hpp|\
        runtime/HostDataPaths.cpp|\
        runtime/JuceRuntimeMainServices.hpp|\
        runtime/LauncherWindow.hpp|\
        runtime/MainPane.hpp|\
        runtime/MidiConnectionManager.hpp|\
        runtime/Runtime.hpp|\
        runtime/Shell.hpp|\
        apps/miniapp/Main.cpp|\
        apps/sheaf-patch/Launcher.hpp|\
        apps/sheaf-patch/LauncherHarnessTests.cpp|\
        apps/sheaf-patch/Main.cpp)
            ;;
        *)
            printf 'JUCE boundary violation: %s:%s:%s\n' "$path" "$line" "$match" >&2
            juce_failed=1
            ;;
    esac
done <"$tmp"

if [ "$juce_failed" -ne 0 ]; then
    printf '\nJUCE references must stay under projects/synth/juce or explicit desktop runtime host files.\n' >&2
    failed=1
fi

# ---------------------------------------------------------------------------
# 2. A backend includes no component-library or producer header.
#
# `PortableUI.hpp` -- the MODEL -- is exactly what a backend is allowed to see.
# Everything built beside it is authoring vocabulary and layout policy.
# ---------------------------------------------------------------------------

register_sample '#include "synth/PortableUILayout.hpp"'
scan 'a backend includes a component-library header (sru-51)' \
    '(#include|from|import).*(PortableUIBuilders|PortableUILayout|PortableUIMetrics|PortableUIStandardLayout)' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

register_sample 'import { x } from "./RuntimePages.js";'
scan 'a backend includes a producer header (sru-51)' \
    '(#include|from|import).*(RuntimePages|RuntimePageStyle|ControllersPageUI|ControllerWizard|RuntimeMainComponent|Braid4|MiniApp)' \
    ${BACKEND_SOURCES_FOR_PRODUCER_SCAN[@]+"${BACKEND_SOURCES_FOR_PRODUCER_SCAN[@]}"}

# ---------------------------------------------------------------------------
# 3. A wire codec includes no library, page, or app header.
# ---------------------------------------------------------------------------

register_sample '#include "synth/ControllersPageUI.hpp"'
scan 'a wire codec includes a library, page, or app header (sru-51)' \
    '(#include|from|import).*(PortableUIBuilders|PortableUILayout|PortableUIMetrics|PortableUIStandardLayout|RuntimePages|RuntimePageStyle|ControllersPageUI|ControllerWizard|RuntimeMainComponent|Braid4|MiniApp)' \
    "${CODEC_SOURCES[@]}"

# ---------------------------------------------------------------------------
# 3a. The component library includes only the model and library peers.
#
# This is the layer that 7.3 did not scan at all: `PortableUIBuilders.hpp`
# included `DspScope.hpp`, so every component-library consumer compiled synth
# DSP into the authoring layer. The backend and producer scans cannot see this
# direction; the library has to police its own imports.
# ---------------------------------------------------------------------------

library_include_failed=0
for path in "${LIBRARY_HEADERS[@]}"; do
    while IFS= read -r violation; do
        [ -n "${violation:-}" ] || continue
        printf 'a component-library header includes a non-model header (sru-51): %s\n' "$violation" >&2
        library_include_failed=1
    done < <(library_include_violations "$path")
done

if [ "$library_include_failed" -ne 0 ]; then
    printf '\nComponent-library headers may include only the portable UI model and library peers.\n' >&2
    failed=1
fi

# ---------------------------------------------------------------------------
# 3b. A producer includes no wire codec and no backend.
#
# The other direction of the same boundary, and it was missing: checks 2 and 3
# both look downward from the backend and the codec, so nothing stopped a page
# from reaching for `BrowserCommandBuffer.hpp` and encoding its own frame, or
# from including a renderer and making a rendering decision. A producer knows
# the model and the library; the wire and the renderers are below it.
# ---------------------------------------------------------------------------

register_sample '#include "synth/browser/BrowserCommandBuffer.hpp"'
scan 'a producer includes a wire codec or a backend (sru-51)' \
    '(#include|from|import).*(BrowserCommandBuffer|PortableJuceBackend|protocol\.js|src/ui\.js|\./ui)' \
    "${PRODUCER_HEADERS[@]}"

# ---------------------------------------------------------------------------
# 4. No deleted policy symbol reappears in a backend.
# ---------------------------------------------------------------------------

# The auto-flow cursor and its spacing constants (deleted in 3.6 and 3.8). A
# backend that flows anything is deciding layout.
register_sample '    FlowCursor cursor{kControlMargin, kControlGap};'
scan 'the deleted auto-flow cursor reappears in a backend (sru-51)' \
    '\b(FlowCursor|flowCursor|cursors|kControlMargin|kControlGap|CONTROL_MARGIN|CONTROL_GAP)\b' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

# The default-size table (deleted in 3.6 and 3.8). Intrinsic extents are the
# library's metrics contract, not a backend table.
register_sample '    const auto size = DefaultSizeForNode(node);'
scan 'the deleted default-size table reappears in a backend (sru-51)' \
    '\b(DefaultSizeForNode|defaultSize)\b' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

# Coordinate classifier family one: draw geometry (deleted in 3.5 and 3.8).
register_sample '    if (DrawCommandsLookLocal(node)) { commandsAreNodeLocal_ = true; }'
scan 'the deleted draw-coordinate classifier reappears in a backend (sru-51)' \
    '(LooksLocal|LookLocal|nodeLocal|commandsAreNodeLocal)' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

# Coordinate classifier family two: node bounds, including the browser's
# `parentBounds` subtraction plumbing (deleted in 3.5 and 3.8).
#
# NOT `parentOrigin`: the JUCE backend accumulates one while folding an ancestor
# chain, which is precisely what the coordinate contract requires of it. Banning
# the word would ban the correct behaviour along with the deleted one. What is
# banned is the CLASSIFIER -- deciding whether bounds are parent-local -- and
# the plumbing that existed only to undo a parent's origin.
register_sample '    if (ExplicitBoundsAreParentLocal(node, parentBounds)) { return; }'
scan 'the deleted node-bounds classifier reappears in a backend (sru-51)' \
    '(ExplicitBoundsAreParentLocal|explicitBoundsAreParentLocal|parentBounds)' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

# The per-variant colour table (retired in 1.2, its branches deleted in 3.7 and
# 3.8, the model field deleted in 7.2). Both the identifier and every one of the
# nine appearance tokens it keyed on.
register_sample '    const juce::Colour fill = ColourForVariant(node.variant);'
scan 'the retired per-variant colour table reappears in a backend (sru-51)' \
    '\bvariant' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}
# Both quote styles: `ui.ts` already mixes them, so a TypeScript
# reintroduction as `'primary'` would have walked straight past a
# double-quote-only pattern.
register_sample "    if (token === 'muted-title') { return kTitleColour; }"
scan 'a retired appearance-variant token reappears in a backend (sru-51)' \
    '['"'"'"](danger|primary|quiet|secondary|muted|muted-title|title|list-row|panel|field)['"'"'"]' \
    ${BACKEND_SOURCES[@]+"${BACKEND_SOURCES[@]}"}

# ---------------------------------------------------------------------------
# 5. JUCE-free compile coverage for the library and the producers.
#
# Each header is compiled ALONE against `include/` with no JUCE include path, so
# a JUCE include cannot resolve. This is the half the `#ifdef JUCE_MAJOR_VERSION`
# guards in the test binaries cannot cover: those prove a test TU sees no JUCE,
# this proves each header stands up without one, on its own, in declaration
# order, with nothing else having included its dependencies first.
# ---------------------------------------------------------------------------

CXX="${CXX:-c++}"

for header in "${LIBRARY_HEADERS[@]}" "${PRODUCER_HEADERS[@]}"; do
    unit="$compile_dir/unit.cpp"
    printf '#include "%s"\n#ifdef JUCE_MAJOR_VERSION\n#error "this header must stay JUCE-free (sru-51)"\n#endif\nint main() { return 0; }\n' \
        "$header" >"$unit"
    if ! "$CXX" -std=c++20 -Iinclude -I. -fsyntax-only "$unit" >"$compile_dir/out.txt" 2>&1; then
        fail "header does not compile JUCE-free and standalone (sru-51): $header"
        sed 's/^/    /' <"$compile_dir/out.txt" >&2
    fi
done

run_scanner_self_test
run_library_include_self_test

if [ "$failed" -ne 0 ]; then
    exit 1
fi

printf 'check_ui_boundary.sh: PASS\n'
