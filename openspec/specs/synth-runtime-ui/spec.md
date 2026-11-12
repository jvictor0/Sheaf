# synth-runtime-ui Specification

Project: `projects/synth`. ID prefix: `sru`.

## Purpose

Define the runtime library UI framework: the main pane with right-hand
sidebar (Audio/Controllers/File entries and max-recent-deadline readout),
the audio, controllers, and file pages, kind-driven expandable controller
configuration with scrollable mapping editors, and the JUCE-free view
model that page logic lives in.
## Requirements
### Requirement: sru-1 — Layout: main pane with sidebar and content host
WHEN a runtime-hosted application presents UI through any active backend, THE runtime library SHALL provide the same portable main pane composed of a right-hand sidebar menu subcomponent and a content host; the content host SHALL preserve the application's configured portable content bounds without clipping or rewriting app coordinates, SHALL display the application's portable UI surface through the active backend by default, SHALL display exactly one library page at a time when one is opened from the sidebar, SHALL return to the application's UI surface when the page is dismissed, and SHALL keep the sidebar adjacent to the content when the host surface is resized or uniformly scaled.

#### Scenario: Default view is the application
- **WHEN** the main pane opens with no page selected in JUCE or Chrome
- **THEN** the content host shows the application's portable UI surface, adapted by the active backend, beside the sidebar

#### Scenario: Page opens and returns
- **WHEN** the user opens a sidebar page and then dismisses it
- **THEN** the page replaces the application UI while open
- **AND** the application UI surface is restored, retaining its state, on dismissal

#### Scenario: Resize keeps one composition
- **WHEN** the host surface is resized or the browser uniformly scales it to a narrower viewport
- **THEN** the sidebar keeps its logical width and position beside the content host
- **AND** the application or active page remains complete and non-overlapping

### Requirement: sru-2 — Sidebar: tabs and deadline readout
WHEN the sidebar renders, THE runtime library SHALL show Audio, Controllers, Sync, and File entries that open their pages in the content host, SHALL show a warning marker on the Controllers entry exactly while the cached scw-2 classification contains at least one available wizard candidate, and SHALL show a max-recent-deadline readout displaying the maximum audio callback load percentage over a rolling window of recent UI frames, updated on the UI timer. Candidate classification SHALL be recomputed from the cached device snapshot after a device-list change and after every successful instrument commit, including while the Controllers page is closed, but SHALL NOT force device enumeration or reconciliation when the device list is unchanged.

#### Scenario: Tabs open their pages
- **WHEN** the user activates the Controllers entry
- **THEN** the Controllers page opens in the content host

#### Scenario: Sync tab opens its page
- **WHEN** the user activates the Sync entry in JUCE or Chrome
- **THEN** the portable Sync page opens in the content host

#### Scenario: Deadline readout holds recent peaks
- **WHEN** a single audio callback spikes the load percentage and later callbacks are cheap
- **THEN** the readout continues to display the spike value until it leaves the rolling window

#### Scenario: Available controller shows warning
- **WHEN** the background MIDI refresh observes a present wizard-recognized pair whose endpoints are unclaimed
- **THEN** the Controllers sidebar entry shows a warning marker without requiring the Controllers page to be open

#### Scenario: Configured or ignored controller clears warning
- **WHEN** the last available candidate is configured or ignored
- **THEN** the warning marker clears on the next portable UI refresh

#### Scenario: A claimed recognized pair never warns
- **WHEN** a recognized present pair has either endpoint claimed by an active or blacklisted record
- **THEN** it is not an available candidate
- **AND** it does not contribute to the Controllers warning marker

#### Scenario: Successful lifecycle commits refresh cached classification
- **WHEN** configuring, ignoring, deleting, blacklisting, or removing from blacklist successfully commits an instrument change
- **THEN** candidate availability and the warning marker are recomputed against the cached device snapshot without waiting for another device-list change

### Requirement: sru-3 — Audio page: interface selection
WHEN the Audio page is open, THE runtime library SHALL let the user choose the audio output device and, when the application's config requests inputs, the input device from the host-provided choices with a system-default entry; SHALL apply selection through the runtime's audio device switching (sar-15); SHALL display the current output and negotiated status; and for input-capable applications SHALL display input permission/availability plus requested and active channel counts in the existing status line and expose `Retry Input` while browser capture is offline.

#### Scenario: Output selection applies
- **WHEN** the user selects an output device on the Audio page
- **THEN** the runtime switches to that device and the page reflects it as current

#### Scenario: Input row only when requested
- **WHEN** the application's config declares zero audio inputs
- **THEN** the Audio page shows no input device selector
- **AND** the page shows no input status or retry action

#### Scenario: Browser input status is explicit
- **WHEN** a browser-hosted application requests `N > 0` inputs
- **THEN** the page shows the System Default input selector
- **AND** its status line names the permission/availability state and reports `requested N / active M`
- **AND** it does not imply that input is monitored to output

#### Scenario: Offline browser input can be retried
- **WHEN** browser permission is denied, capture is unavailable, or an established stream ends
- **THEN** the page exposes a `Retry Input` action outside the realtime callback
- **AND** activating it dispatches the host-neutral retry action while the output page and application remain live

#### Scenario: JUCE input row keeps native choices
- **WHEN** a JUCE-hosted application requests one or more inputs
- **THEN** the page continues to show host-enumerated native input choices
- **AND** the current requested/active diagnostic remains visible through the portable status line

### Requirement: sru-4 — Controllers page: list, state, and adding
WHEN the Controllers page is open, THE runtime library SHALL list every active and blacklisted controller record in instrument order, showing its name, hardware kind, and disposition. Active records SHALL also show each endpoint as online, offline, or unconfigured when its stored reference is empty; show the actual input and output choices as the present devices plus the stored reference when absent; preserve the existing low-level mapping editor; and offer Rename and Delete. Active records whose persisted wizard id resolves in the current registry SHALL additionally offer Reconfigure and Blacklist. Blacklisted records SHALL show their stored endpoint labels, expose Rename and Remove from blacklist, expose Configure only when their wizard id resolves in the current registry, and expose no live endpoint selectors or mapping editor. The page SHALL list currently available wizard candidates separately with Configure and Ignore actions, SHALL preserve the add ("+") action that creates a named active controller of a chosen kind seeded from that kind's default profile, and SHALL commit device selections and lifecycle actions through instrument editing and reconciliation rather than opening or closing handlers directly.

#### Scenario: Connection state is visible
- **WHEN** one active mapped controller is connected and another active controller's device is unplugged
- **THEN** the page shows the first online with its device names and the second offline

#### Scenario: Blacklisted record is visibly inert
- **WHEN** a blacklisted controller record is listed
- **THEN** its row shows a Blacklisted badge and its stored endpoint labels
- **AND** it exposes no mapping disclosure or live endpoint selectors

#### Scenario: Manual and legacy profiles retain generic editing
- **WHEN** an active record has no persisted wizard id
- **THEN** its Rename, Delete, endpoint-selection, and low-level mapping controls remain available
- **AND** wizard Reconfigure and Blacklist actions are not offered

#### Scenario: Unknown opaque wizard id remains recoverable
- **WHEN** a stored Active or Blacklisted record carries a well-formed wizard id that does not resolve in the current registry
- **THEN** the record remains visible and offers its non-wizard Rename and Delete or Remove-from-blacklist actions
- **AND** Reconfigure, Blacklist, and Configure are not offered

#### Scenario: Add controller
- **WHEN** the user presses "+", names the controller, and picks the twister kind
- **THEN** an active controller seeded with the default twister profile appears in the list and in the instrument configuration

#### Scenario: Device choice triggers reconnect
- **WHEN** the user assigns a present input device to an active controller
- **THEN** the preference is stored and reconciliation opens that device for the controller

#### Scenario: Ignore creates a visible blacklist row
- **WHEN** the user activates Ignore for an available controller
- **THEN** its available row is replaced by a persisted Blacklisted row
- **AND** neither endpoint is opened

#### Scenario: Remove from blacklist restores availability
- **WHEN** the user removes a blacklisted record while its recognized pair remains present and unclaimed
- **THEN** the inert record is removed
- **AND** the pair returns to Available controllers and restores the sidebar warning

#### Scenario: Rename rejects duplicates
- **WHEN** the user renames any active or blacklisted record to a name already used by another record
- **THEN** the rename is refused and the prior name remains

#### Scenario: Delete active profile closes hardware
- **WHEN** the user deletes an active connected controller
- **THEN** the record is removed through the instrument commit path
- **AND** reconciliation closes its endpoints and removes its processor/sender routing

#### Scenario: Blacklist retains a dormant wizard profile
- **WHEN** the user blacklists a wizard-associated active controller
- **THEN** its record changes to Blacklisted while retaining its prior profile as dormant reconfiguration seed data
- **AND** reconciliation closes both endpoints and the row exposes no active mapping or endpoint controls

### Requirement: sru-5 — Controllers page: expandable config sections
WHEN a controller row's config section is used, THE runtime library SHALL provide an expandable config area that starts collapsed, containing collapsible submenus — encoders, system messages, and analogs/gestures — that each start collapsed and are omitted entirely when the controller's kind does not support them; the submenus SHALL present the controller's mappings as the block presentation of sru-10/sru-11 (block rows for uniform runs, individual rows otherwise, config-level rows for encoder mode, turn step, and scene blend), SHALL edit them through the view model (encoder channel/CC to parameter slot and position; kind-schema system-message addresses to press/release message-ins per sru-8; analog channel/CC to gesture index and scene blend), SHALL expose scrollable content with distinct visible viewport bounds and content extent so every row remains reachable in JUCE and portable backends, and SHALL remain usable with multiple controllers each carrying dozens of mappings; committed edits apply through the live-edit rebuild path (smi-8).

#### Scenario: Config starts collapsed
- **WHEN** the Controllers page opens
- **THEN** every controller's config section and submenus are collapsed

#### Scenario: Unsupported submenus are skipped
- **WHEN** a launchpad controller's config is expanded
- **THEN** no encoders or analogs submenu is shown
- **AND** the system messages submenu is shown

#### Scenario: Mapping lists scroll
- **WHEN** a controller has more mappings than fit in the visible page
- **THEN** the mapping lists expose a content extent larger than the visible viewport
- **AND** the page can scroll to every mapping row and keep it reachable and editable

#### Scenario: Committed edit reaches the hardware path
- **WHEN** the user edits a system-message association or a block row and commits it
- **THEN** the live instrument configuration is updated and the controller's processors are rebuilt

#### Scenario: Uniform runs present as blocks
- **WHEN** the default WRLD.Bldr controller's encoders submenu is expanded
- **THEN** the 16 turn mappings present as one turn block row and the 16 push mappings as one push block row rather than 32 individual rows

### Requirement: sru-6 — File page: patch commands
WHEN the File page is open, THE runtime library SHALL present the patch commands (new, save, save-as, load, revert) in a polished page layout with a current-patch identity header, runtime patch-root/path context, and last command/browser status, replacing the former shell chrome row; Save with no current patch directory SHALL fall through to the in-app Save As flow; Save As and Load SHALL open the dedicated in-app patch-browser viewer rooted at the runtime-owned `patches/` directory rather than an operating-system file explorer.

#### Scenario: File page carries patch identity
- **WHEN** a patch is saved-as or loaded
- **THEN** the File page shows that patch's name and the command status in the page header/status area

#### Scenario: First save falls through
- **WHEN** the user presses Save before any patch directory exists
- **THEN** the in-app Save As flow opens instead of an error

#### Scenario: Patch browser stays under patches root
- **WHEN** the user browses, saves-as, or loads from the File page
- **THEN** every selectable or creatable patch directory is resolved under the runtime-owned `patches/` directory
- **AND** the UI exposes no arbitrary absolute filesystem picker

#### Scenario: File page layout remains intentional while idle
- **WHEN** no Save As or Load browser viewer is open
- **THEN** the File page still shows the patch identity header, command strip, and status/empty-state region without leaving a rough blank or cramped chooser area

### Requirement: sru-7 — View model: JUCE-free page logic
WHEN the Controllers page builds or edits its content, THE row-tree construction (sections present per kind, labels, mapping row values, expand/collapse state) and edit application onto the instrument configuration SHALL live in JUCE-free library code, so page logic is unit-testable headlessly with multiple controllers and large mapping sets; JUCE components SHALL be thin renderers over this view model.

