CC     ?= gcc
CFLAGS := -Wall -Wextra -Werror -O2 -I.. -I../l1_sim
LDFLAGS := -lpthread -lnuma -lm

# ── OpenCL / PoCL detection ──────────────────────────────────────────────────
# Try pkg-config first; fall back to direct link against -lOpenCL
OPENCL_CFLAGS  := $(shell pkg-config --cflags OpenCL 2>/dev/null)
OPENCL_LDFLAGS := $(shell pkg-config --libs   OpenCL 2>/dev/null)
ifeq ($(OPENCL_LDFLAGS),)
    OPENCL_LDFLAGS := -lOpenCL
endif

# Probe: does OpenCL compile and link?
HAVE_OPENCL := $(shell echo 'int main(){return 0;}' | \
    $(CC) -x c - -o /dev/null $(OPENCL_CFLAGS) $(OPENCL_LDFLAGS) 2>/dev/null \
    && echo 1 || echo 0)

ifeq ($(HAVE_OPENCL),1)
    CFLAGS  += -DHAVE_OPENCL $(OPENCL_CFLAGS)
    LDFLAGS += $(OPENCL_LDFLAGS)
    OBJS_OPENCL := gpu_opencl.o
    $(info [gpu_daemon] OpenCL available — PoCL/GPU backend enabled)
else
    OBJS_OPENCL :=
    $(info [gpu_daemon] OpenCL not found — bare-C fallback only)
endif

# ── Objects ──────────────────────────────────────────────────────────────────
OBJS := gpu_daemon.o gpu_compute.o cxl_memory.o \
        ../l1_sim/ldpc.o ../l1_sim/fft.o $(OBJS_OPENCL)

all: gpu_daemon

gpu_daemon: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

../l1_sim/ldpc.o ../l1_sim/fft.o:
	$(MAKE) -C ../l1_sim ldpc.o fft.o

clean:
	rm -f *.o gpu_daemon

.PHONY: all clean
