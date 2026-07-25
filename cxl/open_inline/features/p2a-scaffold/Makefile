# p2a-scaffold build script — same reproducibility rationale as p2b/p2c/p2d/p2e/p2f's Makefiles.
# Added 2026-07-23 when oi_p2_host.cpp grew a real, multi-feature dependency chain (p2f's LDPC
# hookup, p2b's Gold-sequence module, p2d's DMRS ref-seq generator) as part of replacing the stub
# kernel chain with the real 8-stage pipeline -- the ad-hoc g++ command used until then no longer
# fit in one head.

CXX ?= g++
NPROC := $(shell nproc)

FEATURE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OCUDU_SRC    ?= $(abspath $(FEATURE_ROOT)/../../../third_party/ocudu)
OCUDU_BUILD  ?= $(abspath $(FEATURE_ROOT)/../../.build/ocudu)
P2B_HOST     := $(abspath $(FEATURE_ROOT)/../p2b-k5-k6/src/host)
P2D_HOST     := $(abspath $(FEATURE_ROOT)/../p2d-k2-k3/src/host)
P2F_HOST     := $(abspath $(FEATURE_ROOT)/../p2f-integration/src/host)

CXXFLAGS := -std=c++17 -Wall -Wextra -Isrc/host
LDFLAGS  := -lOpenCL -lpthread -lyaml-cpp

HOST_SRCS := src/host/oi_p2_host.cpp src/host/oi_p2_config.cpp src/host/oi_p2_buffers.cpp \
            src/host/oi_oran_preparse.cpp \
            $(P2B_HOST)/oi_p2_gold_init.cpp \
            $(P2D_HOST)/oi_dmrs_ref_seq.cpp \
            $(P2F_HOST)/oi_p2_cb_segment.cpp $(P2F_HOST)/oi_p2_crc.cpp $(P2F_HOST)/oi_p2_ldpc_decode.cpp

.PHONY: all test run-host-api run-preparse clean

all: build/host_api_test build/preparse_test

build:
	mkdir -p build

build/host_api_test: tests/host_api_test.cpp $(HOST_SRCS) | build
	$(CXX) $(CXXFLAGS) tests/host_api_test.cpp $(HOST_SRCS) $(LDFLAGS) -o $@

build/preparse_test: tests/preparse_test.cpp src/host/oi_oran_preparse.cpp | build
	$(CXX) $(CXXFLAGS) tests/preparse_test.cpp src/host/oi_oran_preparse.cpp -o $@

# host_api_test loads its 7 real kernels' .cl sources via paths relative to CWD ("../../p2c-k1/
# src/kernels/..." etc, see oi_p2_host.cpp) -- must run from tests/, same convention as every
# other p2* slice's test.
run-host-api: build/host_api_test
	cd tests && $(FEATURE_ROOT)/build/host_api_test

run-preparse: build/preparse_test
	cd tests && $(FEATURE_ROOT)/build/preparse_test

test: run-host-api run-preparse

clean:
	rm -rf build
