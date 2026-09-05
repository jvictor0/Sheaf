#pragma once

// MidiConfigBlocks.hpp — JUCE-free block presentation model for the
// Controllers page (midi-config-blocks change, task group 1).
//
// Everything here is pure/deterministic and operates on
// MidiControllerProfileConfig's persisted vectors. Nothing here mutates a
// live instrument or touches JSON directly -- callers (the view model, task
// group 2) own committing expansions through the existing edit path.
//
// Layout:
//  - SystemAddressSchema (D1): the single per-kind address-field table that
//    drives system-message row fields, column headers, and block address
//    forms. Lives here (not MidiConfigViewModel.hpp) because block address
//    forms (SystemBlock) are defined in this header and must derive from the
//    exact same table the row/header code uses -- see D1's "schema drives
//    every surface" requirement (sru-8).
//  - NormalizeMidiProfileConfig + SystemMessageSortKey (D2): canonical
//    ordering, applied by the view model on every commit (task group 2) and
//    defensively by ReconstructSystemBlocks here, so externally-authored
//    unsorted JSON still reconstructs correctly.
//  - EncoderBlock/AnalogBlock/SystemBlock + Expand*/Reconstruct* (D3/D4).

#include "synth/MidiController.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace synth {

// --- D1: per-kind system-message address schema ---------------------------

enum class SystemAddressField {
    AddressType,
    Channel,
    WrldBldrX,
    WrldBldrY,
    LaunchpadX,
    LaunchpadY,
    // Twister's sole editable address field: logical side button 0..5,
    // persisted as control->cc = 8 + button on the fixed channel 3 (the
    // channel itself is display-only, not part of this schema).
    Button,
    Cc,
};

// The ordered address-field tuple for a kind's system-message rows, column
// headers, and block address forms (D1). This is the single shared table --
// row `editableFields`, header strings (via FieldShortLabel in
// MidiConfigViewModel.hpp, which uses the same field names), and
// SystemBlock's address form all derive from this one list, so no kind ever
// shows a dead/inapplicable address column.
//
//   wrldbldr:  Channel, WrldBldrX, WrldBldrY
//   launchpad: LaunchpadX, LaunchpadY
//   twister:   Button
//   generic:   AddressType, Channel, Cc
std::vector<SystemAddressField> SystemAddressSchema(MidiProfileKind kind);

// --- D2: canonical ordering -------------------------------------------------

// A total, deterministic sort key over a system-message association's PRESS
// message plus its kind-specific address tuple, used to order
// `systemMessages` canonically (D2) and as the tie-break/grouping key
// ReconstructSystemBlocks partitions runs by. Two associations compare equal
// under this key's `<` iff they carry the same press message (per the
// semantic-argument table below) AND the same address -- callers needing a
// stable order for exact duplicates still break ties by original position
// (NormalizeMidiProfileConfig uses std::stable_sort for exactly this
// reason).
struct SystemMessageSortKey {
    // MessageIn::Type's declaration order (ParamIncDec=0 .. HoldDrill=24, 25 kinds).
    int typeOrder = 0;
    // Per-type semantic arguments (see the table in design.md D2):
    //   SceneSelect:                   arg1 = sceneIx
    //   SelectParamBank:                arg1 = slotIx,   arg2 = bankIx
    //   NextParamBank/PrevParamBank:     arg1 = slotIx
    //   ToggleGestureSelect/SetGestureSelect: arg1 = gestureIx
    //   ToggleReset/Random/RandomMod:   (no positional args)
    //   ParamIncDec/ParamSetAbsolute/ParamPush: arg1 = slotIx, arg2 = position
    //   SetGestureValue:                arg1 = gestureIx
    //   SetSceneBlend/Start/Continue/Stop/Clock: (no positional args)
    //   GridPress/GridRelease/GridPressureChange:
    //       arg1 = gridSlotIx, signedArg1 = gridX, signedArg2 = gridY,
    //       byteArg = velocity/pressure (zero for release)
    //   SelectGrid:                    arg1 = gridSlotIx, arg2 = gridIx
    std::size_t arg1 = 0;
    std::size_t arg2 = 0;
    int signedArg1 = 0;
    int signedArg2 = 0;
    std::uint8_t byteArg = 0;
    // Held-flag tuple for the modifier set/release variants (SetReset/
    // SetRandom/SetRandomMod carry hasBoolValue+boolValue on a ToggleX type;
    // SetGestureSelect always has a bool). Absent (hasBoolValue=false) sorts
    // before both held states so a plain toggle-type entry (were one ever
    // constructed) would order before its held variants.
    bool hasBoolValue = false;
    bool boolValue = false;
    // Kind's address tuple, flattened to a comparable form regardless of
    // kind (type, channel, x, y, cc) -- fields not meaningful for a given
    // association's kind stay at 0, which is fine since the tie-break only
    // discriminates within a single kind's own associations.
    MidiControlType addrType = MidiControlType::Cc;
    std::uint8_t addrChannel = 0;
    std::uint8_t addrX = 0;
    std::uint8_t addrY = 0;
    std::uint8_t addrCc = 0;

