// ADDED FOR OPERATOR REVIEW SCREENSHOTS ONLY -- not part of `make test`.
// Wired as its own named target, apps/miniapp/Makefile's
// controllers_page_screenshot_harness, invoked directly rather than through
// `test`. juce::MessageManager::runDispatchLoopUntil, the usual synchronous,
// resumable pump for a popup capture, is compiled out entirely unless
// JUCE_MODAL_LOOPS_PERMITTED is defined for juce_events.o (see
// apps/miniapp/Makefile's comment above FROGGERS_APP_DIR for why that define
// is not added there). RenderState3's popup capture is therefore driven by
// PopupDropdownSequencer below, using only the unconditionally-compiled
// MessageManager::runDispatchLoop()/stopDispatchLoop() pair -- no define
// added anywhere, so juce_events.o and the shipped app it links into are
// unchanged.
// Renders the real ControllersPageSurface / MidiConfigViewModel through the
// same synth_juce::PortableComponent the JUCE simulation tests use, drives it
// with the app's own MIDI catalog (synth_froggers::FroggersMidiCatalog(), the
// same one JuceRuntimeMainServices wires into the live app), and writes each
// state to a PNG via juce::Component::createComponentSnapshot -- no on-screen
// window, no macOS screen-capture API, no hand-drawn text.
//
// Every screenshot's underlying state is produced by dispatching the same
// ui::Action values the real page's controls dispatch (kAddPresetDraft,
// kAddController, kToggleConfig, kConnectMessageCommit) against a live
// synth::runtime_ui::ControllersPageSurface, then reading back whatever the
// view model actually produced; nothing here writes a label string directly
// into a rendered node. State 3's popup capture is the one exception: opening
// and reading a JUCE ComboBox's popup menu (showPopup(), getItemText(),
// dismissAllActiveMenus()) needs the real widget, not an action dispatch,
// because the popup is its own top-level component outside the page's node
// tree.
//
// Output: each state writes to <out-dir>/<name>.png. <out-dir> defaults to a
// `screenshots` directory beside this binary and is created if missing;
// pass a directory as argv[1] to override it. The six states named in the
// change proposal's Delivery Gate are the ones the operator reviews; the
// remaining files (the `control_*` positive controls and the empty-field
// extra check) are this harness's own verification evidence, not part of
// that review set.

#include "ControllersPageHarness.hpp"
#include "PortableJuceBackend.hpp"

#include "synth/ControllerWizard.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "synth/MidiConfigViewModel.hpp"

#include "FroggersMidiCatalog.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

constexpr int kPageWidth = 1000;
constexpr int kPageHeight = 900;

// Mirrors synth_runtime::test::ControllersHarnessFixture, but wires the
// callbacks the real app wires (FroggersMidiCatalog-derived layouts/message
// catalogs) instead of leaving them empty, so the add row's Preset dropdown
// shows the app's own devices exactly as JuceRuntimeMainServices builds it
// (runtime/JuceRuntimeMainServices.hpp: callbacks.layouts =
// MakeControllerWizardRegistry(engine.MidiCatalog())).
struct AppFixture
{
    synth::MidiInstrumentConfig instrument;
    synth::MidiConnectionState connection;
    synth::MidiDeviceList devices;
    std::string status;
    int commits = 0;

    void SyncConnectionSize() { connection.controllers.resize(instrument.controllers.size()); }

    void AddRealisticDevices()
    {
        devices.inputs.push_back({"twister-in-id", "Midi Fighter Twister"});
        devices.inputs.push_back({"apc40-in-id", "APC40 mkII"});
        devices.inputs.push_back({"launchpad-in-id", "Launchpad X"});
        devices.outputs.push_back({"twister-out-id", "Midi Fighter Twister"});
        devices.outputs.push_back({"apc40-out-id", "APC40 mkII"});
        devices.outputs.push_back({"launchpad-out-id", "Launchpad X"});
    }

