// sru-48: the named visual acceptance criteria, enforced as structural
// assertions over the RENDERED DOM of the real runtime shell.
//
// These surfaces come from the fixture app's freshly compiled Wasm (`npm test`
// runs `make browser-fixture-app` first), so what is measured here is the real
// producers' current output. The headless half of the same criteria runs over
// the resolved portable tree in `tests/portable_ui_tests.cpp` and
// `juce/ControllersPageSimulationTests.cpp` through
// `tests/support/VisualCriteria.hpp`; this file adds the two that need real
// rendering — text fit and computed-style contrast — and re-checks the
// structural ones against what the browser actually laid out.
//
// Appearance is NOT pinned here. sru-48 was amended on 2026-07-30: no
// screenshot is a baseline and no pixel diff gates a build. These criteria are
// the durable regression surface precisely because they are structural and hold
// at any extent.
import { expect, test, type Page } from "@playwright/test";
import { FIXTURE_APPS, installRealFakeApp, stopRealFakeApp, synthNode } from "./helpers/fake-app.js";
import { installTwisterPair } from "./helpers/fake-midi.js";

// ---------------------------------------------------------------------------
// Task 1.4: the fixed verification environment.
// ---------------------------------------------------------------------------
//
// The DEVICE SCALE FACTOR is pinned at 1 and the viewport is at least as large
// as the composite surface, because `BrowserUiBackend.fitSurface` applies a
// shrink-only `min(1, availableWidth / surfaceWidth)` transform to the surface
// root. Under any scale below 1 every measured rectangle is a scaled one, which
// muddies the text-fit and contrast checks. `the surface renders at scale 1`
// asserts this rather than assuming it.
//
// The VIEWPORT DIMENSIONS are framing, not a correctness gate. sru-54 makes a
// page that cannot fit its surface fail at resolution, so containment holds at
// every extent rather than at a chosen one, and with 6.6's baseline comparison
// deleted nothing later fails on a rendered-appearance difference.
export const VERIFICATION_VIEWPORT = { width: 1280, height: 900 } as const;

// The composite surface is the app's own 640x480 content plus the 96-wide
// sidebar. 736 < 1280, so the surface is never scaled down.
const COMPOSITE_SURFACE_WIDTH = 736;

test.use({ viewport: { ...VERIFICATION_VIEWPORT }, deviceScaleFactor: 1 });

// sru-48 named visual acceptance criteria. Every criterion is either asserted
// structurally in this file or checked by a human at the Task 17 sign-off gate.
// Do not add a criterion here without also adding its check.
//
// The same seven strings, in the same order, are `NamedCriteria()` in
// `tests/support/VisualCriteria.hpp`, which is the headless half of this suite.
export const VISUAL_CRITERIA = [
  "like-type controls share column positions",
  "all spacing drawn from the library's shared spacing metrics",
  "every form control has a visible caption",
  "no overlapping nodes and no container overflow on either axis",
  "every text element renders within its allocated extent",
  "text contrast meets WCAG AA 4.5:1 against its effective background",
  "no text conveys no information to the user",
] as const;

// ---------------------------------------------------------------------------
// Task 1.4: the deterministic fixture state.
// ---------------------------------------------------------------------------
//
// Named concretely so the same state is reachable from a description alone.
// Tasks 5.6 and 5.9 both showed list length changing layout materially, so the
// Controllers page is driven to a length that exercises the scrolling path
// rather than the three-item happy case.
const FIXTURE_CONTROLLER_COUNT = 12;

// Viewport widths the Controllers row's driven-state check re-renders at.
// COMPOSITE_SURFACE_WIDTH (736, above) is where the composite surface's
// shrink-only scale transform first engages; this sweep runs from well below
// that -- deep in the scaled-down range -- up through it and on to 1280, the
// file's own standard unscaled viewport. The overflow and overlap criteria
// compare `getBoundingClientRect()` rectangles against a fixed CSS-pixel
// TOLERANCE, and that rectangle IS affected by the scale transform, so a
// rounding artifact introduced at one scale is invisible at any other -- the
// single fixed viewport every other test in this file renders at cannot see
// one either way.
const CONTROLLER_WIDTH_SWEEP: readonly number[] = [480, 560, 640, 720, 800, 880, 960, 1040, 1120, 1200, 1280];

type SurfaceName = "audio" | "controllers" | "sync" | "file";
const ALL_SURFACES: readonly SurfaceName[] = ["audio", "controllers", "sync", "file"];
// The form grid whose row count does not depend on the host. The Controllers
// and File pages are tables and panels, not form grids; the Audio page IS one,
// but its second row is the input selector, which the shell emits only when the
// host offers an input device -- so in a browser its row count is a property of
// the machine, and a multi-row alignment claim there is not the deterministic
// fixture state task 1.4 requires. Audio's two-row grid is covered instead by
// `TestNamedVisualCriteriaHoldOnEveryPageAndApp` in `portable_ui_tests.cpp`,
// where `showInputCombo` is part of the named fixture.
const FORM_GRID_PAGES: ReadonlyArray<{ surface: SurfaceName; container: string }> = [
  { surface: "sync", container: "runtime.sync.form" },
];

// ---------------------------------------------------------------------------
// The exemptions, one entry at a time.
//
// Every id below is a disclosed residual recorded in
// `openspec/changes/rebuild-portable-ui-component-library/tasks.md`, not a
// class of escape. A control that is not on one of these lists and carries no
// rendered caption fails.
// ---------------------------------------------------------------------------

// Controls with no visible caption, ENUMERATED FROM THE FIXTURE rather than
// matched by pattern. Each entry is one control with one reason. sru-48 says
// every form control carries a visible caption and each of these fails that
// today; whether the fix is a caption per cell or a column heading per table is
// a product decision, and task 6.5 is the pairing session where a human makes
// it. tasks.md 6.5b carries the same list.
//
// A pattern list is what this replaces, and it is why: a bare `.output` suffix
// matched `runtime.audio.output`, a control that DOES carry a caption, and left
// the Audio page with nothing examined at all. Prefix-qualifying the pattern
// fixed that instance and not the mechanism -- any future
// `runtime.controllers.*.output` would still have been waived without ever
// being named. Building the list from fixture row indices means a control the
// page grows is examined and fails.
type CaptionException = { id: string; reason: string };

function controllerCaptionExceptions(rows: number): CaptionException[] {
  const exceptions: CaptionException[] = [
  ];
  for (let ix = 0; ix < rows; ix += 1) {
    const row = `controller row ${ix}`;
    exceptions.push(
      { id: `runtime.controllers.row.${ix}.input`, reason: `${row} MIDI input selector; a table cell whose column has no heading` },
      { id: `runtime.controllers.row.${ix}.output`, reason: `${row} MIDI output selector; a table cell whose column has no heading` },
    );
  }
  return exceptions;
}

// Braid 4's and Mini App's slider exceptions (`braid4.scene.blend`,
// `miniapp.scene.blend`, `miniapp.gesture.value`) are deliberately NOT listed
// here. Neither app is reachable from this harness, so every one of those
// entries would be unmatchable on every surface this suite drives -- a
// permanently stale list, which is exactly what the equality pin below exists
// to catch. They are named, with reasons, in the headless half
// (`portable_ui_tests.cpp`) where the surfaces actually exist, and in
// tasks.md 6.5b for the product owner.

// The ONLY blanket out-of-flow class. A Controllers row's status dots are an
// explicitly bounded `Draw` the producer hand-centres inside its cell, so they
// consume no stacking space and a gap measured against them is meaningless.
// That hand-centring is the sru-47/sru-53 residual under tasks.md 6.5b.
//
// `.visualizer` is deliberately NOT here. It used to be, which made the overlap
// check skip every pair involving an underlay -- including against siblings the
// underlay does not name -- while the comment claimed the exemption was
// underlay-to-target only. The two halves encoded different contracts, and the
// looser one was in the half meant to catch rendered overlap the tree cannot
// see. `siblingOverlaps` now mirrors the headless rule exactly.
const OUT_OF_FLOW_SUFFIXES = [".status_dots"] as const;

