#pragma once

// MidiConfigViewModel.hpp — JUCE-free view model for the Controllers page.
//
// This header (and its .cpp) contain ALL tree/edit logic for the Controllers
// page; the JUCE page is a thin renderer over this model. Nothing here
// includes JUCE, so it is headlessly testable via
// tests/viewmodel_tests.cpp.
//
// Edits operate through the open presentation when one exists. A section
// opens by coalescing persisted truth into a stable, non-unique UI
// representation; row edits mutate that representation and rewrite the
// corresponding persisted section into `out`. Rebuild() updates the model's
// persisted snapshot but does not reshape already-open rows. Closing a
// section discards its presentation; reopening reconstructs the current
// minimal presentation from persisted truth.

#include "synth/MidiConfigBlocks.hpp"
#include "synth/MidiController.hpp"
#include "synth/MidiReconcile.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace synth {

struct MidiAppCatalog;
struct ControllerWizardDescriptor;

// Ring buffer tracking a rolling maximum over its last `capacity` writes.
// Message-thread only (no atomics) -- callers write once per UI timer tick
// and read back for display (e.g. the sidebar's deadline readout).
// Once `capacity` values have been written, each new Write() overwrites the
// oldest slot, so the max "forgets" spikes older than the window.
struct RollingMax {
    explicit RollingMax(std::size_t capacity) : values_(capacity < 1 ? 1 : capacity) {}

    void Write(float v) {
        values_[next_] = v;
        next_ = (next_ + 1) % values_.size();
        if (filled_ < values_.size()) {
            ++filled_;
        }
    }

    float Max() const {
        float result = 0.0f;
        bool any = false;
        for (std::size_t ix = 0; ix < filled_; ++ix) {
            if (!any || values_[ix] > result) {
                result = values_[ix];
                any = true;
            }
        }
        return result;
    }

private:
    std::vector<float> values_;
    std::size_t next_ = 0;
    std::size_t filled_ = 0;
};

// The deadline readout's rolling window: about one second of UI frames --
// long enough that even a single-frame spike stays on screen for many
// redraws after it happens, short enough that it is gone well before a
// reader could mistake a startup transient for the current load. UI frame
// rate is per application (`RuntimeConfig::uiFrameHz`), so the window is
// sized from that rate rather than a fixed frame count.
inline std::size_t DeadlineWindowCapacity(int uiFrameHz) {
    const int hz = uiFrameHz > 0 ? uiFrameHz : 30;
    return static_cast<std::size_t>(hz);
}