    synth::runtime_ui::ControllersPageSurface MakeSurface(
        const synth::MidiAppCatalog& catalog = synth_froggers::FroggersMidiCatalog())
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return instrument; };
        callbacks.connectionState = [this] { return connection; };
        callbacks.enumerateDevices = [this] { return devices; };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig out) {
            instrument = std::move(out);
            SyncConnectionSize();
            ++commits;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [] { return true; };
        callbacks.setStatus = [this](std::string text) { status = std::move(text); };

        callbacks.messageCatalog = synth::MakeUISystemMessageChoices(catalog);
        callbacks.analogActionCatalog = synth::MakeAnalogAppActionChoices(catalog);
        callbacks.layouts = synth::MakeControllerWizardRegistry(catalog);
        return synth::runtime_ui::ControllersPageSurface(std::move(callbacks));
    }
};

void WritePng(const juce::Image& image, const std::string& path)
{
    juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    Require(stream.openedOk(), "could not open output stream for " + path);
    juce::PNGImageFormat png;
    Require(png.writeImageToStream(image, stream), "PNG write failed for " + path);
    std::cout << "wrote " << path << " (" << image.getWidth() << "x" << image.getHeight() << ")\n";
}

void RenderSurface(synth::runtime_ui::ControllersPageSurface& surface, const std::string& path,
                   int height = kPageHeight)
{
    surface.SetContentBounds({0.0f, 0.0f, static_cast<float>(kPageWidth), static_cast<float>(height)});
    surface.MarkDirty();
    surface.RefreshOnTick();
    synth_juce::PortableComponent renderer(surface);
    renderer.setSize(kPageWidth, height);
    renderer.RefreshFromSurface();
    juce::Image image = renderer.createComponentSnapshot(
        renderer.getLocalBounds(), true, 1.0f, juce::SoftwareImageType{});
    WritePng(image, path);
}

// Set once at the top of main() from argv/the executable's own location and
// read by every Render* function below via ShotPath. Not a CLI-parsing
// abstraction -- one process-lifetime output directory is all this harness
// ever needs.
std::string g_shotDir;

// Defaults to a `screenshots` directory beside the built binary (created if
// missing), so the harness works from a fresh checkout on any machine and
// does not depend on any particular operator's temporary directory still
// existing. Pass a directory as argv[1] to write somewhere else instead.
std::string DefaultShotDir(const char* argv0)
{
    namespace fs = std::filesystem;
    const fs::path exePath = fs::absolute(fs::path(argv0));
    return (exePath.parent_path() / "screenshots").string();
}

std::string ShotPath(const std::string& name)
{
    return g_shotDir + "/" + name;
}

// ---------------------------------------------------------------------------
// State 1: an active row whose wizard id resolves, showing the preset's
// device name on line one. Reached by dispatching the real add-row flow:
// select a non-Custom preset (the app's own MIDI Fighter Twister default),
// then Add. InstallDescriptorProfile stamps the new slot's wizardId, so
// ControllerDeviceLabel resolves it against Layouts() and renders the
// descriptor's display name -- never a hand-written string.
// ---------------------------------------------------------------------------
void RenderState1_ResolvedWizardDeviceLabel(bool bust)
{
    AppFixture fixture;
    fixture.AddRealisticDevices();
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();

    const synth::MidiAppCatalog catalog = synth_froggers::FroggersMidiCatalog();
    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);
    const std::string twisterId = catalog.deviceDefaults.front().id;
    Require(catalog.deviceDefaults.front().displayName == "MIDI Fighter Twister",
            "catalog's first device default is the Twister (positive-control assumption)");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddPresetDraft, twisterId));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(fixture.instrument.controllers.size() == 1, "state 1: one controller added");
    Require(fixture.instrument.controllers[0].wizardId.has_value() &&
                *fixture.instrument.controllers[0].wizardId == twisterId,
            "state 1: added row's wizardId is the Twister descriptor id");

    // Positive control: BUST the claim by installing a wizardId a real
    // descriptor cannot resolve (post-commit, bypassing the page), and
    // confirm the rendered label falls back to the stored-endpoint text --
    // i.e. the SAME renderer draws something different when the state it
    // claims to show is different. This is the deliberate change; see the
    // report for what moved and what would have moved if the renderer were
    // dead (nothing -- the two PNGs would be byte-identical).
    if (bust)
    {
        synth::MidiInstrumentConfig busted = fixture.instrument;
        busted.controllers[0].wizardId = "not-a-real-wizard-id";
        fixture.instrument = busted;
    }

    RenderSurface(surface, ShotPath(bust ? "control_state1_busted_unresolved_wizard_id.png"
                                         : "state1_active_row_resolved_wizard_device_name.png"));
}

