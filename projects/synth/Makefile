CXX ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude
DEPFLAGS := -MMD -MP
BUILD_DIR := build
BUILD_SENTINEL := $(BUILD_DIR)/.dir
LIB := $(BUILD_DIR)/libsynth.a
TEST_BIN := $(BUILD_DIR)/parameter_modulation_tests
BUTTON_GRID_TEST_BIN := $(BUILD_DIR)/button_grid_tests
DSP_TEST_BIN := $(BUILD_DIR)/dsp_tests
MASTER_CLOCK_TEST_BIN := $(BUILD_DIR)/master_clock_tests
MODULE_TEST_BIN := $(BUILD_DIR)/module_tests
INSTRUMENT_TEST_BIN := $(BUILD_DIR)/instrument_tests
CONTRACT_TEST_BIN := $(BUILD_DIR)/contract_tests
AUDIO_INPUT_TEST_BIN := $(BUILD_DIR)/audio_input_tests
LOGGING_TEST_BIN := $(BUILD_DIR)/logging_tests
ENGINE_TEST_BIN := $(BUILD_DIR)/engine_tests
RIG_TEST_BIN := $(BUILD_DIR)/rig_tests
MINIAPP_SYSTEM_TEST_BIN := $(BUILD_DIR)/miniapp_system_tests
BRAID4_SYSTEM_TEST_BIN := $(BUILD_DIR)/braid4_system_tests
BRAID4_DEADLINE_TEST_BIN := $(BUILD_DIR)/braid4_deadline_tests
RECONCILE_TEST_BIN := $(BUILD_DIR)/reconcile_tests
RECONCILE_EXECUTOR_TEST_BIN := $(BUILD_DIR)/reconcile_executor_tests
POLLER_TEST_BIN := $(BUILD_DIR)/poller_tests
MIDI_SENDER_TEST_BIN := $(BUILD_DIR)/midi_sender_tests
VIEWMODEL_TEST_BIN := $(BUILD_DIR)/viewmodel_tests
BLOCKS_TEST_BIN := $(BUILD_DIR)/blocks_tests
PORTABLE_UI_TEST_BIN := $(BUILD_DIR)/portable_ui_tests
PORTABLE_UI_LAYOUT_TEST_BIN := $(BUILD_DIR)/portable_ui_layout_tests
RUNTIME_MAIN_COMPONENT_TEST_BIN := $(BUILD_DIR)/runtime_main_component_tests
RUNTIME_FILE_SERVICE_TEST_BIN := $(BUILD_DIR)/runtime_file_service_tests
CONTROLLERS_PAGE_UI_TEST_BIN := $(BUILD_DIR)/controllers_page_ui_tests
BROWSER_CONTRACT_TEST_BIN := $(BUILD_DIR)/browser_runtime_contract_tests
BROWSER_COMMAND_BUFFER_TEST_BIN := $(BUILD_DIR)/browser_command_buffer_tests
BROWSER_AUDIO_DEVICE_TEST_BIN := $(BUILD_DIR)/browser_audio_device_tests
BROWSER_MIDI_BRIDGE_TEST_BIN := $(BUILD_DIR)/browser_midi_bridge_tests
CONTROLLER_WIZARD_TEST_BIN := $(BUILD_DIR)/controller_wizard_tests

