# apps/miniapp — the real SynthMiniapp app (Plan 3 Task 6), built on the
# shared runtime shell (runtime/Runtime.hpp, runtime/Shell.hpp,
# runtime/MainPane.hpp) rather than the bespoke JUCE app the old top-level
# miniapp/ directory used to build.
#
# Sets the parameters expected by runtime/juce_build.mk and includes it.
# APP_SOURCES is just Main.cpp: MiniApp.hpp/MiniAppCore.hpp/DemoModulation.hpp
# are all headers, pulled in transitively and picked up by juce_build.mk's
# dependency list below (so editing them still triggers a rebuild).
#
# Path anchoring: all paths (include, sources, assets) are resolved relative
# to this Makefile's directory via APP_DIR, so the build works correctly
# regardless of the caller's cwd. See juce_build.mk for SYNTH_ROOT anchoring.

APP_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

APP_NAME := SynthMiniapp
APP_BUILD_DIR := $(APP_DIR)/build
APP_SOURCES := $(APP_DIR)/Main.cpp
APP_INFO_PLIST := $(APP_DIR)/Info.plist

include $(APP_DIR)/../../runtime/juce_build.mk

# Extra header dependencies beyond juce_build.mk's SYNTH_JUCE_HEADERS glob
# (that glob only covers projects/synth/juce/*.hpp and the runtime/ headers,
# not this app's own MiniApp.hpp/MiniAppCore.hpp/DemoModulation.hpp), so a
# rebuild is triggered when any of them change.
APP_HEADERS := $(APP_DIR)/MiniApp.hpp $(APP_DIR)/MiniAppRegistration.hpp $(APP_DIR)/MiniAppUI.hpp $(APP_DIR)/MiniAppUiModel.hpp $(APP_DIR)/MiniAppDraw.hpp $(APP_DIR)/MiniAppCore.hpp $(APP_DIR)/DemoModulation.hpp
$(APP): $(APP_HEADERS)

# PortableDrawGeometryTests.cpp covers shared synth::ui encoder, segment, and
# scope draw math. It remains in this JUCE target only because it also
# smoke-tests the JUCE MIDI handler adapters at the end.
GEOMETRY_TEST_SRC := $(SYNTH_ROOT)/juce/PortableDrawGeometryTests.cpp
GEOMETRY_TEST := $(BUILD_DIR)/portable_draw_geometry_tests
PORTABLE_BACKEND_TEST_SRC := $(SYNTH_ROOT)/juce/PortableJuceBackendTests.cpp
PORTABLE_BACKEND_TEST := $(BUILD_DIR)/portable_juce_backend_tests
MINIAPP_PARITY_TEST_SRC := $(SYNTH_ROOT)/juce/MiniAppJuceBackendParityTests.cpp
MINIAPP_PARITY_TEST := $(BUILD_DIR)/miniapp_juce_backend_parity_tests
RUNTIME_PAGES_TEST_SRC := $(SYNTH_ROOT)/juce/RuntimePagesJuceTests.cpp
RUNTIME_PAGES_TEST := $(BUILD_DIR)/runtime_pages_juce_tests
FILE_PAGE_SIM_TEST_SRC := $(SYNTH_ROOT)/juce/FilePageSimulationTests.cpp
FILE_PAGE_SIM_TEST := $(BUILD_DIR)/file_page_simulation_tests
RUNTIME_SHELL_SESSION_TEST_SRC := $(SYNTH_ROOT)/juce/RuntimeShellSessionTests.cpp
RUNTIME_SHELL_SESSION_TEST := $(BUILD_DIR)/runtime_shell_session_tests
CONTROLLERS_PAGE_SIM_TEST_SRC := $(SYNTH_ROOT)/juce/ControllersPageSimulationTests.cpp
CONTROLLERS_PAGE_SIM_TEST := $(BUILD_DIR)/controllers_page_simulation_tests

.PHONY: test

$(GEOMETRY_TEST): $(GEOMETRY_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GEOMETRY_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(PORTABLE_BACKEND_TEST): $(PORTABLE_BACKEND_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PORTABLE_BACKEND_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(MINIAPP_PARITY_TEST): $(MINIAPP_PARITY_TEST_SRC) $(APP_HEADERS) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MINIAPP_PARITY_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(RUNTIME_PAGES_TEST): $(RUNTIME_PAGES_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RUNTIME_PAGES_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(RUNTIME_SHELL_SESSION_TEST): $(RUNTIME_SHELL_SESSION_TEST_SRC) $(APP_HEADERS) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(RUNTIME_SHELL_SESSION_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(FILE_PAGE_SIM_TEST): $(FILE_PAGE_SIM_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(FILE_PAGE_SIM_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

$(CONTROLLERS_PAGE_SIM_TEST): $(CONTROLLERS_PAGE_SIM_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CONTROLLERS_PAGE_SIM_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

# BUILD_DIR is absolute, so `make -C projects/synth/apps/miniapp build/<name>`
# names a different target than the rules above and would otherwise fail with
# "No rule to make target". These aliases let every test binary be requested by
# its short build/ path from any working directory.
build/portable_draw_geometry_tests: $(GEOMETRY_TEST)
build/portable_juce_backend_tests: $(PORTABLE_BACKEND_TEST)
build/miniapp_juce_backend_parity_tests: $(MINIAPP_PARITY_TEST)
build/runtime_pages_juce_tests: $(RUNTIME_PAGES_TEST)
build/runtime_shell_session_tests: $(RUNTIME_SHELL_SESSION_TEST)
build/file_page_simulation_tests: $(FILE_PAGE_SIM_TEST)
build/controllers_page_simulation_tests: $(CONTROLLERS_PAGE_SIM_TEST)

test: check-juce $(GEOMETRY_TEST) $(PORTABLE_BACKEND_TEST) $(MINIAPP_PARITY_TEST) $(RUNTIME_PAGES_TEST) $(FILE_PAGE_SIM_TEST) $(RUNTIME_SHELL_SESSION_TEST) $(CONTROLLERS_PAGE_SIM_TEST)
	$(GEOMETRY_TEST)
	$(PORTABLE_BACKEND_TEST)
	$(MINIAPP_PARITY_TEST)
	$(RUNTIME_PAGES_TEST)
	$(FILE_PAGE_SIM_TEST)
	$(RUNTIME_SHELL_SESSION_TEST)
	$(CONTROLLERS_PAGE_SIM_TEST)