// One editable row rendered inside a section's mapping list.
//
// Presentation (midi-config-blocks change): a
// row is one of three kinds (see `kind` below) --
//   Individual  -- one config element (an encoder turn/push mapping, an
//                  analog gesture mapping, or a system-message association).
//   Block       -- a run of >=2 config elements presented/edited as a single
//                  block (EncoderBlock/AnalogBlock/SystemBlock, per
//                  MidiConfigBlocks.hpp); editableFields exposes the BLOCK's
//                  own fields (BlockStartCc/BlockEndCc/... below), not the
//                  individual cells'.
//   ConfigLevel -- encoder mode, turn step, or scene blend: exactly as
//                  before, never deletable, never part of a block.
// `deletable` is the renderer's single source of truth for whether to show a
// delete ("x") affordance -- true for Individual and Block rows, false for
// ConfigLevel: config-level rows are not deletable.
struct MidiMappingRowVM {
    enum class Field {
        Channel,
        Cc,
        SlotIx,
        Position,
        EncoderMode,
        TurnStep,
        MessageKind,
        MessageArg,
        LaunchpadX,
        LaunchpadY,
        WrldBldrX,
        WrldBldrY,
        GestureIx,
        SceneBlend,
        // Analog app-action row's target combo: index into
        // AnalogActionCatalog() (mirrors MessageKind's index-into-catalog
        // contract, but against the analog-only catalog rather than
        // MessageCatalog()).
        AppAction,
        // Twister side-button system rows only: the logical side
        // button 0..5, persisted as control->cc = 8 + button on the fixed
        // channel 3 (display-only, not independently editable). Kept
        // distinct from Field::Cc (which means "raw CC 0-127" everywhere
        // else) so its 0..5 validation domain and "Btn" label can't be
        // confused with a generic Cc editor.
        Button,
        // --- Block-row fields (kind == Block only) ---
        // A block row's editableFields is drawn from this subset, per its
        // form (EncoderBlock/AnalogBlock/SystemBlock 1-D generic/2-D
        // wrldbldr-launchpad):
        //   EncoderBlock:  Channel, BlockStartCc, BlockEndCc, SlotIx,
        //                  BlockStartPos
        //   AnalogBlock:   Channel, BlockStartCc, BlockEndCc, BlockStartArg
        //                  (start gesture index)
        //   SystemBlock generic (1-D):   BlockMessageType, Channel,
        //                  BlockStartCc, BlockEndCc, BlockStartArg,
        //                  BlockOutputFeedback (BlockBankSlotIx too when
        //                  BlockMessageType == BankSelect)
        //   SystemBlock wrldbldr/launchpad (2-D): BlockMessageType,
        //                  [Channel -- wrldbldr only], BlockStartX,
        //                  BlockStartY, BlockEndX, BlockEndY,
        //                  BlockStartArg, BlockRowMajor,
        //                  BlockOutputFeedback (+ BlockBankSlotIx for
        //                  BankSelect)
        // BlockStartCc/BlockEndCc reuse none of Cc's semantics (Cc means "one
        // mapping's raw cc"); kept distinct so a block's [start,end) pair
        // can't be confused with an individual row's single Cc field.
        BlockStartCc,
        BlockEndCc,
        BlockStartPos,     // EncoderBlock::startPosition
        BlockStartArg,     // AnalogBlock::startGestureIx or SystemBlock::startArg
        BlockBankSlotIx,   // SystemBlock::bankSlotIx (BankSelect message type only)
        BlockStartX,
        BlockStartY,
        BlockEndX,
        BlockEndY,
        BlockRowMajor,        // 0/1 toggle (SystemBlock::rowMajor)
        BlockOutputFeedback,  // 0/1 toggle (SystemBlock::outputFeedback)
        BlockMessageType,     // index into BlockableMessageCatalog() (3 entries)
        // CC/Note selector for encoder pushes and Generic system-message
        // controls. Distinct from both system message-kind fields above.
        AddressType,
        // Grid rows use signed physical/logical coordinates. A Grid Button
        // exposes the minima as its one cell; Grid Block adds exclusive
        // maxima. Grid mappings intentionally have no message/status/note,
        // pressure, feedback, or toggle editor.
        GridSlotIx,
        GridXMin,
        GridXMax,
        GridYMin,
        GridYMax,
    };

    // Groups rows into contiguous runs of the same on-screen schema, so the
    // renderer can insert a column-header row (and, for the non-tabular
    // Encoder groups / the scene-blend group, a divider + short caption)
    // whenever `group` changes from the previous row in a section's row
    // list. See ColumnHeadersForGroup()/FieldShortLabel() for the header
    // strings and FieldIsInteger() for how individual cells format.
    enum class RowGroup {
        EncoderTurn,
        EncoderPush,
        EncoderMode,
        EncoderStep,
        AnalogGesture,
        AnalogAppAction,
        AnalogSceneBlend,
        System,
        Grid,
    };

    enum class Kind { Individual, Block, ConfigLevel };

    std::string label;  // e.g. "turn ch0 cc12 -> slot 0 pos 3", or a block summary
    // Fields this row exposes for editing, in display order. ApplyMappingEdit
    // rejects a (Field) not present in a given row's editable set (the JUCE
    // page is expected to only render controls for fields present here, but
    // the view model itself is the source of truth for what's legal).
    std::vector<Field> editableFields;
    RowGroup group = RowGroup::System;
    Kind kind = Kind::Individual;
    bool deletable = false;  // CanDeleteRow()'s cached answer for this row; see that method's doc comment
};

enum class MidiConfigSection { Encoders, SystemMessages, Analogs };

enum class UISystemMessage {
    ParamIncDec,
    ParamSetAbsolute,
    ParamPush,
    ToggleReset,
    HoldReset,
    ToggleRandom,
    HoldRandom,
    ToggleRandomMod,
    HoldRandomMod,
    ToggleGestureSelect,
    HoldGestureSelect,
    SelectParamBank,
    Start,
    Continue,
    Stop,
    Clock,
    SetGestureValue,
    SceneSelect,
    SetSceneBlend,
    NextParamBank,
    PrevParamBank,
    AppAction,
    HoldDrill,
};