# The browser ships wasm32, where std::size_t is 32 bits. Every gate above
# builds for the 64-bit host, so arithmetic whose result depends on the width
# of std::size_t is otherwise never exercised at the width that ships. These
# two binaries are the ones that reach the block validator and the config view
# model; they are the host sources unchanged, rebuilt for the other target.
# em++ rather than emcc: emcc does not link the C++ runtime, so a test harness
# that throws fails at link time. The vendored toolchain is preferred over one
# on PATH, which is the opposite of browser/Makefile's order and deliberate --
# a different emscripten on PATH links against its own sysroot and fails here.
EMSDK ?= $(HOME)/.local/emsdk
EMSDK_EMXX := $(EMSDK)/upstream/emscripten/em++
WASM32_EMXX ?= $(if $(wildcard $(EMSDK_EMXX)),$(EMSDK_EMXX),$(if $(shell command -v em++ 2>/dev/null),em++,))
NODE ?= node
WASM32_BUILD_DIR := $(BUILD_DIR)/wasm32
WASM32_TESTS := blocks_tests viewmodel_tests
# The test harnesses signal failure by throwing, which emscripten discards
# unless catching is compiled in. -O0 keeps the build near 15s per binary.
WASM32_FLAGS := -std=c++20 -O0 -fexceptions -sNO_DISABLE_EXCEPTION_CATCHING
SRC := src/ParameterModulation.cpp src/ButtonGrid.cpp src/MidiController.cpp src/PatchPersistence.cpp src/DspWavetable.cpp src/Modules.cpp src/MidiReconcile.cpp src/MidiDevicePoller.cpp src/MidiConfigViewModel.cpp src/MidiConfigBlocks.cpp src/MasterClock.cpp src/ControllerWizard.cpp
OBJ := $(BUILD_DIR)/ParameterModulation.o $(BUILD_DIR)/ButtonGrid.o $(BUILD_DIR)/MidiController.o $(BUILD_DIR)/PatchPersistence.o $(BUILD_DIR)/DspWavetable.o $(BUILD_DIR)/Modules.o $(BUILD_DIR)/MidiReconcile.o $(BUILD_DIR)/MidiDevicePoller.o $(BUILD_DIR)/MidiConfigViewModel.o $(BUILD_DIR)/MidiConfigBlocks.o $(BUILD_DIR)/MasterClock.o $(BUILD_DIR)/ControllerWizard.o
DSP_HEADERS := include/synth/DspMath.hpp include/synth/DspNumbers.hpp include/synth/DspTransferFunction.hpp include/synth/DspFilters.hpp include/synth/DspScope.hpp include/synth/DspWavetable.hpp include/synth/DspOscillators.hpp include/synth/DspBuffers.hpp include/synth/DspAdsr.hpp include/synth/DspConstant.hpp include/synth/DspDegrade.hpp include/synth/DspMetering.hpp include/synth/DspNoise.hpp include/synth/DspOla.hpp include/synth/DspResynthesis.hpp include/synth/DspSpectral.hpp include/synth/DspPhasor2Tick.hpp
RANDOM_LFO_HEADERS := include/synth/DspRandomLfo.hpp include/synth/GangedRandomLfoVisualizer.hpp
STANDARD_MODULATOR_HEADERS := include/synth/StandardModulators.hpp include/synth/ParameterModulation.hpp include/synth/ConstantBarVisualizer.hpp include/synth/NoiseWaveformVisualizer.hpp include/synth/DspConstant.hpp include/synth/DspNoise.hpp $(RANDOM_LFO_HEADERS)
RUNTIME_PAGE_HEADERS := include/synth/RuntimePages.hpp include/synth/RuntimePageStyle.hpp include/synth/PortableUI.hpp include/synth/PortableUIBuilders.hpp
APPS_DIR := apps
MINIAPP_DIR := $(APPS_DIR)/miniapp
SHEAF_PATCH_DIR := $(APPS_DIR)/sheaf-patch
JUCE_DIR ?= $(HOME)/JUCE

.PHONY: all build test dsp_tests master_clock_tests contract_tests audio_input_tests engine_tests controller_wizard_tests check-ui-boundary check-ui-boundary-empty-discovery check-app-bundle-plist miniapp sheaf-patch apps browser browser-apps browser-fixture-app browser-apps-run browser-apps-smoke browser-unit-test browser-command-buffer-test browser-audio-device-test browser-midi-bridge-test clean test-wasm32

all: build test

build: $(LIB)

