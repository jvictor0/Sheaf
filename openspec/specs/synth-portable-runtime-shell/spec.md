# synth-portable-runtime-shell Specification

Project: `projects/synth`. ID prefix: `sprs`.

## Purpose

Define the shared portable runtime shell that lets JUCE and browser runtimes host the same application-shaped synth app through one JUCE-free main component, host-service adapters, and browser UI/audio/MIDI verification.
## Requirements
### Requirement: sprs-1 — Composition: shared portable runtime main component
WHEN a full synth application is hosted by a UI runtime, THE synth library SHALL instantiate one JUCE-free main component templated on the application and host services that composes the application's portable surface, runtime sidebar, and runtime pages into one portable node tree and is used by both the JUCE and browser runtimes.

#### Scenario: Both hosts use the same component
- **WHEN** the JUCE and browser runtime templates are instantiated for the same conforming application
- **THEN** each host builds UI frames and dispatches UI actions through the shared portable runtime main component
- **AND** neither host separately defines top-level sidebar or page navigation behavior

#### Scenario: Concrete app knowledge is absent
- **WHEN** the shared component and both host adapters are inspected
- **THEN** they depend only on the application concept, portable surface, runtime services, and generic runtime page models
- **AND** they contain no miniapp-specific or other concrete-app layout, node IDs, actions, parameters, or branches

### Requirement: sprs-2 — Layout: app content remains intact beside runtime chrome
WHEN the shared main component builds its default tree for an application surface that does not declare its own root bounds (sprs-19), and whenever a runtime page is shown for any surface, THE component SHALL preserve the application's configured origin-zero content rectangle without clipping, SHALL compose subtrees using parent-relative node coordinates so that placing a subtree's root places every descendant with it, SHALL position the fixed 96-pixel runtime sidebar root immediately to the app's right, and SHALL make runtime chrome additive to the application width; every subtree the component hands to a backend SHALL arrive fully laid out by the component library's resolver — nodes built without explicit bounds are resolved within their containing subtree root's extent before composition, and no backend positions or sizes them.

#### Scenario: App dimensions are preserved
- **WHEN** an app config declares a 900 by 560 UI and its portable root matches those dimensions
- **THEN** the composite root is 996 by 560
- **AND** the app subtree root remains at 0,0 relative to the composite root with dimensions 900 by 560
- **AND** the sidebar root sits at x 900 relative to the composite root
- **AND** sidebar descendants carry coordinates relative to their parents, resolving on screen within the x 900 through 996 band without any per-descendant translation

#### Scenario: Library-resolved content stays inside app content
- **WHEN** an application builds semantic controls through the component library without explicit bounds
- **THEN** the library resolves their bounds within the application root's 900-pixel width before the tree reaches any backend
- **AND** no control resolves into the sidebar band
- **AND** neither backend computes a position or size for them

#### Scenario: Runtime page replaces only app content
- **WHEN** the Audio, Controllers, or File page opens
- **THEN** exactly that page occupies the 900 by 560 app content rectangle
- **AND** the sidebar remains visible and unchanged at the right

#### Scenario: Invalid app root fails generically
- **WHEN** an application's portable tree has no single origin-zero root matching its configured positive UI dimensions, contains duplicate IDs, references unknown children, contains a cycle, or uses the reserved `runtime.*` node namespace
- **THEN** composition fails with a diagnostic naming the violated portable-tree contract
- **AND** no concrete-app fallback is used

#### Scenario: A self-sized surface with a sidebar slot
- **WHEN** an application surface declares its own root bounds and a sidebar slot (sprs-19)
- **THEN** the composite root has exactly the declared app root bounds
- **AND** the sidebar root sits at the slot node's position relative to the composite root
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestSidebarIsPlacedAtTheDeclaredSlot.

### Requirement: sprs-3 — Services: compile-time-swappable host adapters
WHEN the shared component needs audio, MIDI/controller, patch/file, deadline, or runtime-configuration behavior, THE component SHALL obtain it through a host services concept selected at compile time, with JUCE and browser adapters that expose equivalent generic page snapshots and actions while keeping host API objects outside application code.

#### Scenario: JUCE services retain desktop behavior
- **WHEN** the JUCE main component refreshes or dispatches a runtime page
- **THEN** its adapter delegates to the existing JUCE runtime, engine, audio manager, MIDI connection manager, and patch operations
- **AND** existing desktop page behavior remains available