// ---------------------------------------------------------------------------
// State 2: an active row with no resolved wizard id, showing its bound MIDI
// input as the device label. MakeHarnessWrldBldrSlot's own slot (below) never
// sets wizardId -- exactly the shape the gate names -- so this is the
// fixture's default first controller, unmodified.
// ---------------------------------------------------------------------------
void RenderState2_UnresolvedWizardBoundInputLabel()
{
    synth_runtime::test::ControllersHarnessFixture fixture;
    Require(!fixture.state.instrument.controllers[0].wizardId.has_value(),
            "state 2 precondition: fixture's first controller carries no wizardId");
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    RenderSurface(surface, ShotPath("state2_active_row_unresolved_wizard_bound_input_label.png"));
}

// ---------------------------------------------------------------------------
// State 3: the add row's Preset dropdown open, listing the app's own
// devices, one library device per uncovered kind, and one Custom entry.
// Reached by actually opening the real juce::ComboBox behind NodeIds::
// kAddPreset with ComboBox::showPopup() -- never a drawn list. JUCE's popup
// is its own top-level component; it is captured with the same
// createComponentSnapshot the rest of this file uses (never a screen-capture
// API), then composited onto the page snapshot at the popup's real relative
// screen position, exactly where it draws on screen.
//
// Getting there needs JUCE to pump its native event loop while this process
// waits for the popup's peer to appear. The synchronous, resumable pump for
// that is juce::MessageManager::runDispatchLoopUntil, but its declaration
// (juce_MessageManager.h) is compiled out entirely unless
// JUCE_MODAL_LOOPS_PERMITTED is defined for the translation unit that
// compiles juce_events.o -- the shared object every JUCE target here links,
// including the shipped app. Defining it there was rejected in
// apps/miniapp/Makefile's own comment as a worse structure than accepting
// this harness needs a different mechanism, so this file does not use
// runDispatchLoopUntil at all.
//
// What IS compiled unconditionally is the blocking pair
// MessageManager::runDispatchLoop() / stopDispatchLoop() (juce_events.o
// already has both -- verified by linking against it below). The catch:
// stopDispatchLoop() latches MessageManager's own quit flag permanently, so
// runDispatchLoop() can only be entered once per process; a second call
// returns immediately without pumping anything. Both popup captures this
// state needs (the clean render and the busted positive control) are
// therefore driven from inside ONE such entry, as steps of the Timer-based
// state machine below, and stopDispatchLoop() is called exactly once, after
// both PNGs are written.
// ---------------------------------------------------------------------------
class PopupDropdownSequencer : public juce::Timer
{
public:
    void Run(std::vector<bool> bustFlags)
    {
        jobs_ = std::move(bustFlags);
        StartJob();
        startTimer(5);
    }

    const std::string& Error() const { return error_; }

private:
    std::vector<bool> jobs_;
    std::size_t jobIndex_ = 0;
    int phase_ = 0;
    int ticks_ = 0;
    std::string error_;
    std::unique_ptr<AppFixture> fixture_;
    std::unique_ptr<synth::runtime_ui::ControllersPageSurface> surface_;
    std::unique_ptr<synth_juce::PortableComponent> renderer_;
    synth::MidiAppCatalog catalog_;
    juce::ComboBox* combo_ = nullptr;
    juce::Image page_;