#### Scenario: View model builds without JUCE
- **WHEN** a JUCE-free test builds the view model from an instrument with four controllers of all four kinds
- **THEN** it compiles without JUCE and yields rows whose sections match each kind's support matrix

#### Scenario: Edits round-trip through the view model
- **WHEN** a JUCE-free test applies an encoder-mapping edit through the view model
- **THEN** the underlying instrument configuration reflects the edit and rebuilding the view model shows the new value

### Requirement: sru-8 — Controllers page: per-kind address schema
WHEN system-message mappings are presented or edited, THE runtime library SHALL derive each kind's address fields from a single shared schema — wrldbldr: channel, x, y; launchpad: x, y; twister: logical side-button number 0..5 only (stored as its fixed channel-3 CC `8 + button`; the channel is display-only, not an editable column); generic: channel, cc — and SHALL use that schema for row fields, column headers, and block address forms, so no kind shows dead or inapplicable address columns.

#### Scenario: Twister shows a single address column
- **WHEN** a twister controller's system messages submenu is expanded
- **THEN** each row exposes exactly one editable address field (the logical button number 0..5, persisted as CC `8 + button` on the fixed channel) plus press/release
- **AND** no editable channel field is shown
- **AND** a button value outside 0..5 is refused with a reason

#### Scenario: Schema drives every surface
- **WHEN** a kind's system-message rows, their column headers, and its system block form are compared
- **THEN** all three expose the same address fields in the same order as the shared schema

### Requirement: sru-9 — Controllers page: canonical config ordering
WHEN the view model commits any mapping change (edit, add, delete, or block operation), THE runtime library SHALL normalize the committed profile config's element order — encoder turns and pushes by (slot, position); system-message associations by a total press-message sort key defined for every `MessageIn` type (type order, then the type's semantic arguments — scene index, bank slot+index, gesture index, param slot+position, held-flag tuple for modifier set/release variants — then the kind's address tuple, then stable original order); analog gesture mappings by gesture index — through a JUCE-free library helper, without changing the persistence format; block reconstruction SHALL sort its input view the same way, so configs authored in any order reconstruct identically and become canonical on their first commit.

#### Scenario: Commit normalizes order
- **WHEN** a mapping edit is committed on a config whose system messages are stored out of order
- **THEN** the committed config's system messages are sorted by (message type, argument)
- **AND** the persisted JSON structure is otherwise unchanged

#### Scenario: Unsorted input still reconstructs
- **WHEN** an externally-authored config stores a bank-selector run out of order
- **THEN** expanding the section reconstructs the same block as for the sorted equivalent

### Requirement: sru-10 — Controllers page: block model
WHEN a section is expanded, THE runtime library SHALL reconstruct, via pure JUCE-free functions, a block presentation of the sorted config that is minimal under the reconstruction's canonical traversal (x ascending within a row, row direction ±1 in y, row-major): encoder blocks (channel, start cc, end cc exclusive, slot, start position) for maximal runs (length ≥ 2) of constant slot/channel with consecutive positions and ccs at constant offset, separately for turns and pushes; analog gesture blocks (channel, start cc, end cc exclusive, start gesture) analogously; system blocks only for scene-select, bank-select, and gesture-select messages — presented per the kind's address form (generic: channel + cc run; wrldbldr: channel + exclusive-end x/y rectangle whose end row may be above or below its start row; launchpad: the same rectangle form without channel; twister: never blocked) — for maximal runs of the same type (and same bank slot for bank-select) with consecutive arguments, consistent release pattern (paired set-false for gesture-select, absent otherwise), feedback equal to press on every cell, and a constant output-feedback flag, fitting greedy rectangles of ≥ 2 cells with the y direction fixed by the run's second row; a block SHALL expand to exactly its equivalent individual configs (argument = start + cell index in traversal order; wrldbldr cells derive control cc from position with the block channel authoritative; feedback = press; output-feedback = the block's flag), and reconstruction composed with expansion SHALL round-trip: expanding a reconstructed presentation reproduces the sorted config exactly for every config (duplicate addresses or duplicate messages simply fail the run checks and pass through as individual rows), and reconstructing the expansion of any block reconstruction can itself produce yields that block back; mappings fitting no block — including all non-blockable message types and scene blend — SHALL present as individual rows.

#### Scenario: Encoder run reconstructs to one block
- **WHEN** a config maps ccs 0..15 on one channel to slot 0 positions 0..15 as turns
- **THEN** the encoders presentation contains one turn block (channel, cc 0..16 exclusive, slot 0, start position 0)

#### Scenario: Bank rectangle reconstructs on WRLD.Bldr
- **WHEN** the default WRLD.Bldr profile's bank selectors (banks 0..7 on row y=3, banks 8..15 on row y=2, feedback = press throughout) are reconstructed
- **THEN** the system presentation contains one bank-select block spanning x 0..7 from start row 3 to end row 2 with start argument 0

#### Scenario: Broken run splits minimally
- **WHEN** one cell in the middle of an otherwise uniform scene-select run has a non-consecutive argument
- **THEN** reconstruction emits blocks/rows covering each maximal uniform piece and an individual row for the outlier

#### Scenario: Non-blockable messages stay individual
- **WHEN** a config contains reset/random modifier associations and a scene blend assignment
- **THEN** they present as individual rows regardless of adjacency

#### Scenario: Expansion round-trips
- **WHEN** any reconstructed presentation's blocks are expanded and merged with its individual rows
- **THEN** the result equals the sorted input config exactly

#### Scenario: Block commit is all-or-nothing
- **WHEN** a block edit produces an expansion where any cell fails validation (address out of shape, argument out of domain, duplicate address)
- **THEN** the commit is refused with a reason and the config is unchanged

### Requirement: sru-11 — Controllers page: edit-session stability, add, and delete
WHILE a section is expanded, THE runtime library SHALL render an explicit in-memory edit-session row list for that controller section; the edit session SHALL be created by coalescing the current persisted profile elements only on the collapsed-to-expanded transition, SHALL be discarded on expanded-to-collapsed, SHALL NOT be replaced or re-coalesced by accepted edits, adds, deletes, view-model rebuilds, or canonical persisted-order sorting while the section remains expanded, and SHALL flush accepted changes by expanding the current session rows back into the existing persisted profile arrays, validating the full candidate section, and normalizing the persisted element order; each addable mapping group SHALL offer "+" (append one config with next-free defaults) and, where blocks apply, "+B" (append a block, committed as its expansion) which append session rows at the end of the requested group — EVEN WHEN the group is currently empty, so an empty section is never a dead end, and adding the first mapping into a section whose profile-config container is absent (no encoder-input or analog-input) SHALL create that container as part of the commit; individual mapping rows and block rows SHALL be deletable (a block delete removes all its cells in one commit); config-level rows (encoder mode, turn step, scene blend) SHALL NOT be deletable.

#### Scenario: Empty group still offers add
- **WHEN** a controller's section (e.g. system messages, or analog gestures) currently has zero mappings
- **THEN** the section still shows the group's "+" (and "+B" where blocks apply) so the first mapping can be added

#### Scenario: First add creates an absent container
- **WHEN** the user adds the first encoder (or analog) mapping to a controller whose config has no encoder-input (or analog-input) container
- **THEN** the commit creates the container and adds the mapping rather than refusing

#### Scenario: Session rows survive canonical persisted sorting
- **WHEN** the user edits a row while a section stays expanded
- **THEN** the edit session keeps rendering the same row list with the edited row updated in place
- **AND** the persisted profile arrays are expanded from the session and normalized without reordering the visible session rows

#### Scenario: Duplicate messages resolve distinctly
- **WHEN** two associations at different addresses both send scene-select 0 and one is deleted
- **THEN** exactly the deleted association's row disappears and the other is unaffected

#### Scenario: Edits do not re-group while expanded
- **WHEN** the user adds two individual scene-select rows that happen to form a contiguous run while the section stays expanded
- **THEN** they remain two individual rows until the section is collapsed and re-expanded
- **AND** re-expanding presents them as one block

#### Scenario: Block edit keeps its row
- **WHEN** the user edits a block's start argument and commits
- **THEN** the block row stays in place with updated values
- **AND** the config holds the new expansion normalized into persisted order

#### Scenario: Add block appends
- **WHEN** the user presses "+B" on the system group of a launchpad controller and commits the block form
- **THEN** the block's cells are added to the config in one commit
- **AND** the block row appears at the end of the session group without re-coalescing neighboring rows

#### Scenario: Delete removes exactly its rows
- **WHEN** the user deletes a 16-cell bank block
- **THEN** all 16 associations represented by that session block are removed in one commit and no other mapping changes

#### Scenario: Config-level rows are not deletable
- **WHEN** the encoders section presents encoder mode and turn step
- **THEN** neither exposes a delete affordance

#### Scenario: Close and reopen re-coalesces
- **WHEN** the user closes a section after edits that left adjacent compatible singleton rows visible
- **AND** the user opens that section again
- **THEN** the section coalesces from the current persisted profile arrays and may present those rows as a block

### Requirement: sru-12 — Configuration pages: Back saves runtime configuration
WHEN the user dismisses a runtime configuration page with Back, THE runtime library SHALL save the current runtime configuration before returning to the application view for pages that edit persistent configuration, including the Audio, Controllers, and Sync pages.

#### Scenario: Audio Back saves configuration
- **WHEN** the user changes the audio device selection and presses Back on the Audio page
- **THEN** the runtime saves a configuration document containing the current audio device state
- **AND** returns to the application view

#### Scenario: Controllers Back saves configuration
- **WHEN** the user changes controller setup or mappings and presses Back on the Controllers page
- **THEN** the runtime saves a configuration document containing the current MIDI instrument/controller configuration
- **AND** returns to the application view

#### Scenario: Sync Back saves configuration
- **WHEN** the user changes a clock/transport send or receive toggle or PPQN and presses Back on the Sync page
- **THEN** the runtime applies the sync configuration through its audio-safe handoff, saves it in runtime configuration, and returns to the application view

#### Scenario: File Back does not save runtime configuration
- **WHEN** the user presses Back on the File page
- **THEN** the runtime returns to the application view without writing runtime configuration solely because the File page was dismissed

### Requirement: sru-13 — File page: in-app patch browser
WHEN the File page enters Save As or Load flow, THE runtime library SHALL render a dedicated root-scoped in-app patch-browser viewer over the runtime-owned `patches/` directory, listing patch directories in deterministic flat order, allowing creation of a named new patch directory for Save As, selecting an existing patch directory for Load, supporting row double-click to load or overwrite-save-as existing patch directories, and presenting clear selected-row, empty, error, invalid-input, confirm, and cancel states in the viewer.

#### Scenario: Browser lists patch directories deterministically
- **WHEN** the patch root contains multiple patch directories
- **THEN** the in-app browser lists them in deterministic filename order
- **AND** non-patch version files at the root are not presented as loadable patches

#### Scenario: Save As creates patch directory under root
- **WHEN** the user enters a new patch name in the in-app Save As flow
- **THEN** the runtime creates the corresponding patch directory under the runtime-owned `patches/` directory
- **AND** writes the first patch version file there through the patch manager

#### Scenario: Save As refuses existing patch name in viewer
- **WHEN** the user enters or selects a Save As patch name whose target directory or file already exists under the runtime-owned `patches/` directory
- **THEN** the browser viewer keeps the Save As flow open and shows an inline already-exists status
- **AND** it does not dispatch a save-as callback for that target

#### Scenario: Save As double-click appends to existing patch
- **WHEN** the user double-clicks an existing patch directory row in the Save As browser
- **THEN** the runtime appends a new version file into that patch directory through an explicit overwrite-save-as action
- **AND** ordinary typed Save As confirmation still refuses existing patch directories

#### Scenario: Load selects patch directory under root
- **WHEN** the user confirms an existing patch directory in the in-app Load flow
- **THEN** the runtime asks the patch manager to load that directory
- **AND** the patch manager selects the latest sortable version file in that directory

#### Scenario: Load double-click accepts a flat row
- **WHEN** the user double-clicks a patch directory row in the Load browser
- **THEN** the runtime asks the patch manager to load that directory without requiring a separate Open control