struct UISystemMessageChoice {
    std::string label;
    UISystemMessage message = UISystemMessage::Clock;
    std::string appAction;
    std::string appActionValue;
    std::size_t appActionIx = 0;
};

const std::vector<UISystemMessageChoice>& UISystemMessageCatalog();

// Finds the catalog entry for a message value, searching `catalog` (the
// static library list by default -- forms fixed to the library vocabulary,
// such as the MF Twister wizard, never pass a different one). For
// UISystemMessage::AppAction, `appAction`/`appActionValue` narrow the match
// to the one entry with that action and value (row identity for an
// app-action choice is the pair, never the bare kind); for every other kind
// they are ignored. Forms use this instead of duplicating the Controllers
// page's display labels.
const UISystemMessageChoice* FindUISystemMessageChoice(
    UISystemMessage message, const std::vector<UISystemMessageChoice>& catalog = UISystemMessageCatalog(),
    std::string_view appAction = {}, std::string_view appActionValue = {});

// Builds the press/release/feedback association used by the Controllers
// page for a catalog choice. `argument` follows the existing low-level
// mapping policy: Next/Previous Bank store it in slotIx, while Bank Select,
// Gesture Select, and Scene Select use their respective argument fields --
// ignored for AppAction, whose press carries `choice.appActionIx` instead
// and whose release is always absent (an app action has no hold state at
// this layer).
MidiControllerSystemMessageAssociation MakeUISystemMessageAssociation(
    const UISystemMessageChoice& choice, std::size_t argument = 0);

// Builds the offered message list for an app's catalog: an empty catalog
// (no libraryKinds, no actions) returns a copy of UISystemMessageCatalog()
// unchanged, so an app with no MidiCatalog() sees exactly today's list.
// Otherwise returns one library choice per catalog.libraryKinds entry
// (taken from UISystemMessageCatalog() by kind; HoldDrill has no static
// catalog entry, so it is produced here directly), followed by one
// app-action choice per catalog.actions entry (message = AppAction,
// appAction/appActionValue/appActionIx from that action, label from the
// action's own label).
std::vector<UISystemMessageChoice> MakeUISystemMessageChoices(const MidiAppCatalog& catalog);

// Builds the offered target list for an analog app-action row's combo: one
// choice per catalog.actions entry whose analogRange is set (a plain,
// non-analog action never appears here -- an analog control's app-action
// row can only target an action that takes a continuous value), label from
// the action's own label, message always UISystemMessage::AppAction,
// appAction/appActionValue/appActionIx from that action and its index in
// catalog.actions.
std::vector<UISystemMessageChoice> MakeAnalogAppActionChoices(const MidiAppCatalog& catalog);

// True for every field the renderer formats as a plain integer (no decimal
// places -- Channel, Cc, SlotIx, Position, GestureIx, LaunchpadX/Y,
// WrldBldrX/Y). False for TurnStep (a decimal float) and for the
// non-numeric-editor fields (EncoderMode, AddressType, MessageKind,
// BlockMessageType). MessageArg is a separate numeric field.
bool FieldIsInteger(MidiMappingRowVM::Field field);

// Display names for every EncoderMode, in declaration order (index 0 ==
// Signed7Bit, index 1 == DirectionOnly, index 2 == Absolute).
// ApplyMappingEdit and RowFieldValue use these catalog indices.
const std::vector<std::string>& EncoderModeCatalog();

// Display names for MidiControlType in declaration order (0 = Cc,
// 1 = Note). RowFieldValue()/ApplyMappingEdit use these catalog indices for
// Field::AddressType.
const std::vector<std::string>& ControlAddressTypeCatalog();

// Short column-header label for a single field ("Ch", "CC", "Slot", "Pos",
// "Gesture", "X", "Y", "Step", "Mode", "Message") -- the single
// source of truth the renderer uses to build a header row from a group of
// rows' shared editableFields, so header text can never drift from what a
// row actually renders.
const char* FieldShortLabel(MidiMappingRowVM::Field field);

// The fixed 3-entry catalog backing a system Block row's BlockMessageType
// field/combo -- one entry per BlockableMessage value (MidiConfigBlocks.hpp),
// in that enum's declaration order (0 = SceneSelect, 1 = BankSelect,
// 2 = GestureSelect). ApplyMappingEdit's BlockMessageType case and
// RowFieldValue's BlockMessageType case both treat their double as/return an
// index into this vector, matching the EncoderModeCatalog index convention
// used elsewhere on this page.
const std::vector<std::string>& BlockableMessageCatalog();