    void StartJob()
    {
        const bool bust = jobs_[jobIndex_];
        fixture_ = std::make_unique<AppFixture>();
        fixture_->AddRealisticDevices();

        // Positive control: BUST the claim by constructing the surface's own
        // layouts from a catalog with one device default removed (Ableton
        // APC40) instead of the real one, and confirm the rendered popup's
        // item count drops by one and no longer lists it. This is the
        // deliberate change; see the report for what moved and what would
        // have moved if the renderer were dead (nothing -- both popups would
        // show all 8 entries).
        catalog_ = synth_froggers::FroggersMidiCatalog();
        if (bust)
        {
            catalog_.deviceDefaults.erase(
                std::remove_if(catalog_.deviceDefaults.begin(), catalog_.deviceDefaults.end(),
                               [](const synth::MidiAppDeviceDefault& device) {
                                   return device.id == "froggers.apc40.ableton";
                               }),
                catalog_.deviceDefaults.end());
        }

        surface_ = std::make_unique<synth::runtime_ui::ControllersPageSurface>(fixture_->MakeSurface(catalog_));
        surface_->SetContentBounds({0.0f, 0.0f, static_cast<float>(kPageWidth), static_cast<float>(kPageHeight)});
        surface_->MarkDirty();
        surface_->RefreshOnTick();

        renderer_ = std::make_unique<synth_juce::PortableComponent>(*surface_);
        renderer_->setSize(kPageWidth, kPageHeight);
        renderer_->RefreshFromSurface();

        // A peer is required for ComboBox::showPopup() to resolve a screen
        // position for its menu. Positioned far off any physical display so
        // nothing is ever shown to a user or captured from the display --
        // createComponentSnapshot renders through Graphics, not through
        // reading display pixels, so this placement is cosmetic insurance
        // only.
        renderer_->setTopLeftPosition(-8000, -8000);
        renderer_->addToDesktop(0);
        renderer_->setVisible(true);

        phase_ = 0;
        ticks_ = 0;
    }

    void Fail(const std::string& label)
    {
        error_ = label;
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }

    void timerCallback() override
    {
        try
        {
            TimerCallbackImpl();
        }
        catch (const std::exception& e)
        {
            Fail(e.what());
        }
    }