#### Scenario: Browser audio choices remain generic
- **WHEN** the browser Audio page snapshot is built
- **THEN** it contains exactly one output option with id `system_default` and label `System Default`
- **AND** it contains no input selector or named output-device enumeration

#### Scenario: Browser runtime pages remain application-agnostic
- **WHEN** browser sidebar actions open Audio, Controllers, or File
- **THEN** the shared runtime page models build and dispatch those pages through browser services
- **AND** application code and browser HTML remain unchanged for the selected page

#### Scenario: Controllers refresh through host services
- **WHEN** a UI tick observes changed controller endpoints, connection state, or instrument state
- **THEN** the active host services refresh the shared Controllers page surface and its device list
- **AND** JUCE and browser hosts do not maintain separate controller page models

#### Scenario: File page patch semantics are shared
- **WHEN** the shared File page dispatches direct or confirmed patch actions for New, Save, Revert, Confirmed Save As, Confirmed Overwrite Save As, or Confirmed Load
- **THEN** one JUCE-free runtime-file helper maps those actions to host-provided patch operations and shared status text for both JUCE and browser services
- **AND** raw Save As and Load actions remain shared File page surface actions that open the file browser/confirmation flow rather than invoking patch operations directly
- **AND** the helper projects current patch directory, patch name, patches root, and status text into `FilePageSnapshot` for both hosts
- **AND** host adapters provide only the backend-specific patch operation bindings and data paths, with JUCE patch bindings continuing to log their patch command results
- **AND** neither host maintains an independent File action ladder with duplicated status strings

#### Scenario: Browser deadline sample comes from the runtime-owned callback
- **WHEN** the browser host refreshes the sidebar before a browser callback-load metric has been published
- **THEN** its services adapter supplies 0.0 percent
- **AND** after runtime-owned AudioWorklet callbacks have run, it supplies the averaged callback deadline percentage rather than deriving a misleading value from the legacy audio render timer
- **AND** the JUCE host continues to supply its device-manager CPU usage percentage

### Requirement: sprs-4 — Interaction: browser pointer parity
WHILE a browser-rendered portable node with a pointer-drag action is being dragged, THE browser backend SHALL capture that pointer and dispatch accepted incremental deltas using the JUCE formula `(dx - dy) * 0.0025`, the absolute threshold `0.001`, current surface scale compensation, and existing replacement-delta action formatting.

#### Scenario: Horizontal and vertical motion combine incrementally
- **WHEN** a captured pointer moves right by 8 CSS pixels and up by 4 CSS pixels on an unscaled surface
- **THEN** the backend dispatches a delta of 0.03
- **AND** the next move is measured from that accepted pointer position rather than the original pointer-down position

#### Scenario: Capture keeps an edge drag alive
- **WHEN** a drag starts inside an encoder and the pointer moves outside its element before release
- **THEN** pointer moves continue to dispatch to the originating node until pointer up, cancel, or lost capture

#### Scenario: Tiny movement is retained until meaningful
- **WHEN** incremental movement produces an absolute delta below 0.001
- **THEN** no action is dispatched and the accepted anchor is not advanced

### Requirement: sprs-5 — Drawing: rounded portable arc parity
WHEN a portable Arc draw command is rendered by the browser Canvas backend, THE backend SHALL use rounded line caps and joins for that command without leaking Canvas state to subsequent commands.

#### Scenario: Stationary indicator remains visible
- **WHEN** an encoder indicator is represented by a zero-length or near-zero-length stroked arc
- **THEN** the browser rendering shows a rounded dot/cap comparable to the JUCE backend

#### Scenario: Arc state is isolated
- **WHEN** an Arc command is followed by a command with different stroke semantics
- **THEN** the following command is rendered with its own stroke state

### Requirement: sprs-7 — Verification: cross-backend and Playwright coverage
WHEN the shared main component change is verified, THE synth project SHALL include JUCE-free component/service tests, retained JUCE backend tests, TypeScript backend tests, and Playwright Chromium tests that exercise the static website, shared sidebar/pages, app rendering, audio flow, bidirectional SysEx MIDI with multiple devices and reconnect polling, mouse gestures, and desktop/narrow visual layout.

#### Scenario: Generic fake app runs first
- **WHEN** browser integration tests execute
- **THEN** a generic fake conforming app proves the shared component and browser services without concrete miniapp browser code
- **AND** the real miniapp smoke reuses exactly that runtime and backend

