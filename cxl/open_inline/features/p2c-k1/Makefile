# p2c-k1 build script — same reproducibility rationale as p2b-k5-k6/Makefile (see that file's
# header comment). Reuses the SAME shared OCUDU_BUILD directory p2b bootstrapped
# (open_inline/.build/ocudu by default) and extends it with the additional OCUDU targets K1's
# oracle test needs (the eCPRI/O-RAN CUS serdes layer, not built by any earlier slice).
#
# Quick start:
#   make bootstrap-ocudu   # safe to re-run; only builds what's missing
#   make test

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2A_HOST     := $(abspath $(FEATURE_ROOT)/../p2a-scaffold/src/host)

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

# Link order matters the same way it did in p2b: a lib needs to come before the libs whose
# undefined symbols it resolves. ocudu_ofh_message_serdes/ecpri/compression/instrumentation are
# new to this slice (no earlier p2* sub-feature built the ofh/ tree at all).
OCUDU_LIBS := \
  $(OCUDU_BUILD)/lib/ofh/serdes/libocudu_ofh_message_serdes.a \
  $(OCUDU_BUILD)/lib/ofh/ecpri/libocudu_ofh_ecpri.a \
  $(OCUDU_BUILD)/lib/ofh/compression/libocudu_ofh_compression.a \
  $(OCUDU_BUILD)/lib/instrumentation/libocudu_instrumentation.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/support/math/libocudu_support_math.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -I$(P2A_HOST)
LDFLAGS  := -lOpenCL -lpthread

.PHONY: all test run-k1 check-ocudu bootstrap-ocudu clean

all: build/k1_test

check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/ofh/serdes/libocudu_ofh_message_serdes.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing the ofh/ libs — run 'make bootstrap-ocudu' first" && exit 1)

bootstrap-ocudu:
	@test -d "$(OCUDU_SRC)" || (echo "error: OCUDU_SRC ($(OCUDU_SRC)) not found" && exit 1)
	mkdir -p "$(OCUDU_BUILD)"
	cd "$(OCUDU_BUILD)" && cmake "$(OCUDU_SRC)" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DBUILD_TESTING=OFF \
	  -DENABLE_UHD=OFF -DENABLE_SIDEKIQ=OFF -DENABLE_ZEROMQ=OFF \
	  -DENABLE_FFTW=OFF -DENABLE_MKL=OFF -DENABLE_FFTZ=OFF -DENABLE_ARMPL=OFF \
	  -DENABLE_DPDK=OFF -DENABLE_LIBNUMA=OFF -DENABLE_BACKWARD=OFF
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_ofh_message_serdes -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_ofh_ecpri -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_ofh_compression -j$(NPROC)
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_instrumentation -j$(NPROC)

build:
	mkdir -p build

build/k1_test: tests/k1_test.cpp $(P2A_HOST)/oi_oran_preparse.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/k1_test.cpp $(P2A_HOST)/oi_oran_preparse.cpp $(OCUDU_LIBS) $(LDFLAGS) -o $@

# k1_test locates k1_depacketizer.cl via a path relative to CWD ("../src/kernels/...") and passes
# a runtime OpenCL build-option string with CWD-relative -I flags to clBuildProgram -- both baked
# in assuming CWD == this feature's tests/ directory (same convention as p2b-k5-k6).
run-k1: build/k1_test
	cd tests && $(FEATURE_ROOT)/build/k1_test

test: run-k1

clean:
	rm -rf build