// Declared overlays that cannot say so through `overlayOf`, named ONE AT A TIME
// with the one sibling each is allowed to cover. Not a suffix: a `.warning`
// match would have swallowed `runtime.sync.warning` and
// `runtime.controllers.wizard.warning` too, neither of which is a badge, and
// that is the same class-based escape the caption list was just rebuilt to
// avoid.
//
// A badge is exempt from the overlap check against its target and nothing else,
// and `badgesSeen` below requires the pin to have had a live subject rather
// than being satisfied by absence.
const DECLARED_BADGES: ReadonlyArray<{ id: string; target: string; reason: string }> = [
  {
    id: "runtime.sidebar.controllers.warning",
    target: "runtime.sidebar.controllers",
    // `BuildSidebarTree` hand-assembles already-resolved nodes and never
    // invokes the resolver (tasks.md 7.1), so this deliberate overlay has no
    // `overlayOf` to declare. It renders whenever MIDI discovery has an
    // unconfigured candidate available.
    reason: "sidebar MIDI-warning badge, deliberately over the right edge of the Controllers button",
  },
];

// The shared spacing tables the runtime shell's producers declare, restated by
// hand from the C++ constants named beside each value -- TypeScript cannot read
// them. The authoritative check is the headless half, which builds its allowed
// set from the constants themselves, so a producer that invents a new number
// fails there whatever this list says. What this list adds is the RENDERED
// gap: the headless half measures resolved bounds, and a control whose own
// chrome overflows its extent shows up only here. That is how the Controllers
// disclosure button was caught rendering 26px into a 24px slot.
const SPACING_METRIC_VALUES = [
  0, // a deliberate absence of spacing, not a magic number
  4, // runtime_ui::Layout::kPageMargin, ::kRowGap; ControllersLayout::kPageMargin, ::kLifecycleControlGap
  6, // ControllersLayout::kRowGap
  8, // synth::ui::kSpacing.gap and .labelGap; ControllersLayout::kEndpointBoxGap, ::kAvailableControlGap
  10, // runtime_ui::Layout::kFilePanelPadding
  12, // synth::ui::kSpacing.padding
  16, // ControllersLayout::kStatusLegendPairGap
] as const;

// ---------------------------------------------------------------------------
// In-page helpers, installed into every document before navigation so that
// `page.evaluate` bodies can use them. They are ordinary type-checked code
// rather than an injected source string.
// ---------------------------------------------------------------------------

type CriteriaHelpers = {
  TOLERANCE: number;
  // Far smaller than TOLERANCE: see the comment beside its definition in
  // `installCriteriaHelpers` for why the two must not share a value.
  TEXT_FIT_TOLERANCE: number;
  nodes(): HTMLElement[];
  nodeId(el: HTMLElement): string;
  parentNodeOf(el: HTMLElement): HTMLElement | null;
  contentRectOf(el: HTMLElement): DOMRect;
  childrenOf(el: HTMLElement): HTMLElement[];
  matchesAny(id: string, suffixes: readonly string[], ids: readonly string[]): boolean;
  underlayTargetOf(id: string): string;
  badgeTargetOf(id: string): string;
  intersects(a: DOMRect, b: DOMRect): boolean;
  describeRect(rect: DOMRect): string;
  // The sub-pixel width a text-bearing element's own content needs, and the
  // sub-pixel width it actually has to give it, at the element's own computed
  // font. Both integer DOM properties the text-fit criterion used to compare
  // (`scrollWidth`, `clientWidth`) round to the nearest CSS pixel, which is
  // exactly how a label needing 58.3px in a 58px box reads as "58 fits in 58".
  textNeededWidth(el: HTMLElement): number;
  textAvailableWidth(el: HTMLElement): number;
  contrastRatio(el: HTMLElement):
    | { ratio: number; painted: string; background: string; alpha: number }
    | null;
};

// The kinds whose OWN element paints glyphs. `combo-box` and `text-field` are
// included: a `<select>` and an `<input>` render their value text inside the
// element the backend sized, so a too-tight reservation or a low-contrast field
// shows up there and nowhere else. They were unmeasured before.
const TEXT_BEARING_KINDS = ["label", "status-text", "button", "toggle", "combo-box", "text-field"] as const;

declare global {
  interface Window {
    __visualCriteria: CriteriaHelpers;
  }
}

