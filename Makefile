MAKEFLAGS += --warn-undefined-variables

PROJECTS := conductor web quest-runner dictator realtime-agent sheaf-chat agents xagent synth
CODEX_HOME ?= $(HOME)/.codex
PLUGIN_CREATOR_VALIDATOR := $(CODEX_HOME)/skills/.system/plugin-creator/scripts/validate_plugin.py
TOOL ?=
REF ?=
FORCE ?=

.PHONY: all clean test help openspec-check
.PHONY: $(PROJECTS)
.PHONY: conductor-build conductor-test conductor-run conductor-clean
.PHONY: web-build web-test web-clean
.PHONY: quest-runner-build quest-runner-test quest-runner-run quest-runner-clean
.PHONY: dictator-build dictator-test dictator-run dictator-clean dictator-install-talon-bridge
.PHONY: realtime-agent-build realtime-agent-test realtime-agent-clean realtime-agent-run-cli
.PHONY: sheaf-chat-build sheaf-chat-test sheaf-chat-run sheaf-chat-clean
.PHONY: agents-build agents-test agents-install agents-check agents-clean
.PHONY: agents-install-repo agents-check-repo agents-clean-repo
.PHONY: agents-install-global agents-check-global agents-clean-global
.PHONY: cloudcli-install cloudcli-check
.PHONY: agents-vendor-sync
.PHONY: xagent-build xagent-test xagent-clean
.PHONY: xagent-service-run
.PHONY: xagent-plugin-build xagent-plugin-test xagent-plugin-install-global
.PHONY: synth-build synth-test synth-clean synth-browser-build synth-browser-test

.DEFAULT_GOAL := all

all: $(PROJECTS)

$(PROJECTS):
	$(MAKE) -C projects/$@ all

clean:
	@for project in $(PROJECTS); do \
		$(MAKE) -C projects/$$project clean; \
	done

test: xagent-plugin-test
	@status=0; \
	for project in $(PROJECTS); do \
		$(MAKE) -C projects/$$project test || status=$$?; \
	done; \
	$(MAKE) openspec-check || status=$$?; \
	exit $$status

openspec-check:
	python3 -m unittest tests/openspec_requirement_ids_test.py

conductor-build:
	$(MAKE) -C projects/conductor build

conductor-test:
	$(MAKE) -C projects/conductor test

conductor-run:
	$(MAKE) -C projects/conductor run

conductor-clean:
	$(MAKE) -C projects/conductor clean

web-build:
	$(MAKE) -C projects/web build

web-test:
	$(MAKE) -C projects/web test

web-clean:
	$(MAKE) -C projects/web clean

quest-runner-build:
	$(MAKE) -C projects/quest-runner all

quest-runner-test:
	$(MAKE) -C projects/quest-runner test

quest-runner-run:
	$(MAKE) -C projects/quest-runner run

quest-runner-clean:
	$(MAKE) -C projects/quest-runner clean

dictator-build:
	$(MAKE) -C projects/dictator build

dictator-test:
	$(MAKE) -C projects/dictator test

dictator-run:
	$(MAKE) -C projects/dictator run

dictator-install-talon-bridge:
	$(MAKE) -C projects/dictator install-talon-bridge

dictator-clean:
	$(MAKE) -C projects/dictator clean

realtime-agent-build:
	$(MAKE) -C projects/realtime-agent build

realtime-agent-test:
	$(MAKE) -C projects/realtime-agent test

realtime-agent-clean:
	$(MAKE) -C projects/realtime-agent clean

realtime-agent-run-cli:
	$(MAKE) -C projects/realtime-agent run-cli

sheaf-chat-build:
	$(MAKE) -C projects/sheaf-chat build

sheaf-chat-test:
	$(MAKE) -C projects/sheaf-chat test

sheaf-chat-run:
	$(MAKE) -C projects/sheaf-chat run

sheaf-chat-clean:
	$(MAKE) -C projects/sheaf-chat clean

