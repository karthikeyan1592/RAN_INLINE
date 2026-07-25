# p2e-k4 build script — same reproducibility rationale as p2b/p2c/p2d's Makefiles.
# Reuses the shared OCUDU_BUILD directory and extends it with the channel_modulation target.

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

OCUDU_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a \
  $(OCUDU_BUILD)/lib/phy/upper/liblog_likelihood_ratio.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES)
LDFLAGS  := -lOpenCL -lpthread

.PHONY: all test run-k4 check-ocudu bootstrap-ocudu clean

all: build/k4_test

check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing libocudu_channel_modulation.a — run 'make bootstrap-ocudu' first" && exit 1)

bootstrap-ocudu:
	@test -d "$(OCUDU_SRC)" || (echo "error: OCUDU_SRC ($(OCUDU_SRC)) not found" && exit 1)
	mkdir -p "$(OCUDU_BUILD)"
	cd "$(OCUDU_BUILD)" && cmake "$(OCUDU_SRC)" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_TESTING=OFF \
	  -DENABLE_UHD=OFF -DENABLE_SIDEKIQ=OFF -DENABLE_ZEROMQ=OFF \
	  -DENABLE_FFTW=OFF -DENABLE_MKL=OFF -DENABLE_FFTZ=OFF -DENABLE_ARMPL=OFF \
	  -DENABLE_DPDK=OFF -DENABLE_LIBNUMA=OFF -DENABLE_BACKWARD=OFF
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_channel_modulation -j$(NPROC)

build:
	mkdir -p build

build/k4_test: tests/k4_test.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k4_test.cpp $(OCUDU_LIBS) $(LDFLAGS) -o $@

run-k4: build/k4_test
	cd tests && $(FEATURE_ROOT)/build/k4_test

test: run-k4

clean:
	rm -rf build