#### Scenario: Browser behavior is visible and interactive
- **WHEN** Playwright opens the built static site and activates the runtime
- **THEN** screenshots show intact app content and the right sidebar
- **AND** tests can open and return from runtime pages, drag and double-click app controls, observe finite non-silent audio, and verify MIDI bytes flow in both directions including SysEx

### Requirement: sprs-8 — Browser audio: runtime-owned AudioWorklet callback
WHEN the Chrome static site starts audio for any conforming browser-hosted application, THE browser runtime SHALL require browser ABI v2, use a generic Emscripten Wasm AudioWorklet callback that invokes the C++ `Runtime<App>::Process` path against the same runtime/engine instance used by UI, MIDI, patch, and controller operations, register the host `AudioContext` acquired synchronously by catalog selection with the selected module before native callback startup, and SHALL NOT schedule DSP production through a timer, animation frame, message-loop cadence, ScriptProcessor, or JavaScript sample ring.

#### Scenario: No duplicated application runtime
- **WHEN** the browser audio path starts
- **THEN** it does not construct a second application or runtime instance inside JavaScript or an AudioWorklet
- **AND** it does not use concrete-application JavaScript, HTML, node IDs, actions, or layout

#### Scenario: One selection click starts the native callback
- **WHEN** a user selects a catalog application before its package has been downloaded
- **THEN** the synchronous selection handler creates and resumes one host audio context before awaiting package work
- **AND** after verification and module initialization the generic host registers that same context with the module and starts its native Wasm AudioWorklet
- **AND** no second user gesture or second audio context is required

#### Scenario: Missing native support fails closed
- **WHEN** a loaded runtime module does not expose compatible host-context registration and native AudioWorklet startup
- **THEN** launch fails with a diagnostic before reporting audio online
- **AND** the browser does not start timer-driven, ring-buffered, or otherwise degraded audio

#### Scenario: ABI v1 package is rejected
- **WHEN** a catalog package reports browser ABI v1 or changes the v2 context-aware start signature
- **THEN** compatibility negotiation rejects it before runtime creation
- **AND** the publisher must rebuild the package with the v2 generic browser boundary

#### Scenario: Allocation completes before callback startup
- **WHEN** an application requires Wasm heap allocation or growth during construction and initialization
- **THEN** those operations complete before its native AudioWorklet is started
- **AND** the real-time callback does not allocate or trigger memory growth

#### Scenario: Browser callback verification
- **WHEN** Chromium can be launched with the required host permissions
- **THEN** Playwright verifies every real first-party browser app starts the runtime-owned AudioWorklet callback from one catalog selection and observes advancing processed-block and finite deadline diagnostics
- **AND** the verification cannot pass by merely resuming a silent host context

### Requirement: sprs-9 — JUCE layout: hierarchical generic portable backend
WHEN the generic JUCE backend renders a portable node tree, THE backend SHALL retain semantic `Row`, `Section`, and `ScrollArea` parentage; SHALL position every node at its parent-relative resolved bounds inside its resolved semantic host, accumulating ancestor origins only across ancestors not realized as components; SHALL treat every `Draw` node's complete command buffer as node-local geometry against that node's own origin, clipped to the node's bounds; SHALL contain no coordinate-space classification of any kind — neither of draw-command geometry nor of explicit node bounds — and SHALL NOT flow, size, or default the extent of any node; and SHALL render labels, controls, panels, and draw commands in the resolved location and clipping hierarchy without application-specific node IDs or page branches.

#### Scenario: Parent-local rows do not overlap
- **WHEN** two rows have different bounds in a scroll area's content coordinates and each row contains controls at local y zero
- **THEN** the controls render inside their owning rows at distinct surface y positions
- **AND** neither row obscures controls that precede the scroll area

#### Scenario: Nested root translates by its own bounds only
- **WHEN** a composite tree contains an application root at x zero and a sidebar root at x equal to the application width, whose descendants carry parent-relative coordinates
- **THEN** the generic JUCE backend places the sidebar root at its bounds and every descendant relative to its own parent
- **AND** it neither drops nor doubles the sidebar offset

#### Scenario: No node-bounds coordinate guess survives
- **WHEN** the JUCE and browser backend sources are inspected
- **THEN** neither contains a predicate that classifies a node's explicit bounds as parent-local or surface-absolute by testing whether they fall inside the parent's extent
- **AND** neither contains a predicate that classifies draw-command geometry as node-local or surface-absolute