#### Scenario: Browser cannot escape root
- **WHEN** a browser navigation or typed patch name contains an absolute path or `..`
- **THEN** the runtime rejects that target
- **AND** no file outside the runtime-owned `patches/` directory is read or written

#### Scenario: Browser exposes polished chooser states
- **WHEN** the Save As or Load browser viewer is open
- **THEN** the viewer shows its mode title, selected directory row state, primary confirm action, cancel action, and any invalid-input/status message within the viewer
- **AND** an empty patch root is presented as an empty state rather than a missing or broken list
- **AND** it does not expose separate Open or Parent controls

#### Scenario: Browser exposes unreadable-root error state
- **WHEN** the runtime-owned `patches/` directory cannot be read or refreshed
- **THEN** the browser viewer shows an error state and status inside the viewer
- **AND** it does not dispatch save-as or load callbacks until a valid target is available

### Requirement: sru-14 — Portable UI: semantic controls and drawing commands
WHEN runtime UI pages or synth widgets render, THE runtime UI layer SHALL express their host-independent structure through a JUCE-free portable UI model that supports semantic controls (buttons, toggles, sliders, combo boxes, text fields, labels, rows, sections, scroll areas, and status text), stable node identities for stateful controls, input/action callbacks, and bounded drawing commands for bespoke visual widgets using the canonical `synth::Color` RGBA type; JUCE renderers SHALL consume that model through backend adapters under `projects/synth/juce` rather than owning page behavior directly, and the model SHALL be shaped so neither the JUCE backend nor a future browser/DOM backend is forced through toolkit-awkward abstractions.

#### Scenario: Portable UI model compiles without JUCE
- **WHEN** a JUCE-free synth test includes the portable UI model and builds a representative miniapp/runtime page tree
- **THEN** it compiles without JUCE headers
- **AND** the tree can represent buttons, toggles, sliders, combo boxes, text fields, labels, scroll areas, and custom drawing nodes

#### Scenario: Bespoke widgets emit drawing commands
- **WHEN** the miniapp encoder or waveform widget renders through the portable UI layer
- **THEN** it emits host-neutral geometry, canonical `synth::Color` values, text, path/arc/line/fill, and interaction descriptions sufficient for the JUCE backend to reproduce the existing visual widget
- **AND** no second portable RGBA type is required

#### Scenario: Semantic controls remain backend-native
- **WHEN** the Audio, File, or Controllers page renders a form-like control through the portable UI layer
- **THEN** the page describes the control semantically rather than as raw drawing only
- **AND** the JUCE backend may realize it as native JUCE controls while a browser backend could map it to DOM controls

#### Scenario: Backend owns toolkit details
- **WHEN** a JUCE backend renders a portable UI tree
- **THEN** all JUCE component construction, graphics calls, focus handling, and toolkit-specific event translation happen inside JUCE-owned backend code under `projects/synth/juce`
- **AND** the portable UI tree producer remains free of JUCE references

### Requirement: sru-15 — Controllers page: DOM-friendly semantic presentation
WHEN the Controllers page is rendered, THE runtime UI layer SHALL derive a DOM-friendly semantic tree from the JUCE-free `MidiConfigViewModel`, preserving the page's existing controller list, endpoint selection, connection state display, add-controller flow, expandable config sections, mapping rows, block rows, add/delete affordances, validation/refusal status, focus-safe refresh, and commit-through-`EditInstrument` behavior; the JUCE backend SHALL render that semantic tree without reducing current functionality, SHALL NOT treat the current dense Controllers page look and feel as a visual-parity target, and MAY improve spacing, visual layout grouping, labels, and control presentation without relaxing sru-11's row/block presentation-stability requirements.

#### Scenario: Controller workflows are preserved
- **WHEN** the user adds a controller, selects input/output endpoints, expands sections, edits mapping fields, adds or deletes individual rows, adds or deletes block rows, or changes launchpad variant controls
- **THEN** each accepted action is applied through the existing `MidiConfigViewModel` edit APIs and committed through the runtime's instrument edit path
- **AND** refused actions show a status reason without mutating the live instrument

#### Scenario: Connection and rebuild refresh survives the refactor
- **WHEN** MIDI connection state changes, a patch load/revert changes the instrument, or an out-of-band MIDI processor rebuild occurs
- **THEN** the Controllers page marks its semantic tree dirty and refreshes from the current instrument snapshot and connection state
- **AND** active text editing is not clobbered before the edit commits or focus is released

#### Scenario: Empty groups remain editable
- **WHEN** a controller section has no existing mappings in an addable group
- **THEN** the semantic tree still exposes the group's add affordance and, where supported, block-add affordance
- **AND** adding the first row creates any absent profile-config container needed by the view model

#### Scenario: JUCE renderer may improve presentation
- **WHEN** the JUCE backend renders the Controllers page semantic tree
- **THEN** it may replace the current dense component layout with clearer visual layout grouping, spacing, headers, and form controls rather than preserving the current appearance for its own sake
- **AND** every capability required by `sru-4` through `sru-11` remains available and testable

### Requirement: sru-16 — File page: portable patch explorer
WHEN the File page opens Save As or Load, THE runtime UI layer SHALL represent patch browsing and confirmation as a dedicated rootless JUCE-free portable patch-browser state machine over `synth::PatchBrowser`, including flat directory rows with stable identities and selected state, row double-click accept actions, save-name entry for Save As, confirm/cancel actions, status text, empty/error states, and safe root-constrained path resolution; the File page surface SHALL splice that browser state machine into one File page `NodeTree` without introducing a nested root surface; the JUCE desktop backend SHALL render and dispatch that semantic tree without using `juce::FileChooser`, accepted Load confirmations SHALL call the runtime's `LoadPatch` path, typed Save As confirmations SHALL call `SavePatchAs`, and Save As row double-click confirmations SHALL call an explicit overwrite-save-as runtime path.

#### Scenario: Save As uses in-page browser state
- **WHEN** the user chooses Save As from the File page
- **THEN** the portable tree shows a Save As browser rooted at the runtime patches root
- **AND** the user can type a patch name and confirm only when `PatchBrowser::ResolveSaveAsPath` accepts it and the target does not already exist
- **AND** confirmation dispatches the resolved path through the host save callback

#### Scenario: Load uses in-page browser state
- **WHEN** the user chooses Load from the File page
- **THEN** the portable tree shows a Load browser rooted at the runtime patches root
- **AND** directory rows, selected directory state, double-click row loading, and selected directory confirmation are represented by portable nodes/actions
- **AND** confirmation dispatches the resolved directory through the host load callback

#### Scenario: Save As uses explicit overwrite action for existing rows
- **WHEN** the user double-clicks an existing directory row from the Save As browser
- **THEN** the portable tree dispatches a distinct overwrite-save-as confirmation action for that row
- **AND** normal Save As confirmation remains constrained to new patch names

#### Scenario: Browser state is backend-neutral
- **WHEN** a JUCE-free test builds and drives the File page browser tree
- **THEN** it can inspect rows, double-click rows, cancel, enter save names, and confirm valid paths without JUCE headers
- **AND** the JUCE runtime File page host contains no `juce::FileChooser` usage

#### Scenario: Browser nodes are spliced into one File page tree
- **WHEN** the File page portable tree is built while the browser viewer is open
- **THEN** the tree contains exactly one root node for the File page surface
- **AND** browser rows and controls are descendants of the File page browser section rather than a nested root surface

#### Scenario: JUCE backend renders a polished browser viewer
- **WHEN** the JUCE backend renders the File page portable tree with a browser open
- **THEN** it renders the browser as a full flat viewer region with stable controls, visible selected-row treatment, readable labels/status text, row double-click actions, and primary/cancel actions that remain inside their parent bounds after resize
- **AND** File page behavior remains owned by the portable surface rather than by JUCE-only callbacks

### Requirement: sru-17 — File page: model-based browser verification
WHEN the File page browser behavior is tested, THE synth test suite SHALL include deterministic model-based simulation coverage that drives the production JUCE-free File page/browser state machine and the JUCE portable renderer through randomized Save, Save As, Load, select, row double-click accept/overwrite, save-name edit, confirm, cancel, and resize actions while checking an independent oracle for browser-open state, selected path/name state, dispatched save/load callbacks, and root-constrained path safety.

#### Scenario: Simulation preserves browser invariants
- **WHEN** a seeded simulation dispatches a randomized sequence of File page browser actions
- **THEN** the browser never dispatches a Save As or Load path outside the configured patch root
- **AND** invalid save names, existing Save As targets, unreadable roots, or missing load selections update status without dispatching save/load callbacks
- **AND** cancel closes the browser without dispatching save/load callbacks

#### Scenario: Simulation verifies renderer layout
- **WHEN** the seeded simulation rebuilds the JUCE renderer after each File page action and resize
- **THEN** every visible semantic control has a matching JUCE component of the expected kind
- **AND** row controls, status text, save-name input, and primary/cancel actions remain within their parent/root bounds

#### Scenario: First-save flow is simulated
- **WHEN** the simulation triggers Save while the File page has no current patch directory
- **THEN** the Save As browser opens through the same portable state machine used by the explicit Save As action

### Requirement: sru-18 — File page: current patch versions
WHEN the File page has a current patch directory, THE runtime UI layer SHALL present a Versions list for that current patch in the idle File page region, ordered newest-first by sortable patch version filename, and SHALL allow double-clicking a version row to load that exact version file.

#### Scenario: Versions list shows current patch files
- **WHEN** the current patch directory contains multiple `.json` patch version files
- **THEN** the idle File page shows a Versions section
- **AND** the version rows are ordered newest first by filename

#### Scenario: Version double-click loads exact file
- **WHEN** the user double-clicks a version row
- **THEN** the File page dispatches a load action for that exact version file
- **AND** the patch manager loads that version rather than resolving the latest version from the directory

### Requirement: sru-19 — Launcher: app list and one-way selection
WHEN the Sheaf Patch superapp starts, THE runtime UI SHALL present a launcher page listing registered synth apps with their display name, author, category, and advisory hardware requirements, and SHALL start the selected app when the user activates an app row without blocking launch based on those requirements.

#### Scenario: Launcher lists registered apps
- **WHEN** the Sheaf Patch launcher opens
- **THEN** it lists every registered app sorted by stable app id
- **AND** each row shows the app display name, author, category, and minimum encoder requirement

#### Scenario: Selecting miniapp starts miniapp
- **WHEN** the user activates the miniapp row
- **THEN** the miniapp runtime starts
- **AND** the launcher no longer owns the visible main content for that process session

#### Scenario: No in-app return for this change
- **WHEN** a launched app is running
- **THEN** the runtime UI provides no launcher Back or Home action for returning to the app list
- **AND** returning to the launcher requires quitting and restarting the Sheaf Patch executable

#### Scenario: Category is visible
- **WHEN** the app list contains the current miniapp
- **THEN** the row shows category `test`

#### Scenario: Hardware requirements are advisory
- **WHEN** a registered app declares a minimum encoder requirement
- **THEN** the launcher displays that requirement
- **AND** activating the app row still starts the app regardless of detected hardware capability

### Requirement: sru-20 — Portable UI: encoder mouse interaction parity
WHEN a miniapp encoder is rendered through the portable UI layer on the JUCE desktop backend, THE runtime UI layer SHALL preserve the pre-abstraction encoder mouse interaction behavior: mouse drag over the encoder dispatches a relative parameter increment/decrement for that encoder's bound slot and position using the existing `(deltaX - deltaY) * sensitivity` gesture mapping, and mouse double-click dispatches the encoder push action for that same slot and position; the portable tree producer SHALL remain JUCE-free and the JUCE backend SHALL only translate toolkit mouse events into portable actions.

#### Scenario: Mouse drag turns encoder
- **WHEN** the JUCE backend receives a mouse-down followed by a mouse-drag on a portable miniapp encoder node
- **THEN** the miniapp surface dispatches a `MessageIn::ParamIncDec` message to the UI bus for the encoder's slot and position
- **AND** the message delta follows the pre-abstraction encoder drag formula and sensitivity

