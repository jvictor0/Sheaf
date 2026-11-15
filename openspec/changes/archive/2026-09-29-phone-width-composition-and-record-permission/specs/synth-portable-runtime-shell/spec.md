# Delta — `synth-portable-runtime-shell`

## MODIFIED Requirements

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

## ADDED Requirements

### Requirement: sprs-19 — Composition: a surface may declare its own root bounds and a sidebar slot
WHEN an application surface also implements `ui::SelfSizedSurface`, THE shared main component SHALL validate the application root against the bounds that surface's `RootBounds()` returns instead of the configured or offered size, SHALL, when its `SidebarSlot()` names a node of the application tree, place the runtime sidebar root at that node's position in composite coordinates and make the composite exactly the application root's size while the application page is shown, and SHALL fail composition with a diagnostic naming the slot when the sidebar root does not fit inside the slot node's bounds or the named node is absent. While a runtime page (Audio, Controllers, Sync, File or a registered app page) is shown, the page SHALL keep the content bounds the component gives it at construction and the sidebar SHALL sit at the page root's right edge, as sprs-2 states, whatever the application surface declares. The component SHALL expose the bounds of the last composite it built. A surface that does not implement the interface SHALL compose as sprs-2 states.

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
- **THEN** each composes without error, its root at 0,0 with the configured width and height, the sidebar at the page root's right edge
- **AND** returning to the application composes the slotted tree again
- Check: `projects/synth/tests/runtime_main_component_tests.cpp`, TestEveryRuntimePageOpensBesideASlottedApp.

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