struct MidiControllerRowVM {
    std::string name;
    MidiProfileKind kind = MidiProfileKind::Generic;
    MidiControllerDisposition disposition = MidiControllerDisposition::Active;
    // Registry resolution gates only portable lifecycle affordances. The
    // persisted opaque id remains valid even when this is false.
    bool hasResolvedWizard = false;
    // Whether the slot's config is still exactly what its stored wizard id
    // generates today. Mapping edits, adds, and deletes no longer clear
    // wizardId, so this is how a row is told apart as still-pristine versus
    // diverged from the preset that created it.
    bool matchesWizardProfile = false;
    // The stored opaque wizard id, independent of registry resolution; which
    // preset (if any) created this row, kept through edits/deletes/adds so
    // matchesWizardProfile above and the Restore/Release/Configure controls
    // keep working on a row that has since diverged.
    std::optional<std::string> wizardId;
    bool hasCompleteEndpointPair = false;
    MidiEndpointStatus inputStatus = MidiEndpointStatus::Unconfigured;
    MidiEndpointStatus outputStatus = MidiEndpointStatus::Unconfigured;
    std::string inputDeviceLabel;   // present device name; stored ref + " (offline)"; or "(none)"
    std::string outputDeviceLabel;
    // Stored endpoint references, independent of connection status. A
    // Blacklisted record's endpoints stay deliberately Unconfigured, so
    // "show their stored endpoint labels" cannot be served by the
    // status-derived labels above, and endpoint pickers need the exact stored
    // identifier to stay unambiguous across duplicate same-name devices.
    MidiEndpointRef storedInput;
    MidiEndpointRef storedOutput;
    bool configExpanded = false;    // starts false
    std::vector<MidiConfigSection> sections;  // kind-filtered via KindSupport, each starts collapsed
};

// --- Open section presentation ---------------
//
// Internal to MidiConfigViewModel -- exposed at namespace (not class) scope,
// in a `detail` sub-namespace, purely so the .cpp's free helper functions
// can name these types without becoming member functions; nothing outside
// MidiConfigViewModel.cpp is expected to use `detail::` directly.
// When a section opens, the persisted profile is coalesced into this
// presentation. While it remains open, these rows are the authoritative
// displayed representation. Edits mutate the rows and then rewrite the
// persisted profile from them; Rebuild() does not rebuild these rows from
// sorted profile truth. Closing the section discards this non-unique
// representation, so reopening builds the current minimal representation
// again from persisted truth.
namespace detail {

struct EncoderModeRow {
    EncoderMode mode = EncoderMode::Signed7Bit;
};

struct EncoderStepRow {
    float turnStep = 1.0f;
};

struct AnalogSceneBlendRow {
    std::optional<MidiControlAddress> sceneBlend;
};

using PresentationRowData =
    std::variant<std::monostate, EncoderMidiMapping, AnalogMidiMapping, MidiControllerSystemMessageAssociation,
                 GridButton, EncoderModeRow, EncoderStepRow, AnalogSceneBlendRow, AnalogAppActionMapping>;

struct PresentationRow {
    MidiMappingRowVM::Kind kind = MidiMappingRowVM::Kind::Individual;
    MidiMappingRowVM::RowGroup group = MidiMappingRowVM::RowGroup::System;
    // Individual/config rows carry their current values here. Block rows use
    // `block`, because their UI representation is the block itself rather
    // than each expanded cell.
    PresentationRowData data;
    std::variant<std::monostate, EncoderBlock, AnalogBlock, SystemBlock, GridBlock> block;
};

struct SectionPresentation {
    std::vector<PresentationRow> rows;
    // Pressure entries not claimed by an exact visible grid pair. They are
    // never rendered or edited, and every System Messages flush appends them
    // verbatim before profile normalization/validation.
    std::vector<PolyphonicPressureMapping> hiddenPressureMappings;
};

}  // namespace detail

// JUCE-free view model driving the Controllers page. Rebuild() is a pure
// data transform from the persisted model (MidiInstrumentConfig) and the
// runtime's per-controller connection state (MidiConnectionState) into the
// row tree above. Expand/collapse state is kept keyed by controller NAME (in
// a std::map, so it is stable regardless of controller reordering) and
// survives across Rebuild() calls -- only a controller's first-ever
// appearance starts collapsed.
class MidiConfigViewModel {
public:
    void Rebuild(const MidiInstrumentConfig& instrument, const MidiConnectionState& connection);