#### Scenario: Mouse double-click pushes encoder
- **WHEN** the JUCE backend receives a mouse double-click on a portable miniapp encoder node
- **THEN** the miniapp surface dispatches a `MessageIn::ParamPush` message to the UI bus for the encoder's slot and position

#### Scenario: Portable boundary remains JUCE-free
- **WHEN** a JUCE-free synth test includes the portable UI model and miniapp encoder tree builder
- **THEN** it compiles without JUCE headers
- **AND** it can inspect the encoder node's host-neutral drag and double-click action metadata

### Requirement: sru-21 — Portable UI: reusable scope-waveform drawing
WHEN a synth application needs to draw scope-backed waveforms, THE runtime UI layer SHALL provide JUCE-free shared portable drawing logic that converts a scope writer/channel, connection state, color, and target bounds into bounded waveform draw commands, and SHALL allow multiple applications to use that logic without including another application's headers or depending on a JUCE waveform component.

#### Scenario: MiniApp preserves waveform behavior
- **WHEN** MiniApp migrates from its app-local scope draw math to the shared helper
- **THEN** equivalent scope snapshots and bounds produce equivalent waveform paths, colors, and clipping behavior

#### Scenario: Braid uses independent waveform bounds
- **WHEN** Braid supplies audible and LFO scope channels with non-overlapping panel bounds
- **THEN** the helper produces independently bounded waveform command sets for every supplied channel
- **AND** it does not overlay one channel into another channel's panel

#### Scenario: Shared helper remains JUCE-free
- **WHEN** a synth test includes the shared waveform builder and constructs commands from a fake scope snapshot
- **THEN** it compiles and runs without JUCE headers

### Requirement: sru-22 — Braid 4: waveform and encoder main screen
WHEN the Braid 4 application surface is visible, THE runtime UI SHALL show four audible VCO waveform panels in a 2x2 grid stacked above four LFO waveform panels in a second 2x2 grid, all sixteen cells of its unique bank slot in a 4x4 encoder grid, and one compact global scene strip with selectors for scenes `0/1` plus the shared blend fader; SHALL bind encoder cells in row-major order to slot `0` positions `0..15`; SHALL reflect the currently selected Braid, matrix, LFO, or LFO matrix bank through the existing slot UI state; and SHALL keep both complete waveform grids, the encoder grid, and the scene strip visible without scrolling at the default application size.

#### Scenario: Four waveforms occupy a 2x2 grid
- **WHEN** Braid's default screen is built
- **THEN** oscillator 1, 2, 3, and 4 draw in the top-left, top-right, bottom-left, and bottom-right waveform panels respectively
- **AND** each panel reads its corresponding published VCO UI state and scope channel

#### Scenario: Four LFO waveforms occupy a second 2x2 grid
- **WHEN** Braid's default screen is built
- **THEN** LFO oscillator 1, 2, 3, and 4 draw in a second 2x2 grid below the audible VCO grid
- **AND** each panel reads its corresponding published LFO UI state and scope channel

#### Scenario: Sixteen encoders occupy a 4x4 grid
- **WHEN** Braid's default screen is built
- **THEN** it contains sixteen encoder nodes arranged as four rows of four
- **AND** node `n` dispatches turns and pushes to slot `0`, position `n`

#### Scenario: Reserved positions use shared disconnected encoder state
- **WHEN** the Braid bank is active
- **THEN** encoder nodes `2` and `3` remain present and interactive for slot `0` positions `2` and `3`
- **AND** they use the shared encoder renderer's empty disconnected draw-command state
- **AND** the remaining fourteen nodes render their parameter names, configured color, value state, and native voice indicators

#### Scenario: All four banks reuse the same encoder grid
- **WHEN** any of the audible Braid, audible matrix, LFO Braid, or LFO matrix banks becomes active through the existing bank-selection message path
- **THEN** all sixteen encoder nodes update to the active bank's parameters
- **AND** no second encoder slot or second encoder grid is created

#### Scenario: Scene strip remains global across banks
- **WHEN** the user selects either scene or changes blend and then switches between Braid and matrix banks
- **THEN** the same scene endpoints and blend remain displayed and active
- **AND** the UI creates no bank-local scene selector or fader

#### Scenario: Visual treatment carries astronomical character
- **WHEN** Braid's surface draws its default background and panels
- **THEN** it uses a near-black deep-space treatment with red audible VCO waveform/control accents, green LFO waveform/control accents, orange audible matrix accents, and yellow/green-yellow LFO matrix accents
- **AND** all text and control state remain legible through the portable UI backend

### Requirement: sru-23 — Portable encoder consumes complete parameter snapshot
WHEN a portable synth surface builds an encoder, THE runtime UI layer SHALL derive its complete encoder draw state from one `Parameter::UIState` without app-supplied parameter, indicator, modulation-source, gesture, bank, or scope palette arguments.

#### Scenario: Shared builder is app independent
- **WHEN** Braid 4 and MiniApp build encoders from visible slot cells
- **THEN** both call the same reusable encoder-state and drawing functions
- **AND** neither app contains a color reconstruction or post-snapshot override

### Requirement: sru-24 — Modulation view: visualizer beneath encoder
WHEN a portable encoder grid renders a connected modulation-depth cell whose complete `Parameter::UIState` publishes a non-null visible visualizer, THE runtime UI layer SHALL assign that visualizer bounds exactly equal to the encoder cell's square, append a stable visualizer draw node before the encoder node, and append the existing interactive encoder node above it using the portable tree contract that later overlapping nodes draw above earlier overlapping nodes; WHEN the published pointer is null or the visualizer is intrinsically hidden, THE runtime UI layer SHALL render only the existing encoder node.

#### Scenario: Visualizer and encoder share a square
- **WHEN** a visible modulation-depth cell has square encoder bounds and a visible visualizer
- **THEN** the portable tree contains a visualizer draw node with exactly the encoder bounds
- **AND** the encoder draw node follows it in stacking order with the same bounds

#### Scenario: Encoder remains interactive above visualizer
- **WHEN** a visualizer is rendered beneath an encoder
- **THEN** the encoder retains its existing drag and double-click actions
- **AND** the display-only visualizer exposes no competing pointer action

#### Scenario: Missing visualizer preserves existing rendering
- **WHEN** a visible modulation-depth cell publishes a null visualizer pointer
- **THEN** the tree contains the same encoder node and encoder commands as before this capability
- **AND** contains no visualizer node for that cell

#### Scenario: Top-level cells do not infer source visualizers
- **WHEN** the encoder grid shows an ordinary top-level parameter rather than a materialized modulation-depth control
- **THEN** it renders the encoder without looking up or inferring a visualizer from group topology

### Requirement: sru-25 — Encoder grid: translucent visualizer underlays
WHEN a portable encoder grid renders an encoder node above a visible visualizer underlay, THE runtime UI layer SHALL request the shared encoder draw builder to use an underlay-aware body style whose central body fill is partially translucent while preserving the encoder's existing strokes, arcs, badges, labels, and pointer actions; WHEN no visible visualizer underlay is present, THE runtime UI layer SHALL preserve the existing opaque encoder body style.

#### Scenario: Underlay-backed encoder body is translucent
- **WHEN** a connected encoder draw state indicates a visible visualizer underlay
- **THEN** the encoder draw commands use a partially transparent central body fill
- **AND** retain the existing readable encoder outline and value commands

#### Scenario: Ordinary encoder body remains opaque
- **WHEN** a connected encoder draw state does not indicate a visible visualizer underlay
- **THEN** the encoder draw commands preserve the existing opaque central body fill

#### Scenario: Hidden visualizer does not affect encoder style
- **WHEN** a cell publishes a visualizer pointer whose visualizer is hidden
- **THEN** the portable encoder grid renders no visualizer node
- **AND** requests the ordinary opaque encoder body style

#### Scenario: Underlay styling is shared, not MiniApp drawing
- **WHEN** MiniApp or Braid 4 builds an encoder cell with a visible visualizer pointer
- **THEN** the app surface only marks the shared encoder draw state as having an underlay
- **AND** the alpha choice remains inside shared portable encoder drawing code

### Requirement: sru-26 — Controllers page: absolute encoder mode editing
WHEN an encoder input configuration is presented or edited through the Controllers page, THE runtime library SHALL expose one non-deletable `Encoder mode` config-level row whose declaration-order choices are signed-7-bit, direction-only, and absolute; SHALL edit and flush the selected `EncoderMode` through the existing open-section edit session; SHALL preserve the stored `turnStep` while absolute mode is selected and identify that it affects relative modes only; and SHALL rebuild the live controller processor with the committed mode without re-coalescing or replacing the open session.

#### Scenario: Absolute mode is selectable
- **WHEN** the user edits an encoder input's `Encoder mode` row and selects the third catalog entry
- **THEN** the session row and persisted controller config use `EncoderMode::Absolute`
- **AND** the live-edit rebuild creates an absolute encoder input processor

#### Scenario: Absolute mode survives session flush and rebuild
- **WHEN** an open encoder section commits absolute mode and the view model rebuilds while that section remains open
- **THEN** the same session row remains visible in place and displays absolute mode
- **AND** the persisted controller config remains absolute

#### Scenario: Turn step is retained but ignored by absolute mode
- **WHEN** a controller has a non-default `turnStep` and the user changes its encoder mode from relative to absolute
- **THEN** the Controllers page preserves the stored `turnStep`
- **AND** identifies it as a relative-mode setting
- **AND** changing the absolute encoder position does not apply that step

#### Scenario: Switching back restores relative configuration
- **WHEN** the user changes an absolute encoder input back to signed-7-bit or direction-only mode
- **THEN** subsequent CC input uses the previously stored `turnStep`

#### Scenario: Encoder mode row is not deletable
- **WHEN** the encoder section presents the `Encoder mode` config-level row
- **THEN** the row exposes no delete affordance

### Requirement: sru-27 — Controllers page: Note/CC button address editing

WHEN the Controllers page edits encoder push mappings or Generic controller system-message mappings, THE runtime UI layer SHALL expose a Note/CC selector
with numeric channel and message-number fields, SHALL use numeric note values
rather than musical note names, SHALL omit the selector from encoder turns,
analog mappings, and controller-specific system-message address schemas, and
SHALL commit the selected message type through the existing config edit path.

#### Scenario: Encoder push row selects note

- **WHEN** a user changes an encoder push row from CC to Note and enters channel `1` and number `60`
- **THEN** the committed push mapping is note `60` on channel `1`
- **AND** the row displays the note as numeric value `60`

#### Scenario: Generic system-message row selects note

- **WHEN** a Generic controller system-message row is shown
- **THEN** the row includes the Note/CC selector, channel field, and numeric message-number field

#### Scenario: CC-only rows omit the selector

- **WHEN** an encoder turn, analog mapping, or non-Generic controller-specific system-message row is shown
- **THEN** that row does not offer note addressing

#### Scenario: Blocks do not mix message types

- **WHEN** adjacent button mappings have otherwise contiguous fields but one is CC and one is Note
- **THEN** the Controllers page does not coalesce them into one block row

#### Scenario: Block message type round trips

- **WHEN** a Note encoder-push block or Generic system-message block is expanded and committed
- **THEN** every resulting mapping retains the Note message type

### Requirement: sru-28 — Controllers page: grid buttons and blocks
WHEN a WRLD.Bldr or Launchpad controller's system mappings target button-grid messages, THE runtime UI SHALL present each exact press/release/feedback plus polyphonic-pressure pair as one user-visible grid button or maximal rectangular grid block, SHALL expose the target grid-slot index and the existing kind-specific signed physical x/y range with exclusive maxima, SHALL map physical `(x,y)` directly to the same logical grid coordinate, SHALL always derive press, release, pressure-change, and feedback mappings together on commit, SHALL offer no toggle or aftertouch controls, and SHALL preserve unknown pressure mappings invisibly and losslessly through open-section editing and persistence.

#### Scenario: Grid block expands all input behavior
- **WHEN** the user commits a Launchpad grid block for slot `1` over `[0,8) x [-1,7)`
- **THEN** the profile receives one system association per coordinate with paired `GridPress` and `GridRelease` and grid feedback
- **AND** receives one matching derived polyphonic-pressure mapping per coordinate
- **AND** every range maximum is treated as exclusive

