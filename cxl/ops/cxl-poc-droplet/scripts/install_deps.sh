#!/bin/bash
# install_deps.sh — install ALL system packages on the cxl-poc droplet
# Covers: QEMU/KVM, eBPF, OpenCL/PoCL, CXL tools, bpftime build deps,
#         srsRAN/OAI build deps, DAX/PMDK tools, VM image tools, Python stack.
# Run once after provision.sh. Then run build_tools.sh to compile from source.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.cxl_droplet_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.cxl_droplet_ip missing)"; exit 1; }
DROPLET_IP=$(cat "$IP_FILE")
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"

echo "=== Installing all dependencies on $DROPLET_IP ==="
echo "    Expected time: 5-10 minutes"
echo ""

ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' << 'REMOTE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
KVER=$(uname -r)

apt-get update -qq

# ── Core build tools ─────────────────────────────────────────
apt-get install -y -qq \
  build-essential cmake ninja-build git wget curl \
  flex bison pkg-config autoconf libtool automake \
  libncurses-dev bc

# ── QEMU + KVM ───────────────────────────────────────────────
apt-get install -y -qq \
  qemu-system-x86 qemu-kvm qemu-utils

# ── eBPF / BPF toolchain ─────────────────────────────────────
apt-get install -y -qq \
  clang llvm libclang-dev \
  libelf-dev libssl-dev zlib1g-dev \
  libbpf-dev linux-tools-common

# bpftool versioned → generic fallback
apt-get install -y -qq "linux-tools-${KVER}" 2>/dev/null || \
  apt-get install -y -qq linux-tools-generic 2>/dev/null || true

# kernel headers (needed for bpftime + BPF skeleton gen)
apt-get install -y -qq "linux-headers-${KVER}" 2>/dev/null || \
  apt-get install -y -qq linux-headers-generic 2>/dev/null || true

# linux-modules-extra for CXL kernel modules in QEMU VM
apt-get install -y -qq "linux-modules-extra-${KVER}" 2>/dev/null || true

# ── bpftime build deps ───────────────────────────────────────
# bpftime uses cmake/ninja/clang (above) + these:
apt-get install -y -qq \
  libffi-dev libsystemd-dev \
  libtbb-dev \
  libgtest-dev libgmock-dev \
  lcov

# ── OpenCL / PoCL ────────────────────────────────────────────
apt-get install -y -qq \
  pocl-opencl-icd ocl-icd-opencl-dev opencl-headers clinfo

# ── srsRAN + OAI build deps ──────────────────────────────────
# srsRAN_Project needs: fftw3, boost, mbedtls, zmq, gtest, yaml, sctp, dw
# OAI gNB adds: lapack, blas, gmp, gnutls, nettle, python3-dev
apt-get install -y -qq \
  libfftw3-dev \
  libboost-all-dev \
  libconfig++-dev \
  libyaml-cpp-dev \
  libzmq3-dev \
  libmbedtls-dev \
  libsctp-dev \
  libdw-dev \
  libblas-dev liblapack-dev liblapacke-dev \
  libgmp-dev \
  libgnutls28-dev nettle-dev \
  libpcsclite-dev \
  python3-dev python3-pip \
  libuhd-dev 2>/dev/null || true

# ── CXL / NVDIMM / DAX userspace tools ──────────────────────
apt-get install -y -qq \
  numactl libnuma-dev \
  ndctl daxctl

# cxl-utils (may be bundled with ndctl on Ubuntu 24.04)
apt-cache show cxl-utils &>/dev/null && apt-get install -y -qq cxl-utils || true

# ── PMDK — persistent memory dev kit (for Option A shared DAX mapping) ──
# libpmem2 is the modern PMDK API; libpmem is v1 (also needed by some tools)
apt-get install -y -qq \
  libpmem-dev libpmem2-dev 2>/dev/null || \
  apt-get install -y -qq libpmem-dev 2>/dev/null || true

# ── DAX filesystem tools (for Option C: fs-dax mount) ────────
apt-get install -y -qq \
  e2fsprogs xfsprogs util-linux \
  kmod

# ── VM image tools ───────────────────────────────────────────
# cloud-localds: cloud-init NoCloud seed ISO
# libguestfs-tools: virt-customize for offline image manipulation
apt-get install -y -qq \
  cloud-image-utils libguestfs-tools

# ── CXLMemSim build deps ─────────────────────────────────────
apt-get install -y -qq \
  libspdlog-dev libfmt-dev

# ── Debug / inspection tools ─────────────────────────────────
apt-get install -y -qq \
  strace ltrace \
  pciutils lshw \
  numactl hwloc \
  linux-perf 2>/dev/null || true

# ── Python data stack ────────────────────────────────────────
apt-get install -y -qq \
  python3-matplotlib python3-numpy \
  python3-pandas python3-scipy

echo ""
echo "=== apt installs complete ==="
REMOTE

# ── Verification block ────────────────────────────────────────
echo ""
echo "=== Verification ==="
ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' << 'VERIFY'
ok() { printf "  [OK]  %-28s %s\n" "$1" "$2"; }
err(){ printf "  [!!]  %-28s MISSING\n" "$1"; }

[ -e /dev/kvm ]                     && ok "KVM"            "" || err "KVM"
grep -q avx2 /proc/cpuinfo          && ok "AVX2"           "" || err "AVX2"
command -v qemu-system-x86_64 >/dev/null && ok "qemu"      "$(qemu-system-x86_64 --version | head -1)" || err "qemu"
command -v bpftool >/dev/null        && ok "bpftool"       "$(bpftool version 2>/dev/null | head -1)" || err "bpftool"
command -v clang >/dev/null          && ok "clang"         "$(clang --version | head -1)" || err "clang"
command -v cmake >/dev/null          && ok "cmake"         "$(cmake --version | head -1)" || err "cmake"
command -v cxl >/dev/null            && ok "cxl"           "$(cxl version 2>/dev/null)" || \
  command -v ndctl >/dev/null        && ok "ndctl"         "$(ndctl version 2>/dev/null)" || err "cxl/ndctl"
command -v numactl >/dev/null        && ok "numactl"       "" || err "numactl"
command -v daxctl >/dev/null         && ok "daxctl"        "" || err "daxctl"
command -v virt-customize >/dev/null && ok "virt-customize" "" || err "virt-customize"
clinfo 2>/dev/null | grep -q "Device Name" && ok "OpenCL" "$(clinfo 2>/dev/null | grep 'Device Name' | head -1 | sed 's/.*: //')" || err "OpenCL"
pkg-config --exists OpenCL 2>/dev/null && ok "OpenCL dev"  "" || err "OpenCL dev headers"
dpkg -l libpmem-dev 2>/dev/null | grep -q '^ii' && ok "libpmem-dev" "" || err "libpmem-dev"
dpkg -l libnuma-dev 2>/dev/null | grep -q '^ii' && ok "libnuma-dev" "" || err "libnuma-dev"
dpkg -l libfftw3-dev 2>/dev/null | grep -q '^ii' && ok "libfftw3-dev (srsRAN)" "" || err "libfftw3-dev"
VERIFY

echo ""
echo "install_deps.sh complete — next: run build_tools.sh"