    void TimerCallbackImpl()
    {
        const bool bust = jobs_[jobIndex_];

        if (phase_ == 0)
        {
            // Let the peer settle (mirrors the original's 10 x 20ms wait).
            if (++ticks_ < 20)
            {
                return;
            }

            combo_ = dynamic_cast<juce::ComboBox*>(renderer_->FindByNodeId(synth::runtime_ui::NodeIds::kAddPreset));
            Require(combo_ != nullptr, "state 3: add row Preset combo is rendered as a ComboBox");

            const std::vector<synth::ControllerWizardDescriptor> registry =
                synth::MakeControllerWizardRegistry(catalog_);
            const std::size_t uncoveredLibraryKinds = std::count_if(
                catalog_.libraryDeviceKinds.begin(), catalog_.libraryDeviceKinds.end(),
                [this](synth::MidiProfileKind kind) {
                    return std::none_of(catalog_.deviceDefaults.begin(), catalog_.deviceDefaults.end(),
                                        [kind](const synth::MidiAppDeviceDefault& deviceDefault) {
                                            return deviceDefault.kind == kind;
                                        });
                });
            Require(registry.size() == catalog_.deviceDefaults.size() + uncoveredLibraryKinds,
                    "state 3 positive control precondition: registry is this render's own catalog devices plus "
                    "one library device per kind in catalog_.libraryDeviceKinds no device default covers -- "
                    "proves the fixture reaches the state it claims, busted or not");
            Require(combo_->getNumItems() == static_cast<int>(registry.size()) + 1,
                    "state 3: rendered combo item count is registry size + one Custom entry");
            if (bust)
            {
                Require(catalog_.deviceDefaults.size() == 5,
                        "positive control precondition: busted catalog has one fewer device default (5, not 6)");
                for (int ix = 0; ix < combo_->getNumItems(); ++ix)
                {
                    Require(combo_->getItemText(ix) != juce::String("Akai APC40 mkII (Ableton)"),
                            "positive control: busted catalog's popup no longer lists the removed device");
                }
            }

            page_ = renderer_->createComponentSnapshot(renderer_->getLocalBounds(), true, 1.0f,
                                                        juce::SoftwareImageType{});
            combo_->showPopup();
            phase_ = 1;
            ticks_ = 0;
            return;
        }

        if (phase_ == 1)
        {
            const int n = juce::Desktop::getInstance().getNumComponents();
            for (int c = 0; c < n; ++c)
            {
                juce::Component* comp = juce::Desktop::getInstance().getComponent(c);
                if (comp != nullptr && comp != renderer_.get() && comp->getWidth() > 0 && comp->getHeight() > 0)
                {
                    juce::Image popupImage = comp->createComponentSnapshot(comp->getLocalBounds());
                    Require(popupImage.getWidth() > 0 && popupImage.getHeight() > 0,
                            "state 3: popup snapshot is non-empty");

                    // JUCE's popup positioning clamps to the nearest real
                    // on-screen display, which has nothing to do with this
                    // component's own (deliberately off-screen) peer
                    // position, so screen coordinates cannot place the popup
                    // on the page image. The combo's own bounds within the
                    // page are known and stable regardless of screen
                    // position, and a combo's popup always opens flush
                    // against its lower-left corner, so that is where the
                    // captured popup image is composited.
                    const juce::Rectangle<int> comboBoundsInPage =
                        renderer_->getLocalArea(combo_, combo_->getLocalBounds());
                    const int offsetX = comboBoundsInPage.getX();
                    const int offsetY = comboBoundsInPage.getBottom();
                    {
                        juce::Graphics g(page_);
                        g.drawImageAt(popupImage, offsetX, offsetY);
                        g.setColour(juce::Colours::white);
                        g.drawRect(juce::Rectangle<int>(offsetX, offsetY, popupImage.getWidth(), popupImage.getHeight()),
                                   1);
                    }
                    juce::PopupMenu::dismissAllActiveMenus();
                    phase_ = 2;
                    ticks_ = 0;
                    return;
                }
            }
            Require(++ticks_ < 80, "state 3: the Preset combo's popup menu component appeared and was captured");
            return;
        }

        // phase_ == 2: let the dismissal settle, then finish this job.
        if (++ticks_ < 20)
        {
            return;
        }

        renderer_->removeFromDesktop();
        WritePng(page_, ShotPath(bust ? "control_state3_busted_missing_device_default.png"
                                      : "state3_add_row_preset_dropdown_open.png"));

        renderer_.reset();
        surface_.reset();
        fixture_.reset();

        ++jobIndex_;
        if (jobIndex_ >= jobs_.size())
        {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        StartJob();
    }
};

// ---------------------------------------------------------------------------
// State 4: a row's expanded editor showing its Connect messages list with an
// existing message, an Add button, and a delete button. Reached by adding
// the app's own Ableton APC40 default (its real device default ships one
// connect-time SysEx message, config.openSysEx, in FroggersMidiCatalog.hpp);
// the row's editor is already open, since a row Add installs opens on its
// own.
// ---------------------------------------------------------------------------
std::size_t AddAbletonApc40(AppFixture& fixture, synth::runtime_ui::ControllersPageSurface& surface)
{
    const synth::MidiAppCatalog catalog = synth_froggers::FroggersMidiCatalog();
    const auto found =
        std::find_if(catalog.deviceDefaults.begin(), catalog.deviceDefaults.end(),
                     [](const synth::MidiAppDeviceDefault& device) { return device.id == "froggers.apc40.ableton"; });
    Require(found != catalog.deviceDefaults.end(), "catalog carries the Ableton APC40 default");
    Require(!found->config.openSysEx.empty(),
            "positive-control precondition: the Ableton APC40 default ships a connect message");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddPresetDraft, found->id));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(!fixture.instrument.controllers.empty(), "Ableton APC40 row was added");
    const std::size_t rowIx = fixture.instrument.controllers.size() - 1;
    Require(!fixture.instrument.controllers[rowIx].config.openSysEx.empty(),
            "added row carries the Ableton default's connect message");
    return rowIx;
}

