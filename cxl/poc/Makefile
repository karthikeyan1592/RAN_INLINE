# cxl/poc/Makefile — build Phase 2-4 PoC binaries inside the QEMU VM
#
# Prerequisites (already installed in VM):
#   gcc, clang, bpftool, libOpenCL, libnuma, libbpf
#
# Usage:
#   make all          # build everything
#   make phase2       # CXL + OCL sentinel test
#   make phase3       # compile BPF program
#   make phase4       # E2E controller
#   make run-phase2   # run sentinel test
#   make clean

CC      ?= gcc
CLANG   ?= clang
BPFTOOL ?= bpftool

CFLAGS  = -O2 -g -Wall -Wextra
LDFLAGS = -lOpenCL -lnuma -lpthread

# Path to kernel BTF for BPF compilation
BTF_VMLINUX ?= /sys/kernel/btf/vmlinux

# BPF target arch
BPF_ARCH = x86

.PHONY: all phase2 phase3 phase4 run-phase2 clean

all: phase2 phase3 phase4

# ── Phase 2: CXL + OpenCL sentinel test ────────────────────────────────────
phase2: phase2_cxl_ocl_sentinel

phase2_cxl_ocl_sentinel: phase2_cxl_ocl_sentinel.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "BUILD OK: phase2_cxl_ocl_sentinel"

run-phase2: phase2_cxl_ocl_sentinel
	./phase2_cxl_ocl_sentinel

# ── Phase 3: BPF program ────────────────────────────────────────────────────
phase3: ldpc_llr_mover.bpf.o ldpc_llr_mover.skel.h

vmlinux.h:
	$(BPFTOOL) btf dump file $(BTF_VMLINUX) format c > vmlinux.h
	@echo "Generated vmlinux.h"

ldpc_llr_mover.bpf.o: ldpc_llr_mover.bpf.c vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(BPF_ARCH) \
		-I. -I/usr/include \
		-c $< -o $@
	@echo "BPF compile OK: $@"

ldpc_llr_mover.skel.h: ldpc_llr_mover.bpf.o
	$(BPFTOOL) gen skeleton $< > $@
	@echo "Skeleton OK: $@"

# ── Phase 4: E2E controller ─────────────────────────────────────────────────
phase4: e2e_runner

e2e_runner: e2e_runner.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "BUILD OK: e2e_runner"

# ── Convenience: run full E2E ───────────────────────────────────────────────
LDPC_BIN    ?= /root/cxl/third_party/srsRAN_Project/build/tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark
BPFTIME_LIB ?= /root/cxl/third_party/bpftime/build/runtime/agent/libbpftime-agent.so
BPFTIME_LIBBPF ?= /root/cxl/third_party/bpftime/build/libbpf/libbpf/libbpf.a

# ── Phase 4b: kernel uprobe loader (no bpftime needed, uses kernel BPF) ────
phase4b: ldpc_uprobe_loader

ldpc_uprobe_loader: ldpc_uprobe_loader.c
	$(CC) $(CFLAGS) -o $@ $< \
		-I/root/cxl/third_party/bpftime/build/libbpf \
		$(BPFTIME_LIBBPF) \
		-lelf -lz -lnuma
	@echo "BUILD OK: ldpc_uprobe_loader"

run-e2e: phase4b ldpc_llr_mover.bpf.o
	./ldpc_uprobe_loader \
		--ldpc   $(LDPC_BIN) \
		--bpf    ldpc_llr_mover.bpf.o \
		--offset 0x2fef0 \
		--reps   20 \
		--output e2e_gcp.csv

clean:
	rm -f phase2_cxl_ocl_sentinel e2e_runner ldpc_uprobe_loader \
	      ldpc_llr_mover.bpf.o ldpc_llr_mover.skel.h vmlinux.h \
	      ldpc_llr_mover_nokcore.bpf.c ldpc_llr_mover_nokcore.bpf.o ldpc_llr_mover_nokcore.skel.h
