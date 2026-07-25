# CXL RAN PoC — Dependencies & Environment Bootstrap

## Infrastructure

| Layer | Spec |
|-------|------|
| GCP host | n2-standard-4, zone asia-south2-a, Ubuntu 22.04 |
| GCP project | cxl-systems-lab-26 |
| GCP host IP | 34.131.224.105 (ephemeral — check GCP console if changed) |
| SSH key (local→host) | `/root/.ssh/id_ed25519` (NEVER copy off-machine) |
| SSH key (host→VM) | `/home/karthix25/.ssh/vm_key` |
| VM port | `localhost:2222` on GCP host |
| VM kernel | 6.8.0-124-generic (Ubuntu HWE) |
| CXL NUMA node 1 | 1920 MB (QEMU persistent-memdev, pmem=off) |

---

## GCP Host Packages

```bash
apt-get install -y \
  qemu-system-x86 qemu-utils \
  openssh-client rsync
```

---

## VM Packages (inside QEMU VM)

```bash
apt-get install -y \
  gcc g++ make cmake ninja-build git \
  clang llvm \
  clang-15 llvm-15 \
  bpftool \
  libbpf-dev \
  libelf-dev zlib1g-dev \
  libOpenCL-dev ocl-icd-opencl-dev pocl-opencl-icd \
  libnuma-dev \
  libmbedtls-dev \
  libkmod-dev libudev-dev libsystemd-dev \
  libjson-c-dev uuid-dev libkeyutils-dev \
  meson pkg-config
```

Key version constraints:
- `bpftime` requires LLVM >= 15 → install `clang-15 llvm-15` explicitly
- Ubuntu 22.04 ships `libbpf 0.5.0` — too old to load BTF-compiled BPF objects; use bpftime's bundled libbpf instead
- `ndctl` from apt is v72.1 — lacks `cxl create-region`; must build v80 from source

---

## Third-Party Builds (one-time, in VM)

### 1. ndctl v80

```bash
cd /root/cxl/third_party
git clone https://github.com/pmem/ndctl -b v80
cd ndctl
meson setup build --prefix=/usr/local
ninja -C build
ninja -C build install
# verify:
cxl version   # should print 80
```

### 2. srsRAN_Project (LDPC benchmark)

```bash
cd /root/cxl/third_party
git clone https://github.com/srsran/srsRAN_Project
cd srsRAN_Project
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j2 ldpc_decoder_benchmark
# binary ends up at:
# tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark
```

### 3. bpftime (for bundled libbpf only)

```bash
cd /root/cxl/third_party
git clone https://github.com/eunomia-bpf/bpftime --recursive
cd bpftime && mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-15/cmake
cmake --build . -j2 --target bpftime-agent
# artifacts used by PoC:
#   build/libbpf/libbpf/libbpf.a   — linked into ldpc_uprobe_loader
#   build/libbpf/bpf/*.h            — headers
```

---

## CXL NUMA Node Setup (run after every VM boot)

The script `ops/gcp-cxl-lab/scripts/vm_cxl_setup.sh` does this idempotently:

```bash
# Manual steps:
modprobe cxl_acpi cxl_core cxl_mem cxl_port cxl_pmem

cxl create-region -m mem0 -d decoder0.0 region0
ndctl create-namespace --mode=devdax --region=region0
daxctl reconfigure-device --mode=system-ram dax0.0

# Online the memory pages
for f in /sys/devices/system/memory/memory*/state; do
  [ "$(cat $f)" = "offline" ] && echo online > "$f"
done

# Verify — node 1 must show > 0 MB
numactl --hardware
```

---

## PoC Build (inside VM, `/root/cxl/poc/`)

```bash
cd /root/cxl/poc

# 1. Generate vmlinux.h from running kernel BTF
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# 2. Compile BPF uprobe program
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 \
  -I. -I/usr/include \
  -c ldpc_llr_mover.bpf.c -o ldpc_llr_mover.bpf.o

# 3. Generate skeleton (optional, for skeleton-based loaders)
bpftool gen skeleton ldpc_llr_mover.bpf.o > ldpc_llr_mover.skel.h

# 4. Build Phase 2 sentinel test
gcc -O2 -o phase2_cxl_ocl_sentinel phase2_cxl_ocl_sentinel.c \
  -lOpenCL -lnuma

# 5. Build Phase 4 uprobe loader
BPFTIME=/root/cxl/third_party/bpftime/build
gcc -O2 -o ldpc_uprobe_loader ldpc_uprobe_loader.c \
  -I${BPFTIME}/libbpf \
  ${BPFTIME}/libbpf/libbpf/libbpf.a \
  -lelf -lz -lnuma
```

Or use `make all` (Makefile handles steps 1–5).

---

## Run Phase 2 — CXL + OCL Sentinel

```bash
cd /root/cxl/poc
./phase2_cxl_ocl_sentinel
# Expected: CXL alloc PASS, OCL readback PASS, latency ~34000 ns
```

## Run Phase 4 — E2E Uprobe + BPF Map

```bash
cd /root/cxl/poc
LDPC=/root/cxl/third_party/srsRAN_Project/build/tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark

./ldpc_uprobe_loader \
  --ldpc   "$LDPC" \
  --bpf    ./ldpc_llr_mover.bpf.o \
  --offset 0x2fef0 \
  --reps   20 \
  --output e2e_gcp.csv

# Expected output:
#   uprobe fires: 4080
#   LLR bytes captured: 12204320
#   e2e_gcp.csv written
```

---

## SSH Access Chain

```bash
# Local → GCP host
ssh -i /root/.ssh/id_ed25519 karthix25@34.131.224.105

# GCP host → VM
ssh -i ~/.ssh/vm_key -p 2222 root@localhost

# One-liner from local
ssh -i /root/.ssh/id_ed25519 karthix25@34.131.224.105 \
  'ssh -i ~/.ssh/vm_key -p 2222 root@localhost "<command>"'
```

---

## Critical Notes

1. **`/root/.ssh/id_ed25519` must NOT be copied to GCP host or VM** — use agent forwarding or intermediate keys only
2. **CXL setup must run after every VM reboot** — modules don't auto-load, daxctl config doesn't persist
3. **64 KB max pre-fault for CXL pages** — WC-mapped pages cost ~250 ms each; large memset hangs
4. **uprobe offset `0x2fef0` is binary-specific** — re-derive with `nm ldpc_decoder_benchmark | grep ldpc_decoder_impl.*decode` if binary is rebuilt
5. **PRIMARY_CONFIG: 11,703 µs/slot = 23.4×** — fixed anchor, never re-derived
