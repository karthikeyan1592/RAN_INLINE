# p4-phy-l2-seam build script. The ring/producer/l2-stub library itself is pure C11, no
# OCUDU/OpenCL dependency at all (consumes p2's drain record only through the opaque
# oi_seam_p2_record_view_t field-copy, never links against p2's own headers/libs).
#
# gpu_phy_seam_bridge (tools/, added to resolve LLD Q1) is the one exception: it's gpu-phy's own
# event loop, so it genuinely needs OCUDU/OpenCL + p2's real pipeline + p3's real ingest module,
# same shared OCUDU_BUILD dir p2b/p2f/p3 already populate (bootstrap-ocudu below reuses p3's own
# delegation to p2f's target, same precedent).

CC ?= gcc
CXX ?= g++
CFLAGS := -std=c11 -Wall -Wextra -Isrc -D_POSIX_C_SOURCE=200809L
LDFLAGS := -lpthread

SRCS := src/oi_seam.c src/oi_seam_producer.c src/oi_l2_validate.c

FEATURE_ROOT := $(abspath .)
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2A_ROOT     := $(abspath $(FEATURE_ROOT)/../p2a-scaffold)
P2A_HOST     := $(P2A_ROOT)/src/host
P2B_HOST     := $(abspath $(FEATURE_ROOT)/../p2b-k5-k6/src/host)
P2D_HOST     := $(abspath $(FEATURE_ROOT)/../p2d-k2-k3/src/host)
P2F_ROOT     := $(abspath $(FEATURE_ROOT)/../p2f-integration)
P2F_HOST     := $(P2F_ROOT)/src/host
P3_ROOT      := $(abspath $(FEATURE_ROOT)/../p3-live-tap-ul-inject)
P3_HOST      := $(P3_ROOT)/src/host

OCUDU_INCLUDES := -I$(OCUDU_SRC)/include -I$(OCUDU_SRC)/external/fmt/include -I$(OCUDU_SRC)/external
# No explicit OCUDU_LIBS needed at link time -- matches p3-live-tap-ul-inject/Makefile's own real,
# working build/bit_exact_harness recipe (verified directly this session: links clean with just
# -lOpenCL -lpthread -lyaml-cpp -lz, no static OCUDU_LIBS list, for the identical P2_HOST_SRCS set).
BRIDGE_CXXFLAGS := -std=c++17 -Wall -Wextra $(OCUDU_INCLUDES) -Isrc -I$(P2A_HOST) -I$(P3_HOST)
BRIDGE_LDLIBS := -lOpenCL -lpthread -lyaml-cpp -lz

# Exactly p3-live-tap-ul-inject/Makefile's own P2_HOST_SRCS list (kept in sync by hand, same
# convention that Makefile itself documents) -- this bridge needs the identical real p2 pipeline
# chain p3's bit_exact_harness already links.
P2_HOST_SRCS := $(P2A_HOST)/oi_p2_host.cpp $(P2A_HOST)/oi_p2_config.cpp $(P2A_HOST)/oi_p2_buffers.cpp \
               $(P2A_HOST)/oi_oran_preparse.cpp \
               $(P2B_HOST)/oi_p2_gold_init.cpp \
               $(P2D_HOST)/oi_dmrs_ref_seq.cpp \
               $(P2F_HOST)/oi_p2_cb_segment.cpp $(P2F_HOST)/oi_p2_crc.cpp $(P2F_HOST)/oi_p2_ldpc_decode.cpp

.PHONY: all test run-struct-layout-test run-ordering-test run-wrap-test run-restart-test run-producer-test bootstrap-ocudu clean

all: build/struct_layout_test build/ordering_test build/wrap_test build/restart_test build/producer_test build/l2_stub_main

# gpu_phy_seam_bridge is NOT in `all` (needs bootstrap-ocudu first, same gate p3 already uses) --
# build explicitly via `make build/gpu_phy_seam_bridge` once `make bootstrap-ocudu` has run.
bootstrap-ocudu:
	$(MAKE) -C $(P3_ROOT) bootstrap-ocudu

# oi_seam.c/oi_seam_producer.c use C11 _Atomic, and gpu_phy_seam_bridge.c #includes oi_seam.h --
# g++ rejects C11 `_Atomic` type-specifier syntax even via a __cplusplus-guarded header (real
# compile error hit and fixed this session), so the bridge itself is plain C11 (matching
# l2_stub_main.c's own language choice), compiled to an object via $(CC), then linked together
# with g++ (needed because the p2/p3 sources it calls into are real C++ translation units with
# libstdc++ dependencies -- standard mixed C/C++ linking: compile each side with its own compiler,
# link with the C++ linker driver).
build/oi_seam.o: src/oi_seam.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/oi_seam_producer.o: src/oi_seam_producer.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/gpu_phy_seam_bridge.o: tools/gpu_phy_seam_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Isrc -I$(P2A_HOST) -I$(P3_HOST) -D_POSIX_C_SOURCE=200809L -c $< -o $@

build/gpu_phy_seam_bridge: build/gpu_phy_seam_bridge.o $(P3_HOST)/oi_ingest_af_packet.cpp $(P2_HOST_SRCS) build/oi_seam.o build/oi_seam_producer.o | build
	@test -f "$(OCUDU_BUILD)/lib/phy/upper/channel_coding/libocudu_channel_coding.a" || \
	  (echo "error: OCUDU_BUILD ($(OCUDU_BUILD)) is missing channel_coding libs — run 'make bootstrap-ocudu' first" && exit 1)
	$(CXX) $(BRIDGE_CXXFLAGS) build/gpu_phy_seam_bridge.o $(P3_HOST)/oi_ingest_af_packet.cpp \
	  $(P2_HOST_SRCS) build/oi_seam.o build/oi_seam_producer.o $(OCUDU_LIBS) $(BRIDGE_LDLIBS) -o $@

build:
	mkdir -p build

build/struct_layout_test: tests/struct_layout_test.c src/oi_seam_ring.h | build
	$(CC) $(CFLAGS) tests/struct_layout_test.c -o $@

build/producer_test: tests/producer_test.c src/oi_seam_producer.c | build
	$(CC) $(CFLAGS) tests/producer_test.c src/oi_seam_producer.c -o $@

build/ordering_test: tests/ordering_test.c $(SRCS) | build
	$(CC) $(CFLAGS) tests/ordering_test.c $(SRCS) $(LDFLAGS) -o $@

build/wrap_test: tests/wrap_test.c $(SRCS) | build
	$(CC) $(CFLAGS) tests/wrap_test.c $(SRCS) $(LDFLAGS) -o $@

build/restart_test: tests/restart_test.c $(SRCS) | build
	$(CC) $(CFLAGS) tests/restart_test.c $(SRCS) $(LDFLAGS) -o $@

build/l2_stub_main: src/l2_stub_main.c $(SRCS) | build
	$(CC) $(CFLAGS) src/l2_stub_main.c $(SRCS) $(LDFLAGS) -o $@

run-struct-layout-test: build/struct_layout_test
	./build/struct_layout_test

run-producer-test: build/producer_test
	./build/producer_test

run-ordering-test: build/ordering_test
	./build/ordering_test

run-wrap-test: build/wrap_test
	./build/wrap_test

run-restart-test: build/restart_test
	./build/restart_test

test: run-struct-layout-test run-producer-test run-ordering-test run-wrap-test run-restart-test

clean:
	rm -rf build
