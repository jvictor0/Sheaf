.PHONY: all test run clean venv agent-vm-rebuild agent-vm-fresh agent-vm-run

ifeq ($(SHEAF_AGENT_VM),1)
VENV_DIR ?= .venv-vm
endif
VENV_DIR ?= .venv
PYTHON := $(VENV_DIR)/bin/python
PIP := $(VENV_DIR)/bin/pip

SRC_DIR := src
TEST_MODULES := \
	tests.test_commit_metadata \
	tests.test_service_entrypoint \
	tests.test_quest_service_api \
	tests.test_advance_quest_api \
	tests.test_land_api \
	tests.test_issue_api \
	tests.test_cli \
	tests.test_dashboard_api \
	tests.test_dashboard_shell \
	tests.test_dashboard_git \
	tests.test_dashboard_slice \
	tests.test_no_state_enums \
	tests.test_normalized_state_md \
	tests.test_state_machine_core \
	tests.test_quest_fs_core \
	tests.test_workflow_config \
	tests.test_workflow_state_io \
	tests.test_workflow_interpreter \
	tests.test_workflow_runner_integration \
	tests.test_workflow_compatibility \
	tests.test_workflow_profile_execution \
	tests.test_agent_vm \
	tests.test_harness_quest_thread_core \
	tests.test_project_quest_model \
	tests.test_quest_creation \
	tests.test_experiments \
	tests.test_worktrees \
	tests.test_worktree_execution_path \
	tests.test_experiment_scoped_operations \
	tests.test_experiment_stop_conditions \
	tests.test_experiment_lifecycle \
	tests.test_migration_validation \
	tests.test_agui_mapper \
	tests.test_runtime_logs \
	tests.test_chat_event_bus \
	tests.test_dashboard_chat \
	tests.test_smoke_test

all: test

venv: $(VENV_DIR)/bin/python

$(VENV_DIR)/bin/python: requirements.txt
	python3 -m venv $(VENV_DIR)
	$(PIP) install -r requirements.txt

test: venv
	PYTHONPATH=$(SRC_DIR) $(PYTHON) -m unittest $(TEST_MODULES)

run: venv
	bash start_quest_runner.sh

clean:
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	find . -type f -name '*.pyc' -delete 2>/dev/null || true
	rm -rf .pytest_cache
	find src/quest_runner_service/dashboard_assets -type d -name node_modules -exec rm -rf {} + 2>/dev/null || true

agent-vm-rebuild:
	vm/agent-macos/bin/rebuild-golden

agent-vm-fresh:
	vm/agent-macos/bin/rebuild-golden --fresh

agent-vm-run:
	vm/agent-macos/bin/agent-run $(or $(WORKTREE),$(abspath ../..))
