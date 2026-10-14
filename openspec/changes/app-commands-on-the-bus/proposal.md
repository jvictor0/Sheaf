# Proposal — `app-commands-on-the-bus`

Paired with frogg3rs `frogg3rs-presses-on-the-bus`, whose task list is the
one task list for both trees (the S tasks). Base: b49b7720
(`shifted-encoder-turns`, PR #20). Supersedes the unpushed `app-o1-audit`.

## Why

An app's own commands (Frogg3rs's Randomize, Reset, page select, encoder
press) have no audio-thread delivery in this library. `MessageIn::AppAction`
is popped on the audio thread and forwarded straight back to the message
thread (`ParameterMessageOut::AppAction` → `Engine::MessageThreadTick` →
`PortableSurface().DispatchAction`), which is right for a MIDI controller
addressing a UI action and wrong for a command whose target is audio-thread
state. The app then builds its own bridge back: Frogg3rs used single-slot
atomics, which lose count and order inside one message tick, and
`app-o1-audit` answered with a second queue beside this library's, a shared
sequence number against the patch bus (`InputOrder`, `HasOrderedPresses`),
and a construct that holds an operation until depth storage covers it
(`RunIfDepthStorageCovers`). All of it duplicates ordering this library's
`MessageInBus` already provides.

Tempo already has the right shape: `SetTempoBpmNormalized` and
`TempoBpmIncDec` on both buses, applied to the master clock in
`MessageInBus::Apply` with the catalog's tempo range. This change gives app
commands the same shape.

Depth storage has two gaps an out-of-tree app hits. The low watermark that
triggers a storage request is a fixed multiple of the modulator count, sized
for one encoder press; an app whose one press can create a depth on every
parameter needs a larger number. And a patch whose depths exceed
available storage applies with depths missing and reports Ok, at startup
and while running; the caller that parses a patch, on the message thread,
already has the `ParameterManager` in hand before the load message even
exists, so it provisions the storage the patch's own depths need there,
before pushing.

## What changes

1. **`MessageIn::Type::AppCommand`** (appended after `SetTempoBpmNormalized`;
   every existing enumerator keeps its ordinal, as `SystemMessageSortKey`
   requires). It carries an app-defined command number and a float value,
   and this library never interprets either: the app pushes it from its
   surface and applies it in its own hook. `MessageIn::AppCommand(timestamp,
   command, value)`. `Engine::DrainMessageBus` hands it to
   `app_.ApplyAppCommand(command, value)` under
   `if constexpr (HasAppCommands<App>)` and applies it in the block that
   pops it, in FIFO order with the non-realtime messages on that bus; for
   an app without the hook, `MessageInBus::Apply` drops it, since only that
   app's own surface produces one and no controller profile can name it.
   Start, Continue, Stop and Clock stay realtime messages, lifted out of
   both buses and applied after both drain. No message is ever held at the
   head of a bus.
2. **The low watermark is a group setting.**
   `ParameterGroup::SetStorageLowWatermark(std::size_t)`; the existing
   `RequestParameterStorageBatchIfLow` reads it at its existing call sites
   (every local allocation on the audio thread), compared against
   `AvailableParameterSlots()` as today. Default unchanged
   (`numModulators * 2`).
