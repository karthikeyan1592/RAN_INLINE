# p1-ran-baseline build script — same reproducibility rationale as p2's Makefiles. Builds the
# synthetic eCPRI fixture generator (tools/synth_ecpri_gen.cpp), the only compiled artifact this
# zero-project-code feature needs (P1-R11: nothing here runs inside a rig container; this binary
# is host-side test tooling only, same status as p2's oracle_tx_gen.cpp).

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

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

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES)
LDFLAGS  := -lpthread

.PHONY: all test check-ocudu bootstrap-ocudu clean run-synth-ecpri-gen

all: build/synth_ecpri_gen

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

build:
	mkdir -p build

build/synth_ecpri_gen: tools/synth_ecpri_gen.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tools/synth_ecpri_gen.cpp $(OCUDU_LIBS) $(LDFLAGS) -o $@

run-synth-ecpri-gen: build/synth_ecpri_gen
	./build/synth_ecpri_gen tests/fixtures/synth_ecpri.pcap tests/fixtures/synth_ecpri.json

test: run-synth-ecpri-gen
	bash tests/classifier_test.sh

clean:
	rm -rf build
