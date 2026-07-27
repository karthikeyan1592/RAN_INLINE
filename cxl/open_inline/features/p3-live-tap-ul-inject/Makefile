# p3-live-tap-ul-inject build script — same reproducibility rationale as p2b/p2c/p2d/p2e/p2f's
# Makefiles. Reuses the shared OCUDU_BUILD directory; no new OCUDU targets needed beyond what
# p2f already bootstrapped (this feature's osg_gen links the same oracle-TX-chain libs via the
# shared oi_oracle_pack library p2f-integration owns).

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2A_ROOT     := $(abspath $(FEATURE_ROOT)/../p2a-scaffold)
P2A_HOST     := $(P2A_ROOT)/src/host
P2B_HOST     := $(abspath $(FEATURE_ROOT)/../p2b-k5-k6/src/host)
P2D_HOST     := $(abspath $(FEATURE_ROOT)/../p2d-k2-k3/src/host)
P2F_ROOT     := $(abspath $(FEATURE_ROOT)/../p2f-integration)
P2F_HOST     := $(P2F_ROOT)/src/host

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

# osg_gen needs the TX-side modulation/scrambling/ofh-compression libs (same set oracle_tx_gen
# needs minus the eCPRI/serdes wire-framing libs, which osg_gen never touches -- it writes raw
# grid_payload bytes, not eCPRI/O-RAN frames, see osg_gen.cpp's header).
ORACLE_LIBS := \
  $(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a \
  $(OCUDU_BUILD)/lib/phy/upper/sequence_generators/libocudu_sequence_generators.a \
  $(OCUDU_BUILD)/lib/ofh/compression/libocudu_ofh_compression.a \
  $(OCUDU_BUILD)/lib/instrumentation/libocudu_instrumentation.a \
  $(OCUDU_LIBS)

CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -Isrc/host
LDFLAGS  := -lOpenCL -lpthread -lz

ORACLE_PACK_SRCS := $(P2F_HOST)/oi_oracle_pack.cpp $(P2F_ROOT)/src/host/oi_p2_cb_segment.cpp \
                   $(P2F_ROOT)/src/host/oi_p2_crc.cpp \
                   $(P2D_HOST)/oi_dmrs_ref_seq.cpp $(P2B_HOST)/oi_p2_gold_init.cpp

# Real p2 pipeline host chain (matches p2a-scaffold/Makefile's own HOST_SRCS / p2f-integration's
# RUNNER_SRCS lists -- kept in sync by hand, same convention those Makefiles already document).
P2_HOST_SRCS := $(P2A_HOST)/oi_p2_host.cpp $(P2A_HOST)/oi_p2_config.cpp $(P2A_HOST)/oi_p2_buffers.cpp \
               $(P2A_HOST)/oi_oran_preparse.cpp \
               $(P2B_HOST)/oi_p2_gold_init.cpp \
               $(P2D_HOST)/oi_dmrs_ref_seq.cpp \
               $(P2F_HOST)/oi_p2_cb_segment.cpp $(P2F_HOST)/oi_p2_crc.cpp $(P2F_HOST)/oi_p2_ldpc_decode.cpp

.PHONY: all test run-osg-format-test run-osg-loader-crosscheck-test run-ingest-af-packet-test \
       run-harness-calibrate-test run-pcap-comparator-test check-ocudu bootstrap-ocudu clean

all: build/osg_gen build/osg_format_test build/osg_loader_crosscheck_test build/ingest_af_packet_test \
    build/pcap_comparator build/harness_calibrate_test build/pcap_comparator_test build/bit_exact_harness

check-ocudu:
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_coding libs — run 'make bootstrap-ocudu' first" && exit 1)
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_modulation/libocudu_channel_modulation.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_modulation/ofh libs — run 'make bootstrap-ocudu' first" && exit 1)

# Same OCUDU_BUILD dir p2b/p2f already populate — reuse only, this feature adds no new targets.
bootstrap-ocudu:
	@$(MAKE) -C $(P2F_ROOT) bootstrap-ocudu

build:
	mkdir -p build

build/osg_gen: tools/osg_gen.cpp src/host/oi_osg_format.cpp src/host/oi_osg_schedule.h \
              $(ORACLE_PACK_SRCS) check-ocudu | build
	$(CXX) $(CXXFLAGS) -I$(P2F_ROOT)/src/host tools/osg_gen.cpp src/host/oi_osg_format.cpp $(ORACLE_PACK_SRCS) \
	  $(ORACLE_LIBS) $(LDFLAGS) -o $@

build/osg_format_test: tests/osg_format_test.cpp src/host/oi_osg_format.cpp src/host/oi_osg_schedule.h | build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc/host tests/osg_format_test.cpp src/host/oi_osg_format.cpp -lz -o $@

run-osg-format-test: build/osg_format_test
	./build/osg_format_test

# Compiles the ru_emulator patch's OWN loader file (patches/files/ru_emulator_oracle_grid.cpp)
# directly -- proves it's real, working, self-contained code, not just diff text -- and
# cross-checks it against this project's own writer/reader (oi_osg_format).
build/osg_loader_crosscheck_test: tests/osg_loader_crosscheck_test.cpp patches/files/ru_emulator_oracle_grid.cpp \
                                  src/host/oi_osg_format.cpp check-ocudu | build
	$(CXX) -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -Ipatches/files -Isrc/host \
	  tests/osg_loader_crosscheck_test.cpp patches/files/ru_emulator_oracle_grid.cpp src/host/oi_osg_format.cpp \
	  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a $(OCUDU_BUILD)/external/fmt/libfmt.a -lz -o $@

run-osg-loader-crosscheck-test: build/osg_loader_crosscheck_test
	./build/osg_loader_crosscheck_test

run-osg-gen: build/osg_gen
	mkdir -p /tmp/osg_test
	./build/osg_gen 4 1 0x4601 0 /tmp/osg_test

# P3-U3: real veth pair + real captured corpus + real oi_p2_pipeline. Requires root (veth
# creation, AF_PACKET raw sockets) and must run from tests/ (mvp_config.yaml's kernel .cl source
# paths are CWD-relative, same convention as pipeline_runner.cpp -- see that Makefile's comment).
build/ingest_af_packet_test: tests/ingest_af_packet_test.cpp src/host/oi_ingest_af_packet.cpp \
                            $(P2_HOST_SRCS) check-ocudu | build
	$(CXX) -std=c++17 -Wall -Wextra -I$(P2A_HOST) -Isrc/host \
	  tests/ingest_af_packet_test.cpp src/host/oi_ingest_af_packet.cpp $(P2_HOST_SRCS) \
	  -lOpenCL -lpthread -lyaml-cpp -o $@

run-ingest-af-packet-test: build/ingest_af_packet_test
	cd tests && $(FEATURE_ROOT)/build/ingest_af_packet_test

# M4: pcap byte-comparator (P3-U1/P3-R3). Needs only oi_oran_preparse.cpp (real, shared parser)
# + oi_osg_format.cpp -- no OpenCL/pipeline linkage, this tool never touches oi_p2_host.
build/pcap_comparator: tools/pcap_comparator.cpp src/host/oi_osg_format.cpp src/host/oi_harness_calibrate.h \
                       $(P2A_HOST)/oi_oran_preparse.cpp | build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc/host -I$(P2A_HOST) \
	  tools/pcap_comparator.cpp src/host/oi_osg_format.cpp $(P2A_HOST)/oi_oran_preparse.cpp -lz -o $@

build/harness_calibrate_test: tests/harness_calibrate_test.cpp src/host/oi_harness_calibrate.h | build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc/host tests/harness_calibrate_test.cpp -o $@

run-harness-calibrate-test: build/harness_calibrate_test
	./build/harness_calibrate_test

build/pcap_comparator_test: tests/pcap_comparator_test.cpp src/host/oi_osg_format.cpp build/pcap_comparator | build
	$(CXX) -std=c++17 -Wall -Wextra -Isrc/host -I$(P2A_HOST) \
	  tests/pcap_comparator_test.cpp src/host/oi_osg_format.cpp -lz -o $@

run-pcap-comparator-test: build/pcap_comparator_test
	./build/pcap_comparator_test

# P3-R4 schema regression: compiles the PATCHED ru_emulator_cli11_schema.cpp + ru_emulator_
# appconfig.h (patches/files/, the exact patch content) directly against real OCUDU CLI11/YAML
# support libs. -Ipatches/files comes FIRST so the patched appconfig.h shadows the upstream one;
# ru_emulator_cli11_schema.h/helpers.h (unchanged by the patch) resolve from the real ofh dir.
OFH_DIR := $(OCUDU_SRC)/apps/examples/ofh
build/patch_schema_regression_test: tests/patch_schema_regression_test.cpp \
                                    patches/files/ru_emulator_appconfig.h patches/files/ru_emulator_cli11_schema.cpp \
                                    check-ocudu | build
	$(CXX) -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -Ipatches/files -I$(OFH_DIR) \
	  tests/patch_schema_regression_test.cpp patches/files/ru_emulator_cli11_schema.cpp \
	  $(OCUDU_BUILD)/lib/support/libocudu_yaml_util.a $(OCUDU_BUILD)/lib/support/libocudu_support.a \
	  $(OCUDU_BUILD)/lib/ocudulog/libocudulog.a $(OCUDU_BUILD)/external/fmt/libfmt.a -lyaml-cpp -o $@

run-patch-schema-regression-test: build/patch_schema_regression_test
	./build/patch_schema_regression_test

# Real bug found+fixed 2026-07-26, live on GCP: the manual ru_cfg->emu_cfg conversion in main()
# silently dropped oracle_injection (config parsed fine, files present+valid, injection never
# happened at runtime). ru_emulator_config lives inside ru_emulator.cpp itself, not a header, so
# unlike the CLI11 schema this isn't unit-testable without a disproportionate refactor -- this
# test instead applies the real patch to a fresh pristine OCUDU copy and inspects the real
# resulting source directly. See the test's own header comment for the full account.
run-patch-conversion-regression-test:
	bash tests/patch_conversion_regression_test.sh

# M5: live bit-exact harness driver (P3-I1's core, DEFERRED as a full run -- see
# DEFERRED_LIVE_GATES.md; this target only builds it, compile-verified against the real p2
# pipeline + M3's ingest module, matching this session's local-testing scope).
build/bit_exact_harness: tools/bit_exact_harness.cpp src/host/oi_ingest_af_packet.cpp src/host/oi_osg_format.cpp \
                        src/host/oi_harness_calibrate.h $(P2_HOST_SRCS) check-ocudu | build
	$(CXX) -std=c++17 -Wall -Wextra -I$(P2A_HOST) -Isrc/host \
	  tools/bit_exact_harness.cpp src/host/oi_ingest_af_packet.cpp src/host/oi_osg_format.cpp $(P2_HOST_SRCS) \
	  -lOpenCL -lpthread -lyaml-cpp -lz -o $@

test: run-osg-format-test run-osg-loader-crosscheck-test run-ingest-af-packet-test run-harness-calibrate-test run-pcap-comparator-test run-patch-schema-regression-test run-patch-conversion-regression-test

clean:
	rm -rf build
