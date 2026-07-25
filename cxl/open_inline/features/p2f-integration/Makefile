# p2f-integration build script — same reproducibility rationale as p2b/p2c/p2d/p2e's Makefiles.
# Reuses the shared OCUDU_BUILD directory; no new OCUDU targets needed beyond what p2b already
# bootstrapped (ocudu_channel_coding pulls in ocudu_ldpc, which includes the segmenter_tx/rx +
# crc_calculator this slice's oracle test needs).

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2D_HOST     := $(abspath $(FEATURE_ROOT)/../p2d-k2-k3/src/host)
P2B_HOST     := $(abspath $(FEATURE_ROOT)/../p2b-k5-k6/src/host)
P2A_ROOT     := $(abspath $(FEATURE_ROOT)/../p2a-scaffold)
P2A_HOST     := $(P2A_ROOT)/src/host

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external

OCUDU_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/ldpc/libocudu_ldpc.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/polar/libocudu_polar.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/short/libocudu_short_block.a \
  $(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_crc_calculator.a \
  $(OCUDU_BUILD)/lib/phy/upper/liblog_likelihood_ratio.a \
  $(OCUDU_BUILD)/lib/support/libocudu_support.a \
  $(OCUDU_BUILD)/lib/support/math/libocudu_support_math.a \
  $(OCUDU_BUILD)/lib/ocuduvec/libocuduvec.a \
  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a \
  $(OCUDU_BUILD)/external/fmt/libfmt.a

# oracle_tx_gen also needs the TX-side modulation/scrambling/ofh-wire libs, on top of the coding
# libs above (channel_coding transitively brings in ldpc_segmenter_tx/ldpc_encoder/ldpc_rate_matcher).
ORACLE_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a \
  $(OCUDU_BUILD)/lib/phy/upper/sequence_generators/libocudu_sequence_generators.a \
  $(OCUDU_BUILD)/lib/ofh/serdes/libocudu_ofh_message_serdes.a \
  $(OCUDU_BUILD)/lib/ofh/ecpri/libocudu_ofh_ecpri.a \
  $(OCUDU_BUILD)/lib/ofh/compression/libocudu_ofh_compression.a \
  $(OCUDU_BUILD)/lib/instrumentation/libocudu_instrumentation.a \
  $(OCUDU_LIBS)

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -Isrc/host
LDFLAGS  := -lOpenCL -lpthread

# pipeline_runner links oi_p2_host.cpp's own full dependency chain (its cross-feature host .cpp
# files), same list as p2a-scaffold/Makefile's HOST_SRCS -- kept in sync by hand (small, stable
# list; see p2a-scaffold/Makefile's own comment on why this isn't a shared variable across features).
RUNNER_SRCS := $(P2A_HOST)/oi_p2_host.cpp $(P2A_HOST)/oi_p2_config.cpp $(P2A_HOST)/oi_p2_buffers.cpp \
              $(P2A_HOST)/oi_oran_preparse.cpp \
              $(P2B_HOST)/oi_p2_gold_init.cpp \
              $(P2D_HOST)/oi_dmrs_ref_seq.cpp \
              src/host/oi_p2_cb_segment.cpp src/host/oi_p2_crc.cpp src/host/oi_p2_ldpc_decode.cpp

.PHONY: all test run-cb-segment run-ldpc-decode run-oracle-tx-gen check-ocudu bootstrap-ocudu clean

all: build/cb_segment_test build/ldpc_decode_test build/oracle_tx_gen build/pipeline_runner

check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_coding libs — run 'make bootstrap-ocudu' first" && exit 1)
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_modulation/ofh libs — run 'make bootstrap-ocudu' first" && exit 1)

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
	cd "$(OCUDU_BUILD)" && cmake --build . --target ocudu_channel_modulation ocudu_ofh_message_serdes -j$(NPROC)

build:
	mkdir -p build

build/cb_segment_test: tests/cb_segment_test.cpp src/host/oi_p2_cb_segment.cpp src/host/oi_p2_crc.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/cb_segment_test.cpp src/host/oi_p2_cb_segment.cpp src/host/oi_p2_crc.cpp \
	  $(OCUDU_LIBS) $(LDFLAGS) -o $@

build/ldpc_decode_test: tests/ldpc_decode_test.cpp src/host/oi_p2_ldpc_decode.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tests/ldpc_decode_test.cpp src/host/oi_p2_ldpc_decode.cpp \
	  $(OCUDU_LIBS) $(LDFLAGS) -o $@

build/oracle_tx_gen: tools/oracle_tx_gen.cpp src/host/oi_p2_cb_segment.cpp src/host/oi_p2_crc.cpp \
                    $(P2D_HOST)/oi_dmrs_ref_seq.cpp $(P2B_HOST)/oi_p2_gold_init.cpp check-ocudu | build
	$(CXX) $(CXXFLAGS) tools/oracle_tx_gen.cpp src/host/oi_p2_cb_segment.cpp src/host/oi_p2_crc.cpp \
	  $(P2D_HOST)/oi_dmrs_ref_seq.cpp $(P2B_HOST)/oi_p2_gold_init.cpp $(ORACLE_LIBS) $(LDFLAGS) -o $@

run-cb-segment: build/cb_segment_test
	cd tests && $(FEATURE_ROOT)/build/cb_segment_test

run-ldpc-decode: build/ldpc_decode_test
	cd tests && $(FEATURE_ROOT)/build/ldpc_decode_test

run-oracle-tx-gen: build/oracle_tx_gen
	cd tests && $(FEATURE_ROOT)/build/oracle_tx_gen 4 1 /tmp/oracle_test.pcap /tmp/oracle_test.json

# pipeline_runner loads its 7 real kernels' .cl sources via oi_p2_host.cpp's own CWD-relative
# paths ("../../p2c-k1/src/kernels/..." etc) -- must run from p2a-scaffold/tests/, same convention
# oi_p2_host.cpp already documents (VERIFICATION.md "kernel-path CWD fragility").
build/pipeline_runner: tools/pipeline_runner.cpp $(RUNNER_SRCS) check-ocudu | build
	$(CXX) -std=c++17 -Wall -Wextra -I$(P2A_HOST) -Isrc/host tools/pipeline_runner.cpp $(RUNNER_SRCS) \
	  -lOpenCL -lpthread -lyaml-cpp -o $@

test: run-cb-segment run-ldpc-decode

# Manual smoke invocation (pipeline_test.py invokes the built binary directly with its own CWD
# management -- this target is for ad-hoc/manual runs only).
run-pipeline-runner: build/pipeline_runner build/oracle_tx_gen
	cd tests && $(FEATURE_ROOT)/build/oracle_tx_gen 4 42 /tmp/oracle_smoke.pcap /tmp/oracle_smoke.json
	cd $(P2A_ROOT)/tests && $(FEATURE_ROOT)/build/pipeline_runner $(P2A_ROOT)/tests/fixtures/mvp_config.yaml /tmp/oracle_smoke.pcap 4

clean:
	rm -rf build