#### Scenario: Nested drawing follows its container
- **WHEN** a draw node is inside a row, section, or scrolling content subtree
- **THEN** its painting and interactive pointer target use the same resolved bounds and clipping as sibling controls

#### Scenario: Application drawing is node-local by definition
- **WHEN** an application supplies draw-node bounds in its parent's space and draw commands against the node's own origin
- **THEN** the JUCE backend positions the hosted draw component at the resolved node bounds
- **AND** paints the commands in that component's own space with no translation
- **AND** the drawn pixels within the node are identical to those the application drew before this change

#### Scenario: Backend geometry is derivable from the tree alone
- **WHEN** either backend renders any node of a resolved tree
- **THEN** the node's rendered position equals its own resolved bounds offset by the accumulated origins of its ancestor chain, plus scroll offset and uniform surface scale where applicable
- **AND** no other input contributes to that position

#### Scenario: Refresh preserves live controls
- **WHEN** a stable node ID and kind survive a surface refresh while its text editor is focused or contains an uncommitted draft
- **THEN** the backend retains that JUCE component, focus, and draft while updating its current semantic properties and action

#### Scenario: Reparent preserves a live editor
- **WHEN** a stable text-editor node keeps its ID and kind while its semantic parent changes across refresh
- **THEN** the backend reparents the retained JUCE component without discarding its focus or uncommitted draft

### Requirement: sprs-10 — JUCE scrolling: real portable scroll areas
WHEN the generic JUCE backend renders a `ScrollArea`, THE backend SHALL present its declared bounds as a JUCE viewport, SHALL size one content component to at least `scrollContentWidth` by `scrollContentHeight`, SHALL parent the scroll area's descendants into that content component, and SHALL provide horizontal and vertical scrolling when content exceeds the visible bounds so clipped descendants remain reachable and interactive.

#### Scenario: Content extent is distinct from viewport
- **WHEN** a scroll area declares visible bounds smaller than its content extent
- **THEN** the viewport remains at the visible bounds
- **AND** its content component uses the declared content extent without enlarging the viewport

#### Scenario: Final mapping row is reachable
- **WHEN** the shared Controllers page expands a mapping section whose final row lies below the visible viewport
- **THEN** a user can scroll to the final row
- **AND** the final row remains visible, editable, and clipped to the viewport during scrolling

#### Scenario: Wide controller content scrolls horizontally
- **WHEN** a Controllers row or mapping editor requires more width than the page viewport
- **THEN** the generic JUCE backend exposes horizontal scrolling to the declared content width
- **AND** it does not compress or overlap the row's controls

### Requirement: sprs-11 — Verification: production generic JUCE page coverage
WHEN the generic JUCE backend change is verified, THE synth project SHALL exercise hierarchy, nested roots, stable refresh, nested drawing, and scroll extents in backend tests; SHALL open the real shared Controllers page through the desktop runtime shell and verify non-overlap plus final-row reachability; and SHALL keep the standalone Controllers harness on the same generic backend path used by the desktop application, without retaining a Controllers-specific production renderer as an alternate implementation.

#### Scenario: Desktop shell exercises the production renderer
- **WHEN** the JUCE runtime-shell integration test opens Controllers from the shared sidebar
- **THEN** the Back control, controller rows, Add row, status line, and scroll viewport have non-overlapping resolved bounds
- **AND** the test reaches content beyond the initial viewport through the generic backend

#### Scenario: Harness cannot hide generic regressions
- **WHEN** the standalone Controllers harness renders its fixtures
- **THEN** it uses `PortableComponent` over the production `ControllersPageSurface`
- **AND** it does not instantiate or test a separate Controllers tree renderer

### Requirement: sprs-12 — Deployment: Cloudflare Pages publish artifact
WHEN the browser runtime, catalog launcher, and configured first-party applications have been built, THE synth browser package SHALL provide a deterministic static publish step that assembles a Cloudflare Pages-compatible directory containing the launcher HTML/CSS, compiled generic browser runtime modules, production trusted-catalog source configuration, validated rollback catalog/package copies, and a `_headers` file that preserves cross-origin isolation, Web MIDI permissions, and WASM content typing.

#### Scenario: Publish directory contains deployable launcher assets
- **WHEN** a developer runs the browser publish step after building the TypeScript launcher/runtime and every configured first-party application
- **THEN** the generated publish directory contains the catalog launcher at its root
- **AND** it contains compiled generic browser runtime modules under `dist/src`
- **AND** it contains the complete validated first-party catalog/package set and one generic rollback page per app

