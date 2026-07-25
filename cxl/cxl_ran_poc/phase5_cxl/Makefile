CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE \
          -march=native \
          -I. \
          -I$(BPFTIME_DIR)/third_party/vmlinux/ \
          -I$(BPFTIME_DIR)/build/libbpf/include
LDFLAGS = -lpthread -lrt

BPFTIME_DIR ?= /root/linux_env/cxl/third_party/bpftime
BPFTOOL     ?= $(BPFTIME_DIR)/build/bpftool/bpftool
VMLINUX_INC ?= $(BPFTIME_DIR)/third_party/vmlinux
LIBBPF_A    ?= $(BPFTIME_DIR)/build/libbpf/libbpf.a
LIBBPF_INC  ?= $(BPFTIME_DIR)/build/libbpf

# OpenCL for Phase 2+
OPENCL_CFLAGS  ?= $(shell pkg-config --cflags OpenCL 2>/dev/null || echo "")
OPENCL_LDFLAGS ?= $(shell pkg-config --libs   OpenCL 2>/dev/null || echo "-lOpenCL")

# v7 paths (VM layout — bpftime built in /root/cxl/third_party/bpftime/build)
BPFTIME_V7    ?= /root/cxl/third_party/bpftime
LIBBPF_V7_A   ?= $(BPFTIME_V7)/build/libbpf/libbpf/libbpf.a
LIBBPF_V7_INC ?= $(BPFTIME_V7)/build/libbpf

BENCH_PATH_V7 ?= $(shell find /root/cxl/third_party/srsRAN_Project/build \
                   -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1)

.PHONY: all clean phase1 phase2 phase3 phase4 gate3 gate3-bpf gate-v8 gate-v8-bpf cxl-init

all: phase1

phase1: ring_test consumer

phase2: ring_test consumer consumer_ocl cxl_bit_diff

phase3: ldpc_probe_v5.skel.h ldpc_consumer_v5

phase4: ldpc_probe_v5.skel.h ablation ocl_bench_standalone

# ── Phase 1 ──────────────────────────────────────────────────────────────────

cxl_region.o: cxl_region.c cxl_region.h
	$(CC) $(CFLAGS) -c $< -o $@

ring_test: ring_test.c desc_ring.h cxl_region.o
	$(CC) $(CFLAGS) ring_test.c cxl_region.o -o $@ $(LDFLAGS)

consumer: consumer.c desc_ring.h cxl_region.o
	$(CC) $(CFLAGS) consumer.c cxl_region.o -o $@ $(LDFLAGS)

# ── Phase 2 ──────────────────────────────────────────────────────────────────

consumer_ocl: consumer_ocl.c desc_ring.h cxl_region.o
	$(CC) $(CFLAGS) $(OPENCL_CFLAGS) consumer_ocl.c cxl_region.o \
	  -o $@ $(LDFLAGS) $(OPENCL_LDFLAGS)

cxl_bit_diff: cxl_bit_diff.c cxl_region.o
	$(CC) $(CFLAGS) $(OPENCL_CFLAGS) cxl_bit_diff.c cxl_region.o \
	  -o $@ $(LDFLAGS) $(OPENCL_LDFLAGS)

# ── Phase 3: bpftime descriptor uprobe ───────────────────────────────────────

ldpc_probe_v5.bpf.o: ldpc_probe_v5.bpf.c
	clang -g -O2 -target bpf -D__TARGET_ARCH_x86 \
	  -I$(VMLINUX_INC) -I$(LIBBPF_INC) \
	  -c $< -o $@

ldpc_probe_v5.skel.h: ldpc_probe_v5.bpf.o
	$(BPFTOOL) gen skeleton $< name ldpc_probe_v5 > $@

ldpc_consumer_v5: ldpc_consumer_v5.c ldpc_probe_v5.skel.h desc_ring.h cxl_region.o
	$(CC) -O2 -Wall -std=c11 -D_GNU_SOURCE \
	  -I. -I$(LIBBPF_INC) \
	  ldpc_consumer_v5.c cxl_region.o \
	  -o $@ \
	  $(LIBBPF_A) -lz -lelf -lpthread -lm -ldl