#### Scenario: Single grid button hides MIDI mechanics
- **WHEN** one exact grid mapping is presented individually
- **THEN** the row identifies it as one grid button with controller address and target grid slot
- **AND** it exposes no toggle, note number, MIDI status, aftertouch, or pressure-mapping row

#### Scenario: Exact mappings reconstruct to one block
- **WHEN** canonical system and pressure config contains a uniform rectangular run of matching grid mappings
- **THEN** collapsing and reopening the section reconstructs the maximal grid block
- **AND** expanding that block reproduces the canonical underlying mappings exactly

#### Scenario: Negative coordinates round-trip
- **WHEN** a grid block includes y `-1` and has exclusive maximum y `7`
- **THEN** its row, edit session, expansion, JSON persistence, and reconstruction preserve the signed coordinates

#### Scenario: Grid mappings are always momentary
- **WHEN** a grid button or block is added or edited
- **THEN** the view model generates press and release messages as a pair
- **AND** offers no toggle variant

#### Scenario: Orphan pressure data is preserved but hidden
- **WHEN** externally authored profile config contains a pressure mapping that does not exactly pair with a grid system association
- **THEN** the Controllers page shows no aftertouch line item for it
- **AND** opening, editing an unrelated row, committing, and saving preserves that mapping unchanged

#### Scenario: Pair edit is atomic
- **WHEN** a grid block edit would create an invalid controller address, duplicate address, invalid signed rectangle, or unrepresentable logical target
- **THEN** the commit is refused with a reason
- **AND** neither its system associations nor pressure mappings change

### Requirement: sru-29 — Controllers page: grid mapping model coverage
WHEN grid mapping presentation is tested, THE synth runtime UI test suite SHALL cover pure expansion/reconstruction, profile JSON round trips, open-session edit stability, add/delete operations, signed exclusive ranges, hidden pressure sidecars, and deterministic seeded simulation against an independent model without requiring JUCE or physical MIDI devices.

#### Scenario: Pure model builds without JUCE
- **WHEN** a headless test constructs grid singleton and block rows for WRLD.Bldr and Launchpad profiles
- **THEN** it compiles and runs without JUCE headers
- **AND** produces the expected paired system and pressure config

#### Scenario: Seeded simulation preserves paired mappings
- **WHEN** a deterministic simulation adds, edits, deletes, opens, closes, and reconstructs grid rows and blocks
- **THEN** every visible grid row has exactly one matching pressure entry per cell
- **AND** unknown hidden pressure entries remain equal to the independent oracle

#### Scenario: Renderer never exposes aftertouch
- **WHEN** the portable Controllers tree and JUCE renderer are built from a profile containing grid pressure mappings
- **THEN** visible labels and editable fields contain grid button/block concepts
- **AND** contain no standalone aftertouch or polyphonic-pressure line item

### Requirement: sru-30 — Controllers page: relative bank message editing
WHEN system-message mappings are presented or edited in the low-level controller mapping editor, THE synth runtime UI SHALL expose “Next Bank” and “Previous Bank” message kinds, SHALL present exactly one message `Arg` for either kind and interpret it as `slotIx`, SHALL preserve that argument across message-kind conversion through the shared primary-argument path, and SHALL represent each mapping as an individual press action with no release. The controller-wizard form SHALL instead follow scw-3's more specific form-wide Encoder Slot and disabled per-button argument contract for these two kinds.

#### Scenario: Relative bank rows expose slot as Arg
- **WHEN** a low-level system-message row selects Next Bank or Previous Bank
- **THEN** the row exposes one editable `Arg` field
- **AND** editing `Arg` updates `slotIx` in the press and feedback messages
- **AND** no bank-index argument or release message is present

#### Scenario: Relative bank kind conversion preserves the argument
- **WHEN** a low-level row whose primary argument is `5` changes to Next Bank and then Previous Bank
- **THEN** both resulting messages target slot `5`
- **AND** rebuilding the view model preserves the selected message kind and argument

#### Scenario: Relative bank mappings remain individual
- **WHEN** adjacent controller addresses map to relative bank messages
- **THEN** controller block reconstruction leaves them as individual rows
- **AND** committing the section preserves their canonical message ordering and persisted meanings

#### Scenario: Randomized controller editing covers relative bank messages
- **WHEN** the deterministic controller view-model simulation adds, converts, edits, rebuilds, and deletes relative bank mappings
- **THEN** the production row model and persisted profile arrays remain equal to the independent model

### Requirement: sru-31 — Sync page: portable clock configuration and status
WHEN the runtime Sync page is open, THE runtime library SHALL render through the shared portable UI a send-clock toggle, receive-clock toggle, send-transport toggle, receive-transport toggle, integer PPQN control limited to `1..960`, Back action, and read-only current BPM, lock state, active-source, output-latency, ignored-input, late-event, and dropped-output status; edits SHALL stage in JUCE-free page state and commit through generic host services without application-specific or host-specific UI logic.

#### Scenario: Defaults are safe and standard
- **WHEN** the Sync page opens for a runtime with no saved sync configuration
- **THEN** all four toggles are off and PPQN is 24

#### Scenario: Invalid PPQN is refused in the page
- **WHEN** an action attempts to set PPQN below 1 or above 960
- **THEN** the page retains its prior valid value and shows an inline validation status

#### Scenario: External lock is visible
- **WHEN** MasterClock is locked to a connected controller's external clock
- **THEN** the page shows Locked, that controller's name, and the recovered BPM

#### Scenario: Both hosts use one page model
- **WHEN** Sync controls are rendered and edited in JUCE and Chrome
- **THEN** both hosts use the same portable tree, action names, staged view model, and host-service commit path
- **AND** neither backend contains clock-policy logic

#### Scenario: Nonstandard PPQN is identified
- **WHEN** PPQN is set to a value other than 24
- **THEN** the page indicates that MIDI peers must be configured for the same nonstandard pulse density

### Requirement: sru-32 — Controllers page: three-click configuration wizard flow
WHEN the Controllers page is open, THE runtime library SHALL provide a Configuration Wizard action. It SHALL be visibly disabled with an explanatory no-candidate status when no available candidate exists, SHALL open the sole candidate's form directly when exactly one exists, SHALL present a candidate chooser when more than one exists, and SHALL submit the active form through the controller-wizard commit path. The unique-candidate path SHALL require only Controllers, Configuration Wizard, and Submit activations to install the default profile.

#### Scenario: No candidate explains the disabled action
- **WHEN** no wizard candidate is available
- **THEN** Configuration Wizard remains visible but disabled
- **AND** the page explains that no recognized unconfigured controller pair is present

#### Scenario: Unique candidate is three clicks
- **WHEN** exactly one unconfigured MF Twister pair is present
- **AND** the user activates Controllers, Configuration Wizard, and Submit without changing defaults
- **THEN** an active Twister profile is installed with both endpoints, Encoder Slot 0, sixteen encoders targeting slot 0, and exactly six default side-button mappings
- **AND** no additional selection or confirmation activation is required

#### Scenario: Multiple candidates require selection
- **WHEN** two or more available candidates exist
- **THEN** Configuration Wizard first presents their controller and endpoint labels
- **AND** selecting one opens only that candidate's form

#### Scenario: Form retains edits after refusal
- **WHEN** Submit is refused because validation fails or a new candidate disconnected
- **THEN** the wizard remains open with every entered choice preserved
- **AND** an inline status explains the refusal

#### Scenario: Twister form exposes one slot and six rows
- **WHEN** an MF Twister form is open
- **THEN** it shows one Encoder Slot control and exactly six message/argument rows
- **AND** the rows are presented as two columns of three in physical CC order

#### Scenario: Fast path still exposes Ignore
- **WHEN** the unique-candidate flow opens a form directly
- **THEN** the form provides Ignore this controller as a secondary action
- **AND** activating it creates the blacklisted row without generating an active profile

#### Scenario: Ignore is absent during reconfiguration
- **WHEN** a wizard form was opened from an existing active or blacklisted record
- **THEN** it does not offer Ignore this controller

#### Scenario: Submit refuses a stale session
- **WHEN** the selected candidate or existing record is removed, changes endpoints or disposition, or becomes claimed by a conflicting commit before Submit
- **THEN** Submit refuses the change and retains the form state
- **AND** no instrument commit or configuration save occurs

### Requirement: sru-33 — Controllers page: portable wizard backend parity
WHEN controller discovery, wizard forms, blacklist controls, or profile lifecycle controls are rendered, THE runtime library SHALL derive them from the same portable node tree, stable node ids, portable actions, and host-service commit callbacks in Chrome and JUCE. The browser and JUCE backends SHALL render and dispatch the same ids, labels, choices, values, enabled states, and outcomes, and neither backend SHALL contain controller-wizard, MF Twister, blacklist, profile-generation, or validation policy.

#### Scenario: Browser renders portable wizard controls
- **WHEN** the MF Twister form is open in Chrome
- **THEN** its Encoder Slot, six dropdown/argument pairs, and Submit/Ignore actions are rendered from portable semantic nodes
- **AND** browser events return through `Surface::DispatchAction`

#### Scenario: JUCE renders the same form contract
- **WHEN** the same MF Twister form is open through the JUCE backend
- **THEN** it exposes the same node ids, labels, selected options, enabled argument fields, and actions as Chrome
- **AND** action outcomes mutate the same JUCE-free form state

#### Scenario: Playwright pins the browser-first acceptance path
- **WHEN** the browser integration suite runs with test-controlled MF Twister input/output ports
- **THEN** Playwright verifies the exact three-click installation path and resulting active profile
- **AND** also covers chooser, Ignore, warning, rename, delete, and reconfigure behavior

#### Scenario: JUCE simulation pins backend parity
- **WHEN** the JUCE Controllers-page simulation suite runs
- **THEN** it drives the production portable actions for form editing, submission, Ignore, rename, delete, and reconfigure
- **AND** its node ids, values, enabled states, dispatched actions, and resulting portable state match the JUCE-free surface expectations

### Requirement: sru-34 — Portable UI: semantic enabled state
WHEN a portable semantic control is disabled, THE runtime UI model SHALL carry that enabled state through its node contract, the Chrome backend SHALL render it as a disabled DOM control, the JUCE backend SHALL render it as a disabled JUCE control, and neither backend SHALL dispatch its user action while disabled.

#### Scenario: Wizard argument fields disable portably
- **WHEN** an MF Twister button selects Start, Next Bank, Previous Bank, or another no-argument choice
- **THEN** the argument control has the same disabled state in the portable tree, Chrome, and JUCE
- **AND** user interaction with that disabled control does not mutate form state

### Requirement: sru-43 — Portable UI: single hierarchical authoring library
WHEN any runtime page, runtime shell surface, or synth application builds a portable node tree, THE runtime UI layer SHALL provide one JUCE-free component library through which the tree is authored, supporting container components (column, row, section, scroll area) that nest to arbitrary depth, leaf semantic controls with their colour, text style, caption, selected, enabled, and action state settable at construction, reusable components defined as ordinary callables that emit into the same builder so components can compose other components, and splicing of an externally built subtree; the library SHALL emit the existing `NodeTree` contract (kind-tagged nodes, stable ids, `children` references) so hierarchy and composition require no protocol change beyond the sru-46 coordinate-semantics shift, and runtime page code SHALL NOT assemble `ui::Node` structs by hand outside the library.

#### Scenario: Containers nest through the public API
- **WHEN** a JUCE-free test builds a section containing a scroll area containing rows of controls through the component library
- **THEN** the resulting `NodeTree` contains the corresponding `Section`, `ScrollArea`, and `Row` nodes with correct parent-child `children` references at each depth
- **AND** the tree serializes over the command buffer's existing node/children encoding without hierarchy-specific additions

#### Scenario: Components compose components
- **WHEN** a reusable component (a callable taking the builder) emits a captioned control row, and a page component invokes it multiple times inside a column
- **THEN** each invocation produces its subtree in place with distinct stable node ids
- **AND** neither the page nor the component constructs `ui::Node` values directly

#### Scenario: Construction expresses full control state
- **WHEN** a builder call creates a button with a colour, text style, selected state, and disabled state
- **THEN** the resulting node carries that colour, text style, `selected`, and `enabled` without any post-build mutation of the tree