check-ui-boundary:
	bash scripts/check_ui_boundary.sh

check-ui-boundary-empty-discovery:
	bash scripts/check_ui_boundary_empty_discovery.sh

check-app-bundle-plist:
	bash scripts/check_app_bundle_plist.sh

$(BUILD_SENTINEL):
	mkdir -p $(BUILD_DIR)
	touch $@

$(BUILD_DIR)/ParameterModulation.o: src/ParameterModulation.cpp include/synth/ParameterModulation.hpp include/synth/AtomicColor.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/ButtonGrid.o: src/ButtonGrid.cpp include/synth/ButtonGrid.hpp include/synth/AtomicColor.hpp include/synth/Color.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MidiController.o: src/MidiController.cpp include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/ControllerWizard.o: src/ControllerWizard.cpp include/synth/ControllerWizard.hpp include/synth/MidiController.hpp include/synth/PortableUI.hpp include/synth/MidiReconcile.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/PatchPersistence.o: src/PatchPersistence.cpp include/synth/PatchPersistence.hpp include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/MasterClock.hpp include/synth/Json.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/DspWavetable.o: src/DspWavetable.cpp $(DSP_HEADERS) | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Modules.o: src/Modules.cpp include/synth/Modules.hpp include/synth/ParameterModulation.hpp $(DSP_HEADERS) | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MidiReconcile.o: src/MidiReconcile.cpp include/synth/MidiReconcile.hpp include/synth/MidiController.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MidiDevicePoller.o: src/MidiDevicePoller.cpp include/synth/MidiDevicePoller.hpp include/synth/MidiReconcile.hpp include/synth/ThreadId.hpp include/synth/AsyncLogger.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MidiConfigViewModel.o: src/MidiConfigViewModel.cpp include/synth/MidiConfigViewModel.hpp include/synth/ControllerWizard.hpp include/synth/MidiConfigBlocks.hpp include/synth/MidiController.hpp include/synth/MidiReconcile.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MidiConfigBlocks.o: src/MidiConfigBlocks.cpp include/synth/MidiConfigBlocks.hpp include/synth/MidiController.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/MasterClock.o: src/MasterClock.cpp include/synth/MasterClock.hpp include/synth/DspPhasor2Tick.hpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	ar rcs $@ $^