# ── Phase 4: ablation harness ─────────────────────────────────────────────────

ablation: ablation.c ldpc_probe_v5.skel.h desc_ring.h cxl_region.o
	$(CC) -O2 -Wall -std=c11 -D_GNU_SOURCE \
	  -I. -I$(LIBBPF_INC) $(OPENCL_CFLAGS) \
	  ablation.c cxl_region.o \
	  -o $@ \
	  $(LIBBPF_A) -lz -lelf -lpthread -lm -ldl $(OPENCL_LDFLAGS)

ocl_bench_standalone: ocl_bench_standalone.c cxl_region.o
	$(CC) -O2 -Wall -std=c11 -D_GNU_SOURCE \
	  -I. $(OPENCL_CFLAGS) \
	  ocl_bench_standalone.c cxl_region.o \
	  -o $@ \
	  -lpthread -lm $(OPENCL_LDFLAGS)

# ── Gate 3/4: v7 bpftime LLR mover ──────────────────────────────────────────

gate3: gate3-bpf llr_gate3

gate3-bpf: llr_mover.bpf.o

llr_mover.bpf.o: llr_mover.bpf.c vmlinux.h
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
	  -I. -I$(LIBBPF_V7_INC) -I/usr/include \
	  -c $< -o $@

llr_gate3: llr_gate3.c
	$(CC) -O2 -Wall -std=c11 -D_GNU_SOURCE \
	  -I$(LIBBPF_V7_INC) \
	  $< -o $@ \
	  $(LIBBPF_V7_A) \
	  -lz -lelf -lnuma -lpthread \
	  $(OPENCL_CFLAGS) $(OPENCL_LDFLAGS)

# Convenience: derive BENCH_PATH from srsRAN build (print for confirmation)
bench-path:
	@echo "BENCH_PATH=$(BENCH_PATH_V7)"
	@ls -la "$(BENCH_PATH_V7)" 2>/dev/null || echo "NOT FOUND — rebuild srsRAN"

# ── Gate v8: lddc_llr_mover + cxl_init.so + llr_consumer_v8 ──────────────

gate-v8: gate-v8-bpf cxl_init.so llr_consumer_v8

cxl-init: cxl_init.so

cxl_init.so: cxl_init.c
	$(CC) -O2 -Wall -shared -fPIC -D_GNU_SOURCE \
	  cxl_init.c -o cxl_init.so \
	  -lnuma -lrt

gate-v8-bpf: lddc_llr_mover.bpf.o

lddc_llr_mover.bpf.o: lddc_llr_mover.bpf.c vmlinux.h
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
	  -I. -I$(LIBBPF_V7_INC) -I/usr/include \
	  -c $< -o $@

# Alternative: compile without vmlinux.h using vendor headers (GCP host)
lddc_llr_mover.bpf.novmlinux.o: lddc_llr_mover.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -DNO_VMLINUX \
	  -I$(BPFTIME_V7)/vendor/libbpf/include \
	  -I$(BPFTIME_V7)/vendor/libbpf/include/uapi \
	  -c $< -o lddc_llr_mover.bpf.o

llr_consumer_v8: llr_consumer_v8.c
	$(CC) -O2 -Wall -std=c11 -D_GNU_SOURCE \
	  -I$(LIBBPF_V7_INC) \
	  -I../gpu_daemon/ldpc_cl \
	  $< -o $@ \
	  $(LIBBPF_V7_A) \
	  -lz -lelf -lnuma -lrt -lpthread \
	  $(OPENCL_CFLAGS) $(OPENCL_LDFLAGS)

clean:
	rm -f *.o *.bpf.o *.skel.h \
	      ring_test consumer consumer_ocl cxl_bit_diff \
	      ldpc_consumer_v5 ablation ocl_bench_standalone \
	      llr_mover.bpf.o llr_gate3 \
	      lddc_llr_mover.bpf.o llr_consumer_v8 cxl_init.so