3. **A patch never applies with a depth missing**, at startup or running.
   Storage is provisioned on the message thread, before the load message is
   pushed, rather than retried after a failed apply.
   `ParameterManager::ProvisionStorageForPatchValues` takes the parsed
   patch's `parameterValues` and, for every group `MissingDepthsForValuesJSON`
   reports missing depths for, adds one storage batch sized at that group's
   missing count plus its own watermark (`AddParameterStorageBatch`, called
   directly so the group's own pending low-water request cannot absorb it)
   when the group's available storage would not already cover count plus
   watermark; a group with enough room already is left untouched. This is
   the same call the tick's existing `ParameterStorageBatchNeeded` handling
   uses. Every site that parses a patch document and is about to push it as
   a `LoadFromJSON` message calls this immediately before the push:
   `PatchManager::LoadPatchVersion` (an on-disk Load, at startup or
   running), and a DAW host's `setStateInformation` restore. The message
   `ApplyPatchMessage` later applies is therefore never short of the
   storage its own depths need, and `Engine::ApplyPatchMessageAndNotifyApp`
   does no shortfall check of its own. The engine's existing
   arena-exhausted retry (`ApplyPendingPatchMessages` growing the arena
   inline at startup; `ProcessBlock` stashing `pendingPatchMessage_`/
   `arenaGrowPending_` for `MessageThreadTick` to grow while running) is
   unchanged and independent of this provisioning.
4. **`AppContext` exposes what the engine already publishes** for the
   runtime pages: `clockDiagnostics` (a pointer to
   `ClockDiagnosticsPublication`, whose `ClockDiagnostics` gains
   `transportState`, filled by `MasterClock::DiagnosticsSnapshot` and packed
   into the publication's metadata word) and `syncConfiguration` (the
   requested `SyncConfig`, from `Engine::SyncConfigurationSnapshot`), each
   stating its thread at the field. This is an added requirement; `sar-3` is
   not modified here, because `fold-controller-wizard-into-add-row` (#19)
   already modifies it.
5. **Carried from `app-o1-audit`**, with their spec deltas: storage batches
   appended while the audio thread reads; output processors resend a
   declined update; `Engine.hpp`'s false comments; the library devices an
   app offers; a gesture reference naming a gesture the app has; the
   Launchpad "Model" caption; stale text; `HasMessageThreadTick`; a row
   field edit without the section rebuild; the hygiene commit; and the
   `app-operator-runs` change, left open.
6. **Dropped from `app-o1-audit`**: the construct that held a depth-creating
   operation until storage covered it, and the ordering fix that ran every
   patch message relative to the app's own presses -- this change's
   `MessageIn::AppCommand` on the bus and the patch-storage retry replace
   both; the depth compute-skip (its removal is the WIP the branch stopped
   in, and the audio-equality check never ran); and every O(1)
   implementation gated on a frogg3rs measurement that was not a finding.

## Structural decisions

**A new type rather than an audio-thread interception of `AppAction`.**
`AppAction` is what a controller profile names and what the Controllers
page dispatches to the surface; its consumer is the message thread. A
command's consumer is the audio thread. Splitting by consumer keeps
`Origin` out of the decision, lets a MIDI press and a click join the same
route at the surface, and leaves every existing profile unchanged. The
command number is the app's rather than a catalog index because the
catalog indexes what a controller can be mapped to, and an app's encoder
press is deliberately not in it (`encoderPressAction` is forwarded as its
own kind); the app produces and consumes both ends, so the number is its
own.

**No hold.** A held head would keep the Start, Stop and Clock messages
pushed behind it (they are lifted into the realtime batch only when
popped), so a hold would delay transport by however long storage took.
Storage is kept ahead of the presses instead: the watermark and the Load
rule together keep available storage above one press whenever a press can
arrive.

**A watermark setting, not a covering construct.** The library already
requests storage when available slots fall below a watermark, at every
audio-thread allocation. An app whose one press can add more depths than a
Braid encoder press needs a larger number, not a new mechanism.

**A patch's storage is provisioned before the push, not retried after.**
Every producer of a `LoadFromJSON` message already parses the document, on
the message thread, before that message exists to push -- the same place
and time `MissingDepthsForValuesJSON` needs to run. Provisioning there once
replaces detecting a shortfall after the fact and retrying around it, and
needs no reason of its own beside the arena's: the JSON arena's own
exhaustion keeps its existing retry shape unchanged at both apply sites
(grow inline before audio; stash and retry while running), untouched by
this provisioning. The startup site matters most to the player: a relaunch
opens with the saved patch, and a launch batch that leaves less than the
watermark free would otherwise drop every saved patch that carries a
depth.

## Overlap with other active changes

Four open changes in the stack add `MessageIn::Type` enumerators
(`bank-addressed-absolute-write`: `ParamSetAbsoluteOnBank`;
`app-midi-catalog`: `HoldDrill`; `shift-and-file-export`: `Shift`;
`shifted-encoder-turns`: `SceneBlendIncDec`, `TempoBpmIncDec`,
`SetTempoBpmNormalized`), each appended after the last and each saying
earlier ordinals keep; this change's `AppCommand` is appended after the
last of them, so it is new-branch work and every prior text stays true. No
open change touches `DrainMessageBus`, `RequestParameterStorageBatchIfLow`,
`ApplyPendingPatchMessages` or `ClockDiagnostics`. An open change modifies
`sar-3`, which is why the context fields here are an added requirement
rather than a modified one. Two open changes add a `sar-33`, which this
change renumbers in the later one. `fork/app-midi-out` is the scrapped
MIDI-out record, stays on its old base and is not pushed. The executor
re-enumerates before the first amendment.

## Impact

- `projects/synth/include/synth/`: ParameterModulation.hpp, Engine.hpp,
  AppConcepts.hpp, AppContext.hpp, MasterClock.hpp, MidiConfigBlocks.hpp,
  MidiConfigViewModel.hpp, MidiController.hpp, PatchPersistence.hpp
- `projects/synth/src/`: ParameterModulation.cpp, MasterClock.cpp,
  MidiConfigBlocks.cpp, MidiConfigViewModel.cpp, MidiController.cpp,
  PatchPersistence.cpp
- `projects/synth/tests/` (blocks, parameter-modulation, engine and rig
  cases named in the task list)
- `openspec/` (this change; specs synth-app-runtime,
  synth-parameter-modulation, synth-patch-persistence; the carried deltas
  from `app-o1-audit`; `app-operator-runs`)

## Delivery

The fork's open PRs #9 to #20 are one linear stack against upstream main.
Each piece of this change lands in the PR whose diff introduced the concept
it extends, or in the new branch when its code depends on anything present
only higher in the stack or extends upstream code no PR introduced; the
executor decides each placement from the tree and reports it. Branches
above each amendment are rebased and force-pushed, so their PRs update.
The app command, the watermark setting, the patch storage retry, the
storage race and resend fixes, and the two change directories go up as
`app-commands-on-the-bus`, the next sequential PR on the new tip. `projects/synth test` and the miniapp
target (the runtime shell is not in the synth gate) at the new tip before
any push; the push itself waits for the frogg3rs suite to be green against
the local tip, then the frogg3rs pin moves to the pushed SHA.