async function installCriteriaHelpers(page: Page): Promise<void> {
  await page.addInitScript((badges: ReadonlyArray<{ id: string; target: string }>) => {
    const TOLERANCE = 0.5;
    // `scrollWidth` and `clientWidth` are both integers -- the DOM rounds them
    // to the nearest CSS pixel -- so TOLERANCE=0.5 is exactly wide enough to
    // absorb that rounding without masking a real one-pixel shortfall.
    // `textNeededWidth`/`textAvailableWidth` below are NOT rounded, so reusing
    // 0.5 here would swallow the sub-pixel shortfall this measurement exists
    // to catch (a label needing 0.3px more than it was given). 0.1 keeps
    // comfortable room below that shortfall while still absorbing the
    // sub-hundredth-pixel disagreement between canvas text shaping and CSS
    // text layout for the same font.
    const TEXT_FIT_TOLERANCE = 0.1;
    const nodes = () => [...document.querySelectorAll<HTMLElement>("[data-node-id]")];
    const nodeId = (el: HTMLElement) => el.dataset.nodeId!;
    const parentNodeOf = (el: HTMLElement) =>
      el.parentElement ? el.parentElement.closest<HTMLElement>("[data-node-id]") : null;
    // A ScrollArea contains its declared SCROLL-CONTENT rectangle, not its
    // viewport (sru-5, sprs-10): a row below the visible viewport is contained,
    // not overflowing. The backend puts that content in a relative div sized to
    // `max(bounds, scrollContent)` and hangs it off the element as
    // `scrollContent`, so its rect is the containing rectangle -- and it moves
    // with scrollTop exactly as its children do.
    //
    // Read by the property the backend actually sets, not by position. A
    // positional `firstElementChild` would silently measure the wrong box the
    // day a scroll area gained any other child, and containment would go
    // vacuously green for every scrolling list. A scroll area missing the
    // property is reported rather than quietly falling back.
    const scrollContentOf = (el: HTMLElement): HTMLElement | null =>
      ((el as unknown as { scrollContent?: HTMLElement }).scrollContent ?? null);
    const contentRectOf = (el: HTMLElement) => {
      if (el.dataset.nodeKind !== "scroll-area") return el.getBoundingClientRect();
      const content = scrollContentOf(el);
      if (!content) throw new Error(`scroll area ${el.dataset.nodeId} has no scroll-content element`);
      return content.getBoundingClientRect();
    };
    const childrenOf = (el: HTMLElement) => nodes().filter((candidate) => parentNodeOf(candidate) === el);
    const matchesAny = (id: string, suffixes: readonly string[], ids: readonly string[]) =>
      suffixes.some((suffix) => id.endsWith(suffix)) || ids.includes(id);
    const underlayTargetOf = (id: string) =>
      id.endsWith(".visualizer") ? id.slice(0, -".visualizer".length) : "";
    // The declared badges, looked up BY ID in the table above -- one id, one
    // target. A badge is exempt from the overlap check only against that
    // target, and only while it sits inside it. A badge that drifted onto a
    // different button, or off its own, fails.
    const badgeTargetOf = (id: string) =>
      badges.find((badge) => badge.id === id)?.target ?? "";
    const intersects = (a: DOMRect, b: DOMRect) =>
      a.left + TOLERANCE < b.right && b.left + TOLERANCE < a.right &&
      a.top + TOLERANCE < b.bottom && b.top + TOLERANCE < a.bottom;
    const describeRect = (rect: DOMRect) =>
      `(${rect.left.toFixed(2)},${rect.top.toFixed(2)} ${rect.width.toFixed(2)}x${rect.height.toFixed(2)})`;

    // One canvas, reused for every measurement: `nodes()` is walked once per
    // criterion per surface, so a fresh canvas per element would be an O(n)
    // allocation for what only needs an O(1) one, paid once and read many
    // times.
    const measureCanvas = document.createElement("canvas");
    const measureCtx = measureCanvas.getContext("2d")!;
    // The text actually ON SCREEN for this element. A combo box wraps a
    // `<select>`; `el.textContent` there is the concatenation of every
    // OPTION's label, not what is shown -- only the selected option is
    // rendered, so that is what a width measurement has to read.
    const displayedText = (el: HTMLElement) => {
      const select = el.querySelector("select");
      if (!select) return (el.textContent ?? "").trim();
      const selected = select.options[select.selectedIndex];
      return (selected ? selected.text : select.value).trim();
    };
    // The width the element's own text needs, measured at the element's own
    // computed font rather than read off the rounded-to-the-nearest-pixel
    // `scrollWidth`. `getComputedStyle(el).font` is the resolved shorthand --
    // style, variant, weight, size/line-height and family -- so this is the
    // same font the browser itself laid the text out with.
    const textNeededWidth = (el: HTMLElement) => {
      measureCtx.font = getComputedStyle(el).font;
      return measureCtx.measureText(displayedText(el)).width;
    };
    // The width actually available to that text: the element's own content
    // box. NOT its content box minus padding -- a button or toggle centres
    // its caption (the browser's own default rendering for `<button>`) over a
    // padded hit-target, so the padding sizes the clickable area rather than
    // reserving a text-exclusion inset the way it would for left-aligned flow
    // text. That distinction is invisible for every label and status-text
    // node in this app, because none of them carry any padding at all -- so
    // comparing against the padded box changes nothing for the kinds this
    // measurement exists to catch, and fixes a false positive on a 22px
    // disclosure button (11px of padding on each side) that renders exactly
    // as intended.
    const textAvailableWidth = (el: HTMLElement) => el.clientWidth;

    // What the eye actually sees, composited the way the browser paints it.
    //
    // `getComputedStyle(el).color` reports the author's colour before the
    // element's own `opacity` and before every ancestor's. A disabled control
    // renders at `opacity: 0.58` (`synth-browser.css`), so reading `color`
    // alone approves text nobody can read at the ratio reported.
    //
    // ROUND 2 GOT THE COMPOSITING WRONG in a way worth recording, because it
    // looked right. It faded the FOREGROUND by the cumulative opacity but
    // measured it against the element's own background left at FULL strength --
    // and its background search started at the element itself, so a control
    // with an opaque field background was compared against a colour that is not
    // on screen. CSS group opacity does not work that way: it fades the
    // element's background and its glyphs TOGETHER onto whatever is behind the
    // element. Both must fade, or the ratio describes a rendering that never
    // happened.
    const parseColour = (value: string) => {
      const parts = value.match(/[\d.]+/g);
      if (!parts) return null;
      const [r, g, b, a] = parts.map(Number);
      return { r, g, b, a: a === undefined ? 1 : a };
    };
    const opacityOf = (el: HTMLElement) => {
      const own = Number(getComputedStyle(el).opacity);
      return Number.isNaN(own) ? 1 : own;
    };
    // The opaque colour BEHIND this element's own opacity group. Strictly
    // ancestors -- the element's own background is not its own backdrop, which
    // is the bug above. An ancestor that is itself faded or translucent cannot
    // serve either, so the walk continues past it and accumulates its opacity,
    // since that opacity fades this element's group too.
    const backdropBehind = (el: HTMLElement) => {
      let outerOpacity = 1;
      for (let node = el.parentElement; node; node = node.parentElement) {
        const style = getComputedStyle(node);
        const colour = parseColour(style.backgroundColor);
        const opacity = opacityOf(node);
        if (colour && colour.a >= 1 && opacity >= 1) return { backdrop: colour, outerOpacity };
        outerOpacity *= opacity;
      }
      // The document canvas, which is white unless something opaque was found.
      return { backdrop: { r: 255, g: 255, b: 255, a: 1 }, outerOpacity };
    };
    const composite = (over: { r: number; g: number; b: number },
                       under: { r: number; g: number; b: number },
                       alpha: number) => ({
      r: over.r * alpha + under.r * (1 - alpha),
      g: over.g * alpha + under.g * (1 - alpha),
      b: over.b * alpha + under.b * (1 - alpha),
      a: 1,
    });
    const channel = (value: number) => {
      const c = value / 255;
      return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
    };
    const luminance = (c: { r: number; g: number; b: number }) =>
      0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
    const describeColour = (c: { r: number; g: number; b: number }) =>
      `rgb(${c.r.toFixed(0)}, ${c.g.toFixed(0)}, ${c.b.toFixed(0)})`;
    const contrastRatio = (el: HTMLElement) => {
      const style = getComputedStyle(el);
      const foreground = parseColour(style.color);
      if (!foreground) return null;
      const ownBackground = parseColour(style.backgroundColor) ?? { r: 0, g: 0, b: 0, a: 0 };
      const { backdrop, outerOpacity } = backdropBehind(el);

      // Paint the element at full strength first: its own background onto the
      // backdrop, then its glyphs onto that.
      const paintedBackground = composite(ownBackground, backdrop, ownBackground.a);
      const paintedForeground = composite(foreground, paintedBackground, foreground.a);

      // Then fade the whole group -- background and glyphs alike -- onto the
      // backdrop. This is the step round 2 applied to the foreground only.
      const groupOpacity = Math.max(0, Math.min(1, opacityOf(el) * outerOpacity));
      if (groupOpacity === 0) return null;
      const shownForeground = composite(paintedForeground, backdrop, groupOpacity);
      const shownBackground = composite(paintedBackground, backdrop, groupOpacity);

      const a = luminance(shownForeground);
      const b = luminance(shownBackground);
      return {
        ratio: (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05),
        painted: describeColour(shownForeground),
        // The background AS SHOWN, which is what the ratio was measured
        // against. When the element is faded this is deliberately not the
        // element's declared background colour.
        background: describeColour(shownBackground),
        alpha: groupOpacity * foreground.a,
      };
    };

    window.__visualCriteria = {
      TOLERANCE, TEXT_FIT_TOLERANCE, nodes, nodeId, parentNodeOf, contentRectOf, childrenOf,
      matchesAny, underlayTargetOf, badgeTargetOf, intersects, describeRect,
      textNeededWidth, textAvailableWidth, contrastRatio,
    };
  }, DECLARED_BADGES.map((badge) => ({ id: badge.id, target: badge.target })));
}

async function openSurface(page: Page, surface: SurfaceName): Promise<void> {
  await page.locator(synthNode(`runtime.sidebar.${surface}`)).click();
  await expect(page.locator(synthNode(`runtime.${surface}.root`))).toBeVisible();
}

// The named fixture state: twelve controllers added through the page's own add
// row, so the state is produced by the real surface rather than injected. The
// add row offers a preset and an Add button; the preset combo already holds the
// first entry, so a press is the whole gesture.
async function seedControllers(page: Page, count: number): Promise<void> {
  await openSurface(page, "controllers");
  for (let index = 0; index < count; index += 1) {
    await page.locator(synthNode("runtime.controllers.add_button")).click();
    await expect(page.locator(synthNode(`runtime.controllers.row.${index}`))).toHaveCount(1);
  }
}