    const std::vector<MidiControllerRowVM>& Controllers() const { return controllers_; }

    // The offered message list for the message-kind combo and its row-index
    // lookup. Defaults to a copy of UISystemMessageCatalog() so every
    // existing construction path behaves as today; a host with an app
    // catalog calls SetMessageCatalog(MakeUISystemMessageChoices(...)) once
    // at construction.
    void SetMessageCatalog(std::vector<UISystemMessageChoice> choices);
    const std::vector<UISystemMessageChoice>& MessageCatalog() const { return messageCatalog_; }

    // The offered target list for an analog app-action row's combo (Field::
    // AppAction) and its row-index lookup. Defaults to empty, matching an app
    // with no analog-range actions; a host with an app catalog calls
    // SetAnalogActionCatalog(MakeAnalogAppActionChoices(...)) once at
    // construction, beside SetMessageCatalog.
    void SetAnalogActionCatalog(std::vector<UISystemMessageChoice> choices);
    const std::vector<UISystemMessageChoice>& AnalogActionCatalog() const { return analogActionCatalog_; }

    // The add row's Preset combo options, and the registry every wizard
    // lookup resolves against: installing a preset, comparing a row's
    // config against the preset that created it, and restoring a diverged
    // row. Defaults to MakeControllerWizardRegistry(
    // MidiAppCatalog{}), i.e. the library's Twister-only registry, so every
    // existing construction path behaves as today; a host with an app
    // catalog calls SetLayouts(MakeControllerWizardRegistry(engine.MidiCatalog()))
    // once at construction, beside SetMessageCatalog.
    void SetLayouts(std::vector<ControllerWizardDescriptor> layouts);
    const std::vector<ControllerWizardDescriptor>& Layouts() const;

    void ToggleConfig(std::size_t controllerIx);
    void ToggleSection(std::size_t controllerIx, MidiConfigSection section);
    bool SectionExpanded(std::size_t controllerIx, MidiConfigSection section) const;

    std::vector<MidiMappingRowVM> SectionRows(std::size_t controllerIx, MidiConfigSection section) const;

    // Reads the current value of a single editable field on a single row,
    // identified the same way SectionRows()/ApplyMappingEdit() identify rows:
    // (controllerIx, section, rowIx). Implemented next to SectionRows() (both
    // walk the same row layout via ForEachEncoderRow/ForEachAnalogRow in the
    // .cpp) so the two can never drift apart -- this is the single source of
    // truth for "what does this row's field currently show," used both to
    // seed a JUCE editor's initial displayed text and to revert it after a
    // refused edit. Returns false (leaving `out` untouched) for an
    // out-of-range controllerIx/rowIx, a field not advertised in this row's
    // editableFields (see SectionRows()), or fields represented by a
    // dedicated catalog accessor rather than a numeric scalar (MessageKind,
    // BlockMessageType); otherwise writes the field's
    // current value into `out` and returns true. For Field::EncoderMode,
    // `out` is the current mode's index into EncoderModeCatalog() (not the
    // raw enum value), matching ApplyMappingEdit's index-based contract for
    // that field below.
    bool RowFieldValue(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx,
                       MidiMappingRowVM::Field field, double& out) const;

    int UISystemMessageIndex(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx) const;

    // Looks up a system Block row's current message type as an index into
    // BlockableMessageCatalog(), so a JUCE combo box can preselect the row's
    // current state -- the Field::BlockMessageType counterpart to
    // UISystemMessageIndex() above (RowFieldValue() deliberately refuses
    // BlockMessageType, per its own doc comment, since a message type is not
    // a single numeric value; this is the dedicated accessor callers use
    // instead). Returns
    // -1 for a non-SystemMessages section, an out-of-range (controllerIx,
    // rowIx), or a row that is not a system Block row (an Individual/
    // ConfigLevel row, or an Encoder/Analog Block row -- neither has a
    // BlockMessageType).
    int BlockMessageTypeIndex(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx) const;