    friend bool operator<(const SystemMessageSortKey& a, const SystemMessageSortKey& b);
    friend bool operator==(const SystemMessageSortKey& a, const SystemMessageSortKey& b);
};

// Computes the sort key for one association's PRESS message + address,
// against the given kind (needed to know which address fields are
// meaningful/authoritative -- e.g. WrldBldr's control->channel vs.
// wrldBldrPosition's x/y).
SystemMessageSortKey ComputeSystemMessageSortKey(const MidiControllerSystemMessageAssociation& association,
                                                  MidiProfileKind kind);

// Sorts, in place: encoderInput->turns and ->pushes by (slotIx, position);
// systemMessages by SystemMessageSortKey (stable, so exact duplicates retain
// their original relative order); analogInput->gestures by gestureIx; and
// pressureInput->mappings by logical grid target then physical MIDI address.
// analogInput->sceneBlend (at most one) is untouched (nothing to order).
// Does not change the JSON persistence *shape* -- only vector element order.
// `kind` selects the address schema used for the systemMessages tie-break.
void NormalizeMidiProfileConfig(MidiControllerProfileConfig& config, MidiProfileKind kind);

// --- D3: block structs -------------------------------------------------

struct EncoderBlock {  // turn or push
    bool isPush = false;
    std::uint8_t channel = 0;
    std::uint8_t startCc = 0;  // [startCc, endCc) -- count = endCc - startCc
    std::uint8_t endCc = 0;
    std::size_t slotIx = 0;
    std::size_t startPosition = 0;
    MidiControlType controlType = MidiControlType::Cc;
};

struct AnalogBlock {
    std::uint8_t channel = 0;
    std::uint8_t startCc = 0;  // [startCc, endCc)
    std::uint8_t endCc = 0;
    std::size_t startGestureIx = 0;
};

enum class BlockableMessage { SceneSelect, BankSelect, GestureSelect };

struct SystemBlock {
    MidiProfileKind kind = MidiProfileKind::Generic;  // selects the address form
    BlockableMessage message = BlockableMessage::SceneSelect;
    std::size_t startArg = 0;    // scene/bank/gesture start index
    std::size_t bankSlotIx = 0;  // BankSelect only: the target slot
    bool rowMajor = true;        // expansion traversal order (2-D forms)
    bool outputFeedback = true;  // applied to every expanded cell
    std::uint8_t channel = 0;    // wrldbldr + generic forms
    std::uint8_t startCc = 0, endCc = 0;  // generic (1-D) form, [start, end)
    // 2-D forms, half-open/exclusive ends -- matching the 1-D forms'
    // [start, end) convention. X always traverses ascending: endX = maxX + 1
    // (width = endX - startX, valid iff endX > startX). Y traverses in a
    // direction d in {+1, -1} fixed by the run's second row: endY = lastY +
    // d (height = abs(endY - startY), valid iff endY != startY); a
    // single-row block uses d=+1 so endY = startY+1. `int` (not uint8_t, per
    // design.md D3's original sketch) because Launchpad grid coordinates
    // legitimately go negative (LaunchpadGridPosition::x/y are `int`; the
    // default profile factory's scene-select row sits at y=-1, its bank
    // column at x=8, matching LaunchpadShapeSupports' -1..8/-1..9 domain) --
    // a uint8_t field could not represent that real, already-supported
    // coordinate, and an exclusive end can legitimately reach 9 or -2. No
    // clamping to 0..7 for launchpad. WrldBldr's usage stays within its own
    // 0-7 grid regardless.
    int startX = 0, startY = 0;
    int endX = 0, endY = 0;
    // Launchpad form only: the controller variant every expanded cell's
    // launchpadPosition.controller is stamped with, and the shape
    // ExpandSystemBlock validates every cell's (x,y) against via
    // LaunchpadShapeSupports (finding 4 -- previously hardcoded to
    // LaunchpadX regardless of the slot's actual controller, so e.g. a
    // ProMk3-only edge coordinate like (x=8, y=-1) was wrongly rejected).
    // Irrelevant for other kinds.
    LaunchpadController launchpadController = LaunchpadController::LaunchpadX;
    MidiControlType controlType = MidiControlType::Cc;

    // Number of cells this block denotes (endCc - startCc for the generic
    // form; the exclusive-end rectangle's cell count for the 2-D forms).
    std::size_t CellCount() const;
};

// A user-visible grid button has one shared physical/logical coordinate:
// controller position (x,y) maps directly to grid target (x,y). Only the
// two controller kinds with two-dimensional address forms are supported.
// Grid mappings are intrinsically momentary; there is deliberately no
// toggle field.
struct GridButton {
    MidiProfileKind kind = MidiProfileKind::WrldBldr;
    std::uint8_t channel = 0;  // WRLD.Bldr only
    int x = 0;
    int y = 0;
    LaunchpadController launchpadController = LaunchpadController::LaunchpadX;
    std::size_t gridSlotIx = 0;
    bool outputFeedback = true;
};