$(TEST_BIN): tests/parameter_modulation_tests.cpp include/synth/Json.hpp include/synth/PatchPersistence.hpp include/synth/MasterClock.hpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(BUTTON_GRID_TEST_BIN): tests/button_grid_tests.cpp include/synth/ButtonGrid.hpp include/synth/AtomicColor.hpp include/synth/Color.hpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(DSP_TEST_BIN): tests/dsp_tests.cpp $(LIB) $(DSP_HEADERS) $(STANDARD_MODULATOR_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(MASTER_CLOCK_TEST_BIN): tests/master_clock_tests.cpp $(LIB) include/synth/MasterClock.hpp include/synth/DspPhasor2Tick.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

dsp_tests: $(DSP_TEST_BIN)
	$(DSP_TEST_BIN)

master_clock_tests: $(MASTER_CLOCK_TEST_BIN)
	$(MASTER_CLOCK_TEST_BIN)

contract_tests: $(CONTRACT_TEST_BIN)
	$(CONTRACT_TEST_BIN)

audio_input_tests: $(AUDIO_INPUT_TEST_BIN)
	$(AUDIO_INPUT_TEST_BIN)

engine_tests: $(ENGINE_TEST_BIN)
	$(ENGINE_TEST_BIN)

# -Iapps/miniapp is added for this rule only: DemoModulation.hpp lives under
# apps/miniapp/ (JUCE-free), and MiniAppCore uses its group-level per-sample
# parameter processing helper.
$(MODULE_TEST_BIN): tests/module_tests.cpp apps/miniapp/DemoModulation.hpp $(LIB) include/synth/Modules.hpp include/synth/DspAdsr.hpp include/synth/ParameterModulation.hpp
	$(CXX) $(CPPFLAGS) -Iapps/miniapp $(CXXFLAGS) $< $(LIB) -o $@

$(INSTRUMENT_TEST_BIN): tests/instrument_tests.cpp $(LIB) include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(CONTRACT_TEST_BIN): tests/contract_tests.cpp $(LIB) include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/AppRegistry.hpp include/synth/MasterClock.hpp include/synth/PatchBrowser.hpp include/synth/RuntimePagePolicy.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(AUDIO_INPUT_TEST_BIN): tests/audio_input_tests.cpp tests/support/SynthRig.hpp $(LIB) include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/Engine.hpp include/synth/PortableUI.hpp include/synth/MasterClock.hpp include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp include/synth/PatchPersistence.hpp include/synth/Json.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(LOGGING_TEST_BIN): tests/logging_tests.cpp $(LIB) include/synth/AsyncLogger.hpp include/synth/CircularQueue.hpp include/synth/ThreadId.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(ENGINE_TEST_BIN): tests/engine_tests.cpp $(LIB) include/synth/Engine.hpp include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/AppRegistry.hpp include/synth/AsyncLogger.hpp include/synth/MasterClock.hpp include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp include/synth/PatchPersistence.hpp include/synth/RuntimePagePolicy.hpp include/synth/Json.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(RIG_TEST_BIN): tests/rig_tests.cpp tests/support/SynthRig.hpp $(LIB) include/synth/Engine.hpp include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/AsyncLogger.hpp include/synth/MasterClock.hpp include/synth/MidiController.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp include/synth/PatchPersistence.hpp include/synth/Json.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

# -Iapps/miniapp is added for this rule only: MiniAppCore.hpp/DemoModulation.hpp
# live under apps/miniapp/ (JUCE-free), and this is the one JUCE-free test
# binary that includes them across directories. Building this target proves
# MiniAppCore compiles and links with no JUCE include paths at all.
$(MINIAPP_SYSTEM_TEST_BIN): tests/miniapp_system_tests.cpp tests/support/SynthRig.hpp apps/miniapp/MiniApp.hpp apps/miniapp/MiniAppRegistration.hpp apps/miniapp/MiniAppUI.hpp apps/miniapp/MiniAppUiModel.hpp apps/miniapp/MiniAppDraw.hpp apps/miniapp/MiniAppCore.hpp apps/miniapp/DemoModulation.hpp include/synth/EncoderDraw.hpp include/synth/PortableUI.hpp include/synth/PortableUIBuilders.hpp include/synth/PortableUILayout.hpp include/synth/PortableUIStandardLayout.hpp include/synth/ConstantBarVisualizer.hpp $(STANDARD_MODULATOR_HEADERS) $(LIB) include/synth/Engine.hpp include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/AppRegistry.hpp include/synth/AsyncLogger.hpp include/synth/MasterClock.hpp include/synth/MidiController.hpp include/synth/Modules.hpp include/synth/ParameterModulation.hpp include/synth/RuntimeUIState.hpp include/synth/ButtonGrid.hpp include/synth/PatchPersistence.hpp include/synth/PatchBrowser.hpp include/synth/DspScope.hpp include/synth/DspConstant.hpp include/synth/DspNoise.hpp include/synth/NoiseWaveformVisualizer.hpp include/synth/Json.hpp
	$(CXX) $(CPPFLAGS) -Iapps/miniapp $(CXXFLAGS) $< $(LIB) -o $@

$(BRAID4_SYSTEM_TEST_BIN): tests/braid4_system_tests.cpp tests/support/SynthRig.hpp apps/braid-4/Braid4.hpp apps/braid-4/Braid4UI.hpp apps/braid-4/Braid4UiModel.hpp apps/braid-4/Braid4Draw.hpp apps/braid-4/Braid4Core.hpp $(STANDARD_MODULATOR_HEADERS) $(LIB) include/synth/Engine.hpp include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/MasterClock.hpp include/synth/Modules.hpp include/synth/ParameterModulation.hpp include/synth/PatchPersistence.hpp include/synth/EncoderDraw.hpp include/synth/PortableUI.hpp include/synth/PortableUIBuilders.hpp include/synth/PortableUILayout.hpp include/synth/PortableUIStandardLayout.hpp include/synth/DspBuffers.hpp include/synth/DspScope.hpp include/synth/Json.hpp
	$(CXX) $(CPPFLAGS) -Iapps/braid-4 $(CXXFLAGS) $< $(LIB) -o $@

$(BRAID4_DEADLINE_TEST_BIN): tests/braid4_deadline_tests.cpp apps/braid-4/Braid4Core.hpp $(STANDARD_MODULATOR_HEADERS) $(LIB) include/synth/Engine.hpp include/synth/AppContext.hpp include/synth/AppConcepts.hpp include/synth/MasterClock.hpp include/synth/Modules.hpp include/synth/ParameterModulation.hpp include/synth/DspBuffers.hpp include/synth/DspScope.hpp
	$(CXX) $(CPPFLAGS) -Iapps/braid-4 $(CXXFLAGS) $< $(LIB) -o $@

$(RECONCILE_TEST_BIN): tests/reconcile_tests.cpp $(LIB) include/synth/MidiReconcile.hpp include/synth/MidiController.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(RECONCILE_EXECUTOR_TEST_BIN): tests/reconcile_executor_tests.cpp $(LIB) include/synth/MidiReconcile.hpp include/synth/MidiController.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(POLLER_TEST_BIN): tests/poller_tests.cpp $(LIB) include/synth/MidiDevicePoller.hpp include/synth/MidiReconcile.hpp include/synth/ThreadId.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(MIDI_SENDER_TEST_BIN): tests/midi_sender_tests.cpp $(LIB) include/synth/MidiController.hpp include/synth/ParameterModulation.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(VIEWMODEL_TEST_BIN): tests/viewmodel_tests.cpp $(LIB) include/synth/MidiConfigViewModel.hpp include/synth/MidiConfigBlocks.hpp include/synth/MidiController.hpp include/synth/MidiReconcile.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(BLOCKS_TEST_BIN): tests/blocks_tests.cpp $(LIB) include/synth/MidiConfigBlocks.hpp include/synth/MidiController.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(PORTABLE_UI_TEST_BIN): tests/portable_ui_tests.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $< $(LIB) -o $@

$(PORTABLE_UI_LAYOUT_TEST_BIN): tests/portable_ui_layout_tests.cpp tests/support/VisualCriteria.hpp include/synth/PortableUI.hpp include/synth/PortableUIBuilders.hpp include/synth/PortableUILayout.hpp include/synth/PortableUIMetrics.hpp include/synth/PortableUIStandardLayout.hpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(RUNTIME_MAIN_COMPONENT_TEST_BIN): tests/runtime_main_component_tests.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $< $(LIB) -o $@

$(RUNTIME_FILE_SERVICE_TEST_BIN): tests/runtime_file_service_tests.cpp include/synth/RuntimeFileService.hpp $(RUNTIME_PAGE_HEADERS) $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(CONTROLLERS_PAGE_UI_TEST_BIN): tests/controllers_page_ui_tests.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $< $(LIB) -o $@

$(BUILD_DIR)/BrowserRuntimeAbi.o: browser/cpp/BrowserRuntimeAbi.cpp | $(BUILD_SENTINEL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BROWSER_CONTRACT_TEST_BIN): tests/browser_runtime_contract_tests.cpp $(BUILD_DIR)/BrowserRuntimeAbi.o $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $< $(BUILD_DIR)/BrowserRuntimeAbi.o $(LIB) -o $@

$(BROWSER_COMMAND_BUFFER_TEST_BIN): tests/browser_command_buffer_tests.cpp apps/miniapp/MiniAppDraw.hpp include/synth/browser/BrowserCommandBuffer.hpp $(RUNTIME_PAGE_HEADERS) $(STANDARD_MODULATOR_HEADERS) $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(BROWSER_AUDIO_DEVICE_TEST_BIN): tests/browser_audio_device_tests.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $< $(LIB) -o $@

$(BROWSER_MIDI_BRIDGE_TEST_BIN): tests/browser_midi_bridge_tests.cpp include/synth/browser/BrowserMidiBridge.hpp include/synth/Engine.hpp include/synth/MidiReconcile.hpp include/synth/MidiController.hpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB) -o $@