    // Edits mutate the open presentation row first, then flush the entire
    // section from that presentation into a copy of the last Rebuild()
    // snapshot. On success, `out` holds the fully edited instrument for the
    // host to commit via engine.EditInstrument; the open presentation remains
    // in the edited shape and later Rebuild() calls do not reorder it. On
    // failure, `out` is untouched. If the field value itself is malformed or
    // not editable, the presentation row is rolled back and
    // `presentationChanged` (when supplied) is false. If the field value is
    // syntactically valid but the whole section is temporarily inconsistent
    // (for example a duplicate address while moving X/Y one coordinate at a
    // time), the presentation remains changed, `presentationChanged` is true,
    // and the caller can surface a warning while waiting for a later edit to
    // make the section committable.
    // `field` must be present in this row's SectionRows() editableFields.
    //
    // Numeric domains are enforced here: Channel 0-15, Cc 0-127,
    // SlotIx/Position/GestureIx and message arguments non-negative integers,
    // TurnStep a positive finite float, EncoderMode/AddressType/System
    // message-kind indexes valid catalog positions, LaunchpadX/Y valid for the row's
    // current Launchpad variant, and WrldBldrX/Y in the 0-7 grid.
    bool ApplyMappingEdit(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx,
                          MidiMappingRowVM::Field field, double value, MidiInstrumentConfig& out,
                          std::string* reason = nullptr, bool* presentationChanged = nullptr) const;

    bool AddController(std::string name, MidiProfileKind kind, MidiInstrumentConfig& out,
                       std::string* reason = nullptr) const;

    bool RenameController(std::size_t controllerIx, std::string name,
                          MidiInstrumentConfig& out, std::string* reason = nullptr) const;
    bool DeleteController(std::size_t controllerIx, MidiInstrumentConfig& out,
                          std::string* reason = nullptr) const;
    bool BlacklistController(std::size_t controllerIx, MidiInstrumentConfig& out,
                             std::string* reason = nullptr) const;
    bool RemoveFromBlacklist(std::size_t controllerIx, MidiInstrumentConfig& out,
                             std::string* reason = nullptr) const;
    bool RestoreController(std::size_t controllerIx, MidiInstrumentConfig& out,
                           std::string* reason = nullptr) const;

    bool SetEndpointRef(std::size_t controllerIx, bool output, MidiEndpointRef ref,
                        MidiInstrumentConfig& out) const;

    // --- Presentation: add/delete ------------------------------------
    //
    // Every mutating presentation method below shares ApplyMappingEdit's
    // contract: `out` is populated (and normalized via
    // NormalizeMidiProfileConfig) only on success; on failure `out` is
    // untouched and `reason` (if non-null) explains why. Successful calls
    // also leave the open presentation in the edited shape; the host commits
    // `out` and calls Rebuild() again, and that Rebuild() preserves already
    // open rows exactly as displayed. rowIx always means "presentation-row
    // index" (SectionRows()' index space), the same convention
    // ApplyMappingEdit/RowFieldValue already use.

    // True iff the row at (controllerIx, section, rowIx) may be deleted --
    // Individual and Block rows (MidiMappingRowVM::Kind), never ConfigLevel
    // (encoder mode/turn step/scene blend: "config-level rows are
    // not deletable"). Mirrors (and is the single source of truth behind)
    // each SectionRows() row's own cached `deletable` field -- exposed
    // separately so the renderer can query it without re-deriving the whole
    // row list, e.g. to decide whether to paint a delete button before the
    // row itself is constructed.
    bool CanDeleteRow(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx) const;

    // Deletes the row at (controllerIx, section, rowIx): for an Individual
    // row, removes that one config element (encoder mapping / analog gesture
    // mapping / system-message association); for a Block row, removes every
    // config element the block covers in the SAME commit ("a block
    // delete removes all its cells in one commit"). Refused (false, `out`
    // untouched) for a ConfigLevel row, an out-of-range (controllerIx,
    // rowIx), or a resulting section that fails validation.
    bool DeleteRow(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx, MidiInstrumentConfig& out,
                   std::string* reason = nullptr) const;