// The structural criteria over whatever is on screen right now, for states that
// are not one of the four top-level pages. Same rules as the per-criterion
// tests above, gathered in one pass so a driven state can be checked without
// re-navigating for each one.
async function evaluateStructuralCriteria(page: Page) {
  return page.evaluate(([outOfFlow, textKinds]: readonly [string[], string[]]) => {
    const { TOLERANCE, TEXT_FIT_TOLERANCE, nodes, nodeId, parentNodeOf, contentRectOf, childrenOf, matchesAny,
            underlayTargetOf, badgeTargetOf, intersects, describeRect,
            textNeededWidth, textAvailableWidth, contrastRatio } = window.__visualCriteria;
    const overflows: string[] = [];
    const overlaps: string[] = [];
    const silentText: string[] = [];
    const tooTight: string[] = [];
    const lowContrast: string[] = [];
    let checked = 0;
    let badgesSeen = 0;

    for (const el of nodes()) {
      const parent = parentNodeOf(el);
      if (parent) {
        checked += 1;
        const p = contentRectOf(parent);
        const c = el.getBoundingClientRect();
        if (c.left < p.left - TOLERANCE || c.right > p.right + TOLERANCE ||
            c.top < p.top - TOLERANCE || c.bottom > p.bottom + TOLERANCE)
          overflows.push(`${nodeId(el)} ${describeRect(c)} overflows ${nodeId(parent)} ${describeRect(p)}`);
      }
      const kind = el.dataset.nodeKind!;
      if (["label", "status-text"].includes(kind)) {
        const rect = el.getBoundingClientRect();
        if (rect.width > TOLERANCE && rect.height > TOLERANCE && !el.textContent?.trim())
          silentText.push(`${nodeId(el)} reserves ${describeRect(rect)} and renders no text`);
      }
      if (textKinds.includes(kind) && el.textContent?.trim()) {
        const neededWidth = textNeededWidth(el);
        const availableWidth = textAvailableWidth(el);
        if (neededWidth > availableWidth + TEXT_FIT_TOLERANCE)
          tooTight.push(`${nodeId(el)} needs ${neededWidth.toFixed(2)} in ${availableWidth.toFixed(2)}`);
        const contrast = contrastRatio(el);
        if (contrast && contrast.ratio < 4.5)
          lowContrast.push(`${nodeId(el)} ${contrast.ratio.toFixed(2)}:1 (${contrast.painted} on ${contrast.background})`);
      }
    }

    for (const parent of [...nodes(), document.body]) {
      const children: HTMLElement[] = parent === document.body
        ? nodes().filter((el) => !parentNodeOf(el))
        : childrenOf(parent as HTMLElement);
      for (let a = 0; a < children.length; a += 1) {
        for (let b = a + 1; b < children.length; b += 1) {
          const firstId = nodeId(children[a]);
          const secondId = nodeId(children[b]);
          if (matchesAny(firstId, outOfFlow, []) || matchesAny(secondId, outOfFlow, [])) continue;
          if (underlayTargetOf(firstId) === secondId || underlayTargetOf(secondId) === firstId) continue;
          if (badgeTargetOf(firstId) === secondId || badgeTargetOf(secondId) === firstId) continue;
          const rectA = children[a].getBoundingClientRect();
          const rectB = children[b].getBoundingClientRect();
          if (intersects(rectA, rectB))
            overlaps.push(`${firstId} ${describeRect(rectA)} intersects ${secondId} ${describeRect(rectB)}`);
        }
      }
    }

    // Same badge pin as the four-page overlap test: a declared badge is exempt
    // against its target and nothing else, so it must actually sit inside it.
    // Here the subject set is live -- the wizard states install two Twisters,
    // which is what makes the sidebar warning badge render.
    for (const el of nodes()) {
      const targetId = badgeTargetOf(nodeId(el));
      if (!targetId) continue;
      badgesSeen += 1;
      const target = document.querySelector<HTMLElement>(`[data-node-id="${targetId}"]`);
      if (!target) { overlaps.push(`${nodeId(el)} badges no node named ${targetId}`); continue; }
      const a = el.getBoundingClientRect();
      const b = target.getBoundingClientRect();
      if (a.left < b.left - TOLERANCE || a.right > b.right + TOLERANCE ||
          a.top < b.top - TOLERANCE || a.bottom > b.bottom + TOLERANCE)
        overlaps.push(`${nodeId(el)} ${describeRect(a)} is not inside the node it badges, ${targetId} ${describeRect(b)}`);
    }
    return { overflows, overlaps, silentText, tooTight, lowContrast, checked, badgesSeen };
  }, [OUT_OF_FLOW_SUFFIXES as unknown as string[], TEXT_BEARING_KINDS as unknown as string[]] as const);
}