$(CONTROLLER_WIZARD_TEST_BIN): tests/controller_wizard_tests.cpp include/synth/ControllerWizard.hpp include/synth/MidiController.hpp include/synth/PortableUI.hpp include/synth/MidiReconcile.hpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DNDEBUG $< $(LIB) -o $@

controller_wizard_tests: $(CONTROLLER_WIZARD_TEST_BIN)
	$(CONTROLLER_WIZARD_TEST_BIN)

browser-unit-test: $(BROWSER_CONTRACT_TEST_BIN)
	$(BROWSER_CONTRACT_TEST_BIN)

browser-command-buffer-test: $(BROWSER_COMMAND_BUFFER_TEST_BIN)
	$(BROWSER_COMMAND_BUFFER_TEST_BIN)

browser-audio-device-test: $(BROWSER_AUDIO_DEVICE_TEST_BIN)
	$(BROWSER_AUDIO_DEVICE_TEST_BIN)

browser-midi-bridge-test: $(BROWSER_MIDI_BRIDGE_TEST_BIN)
	$(BROWSER_MIDI_BRIDGE_TEST_BIN)

# Fails rather than skips when the toolchain is missing: a gate that skips
# silently is how the library came to ship a guard that was never run on the
# target it ships to.
test-wasm32:
	@test -n "$(WASM32_EMXX)" || { \
		echo "em++ not found (looked for '$(EMSDK_EMXX)' and on PATH; set EMSDK or WASM32_EMXX)." >&2; \
		echo "The browser target is what this gate exists to cover, so it fails rather than skipping." >&2; \
		exit 1; }
	@command -v $(NODE) >/dev/null 2>&1 || { echo "$(NODE) not found; needed to run the wasm32 binaries." >&2; exit 1; }
	@mkdir -p $(WASM32_BUILD_DIR)
	@for name in $(WASM32_TESTS); do \
		echo "wasm32: building $$name"; \
		$(WASM32_EMXX) $(WASM32_FLAGS) $(CPPFLAGS) tests/$$name.cpp $(SRC) \
			-o $(WASM32_BUILD_DIR)/$$name.js || exit 1; \
		echo "wasm32: running $$name"; \
		$(NODE) $(WASM32_BUILD_DIR)/$$name.js || exit 1; \
	done