    // Appends one new Individual row to `group` with kind-appropriate
    // "next-free" defaults ("+": the lowest address/argument not
    // already used by that group -- see the .cpp's NextFree* helpers for the
    // exact per-group rule) and commits it. `group` must be one of the row
    // groups SectionRows() can produce for `section` on this controller's
    // kind (EncoderTurn/EncoderPush for Encoders; AnalogGesture/
    // AnalogAppAction for Analogs -- AnalogAppAction refused when
    // AnalogActionCatalog() is empty; System for SystemMessages --
    // EncoderMode/EncoderStep/AnalogSceneBlend are config-level, never
    // addable, and refused here) -- refused
    // otherwise. The section need not be expanded first; this call also
    // works against a section whose presentation has not yet been built
    // (SectionRows() lazily builds on first read either way, per this
    // class's presentation doc comment above ToggleSection()).
    bool AddSingle(std::size_t controllerIx, MidiConfigSection section, MidiMappingRowVM::RowGroup group,
                   MidiInstrumentConfig& out, std::string* reason = nullptr) const;

    // Appends one new Block row to `group`, seeded from the next free
    // address/argument range (same "next free" rule AddSingle uses, sized to
    // a small default run -- see the .cpp), committing its expansion in one
    // shot ("+B": "append a block, committed as its expansion").
    // Only legal where blocks apply (EncoderTurn/EncoderPush/AnalogGesture,
    // and System for a kind/message combination that supports blocking --
    // never MfTwister, and never AnalogAppAction, which is
    // individual-only); refused with a reason otherwise
    // (including "no room for a default block" if no >=2-wide free range
    // exists in the group's domain).
    bool AddBlock(std::size_t controllerIx, MidiConfigSection section, MidiMappingRowVM::RowGroup group,
                 MidiInstrumentConfig& out, std::string* reason = nullptr) const;

    // Whether AddSingle(controllerIx, section, group, ...) could possibly
    // succeed for this (section, group) pair -- i.e. the group/section
    // combination AddSingle's own dispatch recognizes at all (encoder turn/
    // push, analog gesture, or system), independent of any per-controller
    // runtime state (missing encoder/analog input, no free address, etc.,
    // which AddSingle still checks and can still refuse for).
    // Keeps the renderer thin: all decisions come from the view model, and
    // this is the single source of truth the renderer queries to
    // decide whether to paint a "+" affordance at all, so that decision can
    // never drift from AddSingle's actual dispatch the way a page-local
    // reimplementation could. `controllerIx` is accepted (and validated) for
    // symmetry with every other per-controller query on this class and to
    // leave room for a future controller-kind-dependent AddSingle rule, but
    // today's dispatch does not vary by kind -- only GroupSupportsBlocks()
    // does (see below). Out-of-range controllerIx returns false.
    bool GroupSupportsAdd(std::size_t controllerIx, MidiConfigSection section,
                          MidiMappingRowVM::RowGroup group) const;

    // Whether AddBlock(controllerIx, section, group, ...) could possibly
    // succeed for this (controllerIx, section, group) -- the AddBlock
    // counterpart to GroupSupportsAdd() above. False
    // for every group GroupSupportsAdd() itself refuses (a group with no
    // individual-row affordance has no block affordance either), and also
    // false for the System group on a MidiProfileKind::MfTwister controller
    // ("twister system messages never block" -- mirrors
    // AddBlock's own MfTwister refusal). Unlike GroupSupportsAdd(), this
    // DOES depend on controllerIx (to read the controller's kind), so an
    // out-of-range controllerIx returns false here for a real reason, not
    // just for API symmetry.
    bool GroupSupportsBlocks(std::size_t controllerIx, MidiConfigSection section,
                             MidiMappingRowVM::RowGroup group) const;

    // The addable row groups for (controllerIx, section), in RowGroup's
    // canonical declaration order (EncoderTurn < EncoderPush < EncoderMode <
    // EncoderStep < AnalogGesture < AnalogAppAction < AnalogSceneBlend <
    // System -- the same order SectionRows()/InsertionIndexForGroup use,
    // matching section display order): every group for which
    // GroupSupportsAdd(controllerIx, section, group) is true. Today that is
    // exactly {EncoderTurn, EncoderPush} for Encoders, {AnalogGesture} (plus
    // {AnalogAppAction} when AnalogActionCatalog() is non-empty) for
    // Analogs, and {System} for SystemMessages -- config-level groups
    // (EncoderMode/EncoderStep/AnalogSceneBlend) never appear here, same as
    // GroupSupportsAdd's own refusal for them.
    //
    // "empty group still offers add": SectionRows()' row-walk in
    // ControllersPage.hpp's SectionBody only emits a RowGroupHeader (and
    // therefore a "+"/"+B" affordance) for a group that has at least one
    // ROW -- so a group with zero existing mappings (or a whole empty
    // section, e.g. a freshly-added generic controller) has no row to hang
    // a header off of and was, before this method existed, a dead end. This
    // is the single source of truth the renderer walks (independent of
    // SectionRows()'s row list) to also emit a header-only affordance for
    // every addable group NOT already covered by an existing row's header,
    // so an empty group is never a dead end. Empty (not just per-group
    // false) for an out-of-range controllerIx, matching every other
    // per-controller query on this class.
    std::vector<MidiMappingRowVM::RowGroup> AddableGroups(std::size_t controllerIx, MidiConfigSection section) const;

