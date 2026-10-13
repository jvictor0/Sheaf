# Delta — synth-portable-runtime-shell

## ADDED Requirements

### Requirement: sprs-13 — Composition: app surfaces resolve against a live extent

WHEN the shell composes the app surface with the sidebar, THE shell SHALL
offer the app surface the live content extent before invoking its
`BuildTree()` and SHALL place the sidebar at the resolved app tree's root
width rather than a compiled-in width.

The extent hook is optional: a surface that does not accept it resolves at
its compiled-in size, and because its resolved root width then equals
`App::Config().uiWidth`, composition is unchanged for such apps.

#### Scenario: legacy app composes identically

- **WHEN** an app surface that ignores the extent hook builds at its
  compiled-in size
- **THEN** the composed tree, including the sidebar x position, is identical
  to composition at the compiled-in width

#### Scenario: extent-aware app tracks the window

- **WHEN** a surface accepts the offered extent and the window is resized
- **THEN** the surface resolves against the new extent and the sidebar sits
  at the resolved root width, with no dead space between content and sidebar

#### Scenario: backend parity

- **WHEN** the browser shell composes the same surface and sidebar
- **THEN** the sidebar placement follows the same resolved-width rule

### Requirement: sprs-14 — Launch: reusable single-app entry point

THE runtime SHALL expose the window construction and registered-app launch
plumbing as a reusable header so an out-of-tree `main` reuses the same code
path the bundled launcher uses, and WHEN the bundled launcher's `initialise`
receives a command-line argument naming a registered appId, THE launcher
SHALL launch that app directly without showing the picker.

#### Scenario: out-of-tree main without a copy

- **WHEN** an out-of-tree app implements its `main` on the reusable header
- **THEN** it launches its registered app with no picker and no copied
  window/launch code

#### Scenario: direct launch by appId

- **WHEN** the bundled launcher starts with an argument naming a registered
  appId
- **THEN** that app launches directly; an unrecognized or absent argument
  shows the picker as today

### Requirement: sprs-15 — Launch: windows sized from intrinsic shell bounds

WHEN a launcher creates the app window, THE launcher SHALL derive the window
size from the shell component's intrinsic bounds and SHALL NOT size it from
raw `config.uiWidth`/`config.uiHeight`.

#### Scenario: picker-launched window shows the full sidebar

- **WHEN** any registered app is launched from the picker
- **THEN** the window opens wide enough to show the sidebar column without
  resizing (config width plus the sidebar width)

### Requirement: sprs-16 — Pages: app-supplied audio-page section

THE audio page SHALL append an app-supplied section beneath the device rows
WHERE an app supplies the optional section builder, confined to the page's
remaining area; WHERE no builder is supplied, THE audio page SHALL render
exactly as before.

#### Scenario: default page unchanged

- **WHEN** an app supplies no section builder
- **THEN** the audio page tree is identical to the pre-change tree

#### Scenario: app section appended

- **WHEN** an app supplies a section builder
- **THEN** the audio page contains the app-built nodes below the device rows,
  confined to the area handed to the builder

### Requirement: sprs-17 — Pages: app-registered sidebar page

THE shell SHALL render an app-registered sidebar page's navigation button
after the built-in pages and route selection to the app-built tree WHERE an
app registers one (id, title, tree builder); WHERE no page is registered,
THE sidebar SHALL contain exactly the built-in pages.

#### Scenario: no registration, no change

- **WHEN** an app registers no extra page
- **THEN** the sidebar shows exactly the built-in pages and navigation
  behaves as before

#### Scenario: registered page reachable

- **WHEN** an app registers one extra page and the user selects its button
- **THEN** the app-built tree renders in the page area and the built-in pages
  remain reachable and unchanged