void RenderState4_ConnectMessagesList()
{
    AppFixture fixture;
    fixture.AddRealisticDevices();
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    const std::size_t rowIx = AddAbletonApc40(fixture, surface);

    // Diagnostic only (not part of the delivered screenshot): read back the
    // Add button's resolved bounds from the portable tree the page itself
    // built, to check a visual oddity spotted in the rendered PNG.
    {
        const synth::ui::NodeTree tree = surface.BuildTree();
        const std::string addId = synth::runtime_ui::NodeIds::ConnectMessageAdd(rowIx);
        for (const synth::ui::Node& node : tree.nodes)
        {
            if (node.id.value == addId)
            {
                std::cout << "DIAGNOSTIC connect-message Add button bounds: w="
                          << node.bounds.width << " h=" << node.bounds.height << "\n";
            }
        }
    }

    RenderSurface(surface, ShotPath("state4_expanded_editor_connect_messages_list.png"));
}

// ---------------------------------------------------------------------------
// State 5: that same editor after a malformed connect-message edit, showing
// the refusal status text. Reached by committing non-hex text through the
// real kConnectMessageCommit action; MidiConfigViewModel::SetConnectMessage
// refuses it and the surface's own SetStatus writes the refusal text the
// page renders at NodeIds::kStatus.
// ---------------------------------------------------------------------------
void RenderState5_MalformedConnectMessageRefusal(bool bust)
{
    AppFixture fixture;
    fixture.AddRealisticDevices();
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    const std::size_t rowIx = AddAbletonApc40(fixture, surface);

    const int commitsBefore = fixture.commits;
    const std::string malformed = bust ? "F0 00 7F F7" /* WELL-FORMED: busts the claim on purpose */
                                       : "not-hex-zz";
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit,
        std::to_string(rowIx) + ":0:" + malformed));

    if (!bust)
    {
        Require(fixture.commits == commitsBefore, "state 5: malformed connect-message edit committed nothing");
    }
    else
    {
        Require(fixture.commits == commitsBefore + 1,
                "positive control: a WELL-FORMED edit (F0 00 7F F7) at the same node DOES commit, "
                "proving the refusal in the non-busted render is the malformed text being rejected, "
                "not a dead action route");
    }

    RenderSurface(surface, ShotPath(bust ? "control_state5_busted_wellformed_message_accepted.png"
                                         : "state5_expanded_editor_malformed_connect_message_refusal.png"));
}

// ---------------------------------------------------------------------------
// Extra check, not one of the six named gate states: committing an entirely
// EMPTY connect-message field is a distinct code path from state 5's
// non-empty malformed text ("not-hex-zz"), and the operator asked this one
// be confirmed separately. A value of "<row>:<msgIx>:" with nothing after
// the second colon splits (via Split's std::getline loop) into only 2 parts
// -- the trailing empty token is dropped -- so HandleConnectMessageCommit's
// arity guard must accept a 2-part value rather than treat it as too few
// parts and return before ever calling SetConnectMessage or SetStatus. With
// that value let through, it reaches SetConnectMessage with an empty
// hexText, which HexDecodeBytes's `sawAnyToken` check refuses, producing a
// visible "Refused: ..." status exactly as a non-empty malformed edit does.
// Verified here by dispatching that exact value and reading back commits and
// status, not by reasoning about the code.
// ---------------------------------------------------------------------------
void RenderExtraCheck_EmptyConnectMessageFieldRefusal()
{
    AppFixture fixture;
    fixture.AddRealisticDevices();
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    const std::size_t rowIx = AddAbletonApc40(fixture, surface);

    const int commitsBefore = fixture.commits;
    const std::string statusBefore = fixture.status;
    // "<rowIx>:0:" -- nothing after the second colon. Split(value, ':') on
    // this drops the trailing empty token, landing exactly on the guard's
    // boundary condition (parts.size() == 2).
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, std::to_string(rowIx) + ":0:"));

    std::cout << "DIAGNOSTIC empty connect-message commit: commits before=" << commitsBefore
              << " after=" << fixture.commits << " status=\"" << fixture.status << "\"\n";

    Require(fixture.commits == commitsBefore, "extra check: empty connect-message edit committed nothing");
    Require(fixture.status != statusBefore && !fixture.status.empty(),
            "extra check: empty connect-message edit produced a visible status change, "
            "not a silent no-op");
    Require(fixture.status.rfind("Refused:", 0) == 0,
            "extra check: the visible status for an empty connect-message edit is a refusal");

    RenderSurface(surface, ShotPath("extra_check_empty_connect_message_field_refusal.png"));
}

