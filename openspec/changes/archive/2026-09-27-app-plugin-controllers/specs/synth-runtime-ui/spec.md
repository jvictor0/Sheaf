# Delta — `synth-runtime-ui`

A plugin host shows only the runtime pages it can serve. `sru-69` adds to
`sru-2`'s sidebar.

## ADDED Requirements

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