agents-build:
	$(MAKE) -C projects/agents build

agents-test:
	$(MAKE) -C projects/agents test

agents-install:
	$(MAKE) -C projects/agents install

agents-check:
	$(MAKE) -C projects/agents check

agents-clean:
	$(MAKE) -C projects/agents clean

agents-install-repo:
	$(MAKE) -C projects/agents install-repo

agents-check-repo:
	$(MAKE) -C projects/agents check-repo

agents-clean-repo:
	$(MAKE) -C projects/agents clean-repo

agents-install-global:
	$(MAKE) -C projects/agents install-global

agents-check-global:
	$(MAKE) -C projects/agents check-global

agents-clean-global:
	$(MAKE) -C projects/agents clean-global

cloudcli-install:
	$(MAKE) -C projects/agents cloudcli-install

cloudcli-check:
	$(MAKE) -C projects/agents cloudcli-check

agents-vendor-sync:
	$(MAKE) -C projects/agents vendor-sync TOOL="$(TOOL)" REF="$(REF)" FORCE="$(FORCE)"

xagent-build:
	$(MAKE) -C projects/xagent build

xagent-test:
	$(MAKE) -C projects/xagent test

xagent-clean:
	$(MAKE) -C projects/xagent clean

xagent-plugin-build:
	python3 plugins/xagent/scripts/package_xagent.py

xagent-plugin-test:
	python3 -m unittest plugins/xagent/scripts/install_global_test.py
	python3 -m unittest plugins/xagent/scripts/controller_stop_hook_test.py
	python3 plugins/xagent/scripts/package_xagent.py --check
	python3 $(PLUGIN_CREATOR_VALIDATOR) plugins/xagent

xagent-plugin-install-global:
	python3 plugins/xagent/scripts/install_global.py install

xagent-service-run:
	$(MAKE) -C projects/xagent service-run

synth-build:
	$(MAKE) -C projects/synth build

synth-test:
	$(MAKE) -C projects/synth test

synth-clean:
	$(MAKE) -C projects/synth clean

synth-browser-build:
	$(MAKE) -C projects/synth browser

synth-browser-test:
	$(MAKE) -C projects/synth browser-unit-test
	$(MAKE) -C projects/synth/browser test

help:
	@echo "Repository targets:"
	@echo "  make all              Build and test every project under projects/"
	@echo "  make test             Run tests for every project"
	@echo "  make clean            Clean every project"
	@echo ""
	@echo "Project shortcuts:"
	@echo "  make <project>        Run that project's default all target"
	@echo "  make <project>-build  Build one project"
	@echo "  make <project>-test   Test one project"
	@echo "  make <project>-install  Install one project (if supported)"
	@echo "  make <project>-run    Run one project's service (if supported)"
	@echo "  make <project>-clean  Clean one project"
	@echo "  make agents-install   Install repo-local Sheaf guidance and user-global shared guidance"
	@echo "  make agents-check     Verify local and global agent guidance"
	@echo "  make agents-install-global  Install user-global agent guidance"
	@echo "  make agents-check-global    Verify user-global agent guidance"
	@echo "  make cloudcli-install Install CloudCLI on loopback and publish it via Tailscale HTTPS"
	@echo "  make cloudcli-check   Verify CloudCLI and its Tailscale HTTPS proxy"
	@echo "  make agents-vendor-sync TOOL=openspec|superpowers REF=<tag>  Refresh vendored tooling pins"
	@echo "  Repo-local outputs contain repository instructions and Sheaf-only skills."
	@echo "  Shared skills install only through agents-install-global."
	@echo "  Plugin-owned skills such as xagent-subagents are excluded from the agents installer."
	@echo ""
	@echo "Projects: $(PROJECTS)"
	@echo ""
	@echo "Examples:"
	@echo "  make conductor"
	@echo "  make conductor-build"
	@echo "  make conductor-run"
	@echo "  make conductor-clean"
	@echo ""
	@echo "See structure/makefile.md for the full Makefile layout."