#### Scenario: External subtrees splice without a nested root
- **WHEN** a page splices an externally built subtree (such as the File page patch-browser tree) into its own tree
- **THEN** the spliced nodes appear as descendants of the splice point with exactly one root node in the final tree

#### Scenario: Config pages contain no hand-rolled nodes
- **WHEN** the runtime page sources are inspected after migration
- **THEN** Audio, Sync, File, and Controllers page tree construction goes through the component library
- **AND** no runtime page code initializes `ui::Node` structs field-by-field

### Requirement: sru-44 — Portable UI: declarative build-time layout with library-owned metrics
WHEN a portable tree is built through container components, THE component library SHALL resolve every contained node's parent-relative `Bounds` at build time from declarative layout inputs — vertical stacking in columns/sections/scroll areas, horizontal stacking in rows, per-child fixed extents, intrinsic defaults, fractions of the container's content extent, or weights over remaining space, each optionally bounded by a declared minimum and maximum extent, resolved by one stated deterministic allocation algorithm: fixed, intrinsic, and fractional extents resolve first (a fraction being taken of the container's extent less its padding, before gaps and siblings are considered); weights then divide whatever main-axis space remains after those extents, all inter-child gaps, and the container's padding; every resolved extent is then clamped to its declared minimum and maximum, and the space freed or demanded by clamping is redistributed in exactly one further pass across only those children that are weighted and not themselves clamped, in proportion to their weights; residual space that no eligible child can absorb is left unallocated at the end of the container rather than forced onto a clamped child, and a container whose children's minima exceed its extent overflows deterministically in declaration order rather than shrinking any child below its minimum — with padding and inter-child gaps drawn from the library's shared spacing metrics — SHALL provide an explicit wrapping-row container option that flows children onto additional lines within the container's extent, SHALL own the intrinsic sizing contract in place of any backend default-size table: per-kind intrinsic extents are library constants, and text-bearing extents are deterministic reservations derived from character count and per-text-style advance metrics, resolved without consulting any backend so that geometry never depends on toolkit font measurement (fitting text within a resolved extent is the backend's own local concern under sru-49), SHALL provide a form-grid facility that aligns the label and control columns of participating rows to shared x-offsets within their container, SHALL resolve each container's children against only that container's own extent so a resolved subtree resolved against the same available extent is identical wherever it is mounted, and SHALL leave explicitly positioned children at their author-supplied parent-relative bounds, treating them as out of flow so they consume no stacking space and their stacked siblings resolve as if they were absent; out-of-flow SHALL be a positioning mode a producer chooses per node and SHALL NOT be an intrinsic property of any node kind, so that a `Draw` node may equally participate in flow; a producer SHALL also be able to place an out-of-flow node by naming the in-flow sibling it overlays, resolving to that sibling's bounds, so that an overlay whose extent is decided by the resolver — such as the sru-25 translucent visualizer underlay beneath a resolver-sized encoder cell — is expressible without its producer computing any geometry; and for an in-flow `Draw` node the library SHALL accept a command factory in place of fixed geometry and SHALL invoke it during resolution with that node's own resolved node-local extent, so a drawn component can fill a layout slot without its producer computing the slot's size; no backend SHALL perform layout or supply default sizes.

#### Scenario: Inserting a row shifts no hand-written offsets
- **WHEN** a page adds one captioned row in the middle of a column
- **THEN** subsequent siblings' resolved bounds move down by the row's extent plus the shared gap metric without any producer-side coordinate edits

#### Scenario: Like controls align in a form grid
- **WHEN** a form grid lays out rows whose labels have differing text lengths
- **THEN** every participating control column starts at the same resolved x-offset within the container
- **AND** every participating label column starts at the same resolved x-offset within the container

#### Scenario: Weights divide remaining space deterministically
- **WHEN** a row contains one fixed-width child and two weight-1 children
- **THEN** the two weighted children resolve to equal widths filling the remaining row width
- **AND** re-resolving the same inputs yields byte-identical bounds

#### Scenario: A component resolves the same everywhere
- **WHEN** the same component is emitted into two different parents that differ in position but offer the same available extent and layout inputs
- **THEN** the resolved parent-relative bounds of the component's subtree are identical in both placements
- **AND** no node in the subtree encodes a surface-global position

#### Scenario: Extents are clamped by declared minima and maxima
- **WHEN** a child declares a weight together with a maximum extent, and its weighted share exceeds that maximum
- **THEN** the child resolves to exactly its maximum extent
- **AND** the freed space is redistributed in one further pass across only the weighted, unclamped siblings, in proportion to their weights
- **AND** space that no eligible sibling can absorb is left unallocated at the end of the container rather than forced onto a clamped child

#### Scenario: A fraction is taken of the container's content extent
- **WHEN** a child declares a fractional extent of 0.46 inside a container whose extent less padding is 868
- **THEN** the child resolves to 399.28 before clamping, independently of the container's gaps and its siblings' extents
- **AND** declaring a maximum of 390 alongside it resolves the child to exactly 390

#### Scenario: Infeasible minima are deterministic, and then fail
- **WHEN** a container's children declare minimum extents whose sum exceeds the container's extent
- **THEN** no child resolves below its declared minimum
- **AND** the order in which they exceed the container is deterministic and follows declaration order, so the diagnostic always names the same first offending child
- **AND** resolution then fails per sru-54 rather than yielding a tree whose overflowing children would be silently clipped — determinism describes *how* the overflow is computed, not that it is an acceptable result

#### Scenario: An in-flow Draw node is given its resolved extent
- **WHEN** a `Draw` node participates in flow and supplies a command factory instead of fixed geometry
- **THEN** the library invokes the factory during resolution with that node's own resolved node-local extent
- **AND** the resulting commands fill the extent the layout allocated, with no producer-side computation of the slot's size
- **AND** re-resolving the same producer code at a different root extent produces commands filling the new extent

#### Scenario: A wrapping row flows onto additional lines
- **WHEN** a wrapping-row container holds more children than fit its extent on one line
- **THEN** the overflowing children resolve onto subsequent lines separated by the shared gap metric
- **AND** the container's resolved extent grows to contain every line

#### Scenario: Text extents are reservations, not measurements
- **WHEN** a label's intrinsic width is resolved
- **THEN** the width derives from the label's character count and its text style's advance metric in the library
- **AND** the resolved bounds are identical regardless of which backend later renders the tree
- **AND** no layout input is obtained by asking a backend to measure text

#### Scenario: An overlay is positioned by naming what it overlays
- **WHEN** a producer marks a node out of flow by naming an in-flow sibling it overlays, and that sibling's extent is decided by the resolver rather than by the producer
- **THEN** the overlay resolves to exactly that sibling's bounds
- **AND** the producer supplies no coordinates or extents of its own
- **AND** the sibling's own resolved bounds and every other sibling's are unchanged by the overlay's presence

#### Scenario: Explicitly positioned children are out of flow
- **WHEN** a container holds an explicitly positioned `Draw` child among stacked siblings
- **THEN** the child's resolved bounds equal its author-supplied parent-relative bounds
- **AND** the stacked siblings resolve to the same bounds they would have had if the explicitly positioned child were absent

### Requirement: sru-45 — Portable UI: direct colour and text style on components
WHEN a portable semantic control is constructed, THE component library SHALL accept an optional colour (plain `synth::Color` RGBA, as `DrawCommand::color` already carries) and an optional text style as direct component properties carried on the node record with no indirection; the carried colour SHALL have one defined meaning per node kind — the control fill for `Button` and `Toggle`, the field background for `ComboBox` and `TextField`, the filled-track accent for `Slider`, the container or surface background fill for `Root`, `Row`, `Section`, and `ScrollArea`, and the text background for `Label` and `StatusText` (whose glyph colour comes from the carried text style, never from the carried colour) — and SHALL be ignored for `Draw`, whose commands carry their own colours; a carried value SHALL take precedence over every backend constant for that node, and a backend SHALL derive its selected, hover, pressed, and disabled presentation from the carried colour rather than substituting a colour of its own; a caption SHALL be expressed as an ordinary library-emitted `Label` node in the control's form-grid row rather than as a node-record field or wire addition; THE JUCE backend SHALL render carried values so that its hardcoded per-variant colour constants no longer decide the appearance of a node that carries them, THE browser backend SHALL render carried values rather than dropping styling, and a node carrying no colour or text style SHALL render each backend's plain default look, including that backend's existing selected and disabled treatment.

#### Scenario: A green button is a green button
- **WHEN** an application constructs a button with a green colour through the component library
- **THEN** the node record carries that RGBA value across the command buffer
- **AND** the button renders green in both the JUCE and browser backends with no lookup or vocabulary in between

#### Scenario: Text style is direct
- **WHEN** a label is constructed with an explicit text style (size and colour)
- **THEN** both backends render that size and colour from the carried value

#### Scenario: Unstyled nodes keep a default look
- **WHEN** a tree sets no colour or text style on its controls
- **THEN** every control still renders with the backend's default appearance
- **AND** no control renders unstyled or invisible

#### Scenario: One colour has one meaning per kind
- **WHEN** the same RGBA value is carried by a `Button`, a `Slider`, a `TextField`, and a `Label`
- **THEN** it fills the button, tints the slider's filled track, backs the text field, and backs the label without recolouring the label's glyphs
- **AND** both backends agree on which surface the value paints for each kind

#### Scenario: Selected state is derived from the carried colour
- **WHEN** a `Button` carrying a colour is selected, hovered, pressed, or disabled
- **THEN** each backend derives that state's appearance from the carried colour
- **AND** neither backend substitutes a per-variant colour constant of its own

#### Scenario: A caption is a label node, not a field
- **WHEN** a form control is built with a caption through the component library
- **THEN** the caption appears in the tree as a `Label` node in the control's form-grid row with its own stable id
- **AND** the `Node` record and the command buffer gain no caption field

#### Scenario: Appearance changes need no backend edit
- **WHEN** a producer changes the colour it passes to a control
- **THEN** that control's appearance changes in both backends
- **AND** no library, codec, or backend source change is required

### Requirement: sru-46 — Portable UI: hierarchical parent-relative coordinates
WHEN a portable tree crosses the model or the command buffer, THE runtime UI layer SHALL express every node's `Bounds` relative to its parent's coordinate space (the root's relative to the surface, and `ScrollArea` children relative to the scroll-content origin), SHALL express all `Draw` command geometry relative to the owning node's own origin with content clipped to the node's bounds, SHALL remove from both backends every coordinate-space classification heuristic — the draw-geometry family (`DrawCommandsLookLocal` and its per-command variants, and the `nodeLocal` dispatch flag) and the node-bounds family (`ExplicitBoundsAreParentLocal` / `explicitBoundsAreParentLocal`, which today decides whether a node's explicit bounds are parent-local or surface-absolute by testing containment within the parent) — so no backend guesses which space any geometry is in, and SHALL carry this coordinate semantics change together with the sru-45 style fields in a single version bump to version 2 applied consistently across every artifact that advertises the UI protocol version — the C++ `kCommandBufferVersion`, the TypeScript `COMMAND_BUFFER_VERSION`, and each Wasm package's exported `synth_browser_ui_protocol_version()` — retaining the strict version-equality check on both ends with no fallback decoding of version-1 buffers; the shell bundle and every app package SHALL be rebuilt and republished together against the new version.

#### Scenario: Moving a parent moves only one record
- **WHEN** a parent node's bounds change position while its subtree is unchanged
- **THEN** only that parent's node record differs on the wire
- **AND** every descendant's serialized bounds are byte-identical to the previous frame

#### Scenario: Draw geometry is node-local without guessing
- **WHEN** a `Draw` node's commands place geometry at the node's origin
- **THEN** both backends render it at the node's position by definition
- **AND** neither backend contains code that classifies command coordinates as local or absolute

#### Scenario: Scroll children ignore scroll offsets
- **WHEN** a `ScrollArea` is scrolled
- **THEN** its children's bounds in the tree and on the wire are unchanged
- **AND** the visible movement is entirely a backend view transform

#### Scenario: Version mismatch fails loudly
- **WHEN** a decoder receives a command buffer whose version differs from its own
- **THEN** decoding fails with an explicit version error and no frame is rendered
- **AND** no fallback or negotiation path exists