test.describe("sru-48 named visual criteria", () => {
  test.beforeEach(async ({ page }) => {
    await installCriteriaHelpers(page);
    await installRealFakeApp(page);
  });

  test.afterEach(async ({ page }) => {
    await stopRealFakeApp(page);
  });

  test("the criteria checklist names every criterion this file checks", async () => {
    // The checklist is the contract; this pins that it did not quietly shrink.
    expect(VISUAL_CRITERIA).toHaveLength(7);
    expect(new Set(VISUAL_CRITERIA).size).toBe(VISUAL_CRITERIA.length);
  });

  test("the surface renders at scale 1", async ({ page }) => {
    // Task 1.4's device-scale pin, asserted rather than assumed: `fitSurface`
    // applies `min(1, availableWidth / surfaceWidth)` to the surface root, so a
    // viewport narrower than the surface would silently scale every rectangle
    // this suite measures.
    const surface = await page.evaluate(() => {
      const root = document.querySelector<HTMLElement>('[data-synth-node-id="runtime.main.root"]')!;
      const rect = root.getBoundingClientRect();
      return { transform: getComputedStyle(root).transform, width: rect.width };
    });
    expect(surface.transform === "none" || surface.transform === "matrix(1, 0, 0, 1, 0, 0)").toBe(true);
    expect(surface.width).toBeGreaterThanOrEqual(COMPOSITE_SURFACE_WIDTH - 0.5);
    expect(VERIFICATION_VIEWPORT.width).toBeGreaterThanOrEqual(COMPOSITE_SURFACE_WIDTH);
  });

  test("controls in the same form-grid column share an x-position and a width", async ({ page }) => {
    for (const { surface, container } of FORM_GRID_PAGES) {
      await openSurface(page, surface);
      const report = await page.evaluate((containerId: string) => {
        const { TOLERANCE, nodeId, childrenOf } = window.__visualCriteria;
        const grid = document.querySelector<HTMLElement>(`[data-node-id="${containerId}"]`);
        if (!grid) return { violations: [`missing form grid ${containerId}`], rows: 0, columns: 0 };
        const rows = childrenOf(grid);
        const columns = new Map<number, HTMLElement[]>();
        let width = 0;
        let compared = 0;
        for (const row of rows) {
          const cells = childrenOf(row);
          if (cells.length === 0) continue;
          if (compared === 0) width = cells.length;
          else if (cells.length !== width) continue;
          compared += 1;
          cells.forEach((cell, index) => {
            if (!columns.has(index)) columns.set(index, []);
            columns.get(index)!.push(cell);
          });
        }
        const violations: string[] = [];
        for (const [index, cells] of columns) {
          const first = cells[0].getBoundingClientRect();
          for (const cell of cells.slice(1)) {
            const rect = cell.getBoundingClientRect();
            if (Math.abs(rect.left - first.left) > TOLERANCE)
              violations.push(`${nodeId(cell)} x=${rect.left} leaves column ${index} at x=${first.left}`);
            if (Math.abs(rect.width - first.width) > TOLERANCE)
              violations.push(`${nodeId(cell)} width=${rect.width} leaves column ${index} width ${first.width}`);
          }
        }
        return { violations, rows: compared, columns: columns.size };
      }, container);

      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      // A silent "no rows compared" is how an alignment check passes without
      // looking at anything.
      expect(report.rows, `${surface}: form grid compared no rows`).toBeGreaterThan(1);
      expect(report.columns, `${surface}: form grid compared no columns`).toBeGreaterThan(1);
    }
  });

  test("no node overflows its parent's containing rectangle on either axis", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate(() => {
        const { TOLERANCE, nodes, nodeId, parentNodeOf, contentRectOf, describeRect } =
          window.__visualCriteria;
        const violations: string[] = [];
        let checked = 0;
        for (const el of nodes()) {
          const parent = parentNodeOf(el);
          if (!parent) continue;
          checked += 1;
          const p = contentRectOf(parent);
          const c = el.getBoundingClientRect();
          if (c.left < p.left - TOLERANCE || c.right > p.right + TOLERANCE ||
              c.top < p.top - TOLERANCE || c.bottom > p.bottom + TOLERANCE) {
            violations.push(`${nodeId(el)} ${describeRect(c)} overflows ${nodeId(parent)} ${describeRect(p)}`);
          }
        }
        return { violations, checked };
      });

      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.checked, `${surface}: containment examined no nodes`).toBeGreaterThan(5);
    }
  });

  test("a scroll area clips its content and keeps it reachable", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    const scroll = page.locator(synthNode("runtime.controllers.scroll"));
    const tail = page.locator(synthNode("runtime.controllers.add_row"));

    const behaviour = await scroll.evaluate((element) => ({
      clipped: element.scrollHeight > element.clientHeight + 0.5,
      overflow: getComputedStyle(element).overflowY,
    }));
    // Twelve controllers is chosen to make this true; at three it is not, and
    // the whole scroll criterion would pass without a scroll area existing.
    expect(behaviour.clipped, "the 12-controller fixture must exceed the viewport").toBe(true);
    expect(behaviour.overflow).toBe("auto");
    await expect(tail).not.toBeInViewport();

    await scroll.evaluate((element) => { element.scrollTop = element.scrollHeight; });
    await expect(tail).toBeInViewport();
  });

  // The title says what this test actually establishes. It checks sibling
  // overlap on the four runtime pages, pins the sidebar badge inside the node it
  // annotates, and pins that these pages emit NO sru-25 underlay at all.
  //
  // It does NOT check underlay congruence in the browser, because no surface
  // reachable from this harness renders one -- underlays live in Braid 4 and
  // Mini App, which the fixture app does not emit and whose first-party launch
  // path is one of the six documented pre-existing Playwright failures.
  // Browser-rendered underlay congruence is therefore not covered here at all;
  // congruence is covered headlessly, with mutation evidence in both directions,
  // by `TestAnUnderlayIsPinnedToItsTargetRatherThanExemptedFromOverlap` in
  // `portable_ui_layout_tests.cpp`.
  test("no two siblings overlap, and these pages emit no underlay", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate((outOfFlow: string[]) => {
        const { TOLERANCE, nodes, nodeId, parentNodeOf, childrenOf, matchesAny, underlayTargetOf,
                badgeTargetOf, intersects, describeRect } = window.__visualCriteria;
        const violations: string[] = [];
        let comparedPairs = 0;
        for (const parent of [...nodes(), document.body]) {
          const children: HTMLElement[] = parent === document.body
            ? nodes().filter((el) => !parentNodeOf(el))
            : childrenOf(parent as HTMLElement);
          for (let a = 0; a < children.length; a += 1) {
            for (let b = a + 1; b < children.length; b += 1) {
              const first = children[a];
              const second = children[b];
              const firstId = nodeId(first);
              const secondId = nodeId(second);
              // Mirrors `SiblingOverlapViolations` in `VisualCriteria.hpp`
              // exactly. Two skips, and only two:
              //
              //  - the hand-centred status dots, which are out of flow;
              //  - an underlay against THE ONE SIBLING IT NAMES.
              //
              // An underlay is still compared against every other sibling, so
              // one that drifted onto its neighbour fails here. Congruence with
              // the named target is pinned headlessly, not in this file — see
              // the comment above this test. Before this, matching either
              // id against the out-of-flow list skipped every pair involving a
              // `.visualizer`, which is a far broader exemption than the
              // comment above it claimed.
              if (matchesAny(firstId, outOfFlow, []) || matchesAny(secondId, outOfFlow, [])) continue;
              if (underlayTargetOf(firstId) === secondId || underlayTargetOf(secondId) === firstId) continue;
              if (badgeTargetOf(firstId) === secondId || badgeTargetOf(secondId) === firstId) continue;
              comparedPairs += 1;
              const rectA = first.getBoundingClientRect();
              const rectB = second.getBoundingClientRect();
              if (intersects(rectA, rectB))
                violations.push(`${firstId} ${describeRect(rectA)} intersects ${secondId} ${describeRect(rectB)}`);
            }
          }
        }
        // A named badge is exempt against its target and nothing else, so what
        // it is doing has to be pinned rather than assumed: it must sit inside
        // the node it annotates. The sidebar badge renders only while MIDI
        // discovery has an unconfigured candidate, which these four pages do
        // not, so `badgesSeen` is reported here and pinned where a badge is
        // actually on screen -- the wizard test, which installs two Twisters.
        let badgesSeen = 0;
        for (const el of nodes()) {
          const targetId = badgeTargetOf(nodeId(el));
          if (!targetId) continue;
          badgesSeen += 1;
          const target = document.querySelector<HTMLElement>(`[data-node-id="${targetId}"]`);
          if (!target) { violations.push(`${nodeId(el)} badges no node named ${targetId}`); continue; }
          const a = el.getBoundingClientRect();
          const b = target.getBoundingClientRect();
          if (a.left < b.left - TOLERANCE || a.right > b.right + TOLERANCE ||
              a.top < b.top - TOLERANCE || a.bottom > b.bottom + TOLERANCE)
            violations.push(`${nodeId(el)} ${describeRect(a)} is not inside the node it badges, ${targetId} ${describeRect(b)}`);
        }
        // Underlays are COUNTED, not checked for congruence. There is no
        // congruence branch here on purpose: no surface this harness can reach
        // renders an underlay, so such a branch would be unreachable code
        // dressed as coverage -- which is what it was before. The count is the
        // live assertion, and it is pinned at zero below.
        const underlaysSeen = nodes().filter((el) => underlayTargetOf(nodeId(el)) !== "").length;
        return { violations, comparedPairs, underlaysSeen, badgesSeen };
      }, OUT_OF_FLOW_SUFFIXES as unknown as string[]);

      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.comparedPairs, `${surface}: overlap compared no sibling pairs`).toBeGreaterThan(3);
      // This is a statement of fact about these pages, not a congruence check:
      // they emit no sru-25 underlay. Asserting it keeps the fact honest instead
      // of letting an empty subject set read as coverage. The day a runtime page
      // does render one, this fails and asks for browser congruence coverage to
      // be written -- which is a decision for whoever adds that underlay, not
      // something to inherit from unreachable code sitting here.
      expect(report.underlaysSeen,
        `${surface}: a runtime page now renders an sru-25 underlay -- browser congruence coverage ` +
        `does not exist for it, so write it and replace this pin`).toBe(0);
      // Same treatment for the badge. No MIDI is installed in this test, so
      // discovery has no candidate and the sidebar warning does not render. The
      // live badge pin is in the wizard test, which does install one; if a badge
      // ever appears here too, this stops being a statement of fact.
      expect(report.badgesSeen,
        `${surface}: a declared badge now renders on a plain page -- pin what it does here as well`)
        .toBe(0);
    }
  });

  test("every gap and padding is a shared spacing-metric value", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate(([allowed, outOfFlow]: readonly [number[], string[]]) => {
        const { TOLERANCE, nodes, nodeId, contentRectOf, childrenOf, matchesAny } =
          window.__visualCriteria;
        const violations: string[] = [];
        const observed = new Set<number>();
        const round = (value: number) => Math.round(value * 100) / 100;
        const isAllowed = (value: number) =>
          allowed.some((candidate) => Math.abs(candidate - value) <= TOLERANCE);
        const record = (gap: number, before: string, after: string, parent: string) => {
          observed.add(round(gap));
          if (!isAllowed(gap))
            violations.push(`gap ${round(gap)} between ${before} and ${after} under ${parent} is not a shared spacing-metric value`);
        };
        for (const parent of nodes()) {
          const children = childrenOf(parent).filter((el) => !matchesAny(nodeId(el), outOfFlow, []));
          if (children.length === 0) continue;
          const container = contentRectOf(parent);
          const rects = new Map<HTMLElement, DOMRect>(
            children.map((el) => [el, el.getBoundingClientRect()] as const));
          // The container's own LEADING inset on both axes is its padding.
          // Only the leading one is measurable: design.md D3 rule 5 leaves
          // residual space no eligible child can absorb unallocated at the END
          // of the container, so a trailing inset is legitimately padding plus
          // slack.
          record(Math.min(...children.map((el) => rects.get(el)!.left)) - container.left,
                 nodeId(parent), "its leading edge on x", nodeId(parent));
          record(Math.min(...children.map((el) => rects.get(el)!.top)) - container.top,
                 nodeId(parent), "its leading edge on y", nodeId(parent));
          if (children.length < 2) continue;

          // A wrapping row puts its children on several lines, so one sorted
          // sequence along the main axis would read the wrap itself as a large
          // negative gap. Group by cross-axis offset first.
          const horizontal = parent.dataset.nodeKind === "row";
          const mainStart = (el: HTMLElement) => horizontal ? rects.get(el)!.left : rects.get(el)!.top;
          const mainEnd = (el: HTMLElement) => horizontal ? rects.get(el)!.right : rects.get(el)!.bottom;
          const crossStart = (el: HTMLElement) => horizontal ? rects.get(el)!.top : rects.get(el)!.left;
          const crossEnd = (el: HTMLElement) => horizontal ? rects.get(el)!.bottom : rects.get(el)!.right;
          const lines = new Map<number, HTMLElement[]>();
          for (const el of children) {
            const key = round(crossStart(el));
            if (!lines.has(key)) lines.set(key, []);
            lines.get(key)!.push(el);
          }
          const offsets = [...lines.keys()].sort((a, b) => a - b);
          for (const offset of offsets) {
            const line = lines.get(offset)!.sort((a, b) => mainStart(a) - mainStart(b));
            for (let ix = 1; ix < line.length; ix += 1)
              record(mainStart(line[ix]) - mainEnd(line[ix - 1]), nodeId(line[ix - 1]), nodeId(line[ix]), nodeId(parent));
          }
          for (let ix = 1; ix < offsets.length; ix += 1) {
            const previous = lines.get(offsets[ix - 1])!;
            const next = lines.get(offsets[ix])!;
            record(offsets[ix] - Math.max(...previous.map(crossEnd)),
                   nodeId(previous[0]), nodeId(next[0]), nodeId(parent));
          }
        }
        return { violations, observed: [...observed] };
      }, [SPACING_METRIC_VALUES as unknown as number[], OUT_OF_FLOW_SUFFIXES as unknown as string[]] as const);

      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.observed.length, `${surface}: spacing measured nothing`).toBeGreaterThan(1);
    }
  });

  test("every text element fits its allocated extent", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate((textKinds: string[]) => {
        const { TOLERANCE, TEXT_FIT_TOLERANCE, nodes, nodeId, textNeededWidth, textAvailableWidth } =
          window.__visualCriteria;
        const violations: string[] = [];
        let measured = 0;
        for (const el of nodes()) {
          if (!textKinds.includes(el.dataset.nodeKind!)) continue;
          if (!el.textContent || !el.textContent.trim()) continue;
          measured += 1;
          const neededWidth = textNeededWidth(el);
          const availableWidth = textAvailableWidth(el);
          if (neededWidth > availableWidth + TEXT_FIT_TOLERANCE)
            violations.push(`${nodeId(el)} needs ${neededWidth.toFixed(2)} in ${availableWidth.toFixed(2)}: "${el.textContent.trim()}"`);
          if (el.scrollHeight > el.clientHeight + TOLERANCE)
            violations.push(`${nodeId(el)} needs ${el.scrollHeight} high in ${el.clientHeight}: "${el.textContent.trim()}"`);
        }
        return { violations, measured };
      }, TEXT_BEARING_KINDS as unknown as string[]);

      // A failure names a too-tight metrics reservation, not a page bug.
      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.measured, `${surface}: text fit measured no text`).toBeGreaterThan(2);
    }
  });

  test("every form control has a visible caption", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      // PER SURFACE, so that every entry must match on the surface it names.
      // A single flat list across all four pages cannot be pinned for equality:
      // most entries would be absent on most pages, which is how a stale entry
      // survives forever.
      const exceptions = surface === "controllers"
        ? controllerCaptionExceptions(FIXTURE_CONTROLLER_COUNT)
        : [];
      const report = await page.evaluate((excepted: string[]) => {
        const { nodes, nodeId } = window.__visualCriteria;
        const violations: string[] = [];
        let examined = 0;
        let residualsMatched = 0;
        let total = 0;
        for (const el of nodes()) {
          const kind = el.dataset.nodeKind;
          if (!["combo-box", "text-field", "toggle", "slider"].includes(kind!)) continue;
          const id = nodeId(el);
          total += 1;
          // Membership, not a pattern. A control the page grows is not on the
          // list, so it is examined and must carry a caption.
          if (excepted.includes(id)) { residualsMatched += 1; continue; }
          examined += 1;
          if (document.querySelector(`[data-node-id="${id}.caption"]`)) continue;
          // A toggle renders its own label; a combo box and a text field do
          // not, whatever their `label` field says (design.md OQ5).
          if (kind === "toggle" && el.textContent && el.textContent.trim()) continue;
          violations.push(id);
        }
        return { violations, examined, residualsMatched, total };
      }, exceptions.map((exception) => exception.id));

      expect(report.violations, `${surface}: uncaptioned ${report.violations.join("; ")}`).toEqual([]);
      // EQUALITY, mirroring `RequireSurfaceMeetsTheNamedCriteria` in the
      // headless half: every named exception must be present on this surface.
      // `> 0` was not enough -- it lets an entry naming a control that has been
      // renamed or removed sit in the list forever, waiving something that no
      // longer exists, which is how an exception list rots into a permanent
      // waiver.
      expect(report.residualsMatched,
        `${surface}: the caption exception list names ${exceptions.length} controls but ` +
        `${report.residualsMatched} are on this surface -- an entry is stale`)
        .toBe(exceptions.length);
      // Every surface that declares form controls must have looked at
      // something. `controllers` is the one page where every control is a
      // table cell and therefore excepted, so its floor is on the exceptions
      // instead -- which is exactly the page where a silent pass would matter
      // most, so its total is pinned separately below.
      if (report.total > 0 && surface !== "controllers")
        expect(report.examined, `${surface}: caption check examined no form control`).toBeGreaterThan(0);
      if (surface === "controllers") {
        // Each collapsed row carries an input and an output selector; the add
        // row carries its preset combo. The rename field belongs to a row's
        // expanded editor and is not on this surface. A control the page grows
        // moves this number and fails here by name, which is what the old
        // suffix class could not do.
        expect(report.total,
          "controllers: the page carries a different number of form controls than the fixture declares")
          .toBe(FIXTURE_CONTROLLER_COUNT * 2 + 1);
      }
    }
  });

  test("no text element renders an empty string", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate(() => {
        const { TOLERANCE, nodes, nodeId, describeRect } = window.__visualCriteria;
        const violations: string[] = [];
        let examined = 0;
        for (const el of nodes()) {
          if (!["label", "status-text"].includes(el.dataset.nodeKind!)) continue;
          examined += 1;
          const rect = el.getBoundingClientRect();
          if (rect.width > TOLERANCE && rect.height > TOLERANCE &&
              (!el.textContent || !el.textContent.trim()))
            violations.push(`${nodeId(el)} reserves ${describeRect(rect)} and renders no text`);
        }
        return { violations, examined };
      });
      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.examined,
        `${surface}: the empty-text check found no label or status text to examine`).toBeGreaterThan(0);
    }
  });

  test("text contrast meets WCAG AA 4.5:1 after compositing alpha and opacity", async ({ page }) => {
    await seedControllers(page, FIXTURE_CONTROLLER_COUNT);
    for (const surface of ALL_SURFACES) {
      await openSurface(page, surface);
      const report = await page.evaluate((textKinds: string[]) => {
        const { nodes, nodeId, contrastRatio } = window.__visualCriteria;
        const violations: string[] = [];
        let measured = 0;
        let faded = 0;
        for (const el of nodes()) {
          if (!textKinds.includes(el.dataset.nodeKind!)) continue;
          if (!el.textContent || !el.textContent.trim()) continue;
          const contrast = contrastRatio(el);
          if (!contrast) continue;
          measured += 1;
          if (contrast.alpha < 1) faded += 1;
          if (contrast.ratio < 4.5)
            violations.push(`${nodeId(el)} ${contrast.ratio.toFixed(2)}:1 (${contrast.painted} on ${contrast.background}, alpha ${contrast.alpha.toFixed(2)})`);
        }
        return { violations, measured, faded };
      }, TEXT_BEARING_KINDS as unknown as string[]);

      expect(report.violations, `${surface}: ${report.violations.join("; ")}`).toEqual([]);
      expect(report.measured, `${surface}: contrast measured no text`).toBeGreaterThan(2);
    }
  });

  // Mutation evidence for the two browser-only criteria. Every headless
  // predicate has a test proving it can fail and name its offender
  // (`portable_ui_layout_tests.cpp`); text fit and contrast had none, and they
  // are the two the headless half structurally cannot do. These inject a
  // failure into the live DOM and require the same code that passes above to
  // catch it.
  test("the text-fit and contrast checks can actually fail", async ({ page }) => {
    await openSurface(page, "sync");

    const textFit = await page.evaluate(() => {
      const { TEXT_FIT_TOLERANCE, nodes, nodeId, textNeededWidth, textAvailableWidth } = window.__visualCriteria;
      const label = nodes().find((el) => el.dataset.nodeKind === "label" && !!el.textContent?.trim());
      if (!label) return { injected: false, caught: [] as string[] };
      const width = label.style.width;
      // Squeeze the reservation, exactly as a too-tight metrics entry would.
      label.style.width = "4px";
      const caught: string[] = [];
      // The sub-pixel measurement, not the retired integer one: this must
      // stay proof that the CURRENT text-fit code can fail, not proof that a
      // superseded reimplementation of it can.
      if (textNeededWidth(label) > textAvailableWidth(label) + TEXT_FIT_TOLERANCE) caught.push(nodeId(label));
      label.style.width = width;
      return { injected: true, caught };
    });
    expect(textFit.injected, "sync must render a label to squeeze").toBe(true);
    expect(textFit.caught, "a label squeezed to 4px must fail the text-fit check").not.toEqual([]);

    const contrast = await page.evaluate(() => {
      const { nodes, contrastRatio } = window.__visualCriteria;
      const label = nodes().find((el) => el.dataset.nodeKind === "label" && !!el.textContent?.trim());
      if (!label) return { before: 0, afterColour: 0, afterOpacity: 0 };
      const before = contrastRatio(label)!.ratio;
      const colour = label.style.color;
      const opacity = label.style.opacity;
      // Two independent ways to fail: a low-contrast colour, and a colour that
      // passes on paper but is faded by opacity. The second is the one the old
      // check approved, because it read `color` and ignored compositing.
      label.style.color = "rgb(24, 26, 28)";
      const afterColour = contrastRatio(label)!.ratio;
      label.style.color = colour;
      label.style.opacity = "0.15";
      const afterOpacity = contrastRatio(label)!.ratio;
      label.style.opacity = opacity;
      return { before, afterColour, afterOpacity };
    });
    expect(contrast.before, "the unmodified label passes today").toBeGreaterThanOrEqual(4.5);
    expect(contrast.afterColour, "a near-background colour must fail").toBeLessThan(4.5);
    expect(contrast.afterOpacity, "a passing colour faded to 15% opacity must also fail").toBeLessThan(4.5);

    // The case the two mutations above structurally CANNOT reach, and the one
    // this check exists for: an element with its OWN OPAQUE BACKGROUND, faded.
    // A label has a transparent background, so its backdrop is an ancestor's
    // either way and the round-2 compositing agreed with the correct one by
    // accident. Give it an opaque background and the two models diverge: the
    // old one measured glyphs against that background at full strength, which
    // is a rendering the browser never produces.
    const faded = await page.evaluate(() => {
      const { nodes, contrastRatio } = window.__visualCriteria;
      const label = nodes().find((el) => el.dataset.nodeKind === "label" && !!el.textContent?.trim());
      if (!label) return null;
      const saved = { background: label.style.backgroundColor, colour: label.style.color, opacity: label.style.opacity };
      // Opaque field-like background with strong text on it: at full strength
      // this passes comfortably.
      label.style.backgroundColor = "rgb(255, 255, 255)";
      label.style.color = "rgb(0, 0, 0)";
      const opaque = contrastRatio(label)!;
      // Now fade the group. Both the white background and the black glyphs
      // collapse toward the backdrop together, so the ratio goes to ~1:1.
      label.style.opacity = "0.05";
      const dimmed = contrastRatio(label)!;
      Object.assign(label.style, {
        backgroundColor: saved.background, color: saved.colour, opacity: saved.opacity,
      });
      return { opaque, dimmed };
    });
    expect(faded, "sync must render a label to fade").not.toBeNull();
    expect(faded!.opaque.ratio, "black on an opaque white background passes at full strength")
      .toBeGreaterThanOrEqual(4.5);
    expect(faded!.dimmed.ratio, "the same element at 5% opacity must fail").toBeLessThan(4.5);
    // And the fix itself, pinned directly rather than only through the ratio:
    // the background the ratio was measured against is the FADED one, not the
    // element's declared white. Round 2 reported "rgb(255, 255, 255)" here.
    expect(faded!.dimmed.background,
      "the measured background must be the composited one, not the element's declared background")
      .not.toBe("rgb(255, 255, 255)");
  });

  // Sync's validation error and warning are named states in task 1.4's fixture
  // and were previously evaluated only headlessly, even though the browser can
  // reach them: they are a consequence of what is typed into the PPQN field, not
  // of host state. So they are driven here through the page's own field. This is
  // the state that produced the empty-band defect 6.2 fixed, and it is the one
  // state where two diagnostic text nodes are on screen at once -- exactly the
  // shape a too-tight reservation or a low-contrast muted style shows up in.
  test("the Sync page's diagnostic state meets the structural criteria", async ({ page }) => {
    await openSurface(page, "sync");
    const ppqn = page.locator(`${synthNode("runtime.sync.ppqn")} input`);

    // A rejected value: the validation band is present and says something.
    await ppqn.fill("96x");
    const validation = page.locator(synthNode("runtime.sync.validation"));
    await expect(validation).toContainText("1 to 960");
    const invalid = await evaluateStructuralCriteria(page);
    expect(invalid.overflows, `sync invalid: ${invalid.overflows.join("; ")}`).toEqual([]);
    expect(invalid.overlaps, `sync invalid: ${invalid.overlaps.join("; ")}`).toEqual([]);
    expect(invalid.silentText, `sync invalid: ${invalid.silentText.join("; ")}`).toEqual([]);
    expect(invalid.tooTight, `sync invalid: ${invalid.tooTight.join("; ")}`).toEqual([]);
    expect(invalid.lowContrast, `sync invalid: ${invalid.lowContrast.join("; ")}`).toEqual([]);
    expect(invalid.checked, "sync invalid: examined no nodes").toBeGreaterThan(5);

    // A valid but nonstandard value: the validation band is GONE (an empty band
    // would fail `silentText`, which is the point of asserting absence) and the
    // warning band is present instead.
    await ppqn.fill("96");
    await expect(validation).toHaveCount(0);
    await expect(page.locator(synthNode("runtime.sync.warning"))).toContainText("nonstandard");
    const warned = await evaluateStructuralCriteria(page);
    expect(warned.overflows, `sync warning: ${warned.overflows.join("; ")}`).toEqual([]);
    expect(warned.overlaps, `sync warning: ${warned.overlaps.join("; ")}`).toEqual([]);
    expect(warned.silentText, `sync warning: ${warned.silentText.join("; ")}`).toEqual([]);
    expect(warned.tooTight, `sync warning: ${warned.tooTight.join("; ")}`).toEqual([]);
    expect(warned.lowContrast, `sync warning: ${warned.lowContrast.join("; ")}`).toEqual([]);
    expect(warned.checked, "sync warning: examined no nodes").toBeGreaterThan(5);
  });

  // The wizard chooser and the wizard form are named states in task 1.4's
  // fixture, and they were previously evaluated only headlessly -- so a
  // browser-only regression in their chrome, text metrics or contrast passed
  // unseen. They are driven here through the page's own MIDI discovery rather
  // than injected, so what is measured is the real surface.
  test("the wizard chooser and form meet the structural criteria", async ({ page }) => {
    await installTwisterPair(page, 1);
    await installTwisterPair(page, 2);
    await openSurface(page, "controllers");
    const launch = page.locator(synthNode("runtime.controllers.wizard.launch"));
    await expect(launch).toBeEnabled({ timeout: 10_000 });
    await launch.click();

    const chooserCandidates = page.locator(
      '[data-synth-node-id^="runtime.controllers.wizard.chooser.candidate."]');
    await expect(chooserCandidates).toHaveCount(2);
    const chooser = await evaluateStructuralCriteria(page);
    expect(chooser.overflows, `chooser: ${chooser.overflows.join("; ")}`).toEqual([]);
    expect(chooser.overlaps, `chooser: ${chooser.overlaps.join("; ")}`).toEqual([]);
    expect(chooser.silentText, `chooser: ${chooser.silentText.join("; ")}`).toEqual([]);
    expect(chooser.tooTight, `chooser: ${chooser.tooTight.join("; ")}`).toEqual([]);
    expect(chooser.lowContrast, `chooser: ${chooser.lowContrast.join("; ")}`).toEqual([]);
    expect(chooser.checked, "chooser: examined no nodes").toBeGreaterThan(5);
    // Two Twisters are discovered and unconfigured, so the sidebar's warning
    // badge is on screen. This is the one state in the browser half where the
    // badge exemption has a live subject, so the "sits inside its target" pin
    // above is real coverage rather than an absence.
    expect(chooser.badgesSeen,
      "chooser: no declared badge rendered, so the badge exemption was never exercised")
      .toBeGreaterThan(0);

    await chooserCandidates.first().click();
    await expect(page.locator(synthNode("runtime.controllers.wizard.submit"))).toBeVisible();
    const form = await evaluateStructuralCriteria(page);
    // The Twister form declares itself wider than the 640 page that shows it
    // (664 when the overhang was found, 684 now that the message selectors were
    // widened to stop clipping their own text), and it used to overhang and be
    // clipped. The page now hosts it in a ScrollArea, and containment is against
    // that region's scroll-content rectangle. A regression here means the form
    // is being cut off again.
    expect(form.overflows, `wizard form: ${form.overflows.join("; ")}`).toEqual([]);
    expect(form.overlaps, `wizard form: ${form.overlaps.join("; ")}`).toEqual([]);
    expect(form.silentText, `wizard form: ${form.silentText.join("; ")}`).toEqual([]);
    expect(form.tooTight, `wizard form: ${form.tooTight.join("; ")}`).toEqual([]);
    expect(form.lowContrast, `wizard form: ${form.lowContrast.join("; ")}`).toEqual([]);
    expect(form.checked, "wizard form: examined no nodes").toBeGreaterThan(20);

    // And the fields the old overhang cut off are reachable by scrolling
    // rather than lost.
    const scroll = page.locator(synthNode("runtime.controllers.wizard.form.scroll"));
    await expect(scroll).toHaveCount(1);
    const argument = page.locator(synthNode("controller-wizard.twister.button.5.argument"));
    await expect(argument).toHaveCount(1);
    await argument.scrollIntoViewIfNeeded();
    await expect(argument).toBeInViewport();
  });

  // `seedControllers` adds rows but never opens one, so no criterion in this
  // file had ever seen a controller row's own editor -- its disclosure
  // toggle, a section toggle, or the mapping-group header underneath an open
  // section. That header is exactly where a too-tight column reservation
  // (the "Start Pos" column) shipped: nothing here had ever rendered it.
  // Driven through the row's own disclosure and section-toggle controls, the
  // same pattern the wizard chooser/form test above uses for its own driven
  // state.
  //
  // This state is also re-checked across CONTROLLER_WIDTH_SWEEP instead of at
  // the single fixed viewport every other structural check in this file
  // uses; see the comment on that constant for what the range covers and why.
  test("the controller row's Encoders group header meets the structural criteria across a range of widths", async ({ page }) => {
    await openSurface(page, "controllers");
    // The add row's Preset combo already defaults to the library's own
    // registered wizard descriptor (MIDI Fighter Twister) -- Add needs no
    // other input to produce a row with an Encoders section.
    await page.locator(synthNode("runtime.controllers.add_button")).click();
    await expect(page.locator(synthNode("runtime.controllers.row.0"))).toHaveCount(1);

    await page.locator(synthNode("runtime.controllers.row.0.disclosure")).click();
    const encodersToggle = page.locator(synthNode("runtime.controllers.row.0.section.0.toggle"));
    await expect(encodersToggle).toBeVisible({ timeout: 10_000 });
    await encodersToggle.click();

    // The state actually arrived, not just that nothing threw: the Encoders
    // Turn and Push group headers each carry one "Start Pos" column -- the
    // exact label whose reservation this suite could not previously see.
    await expect(page.getByText("Start Pos", { exact: true })).toHaveCount(2);

    // Resize, then wait for `fitSurface`'s own shrink transform to actually
    // settle at the scale this width implies, rather than assuming a fixed
    // delay is enough -- the same "assert the premise" rule the scale-1 test
    // above applies to the file's default viewport.
    const waitForScaleAtWidth = async (width: number) => {
      await page.setViewportSize({ width, height: VERIFICATION_VIEWPORT.height });
      const expectedScale = Math.min(1, width / COMPOSITE_SURFACE_WIDTH);
      await page.waitForFunction((expected: number) => {
        const root = document.querySelector<HTMLElement>('[data-synth-node-id="runtime.main.root"]');
        if (!root) return false;
        const transform = getComputedStyle(root).transform;
        const match = transform.match(/matrix\(([-\d.]+),/);
        const scale = transform === "none" ? 1 : match ? Number(match[1]) : 1;
        return Math.abs(scale - expected) < 0.01;
      }, expectedScale);
    };

    for (const width of CONTROLLER_WIDTH_SWEEP) {
      await waitForScaleAtWidth(width);
      const report = await evaluateStructuralCriteria(page);
      expect(report.overflows, `width ${width}: ${report.overflows.join("; ")}`).toEqual([]);
      expect(report.overlaps, `width ${width}: ${report.overlaps.join("; ")}`).toEqual([]);
      expect(report.silentText, `width ${width}: ${report.silentText.join("; ")}`).toEqual([]);
      expect(report.tooTight, `width ${width}: ${report.tooTight.join("; ")}`).toEqual([]);
      expect(report.lowContrast, `width ${width}: ${report.lowContrast.join("; ")}`).toEqual([]);
      expect(report.checked, `width ${width}: examined no nodes`).toBeGreaterThan(5);
    }
  });

  // sru-48 asks for a rendered re-render at a second root extent, asserting
  // weighted children redistribute while fixed and intrinsic ones hold. The
  // headless half proves the resolver does this; what only the browser can show
  // is that the DOM the backend built actually followed.
  //
  // ROUND 2 GOT THIS WRONG, and the way it was wrong is worth keeping written
  // down. It resized the VIEWPORT and compared before and after. But a page's
  // content bounds are set once, in `RuntimeMainComponent`'s constructor, from
  // `App::Config().uiHeight` -- a per-app compile-time declaration -- and
  // `fitSurface` applies only a shrink-only *width* scale on top. So the
  // resolver never saw a second extent, every measurement was identical, and
  // the test asserted `0 ≈ 0`. It could not fail.
  //
  // The root extent is varied the one way this shell allows: a second fixture
  // app declaring `uiHeight = 720` instead of 480, compiled from the same
  // header. Same page producer, same backend, two genuinely different root
  // extents, both rendered. The premise assertions below fail loudly if that
  // ever stops being true, rather than degrading to a comparison of a layout
  // with itself.
  test("a second root extent redistributes weighted children in the rendered DOM", async ({ page, browser }) => {
    const measure = async (target: Page) => target.evaluate(() => {
      const rect = (id: string) => {
        const el = document.querySelector<HTMLElement>(`[data-node-id="${id}"]`);
        return el ? el.getBoundingClientRect() : null;
      };
      return {
        rootHeight: rect("runtime.sync.root")?.height ?? 0,
        weighted: rect("runtime.sync.status")?.height ?? 0,   // the absorbing region
        fixedButton: rect("runtime.sync.back")?.height ?? 0,  // Extent::Px, must not move
        intrinsicForm: rect("runtime.sync.form")?.height ?? 0, // intrinsic stack, must not move
      };
    });

    // The standard 480-high fixture is already installed by `beforeEach`.
    await openSurface(page, "sync");
    const short = await measure(page);

    // The 720-high fixture gets its OWN context rather than being installed
    // over the running one: `installRealFakeApp` builds a whole launcher and
    // runtime client per page, and the teardown contract in `stopRealFakeApp`
    // is per-app. Two contexts keeps each app's lifecycle intact and still
    // measures the same producer through the same backend.
    const tallContext = await browser.newContext({
      viewport: { ...VERIFICATION_VIEWPORT },
      deviceScaleFactor: 1,
    });
    const tallPage = await tallContext.newPage();
    let tall: Awaited<ReturnType<typeof measure>>;
    try {
      await installCriteriaHelpers(tallPage);
      await installRealFakeApp(tallPage, FIXTURE_APPS.tall);
      await openSurface(tallPage, "sync");
      tall = await measure(tallPage);
      await stopRealFakeApp(tallPage);
    } finally {
      await tallContext.close();
    }

    // The premise, asserted rather than assumed: two DIFFERENT root extents were
    // actually rendered, and they differ by exactly what the two apps declare.
    // Without this the rest degrades into `0 ≈ 0`, which is precisely how the
    // round-2 version of this test passed while proving nothing.
    expect(short.rootHeight, "the short surface rendered").toBeGreaterThan(0);
    expect(tall.rootHeight - short.rootHeight,
      `the two fixture apps must resolve at different root extents, got ${short.rootHeight} and ${tall.rootHeight}`)
      .toBeCloseTo(FIXTURE_APPS.tall.uiHeight - FIXTURE_APPS.standard.uiHeight, 1);

    // A different root extent must not resize a fixed or intrinsic child: if it
    // does, something outside the library is sizing them.
    expect(tall.fixedButton, "the fixed-extent Back button held its height").toBeCloseTo(short.fixedButton, 1);
    expect(tall.intrinsicForm, "the intrinsic form held its height").toBeCloseTo(short.intrinsicForm, 1);
    // The weighted region grows, and it absorbs the WHOLE difference -- nothing
    // else moved and nothing was left unallocated.
    expect(tall.weighted, "the absorbing region grew").toBeGreaterThan(short.weighted);
    expect(tall.weighted - short.weighted).toBeCloseTo(tall.rootHeight - short.rootHeight, 1);
  });
});