test: check-ui-boundary check-ui-boundary-empty-discovery check-app-bundle-plist test-wasm32 $(TEST_BIN) $(BUTTON_GRID_TEST_BIN) $(DSP_TEST_BIN) $(MASTER_CLOCK_TEST_BIN) $(MODULE_TEST_BIN) $(INSTRUMENT_TEST_BIN) $(CONTRACT_TEST_BIN) $(AUDIO_INPUT_TEST_BIN) $(LOGGING_TEST_BIN) $(ENGINE_TEST_BIN) $(RIG_TEST_BIN) $(MINIAPP_SYSTEM_TEST_BIN) $(BRAID4_SYSTEM_TEST_BIN) $(BRAID4_DEADLINE_TEST_BIN) $(RECONCILE_TEST_BIN) $(RECONCILE_EXECUTOR_TEST_BIN) $(POLLER_TEST_BIN) $(MIDI_SENDER_TEST_BIN) $(VIEWMODEL_TEST_BIN) $(BLOCKS_TEST_BIN) $(PORTABLE_UI_TEST_BIN) $(PORTABLE_UI_LAYOUT_TEST_BIN) $(RUNTIME_MAIN_COMPONENT_TEST_BIN) $(RUNTIME_FILE_SERVICE_TEST_BIN) $(CONTROLLERS_PAGE_UI_TEST_BIN) $(BROWSER_CONTRACT_TEST_BIN) $(BROWSER_COMMAND_BUFFER_TEST_BIN) $(BROWSER_AUDIO_DEVICE_TEST_BIN) $(BROWSER_MIDI_BRIDGE_TEST_BIN) $(CONTROLLER_WIZARD_TEST_BIN)
	$(TEST_BIN)
	$(BUTTON_GRID_TEST_BIN)
	$(DSP_TEST_BIN)
	$(MASTER_CLOCK_TEST_BIN)
	$(MODULE_TEST_BIN)
	$(INSTRUMENT_TEST_BIN)
	$(CONTRACT_TEST_BIN)
	$(AUDIO_INPUT_TEST_BIN)
	$(LOGGING_TEST_BIN)
	$(ENGINE_TEST_BIN)
	$(RIG_TEST_BIN)
	$(MINIAPP_SYSTEM_TEST_BIN)
	$(BRAID4_SYSTEM_TEST_BIN)
	$(BRAID4_DEADLINE_TEST_BIN)
	$(RECONCILE_TEST_BIN)
	$(RECONCILE_EXECUTOR_TEST_BIN)
	$(POLLER_TEST_BIN)
	$(MIDI_SENDER_TEST_BIN)
	$(VIEWMODEL_TEST_BIN)
	$(BLOCKS_TEST_BIN)
	$(PORTABLE_UI_TEST_BIN)
	$(PORTABLE_UI_LAYOUT_TEST_BIN)
	$(RUNTIME_MAIN_COMPONENT_TEST_BIN)
	$(RUNTIME_FILE_SERVICE_TEST_BIN)
	$(CONTROLLERS_PAGE_UI_TEST_BIN)
	$(BROWSER_CONTRACT_TEST_BIN)
	$(BROWSER_COMMAND_BUFFER_TEST_BIN)
	$(BROWSER_AUDIO_DEVICE_TEST_BIN)
	$(BROWSER_MIDI_BRIDGE_TEST_BIN)
	$(CONTROLLER_WIZARD_TEST_BIN)
	@if [ -d "$(JUCE_DIR)/modules" ]; then \
		$(MAKE) -C $(MINIAPP_DIR) test; \
	else \
		echo "JUCE not found at $(JUCE_DIR); skipping $(MINIAPP_DIR) JUCE-backed tests."; \
	fi

miniapp:
	$(MAKE) -C $(MINIAPP_DIR)

sheaf-patch:
	$(MAKE) -C $(SHEAF_PATCH_DIR)

apps:
	$(MAKE) -C $(APPS_DIR)/miniapp
	$(MAKE) -C $(SHEAF_PATCH_DIR)

browser:
	$(MAKE) -C browser build

browser-apps:
	$(MAKE) -C browser browser-apps

browser-fixture-app:
	$(MAKE) -C browser browser-fixture-app

browser-apps-run:
	$(MAKE) -C browser browser-apps-run

browser-apps-smoke:
	$(MAKE) -C browser browser-apps-smoke

clean:
	rm -rf $(BUILD_DIR)
	if [ -f "$(MINIAPP_DIR)/Makefile" ]; then $(MAKE) -C $(MINIAPP_DIR) clean; fi
	if [ -f "$(SHEAF_PATCH_DIR)/Makefile" ]; then $(MAKE) -C $(SHEAF_PATCH_DIR) clean; fi

-include $(wildcard $(BUILD_DIR)/*.d)