#### Scenario: Every version site advertises the same version
- **WHEN** the version-2 runtime is built
- **THEN** the C++ command-buffer constant, the TypeScript constant, and every Wasm package's exported UI protocol version all report version 2
- **AND** a package still advertising version 1 is rejected before any frame is rendered

#### Scenario: Publication moves as one set
- **WHEN** the version-2 runtime is published
- **THEN** the shell bundle and all app packages in the catalog are rebuilt against version 2 in one whole-catalog publication
- **AND** rollback restores the previous complete publication rather than mixing versions

### Requirement: sru-47 — Configuration pages: rebuilt on the component library
WHEN the Audio, Sync, File, and Controllers pages are rebuilt on the component library, THE runtime library SHALL preserve every behavior these pages are required to have by sru-3 through sru-13, sru-15 through sru-18, sru-26 through sru-33, and sru-34 — including Controllers edit-session stability (sru-11) and scrollable mapping reachability (sru-5) — while replacing manual pixel arithmetic with declarative layout; every form control on these pages SHALL have a visible caption naming what it controls (including the Audio page's output and input device selectors, visible while a device is selected), like-type controls SHALL align through the form grid, and the pages SHALL contain no text that conveys no information to the user, with any removal of text whose presence a spec scenario pins made only through an explicit spec change rather than silently.

#### Scenario: Page behavior suites stay green through migration
- **WHEN** each page's tree construction moves to the component library
- **THEN** the page's existing behavioral and simulation test suites pass unchanged in what they assert about behavior
- **AND** only position-pinning expectations are re-pinned to the new resolved layout

#### Scenario: Audio selectors are captioned
- **WHEN** the rebuilt Audio page renders with an output selector and (when configured) an input selector
- **THEN** each selector shows a visible caption identifying it while a device is selected, in both backends
- **AND** a hidden input selector shows no orphaned caption

#### Scenario: Controllers edit session survives the rebuild
- **WHEN** a user edits mapping rows while a Controllers section stays expanded on the rebuilt page
- **THEN** the edit-session row list behaves exactly as sru-11 requires (no re-coalescing, stable rows, session flush on collapse)

#### Scenario: Content audit removes only uninformative text
- **WHEN** the per-page content audit completes
- **THEN** removed strings are limited to text carrying no user information (decorative chrome, duplicate labels, leaked placeholders)
- **AND** every status or readout pinned by an existing requirement remains present and reachable

#### Scenario: Hierarchy replaces offset arithmetic
- **WHEN** the rebuilt pages' sources are inspected
- **THEN** page-level `y +=` style offset accumulation and per-site pixel constants are gone
- **AND** grouping is expressed through nested sections, rows, and grids

### Requirement: sru-48 — Portable UI: named visual criteria with a Playwright verification loop
WHEN the rebuilt pages' appearance is verified, THE synth project SHALL define named visual acceptance criteria — like-type controls share column positions; all spacing drawn from the library's shared spacing metrics; every form control captioned; no overlapping nodes and no container overflow on either axis at the reference viewport; every text element rendered within its allocated extent; text contrast meeting at least the WCAG AA 4.5:1 ratio against its effective background; no non-informative text — SHALL fix one named reference viewport, device scale factor, and deterministic page and app fixture state under which every criterion is evaluated and every screenshot captured, SHALL enforce the machine-checkable criteria through Playwright structural assertions against the rendered browser DOM (including an extent check that re-renders at a different root extent and asserts weighted redistribution, verifying sru-50's property without implementing host resizing) and through headless bounds assertions for the resolved portable tree, and SHALL drive overall appearance to a good state through an iteration loop in which page and app screenshots are captured and evaluated against the criteria, concluding in a single collaborative human review of the final appearance. **Appearance SHALL NOT be pinned as a regression gate**: no screenshot is committed as a baseline, and no pixel-diff comparison gates verification. Screenshots exist to inform the iteration and the final human review, not to fail a later build. The machine-checkable criteria above are the durable regression surface — they are structural and hold at any extent — while the aesthetic judgement is made once, together, and is not re-litigated by a test.

#### Scenario: Appearance is reviewed once, not pinned
- **WHEN** the rebuilt pages and apps reach a state satisfying every machine-checkable criterion
- **THEN** their appearance is reviewed collaboratively with a human and adjusted to a final agreed result
- **AND** no screenshot baseline is committed and no later build fails on a rendered-appearance difference

#### Scenario: Alignment is machine-checked
- **WHEN** the Playwright suite renders a rebuilt config page
- **THEN** it asserts equal rendered x-positions for controls declared in the same form-grid column
- **AND** asserts no horizontal overflow at the reference viewport

#### Scenario: Containment and overlap are machine-checked
- **WHEN** the structural assertions run over a rendered page
- **THEN** every node's rendered rectangle lies within its parent's containing rectangle on both axes — that rectangle being the parent's declared scroll-content rectangle where the parent is a `ScrollArea`, so a row below the visible viewport is contained rather than overflowing
- **AND** a `ScrollArea` whose content exceeds its visible extent clips that content to its viewport and leaves it scroll-reachable, which is asserted separately from containment
- **AND** no two sibling nodes' rectangles intersect, and no node overhangs its parent's rectangle, except where the producer declares a deliberate overlay or overhang — such as a visualizer underlay beneath an encoder
- **AND** every gap and padding between stacked siblings equals a value drawn from the library's shared spacing metrics

#### Scenario: Text fit is machine-checked
- **WHEN** the structural assertions run over a rendered page
- **THEN** every text element fits its allocated extent with no overflow or unintended truncation at the reference viewport
- **AND** a failing element is named, exposing a too-tight sizing reservation

#### Scenario: Contrast is checked against rendered styles
- **WHEN** the structural assertions run over a rendered page
- **THEN** each text element's computed colour is checked against its effective background for the stated minimum contrast
- **AND** a failing pair names the element and the computed colours

#### Scenario: The final appearance is agreed with a human, not pinned
- **WHEN** the visual iteration loop concludes for a page
- **THEN** its appearance is reviewed with a human and adjusted to a final agreed result
- **AND** no screenshot is committed as a baseline and no later render is compared against one

#### Scenario: Appearance drift is caught structurally or not at all
- **WHEN** a later change alters a page's rendering
- **THEN** it fails only if it breaks one of the named machine-checkable criteria, which hold at any extent
- **AND** a rendered-appearance difference that satisfies every criterion is not by itself a failure

### Requirement: sru-49 — Portable UI: backends are dumb renderers
WHEN a backend renders a portable tree, THE backend SHALL receive a fully resolved tree and paint it without performing layout, inferring coordinate space, supplying default sizes, or deciding appearance — every position, extent, and carried style it needs is on the node — and THE runtime UI layer SHALL remove from both backends both coordinate-space classifier families (the draw-geometry classifier and the node-bounds parent-local classifier), the auto-flow layout cursor with its per-kind default-size table and lowest-Draw starting-y scan, and the hardcoded per-variant colour constants as appearance policy; backend responsibilities SHALL be limited to toolkit realization, input translation into portable actions, scroll view transforms, the existing uniform surface scaling, interaction-state presentation derived from carried properties, and fitting text within the extent already resolved for it — a purely local decision requiring no knowledge of the surrounding tree, which each backend SHALL make for itself by shrinking, truncating, or clipping so that text never overflows its node's bounds.

#### Scenario: Rendered positions are derivable from the tree alone
- **WHEN** either backend renders any node of a resolved tree
- **THEN** the node's rendered position equals its own resolved bounds offset by the accumulated origins of its ancestor chain (plus scroll offset and uniform surface scale where applicable)
- **AND** a geometry property test in each backend's suite verifies this for every node of a representative tree

#### Scenario: A node without resolved bounds is not rescued
- **WHEN** a tree reaches a backend containing a node with no resolved bounds
- **THEN** the backend renders it at its parent's origin with zero-based extent rather than flowing or sizing it
- **AND** no backend contains a layout cursor or default-size table to fall back on

#### Scenario: Deleted policy stays deleted
- **WHEN** backend sources are inspected
- **THEN** they contain no draw-geometry coordinate classifier, no node-bounds parent-local classifier, no auto-flow cursor or per-kind default-size table, and no per-variant colour table deciding carried appearance
- **AND** the inspection runs as a committed grep-backed check, not a one-time review

#### Scenario: Fitting text does not feed layout back
- **WHEN** a backend's font renders a string wider than the extent resolved for it
- **THEN** the backend shrinks, truncates, or clips the text within the node's bounds
- **AND** the node's resolved bounds are unchanged, so no other node moves
- **AND** the backend reports no measurement back into layout

### Requirement: sru-50 — Portable UI: extent-driven layout
WHEN the component library resolves a tree, THE layout resolver SHALL derive every position and extent from the extent it is given rather than from any hardcoded surface size, so that rebuilding the same producer code against a different root extent redistributes weighted and intrinsic children correctly with no producer-side recomputation and no backend involvement; this change SHALL establish that property and SHALL NOT introduce host window resizing, minimum host-window, surface, or root-extent declarations, or any change to the existing uniform surface scaling, which remains exactly as it is today. This exclusion covers host and surface sizing only; it does not restrict sru-44's per-child minimum and maximum layout extents, which are ordinary layout inputs.

#### Scenario: A different root extent redistributes children
- **WHEN** the same producer code is resolved twice against root extents of different widths
- **THEN** the weighted children resolve to proportionally different extents in the two results
- **AND** fixed and intrinsic children keep their extents in both

#### Scenario: No hardcoded surface size in the resolver
- **WHEN** the layout resolver's sources are inspected
- **THEN** no resolution path depends on a compiled-in surface width or height
- **AND** every container resolves against the extent passed to it

#### Scenario: Resizing is not implemented here
- **WHEN** this change is complete
- **THEN** host window resize handling is unchanged
- **AND** the existing shrink-to-fit uniform surface scaling behaves as it did before

### Requirement: sru-51 — Portable UI: enforced layering
WHEN the portable UI stack is organized, THE runtime UI layer SHALL maintain explicit layers with bounded knowledge — the model (node/tree/draw/colour/text-style/action data and its coordinate contract, knowing nothing above it), the component library with layout resolver and metrics contract (knowing only the model; owning all layout and sizing policy), producers (config pages and apps, owning their own content and appearance choices; knowing model and library; JUCE-free; no wire or backend knowledge), the wire codec (model to bytes and back; no policy), and the backends (consuming resolved trees and translating input; never including the library's authoring or layout API; containing no layout, sizing, coordinate, or appearance policy) — and SHALL enforce these boundaries through JUCE-free compile tests for the model, library, and producer layers and a committed include-graph inspection for the codec and backend layers.

#### Scenario: Library and producers compile JUCE-free
- **WHEN** the JUCE-free test target builds the component library, config pages, and app surfaces
- **THEN** they compile without JUCE headers
- **AND** without any backend or wire-codec includes

#### Scenario: Backends do not include authoring machinery
- **WHEN** the committed include-graph inspection runs over backend sources
- **THEN** neither backend includes the component library's authoring or layout headers
- **AND** the inspection fails if a deleted policy symbol (layout cursor, default-size table, coordinate classifier, per-variant colour table) reappears

#### Scenario: Appearance ownership is single-layer
- **WHEN** a producer's appearance is changed
- **THEN** the edit happens in that producer's own sources
- **AND** no library, codec, or backend source changes

### Requirement: sru-52 — Portable UI: Draw node click actions
WHEN a portable `Draw` node carries a plain click action, THE runtime UI layer SHALL dispatch that action on a single click in every backend, independently of whether the node also carries a pointer-drag or double-click action, and the component library SHALL let a producer attach that click action when the node is constructed; a `Draw` node carrying no action SHALL remain non-interactive and SHALL NOT intercept pointer input.

#### Scenario: Click-only Draw node dispatches on single click
- **WHEN** a tree contains a `Draw` node whose only interaction is a plain click action, and the user single-clicks it
- **THEN** the JUCE backend dispatches that action exactly once
- **AND** the browser backend dispatches that action exactly once
- **AND** neither backend requires a double-click or a drag to reach it

#### Scenario: Click coexists with drag and double-click
- **WHEN** a `Draw` node carries a click action, a pointer-drag action, and a double-click action
- **THEN** a single click dispatches the click action alone
- **AND** a drag gesture past the drag threshold dispatches the pointer-drag action and no click action
- **AND** a double-click dispatches the same ordered sequence of actions, with the same number of click dispatches, that a `Button` node carrying the same click and double-click actions dispatches for a double-click in that same backend

#### Scenario: The double-click sequence is pinned exactly
- **WHEN** each backend's suite exercises a double-click on a `Draw` node carrying both a click and a double-click action
- **THEN** the test asserts the complete ordered list of dispatched actions and the exact count of each
- **AND** the same assertion is made for a `Button` node carrying the same actions, so any divergence between the two kinds fails

#### Scenario: Inert Draw nodes stay transparent to input
- **WHEN** a `Draw` node carries no click, drag, or double-click action
- **THEN** neither backend dispatches an action for pointer input over that node
- **AND** the node does not intercept pointer input from what is behind it

#### Scenario: Disabled Draw nodes do not dispatch
- **WHEN** a `Draw` node carrying a click action is disabled
- **THEN** clicking it dispatches no action in either backend

### Requirement: sru-53 — Portable UI: standard synth application layout
WHEN a synth application presents the conventional encoder-and-visualizer arrangement, THE component library SHALL provide one reusable standard layout component composing a title row, a visualizer column holding two stacked component slots anchored on the left, an encoder region to its right, and a widget bay spanning the bottom; each slot and region SHALL accept an arbitrary application-supplied component and THE layout SHALL prescribe nothing about their contents — no grid, no cell count, no visualizer or encoder semantics — beyond position and extent; the layout's proportions SHALL be derived from the arrangement Braid 4 and Mini App already share; every semantic control an application presents outside its visualizer slots and encoder region SHALL be supplied to the widget bay, so that both Braid 4 and Mini App populate the bay and no application-supplied control is left without a declared region; and Braid 4 and Mini App SHALL both be built on the component rather than on their own hand-computed layouts, resolving through the ordinary layout resolver so its regions redistribute under sru-50.

#### Scenario: Slots accept arbitrary components
- **WHEN** one application supplies a grid of scope cells to a visualizer slot and another supplies a single waveform component to the same slot
- **THEN** both resolve correctly within the slot's extent
- **AND** the standard layout contains no grid, cell-count, or content-kind logic of its own

#### Scenario: Both applications share the standard layout
- **WHEN** Braid 4 and Mini App build their main surfaces
- **THEN** both compose the same standard layout component
- **AND** neither computes its own region positions
- **AND** `Braid4PageLayout`, `Braid4EncoderGridLayout`, `MiniAppPageLayout`, and `EncoderGridLayout` no longer own surface-level position arithmetic

#### Scenario: Application content keeps its existing behavior
- **WHEN** Braid 4 supplies its VCO scope cells to the upper slot, its LFO scope cells to the lower slot, its sixteen encoders to the encoder region, and its four bank buttons, scene buttons, and scene-blend slider to the widget bay
- **THEN** every scope remains individually addressable and independently bounded as sru-21 requires
- **AND** every one of those semantic controls dispatches the same action it dispatches today

#### Scenario: Every application control has a region
- **WHEN** Mini App builds its main surface with its waveform components, sixteen encoders, two bank buttons, four toggles, scene buttons, Start and Stop buttons, and two sliders
- **THEN** every one of those controls resolves inside a declared standard-layout region
- **AND** no control is positioned outside the layout or overlapping another control

#### Scenario: An application may leave the widget bay empty
- **WHEN** an application supplies no widget-bay content
- **THEN** the bay occupies no space and the regions above it take its extent
- **AND** no placeholder or empty chrome is rendered

#### Scenario: Standard layout resolves like any component
- **WHEN** the standard layout is resolved against different root extents
- **THEN** its regions redistribute through the ordinary resolver with no layout logic of its own outside container declarations

### Requirement: sru-54 — Portable UI: every container absorbs its overflow or fails loudly
WHEN the component library resolves a container whose in-flow children cannot fit its extent along the stacking axis, THE layout resolver SHALL treat that as an error rather than an accepted outcome: it SHALL fail with a diagnostic naming the container, the axis, the extent available, the extent required, and the identity of the first child that does not fit, and it SHALL NOT silently clip, truncate, or drop the overflowing children. A container SHALL be able to absorb its own overflow by declaring either a `ScrollArea`, whose children lay out in scroll-content space and whose resolved content extent the resolver publishes so the tail stays reachable, or at least one weighted in-flow child that takes up the remaining space; a container that declares neither and whose intrinsic content exceeds its extent is a producer defect, not a rendering outcome. THE component library SHALL NOT require producers to express sizes as pixel literals to satisfy this requirement — an item inside a scrolling region legitimately carries its own extent along the scroll axis, because a fraction of the container would be circular for a list whose length varies.

#### Scenario: A page that cannot fit its surface fails at build time
- **WHEN** a page's in-flow content exceeds the surface extent it is resolved against, and the page declares no scroll area and no weighted child to absorb the difference
- **THEN** resolution fails with a diagnostic naming the container, the axis, the available and required extents, and the first child that does not fit
- **AND** no tree is handed to a backend, so the failure cannot reach a user as invisible clipped content

#### Scenario: A scroll area absorbs a list longer than its viewport
- **WHEN** a list of arbitrary length is declared inside a `ScrollArea` whose viewport is smaller than the list
- **THEN** resolution succeeds, every item keeps its own extent, and the resolver publishes a scroll-content extent that contains the last item
- **AND** the same declaration resolves at a different viewport extent with no producer change

#### Scenario: A weighted child absorbs the remainder
- **WHEN** a container's other children are intrinsic and one child is weighted
- **THEN** the weighted child takes the remaining space and resolution succeeds at any container extent large enough for the intrinsic children

#### Scenario: The rebuilt config pages absorb their own content
- **WHEN** each rebuilt config page is resolved at the smallest surface extent any first-party app declares
- **THEN** every page resolves without error, with its variable-length region absorbing the difference rather than the page relying on the surface being tall enough

### Requirement: sru-55 — Portable UI: container background and border
WHEN a portable container is constructed, THE component library SHALL accept the container's own appearance at construction on the same footing as a semantic control's — a background fill colour and a border described by colour, width, and corner radius — for `Root`, `Row`, `Section`, and `ScrollArea`, carried on the node record as optional values with explicit wire presence, absent meaning the backend's plain default look; and both backends SHALL render the carried fill and border, the fill covering the container's own area including its padding and the gaps between its children, which no child's colour can paint. This closes the gap where sru-45 already assigned `Root`, `Row`, `Section`, and `ScrollArea` the meaning "container or surface background fill" and both backends already rendered it, while no container builder accepted a colour, so no producer could ask for one.

#### Scenario: A panel is a panel
- **WHEN** a producer constructs a section carrying a fill colour and a border
- **THEN** both backends paint that fill across the section's whole area, including its padding and the gaps between its children, with the border drawn at the declared width, colour, and corner radius

#### Scenario: Container appearance needs no out-of-flow stand-in
- **WHEN** a page needs a filled, bordered, rounded panel behind a group of controls
- **THEN** it declares the appearance on the container itself
- **AND** it does not emit an out-of-flow `Draw` underlay to stand in for the container's own background

#### Scenario: Unstyled containers keep the default look
- **WHEN** a container carries neither fill nor border
- **THEN** both backends render their existing default appearance for that container kind

### Requirement: sru-56 — Portable UI: browser command-buffer serialization
WHEN a portable UI surface is rendered by the browser backend, THE runtime UI layer SHALL provide a JUCE-free serialization boundary that converts the current `NodeTree` into a browser-consumable command buffer containing stable node identities, hierarchy, bounds, semantic control records, actions, scroll extents, string-table entries, and draw-command records; the serialization boundary SHALL be testable without JUCE, DOM, Canvas, or application-specific browser code.

#### Scenario: Representative tree serializes without host APIs
- **WHEN** a JUCE-free test builds a representative runtime/app portable UI tree and serializes it for the browser backend
- **THEN** the resulting buffer contains records for semantic controls, scroll areas, actions, and draw commands
- **AND** the test compiles without JUCE, DOM, Canvas, or JavaScript API headers

#### Scenario: Stable IDs survive frame updates
- **WHEN** two consecutive UI frames contain the same logical slider, text field, or draw node with changed value or draw commands
- **THEN** the serialized records preserve the same node identity
- **AND** the browser backend can update the existing browser-side control rather than recreating it solely because its value changed

### Requirement: sru-57 — Portable UI: browser backend rendering and dispatch
WHEN the browser backend receives a portable UI command buffer, THE backend SHALL render semantic controls as browser controls or browser-owned equivalents, render draw-command nodes through a batched canvas-oriented renderer, preserve scroll viewport and content-extent semantics, and translate browser input events into portable `Action` dispatches without requiring application-specific DOM or HTML.

#### Scenario: Semantic controls render from node kind
- **WHEN** the buffer contains button, toggle, slider, combo box, text field, label, status text, row, section, or scroll-area records
- **THEN** the browser backend renders them according to their portable node kind and state
- **AND** it does not inspect the concrete application type to choose the control

#### Scenario: Draw nodes render in browser-owned batches
- **WHEN** the buffer contains a draw node with multiple draw-command records
- **THEN** the browser backend renders those records through a browser-owned canvas path after receiving the buffer
- **AND** C++ draw-command producers make no synchronous Canvas or DOM calls

#### Scenario: Scroll extents remain reachable
- **WHEN** a portable scroll area has content height greater than its visible bounds
- **THEN** the browser backend maps visible bounds and content extent so the bottom content remains reachable

#### Scenario: Browser input dispatches portable actions
- **WHEN** the user changes a browser-rendered control, drags a draw node, or double-clicks a row or draw node
- **THEN** the browser backend sends the corresponding portable action name and value back to the runtime
- **AND** application behavior is reached only through `Surface::DispatchAction`
- **AND** encoder/rotary drag actions preserve the existing replacement-delta semantics expected by portable controls, including replacement of the suffix after the final colon in action values
- **AND** double-click dispatch preserves portable push actions for rows and draw nodes that define them

### Requirement: sru-58 — Portable UI: no app-specific browser fallback
WHEN browser UI support is added for a synth application, THE runtime UI layer SHALL use the same portable tree, command-buffer serializer, and browser backend for all synth applications; it SHALL NOT provide hand-written browser HTML, app-specific DOM layout, or app-specific fallback controls for the miniapp or any other concrete app.

#### Scenario: Miniapp uses generic portable backend
- **WHEN** the miniapp is rendered in Chrome
- **THEN** every visible control and draw region comes from the miniapp's portable `Surface` tree through the generic browser backend
- **AND** no miniapp-specific HTML template or DOM construction path is loaded

#### Scenario: Unsupported portable node blocks generically
- **WHEN** the browser backend encounters a portable node kind or draw command it cannot render
- **THEN** it reports the unsupported generic portable feature
- **AND** it does not substitute a miniapp-specific workaround

### Requirement: sru-69 — Sidebar: a host declares its pages
WHERE a host declares which of the Audio, Controllers, Sync and File entries and the load readout its sidebar offers, THE runtime library SHALL show exactly the declared entries, in `sru-2`'s order, SHALL size the sidebar to the rows it shows, and SHALL route only to declared pages, so that a dispatched action for an undeclared page leaves the current page unchanged; a host that declares nothing SHALL get every entry, as `sru-2` describes.

#### Scenario: A plugin host shows Controllers and File
- **WHEN** a host declares Controllers and File
- **THEN** the sidebar shows those two entries and is two rows tall, each opens its page, and Back returns to the application
- **AND** the sidebar shows no Audio entry, no Sync entry and no load readout, and dispatching the Audio or Sync sidebar action leaves the application page showing
- Check: not yet delivered: NEW TestSidebarShowsOnlyTheDeclaredPages in tests/runtime_main_component_tests.cpp (frogg3rs task S5)

#### Scenario: A host that declares nothing is unchanged
- **WHEN** a host declares no page set
- **THEN** the sidebar shows Audio, Controllers, Sync, File and the load readout
- Check: `tests/runtime_main_component_tests.cpp`, `TestSidebarOpensEachPageAndBackRestoresApp` (each of the four entries opens its page) and `TestRefreshUpdatesRuntimePageModelsAndRollingDeadline` (the load readout is present)