#### Scenario: Production discovery uses the GitHub Pages publisher
- **WHEN** Cloudflare Pages serves the production launcher and it reads its first configured catalog source
- **THEN** that source is the stable GitHub Pages catalog URL published from this repository
- **AND** selecting Mini App or Braid 4 fetches that app's immutable package from GitHub Pages through the generic cross-origin package path
- **AND** the checked-in localhost source list remains relative to the local development server

#### Scenario: Cloudflare headers preserve runtime requirements
- **WHEN** Cloudflare Pages serves the generated publish directory
- **THEN** every route is covered by headers declaring `Cross-Origin-Opener-Policy: same-origin`
- **AND** every route is covered by headers declaring `Cross-Origin-Embedder-Policy: require-corp`
- **AND** every route is covered by headers declaring `Permissions-Policy: midi=(self)`
- **AND** WASM assets are covered by headers declaring `Content-Type: application/wasm`

#### Scenario: Missing catalog or package artifact fails early
- **WHEN** a developer runs the browser publish step before the trusted source list, first-party catalog, configured application entry, or declared package sidecar exists
- **THEN** the publish step fails before writing a complete publish directory
- **AND** the diagnostic names the missing artifact or invalid catalog reference

#### Scenario: Git-backed Cloudflare build bootstraps Emscripten
- **WHEN** Cloudflare Pages runs the synth browser Git-backed build command in an environment without `em++`
- **THEN** the build command installs and activates Emscripten before invoking the generic browser application targets
- **AND** the build command publishes the generated launcher only after every configured application, catalog, and package validates

### Requirement: sprs-6 — Browser layout: one parent-relative coordinate system
WHEN the browser backend renders a composite frame, THE backend SHALL treat every node's resolved bounds as coordinates in its parent node's space — the single parentless root's bounds being surface coordinates, and a `ScrollArea` child's bounds being coordinates in its parent's scroll-content space — SHALL position each node's DOM element directly from those bounds as an absolutely-positioned child of its parent's element with no origin subtraction and no coordinate-space classification, SHALL derive the host height from the resolved root extent rather than from flowed content, SHALL scale the single composite root uniformly when the viewport is narrower than the surface, and SHALL NOT flow, size, or expand any node — a node arriving without resolved bounds renders at its parent's origin with zero-based extent.

#### Scenario: Nested DOM consumes parent-relative bounds directly
- **WHEN** a sidebar root sits at x 900 relative to the composite root and its descendants carry coordinates relative to their own parents
- **THEN** each descendant's CSS offset equals its wire bounds with no parent-origin subtraction
- **AND** the descendants render in the same surface positions as the generic JUCE backend

#### Scenario: Host height follows the resolved root
- **WHEN** a page's content is resolved by the component library within the root extent
- **THEN** the browser host height is the resolved root extent
- **AND** no content is placed below it by backend flow

#### Scenario: Unbounded text is not expanded by the backend
- **WHEN** a label or status text carries more text than fits its resolved extent
- **THEN** the backend fits the text within that extent by shrinking, truncating, or clipping
- **AND** the node's resolved bounds are unchanged, so no adjacent control moves

#### Scenario: Narrow viewport preserves composition
- **WHEN** the browser viewport is narrower than the composite root
- **THEN** app content and sidebar scale together from one origin
- **AND** no independent-root gap or overlap appears

### Requirement: sprs-19 — Composition: a surface may declare its own root bounds and a sidebar slot
WHEN an application surface also implements `ui::SelfSizedSurface`, THE shared main component SHALL validate the application root against the bounds that surface's `RootBounds()` returns instead of the configured or offered size, SHALL, when its `SidebarSlot()` names a node of the application tree, place the runtime sidebar root at that node's position in composite coordinates and make the composite exactly the application root's size while the application page is shown, and SHALL fail composition with a diagnostic naming the slot when the sidebar root does not fit inside the slot node's bounds or the named node is absent. While a runtime page (Audio, Controllers, Sync, File or a registered app page) is shown, the sidebar SHALL sit at the page root's right edge, as sprs-2 states; the page SHALL take the application root's width less the sidebar width and the application root's height when the surface declares a slot, so the composite is exactly the application root, and SHALL keep the content bounds the component gives it at construction otherwise. The component SHALL expose the bounds of the last composite it built. A surface that does not implement the interface SHALL compose as sprs-2 states.

