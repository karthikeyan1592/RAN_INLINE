# p2d-k2-k3 build script — same reproducibility rationale as p2b-k5-k6/Makefile and p2c-k1/Makefile.
# Reuses the shared OCUDU_BUILD directory (open_inline/.build/ocudu) and extends it with the
# channel-estimation/equalization targets K2/K3's oracle tests need.

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2A_HOST     := $(abspath $(FEATURE_ROOT)/../p2a-scaffold/src/host)
P2B_HOST     := $(abspath $(FEATURE_ROOT)/../p2b-k5-k6/src/host)

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

# K2 libs (channel estimator + DM-RS PUSCH ref-seq gen + interpolator, for tests/k2_test.cpp)
OCUDU_K2_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/signal_processors/libocudu_dmrs_pusch.a \
  $(OCUDU_BUILD)/lib/phy/upper/signal_processors/libocudu_channel_estimator.a \
  $(OCUDU_BUILD)/lib/phy/upper/signal_processors/libocudu_dmrs_helper.a \
  $(OCUDU_BUILD)/lib/phy/upper/sequence_generators/libocudu_sequence_generators.a \
  $(OCUDU_BUILD)/lib/phy/generic_functions/libocudu_dft.a \
  $(OCUDU_BUILD)/lib/phy/support/libocudu_phy_support.a \
  $(OCUDU_BUILD)/lib/ran/libocudu_ran.a \
  $(OCUDU_BUILD)/lib/instrumentation/libocudu_instrumentation.a \
  $(OCUDU_BUILD)/lib/support/math/libocudu_support_math.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

# K3 libs (channel equalizer, for tests/k3_test.cpp)
OCUDU_K3_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/equalization/libocudu_channel_equalizer.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -I$(P2A_HOST) -I$(P2B_HOST) -Isrc/host
LDFLAGS  := -lOpenCL -lpthread

.PHONY: all test run-k2 run-k3 check-ocudu bootstrap-ocudu clean

all: build/k2_test build/k3_test

check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/signal_processors/libocudu_channel_estimator.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_estimator/equalizer libs — run 'make bootstrap-ocudu' first" && exit 1)
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/equalization/libocudu_channel_equalizer.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing libocudu_channel_equalizer.a — run 'make bootstrap-ocudu' first" && exit 1)

bootstrap-ocudu:
	@test -d "$(OCUDU_SRC)" || (echo "error: OCUDU_SRC ($(OCUDU_SRC)) not found" && exit 1)
	mkdir -p "$(OCUDU_BUILD)"
	cd "$(OCUDU_BUILD)" && cmake "$(OCUDU_SRC)" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_TESTING=OFF \
	  -DENABLE_UHD=OFF -DENABLE_SIDEKIQ=OFF -DENABLE_ZEROMQ=OFF \
	  -DENABLE_FFTW=OFF -DENABLE_MKL=OFF -DENABLE_FFTZ=OFF -DENABLE_ARMPL=OFF \
	  -DENABLE_DPDK=OFF -DENABLE_LIBNUMA=OFF -DENABLE_BACKWARD=OFF
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_dmrs_pusch -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_channel_estimator -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_phy_support -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_channel_equalizer -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_sequence_generators -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_dft -j$(NPROC)

build:
	mkdir -p build

build/k2_test: tests/k2_test.cpp src/host/oi_dmrs_ref_seq.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k2_test.cpp src/host/oi_dmrs_ref_seq.cpp $(P2B_HOST)/oi_p2_gold_init.cpp \
	  $(OCUDU_K2_LIBS) $(LDFLAGS) -o $@

build/k3_test: tests/k3_test.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k3_test.cpp $(OCUDU_K3_LIBS) $(LDFLAGS) -o $@

run-k2: build/k2_test
	cd tests && $(FEATURE_ROOT)/build/k2_test

run-k3: build/k3_test
	cd tests && $(FEATURE_ROOT)/build/k3_test

test: run-k2 run-k3

clean:
	rm -rf build