// ---------------------------------------------------------------------------
// State 6: a row's expanded editor showing no "Pressure mappings" list,
// while a grid row's own pressure mapping still shows and edits inside that
// grid row. ControllersPageUI.hpp renders no per-row pressure-mapping editor
// (grep confirms zero occurrences of "pressure" anywhere in that file), so
// the first half is the WrldBldr fixture row's editor with its System
// Messages section (which is where grid
// rows live -- MidiMappingRowVM::RowGroup::Grid is a subset of
// MidiConfigSection::SystemMessages, not its own section) opened through the
// real kToggleSection action, and one grid row added through the real
// kAddSingle action, exactly as a player would.
//
// The second half is a claim about behaviour, not appearance: reading
// MidiMappingRowVM::Field's own comment ("Grid mappings intentionally have
// no message/status/note, pressure, feedback, or toggle editor") and
// GridButtonLabel()/GridBlockLabel() in MidiConfigViewModel.cpp confirms a
// grid row carries no pressure-specific control or label text -- a row with
// a paired pressure mapping renders pixel-identical to one without. No
// screenshot can show that difference; see the report for what this proves
// instead (the pressure mapping round-trips through the row's own address
// fields rather than being orphaned into hiddenPressureMappings).
// ---------------------------------------------------------------------------
void RenderState6_NoPressureMappingRowEditor()
{
    synth_runtime::test::ControllersHarnessFixture fixture;
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    surface.SetEnumerateDevices(fixture.state.devices);
    surface.SetContentBounds({0.0f, 0.0f, static_cast<float>(kPageWidth), static_cast<float>(kPageHeight)});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Controller 0 ("wrld") supports grid mappings (WrldBldr kind).
    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "0:system_messages"));
    const int commitsBefore = fixture.state.commits;
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle, "0:system_messages:grid"));
    Require(fixture.state.commits == commitsBefore + 1, "state 6: adding a grid row committed");

    RenderSurface(surface, ShotPath("state6_expanded_editor_no_pressure_mapping_row_list.png"), 1300);
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;

    g_shotDir = (argc > 1) ? std::string(argv[1]) : DefaultShotDir(argv[0]);
    std::filesystem::create_directories(g_shotDir);

    std::cout << "Rendering Controllers page screenshots to " << g_shotDir << "...\n";

    RenderState1_ResolvedWizardDeviceLabel(/*bust=*/false);
    RenderState1_ResolvedWizardDeviceLabel(/*bust=*/true);
    RenderState2_UnresolvedWizardBoundInputLabel();
    RenderState4_ConnectMessagesList();
    RenderState5_MalformedConnectMessageRefusal(/*bust=*/false);
    RenderState5_MalformedConnectMessageRefusal(/*bust=*/true);
    RenderExtraCheck_EmptyConnectMessageFieldRefusal();
    RenderState6_NoPressureMappingRowEditor();

    // State 3 needs a live popup capture, which needs a blocking pump of
    // JUCE's native event loop (see PopupDropdownSequencer's comment above).
    // MessageManager::runDispatchLoop() can only be entered once per
    // process, so both the clean and busted state-3 renders are driven from
    // inside this single entry.
    PopupDropdownSequencer state3Sequencer;
    state3Sequencer.Run({/*bust=*/false, /*bust=*/true});
    juce::MessageManager::getInstance()->runDispatchLoop();
    if (!state3Sequencer.Error().empty())
    {
        throw std::runtime_error("state 3: " + state3Sequencer.Error());
    }

    std::cout << "All screenshots written.\n";
    return 0;
}
