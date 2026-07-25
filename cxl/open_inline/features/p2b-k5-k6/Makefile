# p2b-k5-k6 build script — exists specifically to close the reproducibility gap flagged during
# independent re-verification (2026-07-22): the OCUDU build recipe and static-link order used to
# get k5_test/k6_test passing were reconstructed by hand from p0-rig-scaffold's Dockerfile and were
# not written down anywhere in this slice. This Makefile is that write-down, made runnable.
#
# Quick start:
#   make bootstrap-ocudu   # only needed once (or after OCUDU_BUILD has been wiped, e.g. a
#                          # /tmp-scratch session dir getting cleared — this happened twice
#                          # during this slice's own development)
#   make test              # builds + runs both k5_test and k6_test, from the correct cwd
#
# OCUDU_BUILD defaults to a path under open_inline/ (NOT /tmp) precisely so it survives across
# sessions; override with `make test OCUDU_BUILD=/some/other/build/dir` if you already have one
# (e.g. a fuller build done for another slice).

CXX ?= g++
NPROC := $(shell nproc)

# --- Paths -------------------------------------------------------------------------------------
FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)

# --- OCUDU include paths -------------------------------------------------------------------------
OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

# --- OCUDU static libs, in the link order p0-rig-scaffold's Dockerfile uses (channel_coding BEFORE
# ldpc matters: ldpc's undefined symbols are resolved by channel_coding_factories.cpp.o, which the
# linker only looks at once, so it must come first) + sequence_generators (K5-only; not part of
# p0's own Dockerfile build, needed here for the first time this slice). Getting this order wrong
# produces undefined-reference linker errors, not a silent bad build — annoying but not dangerous.
OCUDU_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/ldpc/libocudu_ldpc.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/polar/libocudu_polar.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/short/libocudu_short_block.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_crc_calculator.a \
  $(OCUDU_BUILD)/lib/phy/upper/sequence_generators/libocudu_sequence_generators.a \
  $(OCUDU_BUILD)/lib/phy/upper/liblog_likelihood_ratio.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/support/math/libocudu_support_math.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -I$(FEATURE_ROOT)/src/host
LDFLAGS  := -lOpenCL -lpthread

.PHONY: all test run-k5 run-k6 check-ocudu bootstrap-ocudu clean

all: build/k5_test build/k6_test

# --- Sanity check before attempting to link, so a missing OCUDU_BUILD gives a clear one-line
# instruction instead of a wall of undefined-reference errors.
check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) has no built libs — run 'make bootstrap-ocudu' first" && exit 1)
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/sequence_generators/libocudu_sequence_generators.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing libocudu_sequence_generators.a — run 'make bootstrap-ocudu' first" && exit 1)

bootstrap-ocudu:
	@test -d "$(OCUDU_SRC)" || (echo "error: OCUDU_SRC ($(OCUDU_SRC)) not found" && exit 1)
	mkdir -p "$(OCUDU_BUILD)"
	cd "$(OCUDU_BUILD)" && cmake "$(OCUDU_SRC)" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_TESTING=OFF \
	  -DENABLE_UHD=OFF -DENABLE_SIDEKIQ=OFF -DENABLE_ZEROMQ=OFF \
	  -DENABLE_FFTW=OFF -DENABLE_MKL=OFF -DENABLE_FFTZ=OFF -DENABLE_ARMPL=OFF \
	  -DENABLE_DPDK=OFF -DENABLE_LIBNUMA=OFF -DENABLE_BACKWARD=OFF
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_channel_coding -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_sequence_generators -j$(NPROC)

build:
	mkdir -p build

build/k5_test: tests/k5_test.cpp src/host/oi_p2_gold_init.cpp src/host/oi_p2_gold_init.h check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k5_test.cpp src/host/oi_p2_gold_init.cpp $(OCUDU_LIBS) $(LDFLAGS) -o $@

build/k6_test: tests/k6_test.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k6_test.cpp $(OCUDU_LIBS) $(LDFLAGS) -o $@

# k5_test/k6_test locate their .cl source via a path relative to CWD ("../src/kernels/...") and
# pass a runtime OpenCL build-option string with a CWD-relative -I ("-I../../p2a-scaffold/src/
# kernels") to clBuildProgram — both baked in assuming CWD == this feature's tests/ directory.
# That assumption is the whole reason this Makefile always cd's into tests/ before running rather
# than leaving it to whoever calls the binary.
run-k5: build/k5_test
	cd tests && $(FEATURE_ROOT)/build/k5_test

run-k6: build/k6_test
	cd tests && $(FEATURE_ROOT)/build/k6_test

test: run-k5 run-k6

clean:
	rm -rf build
