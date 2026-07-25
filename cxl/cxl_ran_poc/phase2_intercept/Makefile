BPFTIME     := /root/linux_env/cxl/third_party/bpftime
BPFTIME_BLD := $(BPFTIME)/build

LIBBPF_INC  := $(BPFTIME_BLD)/libbpf
LIBBPF_A    := $(BPFTIME_BLD)/libbpf/libbpf.a

BPFTOOL     := $(BPFTIME_BLD)/bpftool/bpftool
VMLINUX_INC := $(BPFTIME)/third_party/vmlinux

OAI_LDPC    := /root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build
LIBLDPC_SO  := $(OAI_LDPC)/libldpc.so

CL_KERN_DIR := ../gpu_daemon/ldpc_cl

CC      := gcc
CFLAGS  := -O2 -Wall -I$(LIBBPF_INC) -I.

LDFLAGS := $(LIBBPF_A) \
           -lz -lelf \
           -lOpenCL \
           -lpthread -lm -ldl

all: ldpc_probe.bpf.o ldpc_probe.skel.h ldpc_consumer ldpc_measure xdp_rfsim_observe.bpf.o nic_timeline_consumer

ldpc_probe.bpf.o: ldpc_probe.bpf.c
	clang -g -O2 -target bpf \
		-D__TARGET_ARCH_x86 \
		-I$(VMLINUX_INC) \
		-I$(BPFTIME_BLD)/libbpf \
		-c $< -o $@

ldpc_probe.skel.h: ldpc_probe.bpf.o
	$(BPFTOOL) gen skeleton $< name ldpc_probe > $@

ldpc_consumer: ldpc_consumer.c ldpc_probe.skel.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

ldpc_measure: ldpc_measure.c ldpc_probe.skel.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

xdp_rfsim_observe.bpf.o: xdp_rfsim_observe.bpf.c
	clang -g -O2 -target bpf \
		-D__TARGET_ARCH_x86 \
		-I$(VMLINUX_INC) \
		-I$(BPFTIME_BLD)/libbpf \
		-c $< -o $@

nic_timeline_consumer: nic_timeline_consumer.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBBPF_A) -lz -lelf -lpthread -lm -ldl

clean:
	rm -f ldpc_probe.bpf.o ldpc_probe.skel.h ldpc_consumer ldpc_measure \
	      xdp_rfsim_observe.bpf.o nic_timeline_consumer

.PHONY: all clean

# ---- Gate 2 run helpers ----
# 1. Start consumer (bpftime syscall-server mode):
#      LD_PRELOAD=$(BPFTIME_BLD)/runtime/syscall-server/libbpftime-syscall-server.so \
#        SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf \
#        ./ldpc_consumer 200 2>&1 | tee /tmp/gate2_consumer.log &
#
# 2. Start gNB in gnb-ns with bpftime agent:
#      ip netns exec gnb-ns env \
#        LD_PRELOAD=$(BPFTIME_BLD)/runtime/agent/libbpftime-agent.so \
#        SPDLOG_LEVEL=warn \
#        $(OAI_LDPC)/../nr-softmodem \
#           -O /root/linux_env/cxl/cxl_ran_poc/configs/gnb.sa.band78.fr1.106PRB.usrpb210.conf \
#           --rfsim --log_config.global_log_options level,time \
#           2>&1 | tee /tmp/gate2_gnb.log &
#
# 3. Start UE in ue-ns:
#      ip netns exec ue-ns \
#        $(OAI_LDPC)/../nr-uesoftmodem \
#           -O /root/linux_env/cxl/cxl_ran_poc/configs/ue.conf \
#           --rfsim -r 106 --numerology 1 -C 3619200000 \
#           --log_config.global_log_options level,time \
#           2>&1 | tee /tmp/gate2_ue.log &
#
# 4. Wait 30–60 s, then kill consumer to get summary.