#### Scenario: The declared root is validated
- **WHEN** a self-sized surface's root differs from the bounds it declares
- **THEN** composition fails naming the application-root contract
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestSelfSizedRootIsValidatedAgainstItsDeclaredBounds.

#### Scenario: The sidebar lands in the slot
- **WHEN** a self-sized surface declares a slot that holds the sidebar
- **THEN** the sidebar root sits at the slot's composite position and the composite equals the app root
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestSidebarIsPlacedAtTheDeclaredSlot.

#### Scenario: Every runtime page opens beside a slotted app
- **WHEN** a self-sized surface narrower than its configured width declares a slot, and Audio, Controllers, Sync and File are opened in turn
- **THEN** each composes without error, its root at 0,0 with the app root's width less the sidebar and the app root's height, the sidebar at the page root's right edge, and the composite equal to the app root
- **AND** returning to the application composes the slotted tree again
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestEveryRuntimePageOpensBesideASlottedApp.

#### Scenario: A runtime page beside an unslotted app keeps the configured size
- **WHEN** a self-sized surface declares no slot and Audio is opened
- **THEN** the page root is at 0,0 with the configured width and height and the sidebar at its right edge
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestRuntimePageKeepsConfiguredBoundsBesideAnUnslottedApp.

#### Scenario: A slot too small fails loudly
- **WHEN** the declared slot is smaller than the sidebar root
- **THEN** composition fails with a diagnostic naming the slot node
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestSidebarLargerThanItsSlotFailsComposition.

### Requirement: sprs-20 — JUCE shell: the composite fits the window width and scrolls vertically
WHEN the JUCE runtime shell lays out its main pane, THE shell SHALL size the pane to the larger of the shell and the last composite in each dimension, SHALL scale the pane uniformly by the shell width divided by the pane width, never above 1, SHALL scroll vertically when the scaled height exceeds the shell height, SHALL follow a change in the composite's bounds on the next refresh, and SHALL NOT scroll for a drag that starts on a node that takes drags; a drag that starts anywhere else SHALL scroll. After every layout of the pane, the shell SHALL offer its own bounds, less the sidebar, as the content extent, so that an extent-aware application surface (sprs-13) tracks the window as it grows and as it shrinks; a host with no shell SHALL keep offering the pane's own bounds. A resizable desktop window shrunk narrower than its composite SHALL scale its content to the window width.

#### Scenario: A phone-width shell fits and scrolls
- **WHEN** the shell is 412 by 150 pixels and the miniapp composite (996 by 560) is scaled to 412 by about 232
- **THEN** the pane is scaled to the shell width and the shell scrolls by the scaled height less 150, to the composite's scaled bottom
- Check: `projects/synth/juce/RuntimeShellSessionTests.cpp`, CheckNarrowShellFitsWidthAndScrolls, run through `apps/miniapp`'s `test` target.

#### Scenario: A drag on a control does not scroll
- **WHEN** a drag starts on a node that takes drags inside the scrolled shell
- **THEN** the node receives the drag and the scroll position is unchanged
- **AND** a drag starting outside any such node moves the scroll position
- Check: `projects/synth/juce/RuntimeShellSessionTests.cpp`, CheckDragOnAControlDoesNotScroll.

#### Scenario: An extent-aware app still grows with its window
- **WHEN** the extent-aware wiring app's shell is resized to 800 by 600
- **THEN** the offered content extent becomes 704 by 600, the scale stays 1 and nothing scrolls
- Check: `projects/synth/juce/RuntimeShellSessionTests.cpp`, the existing WiringExtentAwareApp resize check in `main`.

#### Scenario: An extent-aware app shrinks with its window
- **WHEN** the extent-aware wiring app's shell, grown to 800 by 600, is set to 600 by 400
- **THEN** the offered content extent becomes 504 by 400 at once
- **AND** after the next refresh the pane is 600 by 400 at scale 1
- Check: `projects/synth/juce/RuntimeShellSessionTests.cpp`, CheckExtentAwareAppShrinksWithItsShell.

#### Scenario: A desktop window is unchanged at its composite size
- **WHEN** the shell is at least as large as the composite
- **THEN** the scale is 1 and no scrollbar is shown
- Check: `projects/synth/juce/RuntimeShellSessionTests.cpp`, CheckNarrowShellFitsWidthAndScrolls.

