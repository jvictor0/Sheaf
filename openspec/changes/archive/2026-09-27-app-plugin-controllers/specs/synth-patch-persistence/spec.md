# Delta — `synth-patch-persistence`

A plugin host serializes its own state for the DAW through the patch manager
that also serves its File page, and restores which patch is current from that
state without loading the patch.

## ADDED Requirements

### Requirement: spp-14 — Patch output bus: the patch manager is the only serialize requester
WHEN a host needs a serialized snapshot of the live patch for itself, THE patch manager SHALL request it on the patch input bus with a request id from its own counter, SHALL write no file for it, and SHALL hand the snapshot document to the consumer the host installed inside the same response-processing call that pops it, before any further serialize request is pushed. THE patch manager SHALL keep at most one serialize request outstanding. WHEN a save, save-as or save-as-overwrite is requested while a snapshot is outstanding, THE patch manager SHALL hold it in one slot, SHALL answer it as pending rather than busy, and SHALL dispatch it in the call that consumes the snapshot, after the consumer returns; WHEN a snapshot is requested while a save is outstanding or held, THE patch manager SHALL refuse it as busy and push nothing. A new-patch or load command SHALL drop a held save exactly as it drops an outstanding one.

#### Scenario: A save asked for during a snapshot is held, then written
- **WHEN** a host snapshot is outstanding and a save with a current patch directory is requested
- **THEN** the save answers pending, not busy
- **AND** after the snapshot is serialized and consumed, the held save is dispatched, and after its own serialization the save reports written with a new version file in that directory
- Check: not yet delivered: NEW host_snapshot_holds_a_save_requested_while_it_is_outstanding in tests/engine_tests.cpp (frogg3rs task S2)

#### Scenario: The consumer receives the live values and no file is written
- **WHEN** a host snapshot is requested and served
- **THEN** the consumer is called once with a document whose parameter values equal the engine's
- **AND** no file under the patches root changes
- Check: not yet delivered: NEW host_snapshot_reaches_the_consumer_and_writes_no_file in tests/engine_tests.cpp (frogg3rs task S2)

#### Scenario: A snapshot is refused while a save is outstanding
- **WHEN** a save is outstanding and a host snapshot is requested
- **THEN** the request answers busy and the patch input bus holds no new message
- Check: not yet delivered: NEW host_snapshot_is_refused_while_a_save_is_outstanding in tests/engine_tests.cpp (frogg3rs task S2)

### Requirement: spp-15 — Patch lifecycle: a host names the current patch without loading it
WHEN a host names a patch by a path relative to the engine's patches root, THE engine SHALL record the directory it names as the current patch directory without pushing any patch message, only when that path is not absolute, has no `..` component, and resolves, after its trailing separator is removed and symbolic links are followed, to an existing directory strictly inside the patches root; OTHERWISE, and when the host names no patch, THE engine SHALL leave no current patch directory. THE engine SHALL report the current patch directory back as a path relative to the patches root.

#### Scenario: A restored current patch takes the next save
- **WHEN** a host names `p` and then a save is requested
- **THEN** the naming pushed no patch message, the current patch reads back as `p`, and the save writes its version in the patches root's `p` directory
- Check: not yet delivered: NEW naming_an_existing_patch_makes_it_current_without_a_message in tests/engine_tests.cpp (frogg3rs task S3)

#### Scenario: A name that does not resolve inside the root is refused
- **WHEN** a host names a missing directory, a regular file, the root itself, `.`, a path containing `..`, an absolute path, or a symbolic link inside the root that points outside it
- **THEN** there is no current patch directory, and a save reports that a save-as path is required
- Check: not yet delivered: NEW naming_a_patch_outside_the_root_leaves_no_current_patch in tests/engine_tests.cpp (frogg3rs task S3)
