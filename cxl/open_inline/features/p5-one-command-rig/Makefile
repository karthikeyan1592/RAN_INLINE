SHELL := /bin/bash
FEATURE_ROOT := $(shell cd "$(dir $(lastword $(MAKEFILE_LIST)))" && pwd)
REPO_ROOT := $(abspath $(FEATURE_ROOT)/../..)

OI_TIER ?= sim
OI_P5_TIMEOUT_SCALE ?= 1.0
OI_P5_RUN_ID ?=
OI_P5_ARTIFACTS_DIR ?= $(REPO_ROOT)/artifacts/p5
OI_P5_ONLY_PHASE ?=
OI_P5_KEEP_UP ?= 0

.PHONY: all simtest simtest-keep-up simtest-clean test test-g1 lint clean

all: test

simtest:
	python3 $(FEATURE_ROOT)/helpers/simtest_runner.py \
		--tier $(OI_TIER) \
		--timeout-scale $(OI_P5_TIMEOUT_SCALE) \
		--artifacts-dir $(OI_P5_ARTIFACTS_DIR) \
		--root $(REPO_ROOT) \
		$(if $(OI_P5_RUN_ID),--run-id $(OI_P5_RUN_ID),) \
		$(if $(OI_P5_ONLY_PHASE),--only-phase $(OI_P5_ONLY_PHASE),)

simtest-keep-up:
	python3 $(FEATURE_ROOT)/helpers/simtest_runner.py \
		--tier $(OI_TIER) \
		--timeout-scale $(OI_P5_TIMEOUT_SCALE) \
		--artifacts-dir $(OI_P5_ARTIFACTS_DIR) \
		--root $(REPO_ROOT) \
		--keep-up \
		$(if $(OI_P5_RUN_ID),--run-id $(OI_P5_RUN_ID),) \
		$(if $(OI_P5_ONLY_PHASE),--only-phase $(OI_P5_ONLY_PHASE),)

simtest-clean:
	rm -rf $(OI_P5_ARTIFACTS_DIR)

# P5-G1: mock-suite fixture test (fully local, no p1-p4 real suites required)
test-g1:
	python3 $(FEATURE_ROOT)/tests/test_p5_g1.py

# Schema + unit tests (discovery, classification, rollup, ledger schema, compare_ledgers)
test-unit:
	python3 $(FEATURE_ROOT)/tests/test_discover_suites.py
	python3 $(FEATURE_ROOT)/tests/test_ledger_build.py
	python3 $(FEATURE_ROOT)/tests/test_compare_ledgers.py

test: test-unit test-g1

lint:
	bash $(FEATURE_ROOT)/helpers/lint_no_perf.sh $(FEATURE_ROOT)

clean: simtest-clean
