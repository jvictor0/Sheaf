.PHONY: all build test install check clean
.PHONY: install-repo check-repo clean-repo
.PHONY: install-global check-global clean-global
.PHONY: vendor-sync
.PHONY: cloudcli-install cloudcli-check

AGENTS_INSTALL_FLAGS ?=
TOOL ?=
REF ?=
FORCE ?=

all: check

build: check

test:
	python3 -m unittest discover -s scripts -p '*_test.py'
	python3 -m unittest discover -s utils -p '*_test.py'
	python3 scripts/install.py check --scope repo

install:
	python3 scripts/install.py install --scope all $(AGENTS_INSTALL_FLAGS)
	python3 scripts/install_superpowers.py install $(AGENTS_INSTALL_FLAGS)

check:
	python3 scripts/install.py check --scope all
	python3 scripts/install_superpowers.py check

clean:
	python3 scripts/install.py clean --scope all
	python3 scripts/install_superpowers.py clean

install-repo:
	python3 scripts/install.py install --scope repo $(AGENTS_INSTALL_FLAGS)

check-repo:
	python3 scripts/install.py check --scope repo

clean-repo:
	python3 scripts/install.py clean --scope repo

install-global:
	python3 scripts/install.py install --scope global $(AGENTS_INSTALL_FLAGS)
	python3 scripts/install_superpowers.py install $(AGENTS_INSTALL_FLAGS)

check-global:
	python3 scripts/install.py check --scope global
	python3 scripts/install_superpowers.py check

clean-global:
	python3 scripts/install.py clean --scope global
	python3 scripts/install_superpowers.py clean

cloudcli-install:
	scripts/install_cloudcli.sh install

cloudcli-check:
	scripts/install_cloudcli.sh check

vendor-sync:
	@if [ -z "$(TOOL)" ] || [ -z "$(REF)" ]; then \
		echo "usage: make vendor-sync TOOL=openspec|superpowers REF=<tag-or-version>"; \
		exit 2; \
	fi
	python3 scripts/vendor_sync.py --tool "$(TOOL)" --ref "$(REF)" $(if $(FORCE),--force,)