    // The editableFields a FRESH Individual row in `group` would carry --
    // i.e. exactly what SectionRows() would show for a first added row in
    // that (section, group), so an empty-group's header-only affordance
    // (AddableGroups above) renders the right columns even though no row
    // exists yet to read them off of. Single source of truth shared with
    // BuildSectionRows()'s own per-group field tables (encoders: Channel,
    // Cc, SlotIx, Position; analog gesture: Channel, Cc, GestureIx; analog
    // app action: Channel, Cc, AppAction; system: SystemRowEditableFields(kind),
    // i.e. SystemAddressSchema(kind)'s fields plus MessageKind/MessageArg) so
    // the two can never drift apart.
    // `group` need not be addable (AddableGroups/GroupSupportsAdd) -- this
    // answers "what would this group's columns be," independent of whether
    // adding into it is currently legal; callers needing the addable subset
    // combine this with AddableGroups themselves (as SectionBody does).
    // Empty for an out-of-range controllerIx or a group/section combination
    // BuildSectionRows() never produces an Individual row for (EncoderMode/
    // EncoderStep/AnalogSceneBlend, or a group not appropriate to `section`).
    std::vector<MidiMappingRowVM::Field> GroupColumnFields(std::size_t controllerIx, MidiConfigSection section,
                                                           MidiMappingRowVM::RowGroup group) const;

    // Re-keys the per-row UI caches (expand state and section presentations)
    // from `from` to `to` after a controller is renamed. Both caches are
    // keyed by controller name because that is the only stable-enough handle
    // Rebuild() has across a rebuild; without this call a rename would look,
    // to those name-keyed maps, exactly like `from` disappearing and `to`
    // appearing fresh, so the row's expand/section state would be silently
    // discarded by Rebuild()'s own orphan sweeps. Calling this before the
    // next Rebuild() keeps that state attached to the renamed row instead.
    void NoteControllerRenamed(const std::string& from, const std::string& to);

private:
    struct ExpandState {
        bool configExpanded = false;
        std::map<MidiConfigSection, bool> sections;
    };

    ExpandState& StateFor(const std::string& name);
    const ExpandState* StateForConst(const std::string& name) const;

    // Keyed by (controller name, section) so presentation survives a
    // Rebuild() the same way ExpandState does (name-keyed, reorder-stable).
    using PresentationKey = std::pair<std::string, MidiConfigSection>;

    std::vector<MidiMappingRowVM> BuildSectionRows(std::size_t controllerIx, MidiConfigSection section) const;
    detail::SectionPresentation& PresentationFor(std::size_t controllerIx, MidiConfigSection section) const;
    void DiscardPresentation(const std::string& name, MidiConfigSection section);

    MidiInstrumentConfig instrument_;
    MidiConnectionState connection_;
    std::vector<MidiControllerRowVM> controllers_;
    std::vector<UISystemMessageChoice> messageCatalog_ = UISystemMessageCatalog();
    std::vector<UISystemMessageChoice> analogActionCatalog_;
    // Empty means "use the default registry"; Layouts() resolves that
    // default lazily so this header need not depend on ControllerWizard.hpp.
    std::vector<ControllerWizardDescriptor> layouts_;
    std::map<std::string, ExpandState> expandState_;
    // `mutable`: SectionRows() is const (matching its pre-existing contract
    // used throughout the renderer/tests) but must lazily build a missing
    // presentation entry on first read -- the lazy-build is an internal
    // caching detail, not an observable state change (the built presentation
    // is a pure function of instrument_ at read time), same justification as
    // any other const-correct memoization.
    mutable std::map<PresentationKey, detail::SectionPresentation> presentations_;
};

} // namespace synth