// Signed half-open rectangle with x traversing ascending fastest and y
// traversing from startY toward (but excluding) endY. A descending y range
// therefore uses endY < startY, matching SystemBlock's existing address form.
struct GridBlock {
    MidiProfileKind kind = MidiProfileKind::WrldBldr;
    std::uint8_t channel = 0;  // WRLD.Bldr only
    int startX = 0;
    int startY = 0;
    int endX = 0;
    int endY = 0;
    LaunchpadController launchpadController = LaunchpadController::LaunchpadX;
    std::size_t gridSlotIx = 0;
    bool outputFeedback = true;
};

struct GridMappingExpansion {
    std::vector<MidiControllerSystemMessageAssociation> systemMessages;
    std::vector<PolyphonicPressureMapping> pressureMappings;
};

struct ReconstructedGridRow {
    bool isBlock = false;
    GridButton button;
    GridBlock block;
    // Indices refer to the caller's original vectors. They make exact
    // ownership explicit for view-model/session code and tests.
    std::vector<std::size_t> systemIndices;
    std::vector<std::size_t> pressureIndices;
};

struct GridMappingReconstruction {
    std::vector<ReconstructedGridRow> rows;
    // Canonically ordered system associations that were not exact grid pairs.
    std::vector<MidiControllerSystemMessageAssociation> remainingSystemMessages;
    // Unmatched pressure data stays in caller order and is never rewritten.
    std::vector<PolyphonicPressureMapping> orphanPressureMappings;
};

// --- D3: expansion (block -> exact individual configs) ---------------------
//
// All Expand* functions validate every cell before returning any result:
// on any validation failure (address out of shape, argument out of domain,
// duplicate address within the block, malformed block shape) they return
// false and leave `out` untouched (all-or-nothing, sru-10's "block commit is
// all-or-nothing" scenario). `reason` (if non-null) is set to a
// human-readable explanation.

bool ExpandEncoderBlock(const EncoderBlock& block, std::vector<EncoderMidiMapping>& out,
                        std::string* reason = nullptr);
bool ExpandAnalogBlock(const AnalogBlock& block, std::vector<AnalogMidiMapping>& out, std::string* reason = nullptr);
bool ExpandSystemBlock(const SystemBlock& block, std::vector<MidiControllerSystemMessageAssociation>& out,
                       std::string* reason = nullptr);
bool ExpandGridButton(const GridButton& button, GridMappingExpansion& out, std::string* reason = nullptr);
bool ExpandGridBlock(const GridBlock& block, GridMappingExpansion& out, std::string* reason = nullptr);
GridMappingReconstruction ReconstructGridMappings(
    const std::vector<MidiControllerSystemMessageAssociation>& systemMessages,
    const std::vector<PolyphonicPressureMapping>& pressureMappings, MidiProfileKind kind);

// --- D4: reconstruction (sorted config -> minimal block presentation) ------
//
// A ReconstructedRow identifies either a block (covering >= 2 consecutive
// input elements) or a single unblocked input element, in canonical
// traversal order, covering the input exactly (every input index appears in
// exactly one output row, block or individual).
struct ReconstructedEncoderRow {
    bool isBlock = false;
    EncoderBlock block;                 // valid iff isBlock
    std::vector<std::size_t> indices;   // input indices this row covers, in order
};

struct ReconstructedAnalogRow {
    bool isBlock = false;
    AnalogBlock block;
    std::vector<std::size_t> indices;
};

struct ReconstructedSystemRow {
    bool isBlock = false;
    SystemBlock block;
    std::vector<std::size_t> indices;
};

// Input MUST already be sorted by (slotIx, position) (NormalizeMidiProfileConfig
// or equivalent) -- reconstruction sorts defensively itself is NOT performed
// here for encoder/analog (the view model normalizes on every commit, D2);
// callers reconstructing from a possibly-unsorted external source should
// call NormalizeMidiProfileConfig first. ReconstructSystemBlocks (below)
// DOES sort defensively since sru-9's "unsorted input still reconstructs"
// scenario calls it out explicitly for system messages.
std::vector<ReconstructedEncoderRow> ReconstructEncoderBlocks(const std::vector<EncoderMidiMapping>& mappings,
                                                               bool isPush);
std::vector<ReconstructedAnalogRow> ReconstructAnalogBlocks(const std::vector<AnalogMidiMapping>& mappings);

// Sorts a defensive copy of `associations` per SystemMessageSortKey before
// partitioning into runs (sru-9 "unsorted input still reconstructs"), so an
// externally-authored out-of-order config reconstructs identically to its
// sorted equivalent. `indices` in the returned rows refer to positions in
// this SORTED view, not the caller's original `associations` order --
// callers that need to map back to their own storage should sort their
// storage the same way first (e.g. via NormalizeMidiProfileConfig).
std::vector<ReconstructedSystemRow> ReconstructSystemBlocks(
    const std::vector<MidiControllerSystemMessageAssociation>& associations, MidiProfileKind kind);

}  // namespace synth
