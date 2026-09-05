APP_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

APP_NAME := SheafPatch
APP_BUILD_DIR := $(APP_DIR)/build
APP_SOURCES := $(APP_DIR)/Main.cpp
APP_INFO_PLIST := $(APP_DIR)/Info.plist

include $(APP_DIR)/../../runtime/juce_build.mk

MINIAPP_DIR := $(APP_DIR)/../miniapp
BRAID4_DIR := $(APP_DIR)/../braid-4
ONE_SECOND_DELAY_DIR := $(APP_DIR)/../one-second-delay
CPPFLAGS += -I$(BRAID4_DIR) -I$(ONE_SECOND_DELAY_DIR)

# Optional out-of-tree application. Setting EXTRA_APP_DIR requires all of the
# other EXTRA_APP_* variables; with none of them set the build is unchanged.
EXTRA_APP_DIR ?=
EXTRA_APP_HEADER ?=
EXTRA_APP_TYPE ?=
EXTRA_APP_REGISTRAR ?=
EXTRA_APP_HEADERS ?=
ifneq ($(EXTRA_APP_DIR),)
$(if $(EXTRA_APP_HEADER),,$(error EXTRA_APP_HEADER is required when EXTRA_APP_DIR is set))
$(if $(EXTRA_APP_TYPE),,$(error EXTRA_APP_TYPE is required when EXTRA_APP_DIR is set))
$(if $(EXTRA_APP_REGISTRAR),,$(error EXTRA_APP_REGISTRAR is required when EXTRA_APP_DIR is set))
$(if $(EXTRA_APP_HEADERS),,$(error EXTRA_APP_HEADERS is required when EXTRA_APP_DIR is set; list the headers so edits trigger a rebuild))
CPPFLAGS += -I$(EXTRA_APP_DIR) \
	-DSHEAF_PATCH_EXTRA_APP_HEADER='"$(EXTRA_APP_HEADER)"' \
	-DSHEAF_PATCH_EXTRA_APP_TYPE=$(EXTRA_APP_TYPE) \
	-DSHEAF_PATCH_EXTRA_APP_REGISTRAR=$(EXTRA_APP_REGISTRAR)
endif

APP_HEADERS := $(APP_DIR)/Launcher.hpp \
	$(BRAID4_DIR)/Braid4Registration.hpp \
	$(BRAID4_DIR)/Braid4.hpp \
	$(BRAID4_DIR)/Braid4UI.hpp \
	$(BRAID4_DIR)/Braid4UiModel.hpp \
	$(BRAID4_DIR)/Braid4Draw.hpp \
	$(BRAID4_DIR)/Braid4Core.hpp \
	$(ONE_SECOND_DELAY_DIR)/OneSecondDelayRegistration.hpp \
	$(ONE_SECOND_DELAY_DIR)/OneSecondDelay.hpp \
	$(MINIAPP_DIR)/MiniAppRegistration.hpp \
	$(MINIAPP_DIR)/MiniApp.hpp \
	$(MINIAPP_DIR)/MiniAppUI.hpp \
	$(MINIAPP_DIR)/MiniAppUiModel.hpp \
	$(MINIAPP_DIR)/MiniAppDraw.hpp \
	$(MINIAPP_DIR)/MiniAppCore.hpp \
	$(MINIAPP_DIR)/DemoModulation.hpp
# Must stay after the APP_HEADERS assignment and before the rules that use it.
APP_HEADERS += $(EXTRA_APP_HEADERS)
$(APP): $(APP_HEADERS)

LAUNCHER_HARNESS_TEST_SRC := $(APP_DIR)/LauncherHarnessTests.cpp
LAUNCHER_HARNESS_TEST := $(BUILD_DIR)/launcher_harness_tests

.PHONY: test

$(LAUNCHER_HARNESS_TEST): $(LAUNCHER_HARNESS_TEST_SRC) $(APP_HEADERS) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(SYNTH_HEADERS) $(SYNTH_JUCE_HEADERS) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LAUNCHER_HARNESS_TEST_SRC) $(SYNTH_SRC) $(SYNTH_RUNTIME_SRC) $(JUCE_MODULE_OBJ) $(JUCE_C_MODULE_OBJ) -o $@ $(JUCE_PLATFORM_LDFLAGS)

test: check-juce $(LAUNCHER_HARNESS_TEST)
	$(LAUNCHER_HARNESS_TEST)
