#!/bin/bash
# install_deps.sh — install ALL system packages on GCP CXL lab instance
# Adapted from ops/cxl-poc-droplet/scripts/install_deps.sh (DigitalOcean version)
# Key difference: GCP uses karthix25+sudo, not root. Runs commands locally on instance.
#
# Run after: provision.sh
# Run before: rsync_source.sh + build_tools.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.gcp_instance_ip missing)"; exit 1; }
INSTANCE_IP=$(cat "$IP_FILE")

SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
GCP_USER="${GCP_USER:-karthix25}"
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=30"

echo "=== Installing all dependencies on $INSTANCE_IP ==="
echo "    Expected time: 5-10 minutes"
echo ""

ssh $SSH_OPTS "${GCP_USER}@${INSTANCE_IP}" 'sudo bash -s' << 'REMOTE'
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

apt-get install -y -qq "linux-tools-${KVER}" 2>/dev/null || \
  apt-get install -y -qq linux-tools-generic 2>/dev/null || true

apt-get install -y -qq "linux-headers-${KVER}" 2>/dev/null || \
  apt-get install -y -qq linux-headers-generic 2>/dev/null || true

apt-get install -y -qq "linux-modules-extra-${KVER}" 2>/dev/null || true

# ── bpftime build deps ───────────────────────────────────────
apt-get install -y -qq \
  libffi-dev libsystemd-dev \
  libtbb-dev \
  libgtest-dev libgmock-dev \
  lcov

# ── OpenCL / PoCL ────────────────────────────────────────────
apt-get install -y -qq \
  pocl-opencl-icd ocl-icd-opencl-dev opencl-headers clinfo

# ── srsRAN + OAI build deps ──────────────────────────────────
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

apt-cache show cxl-utils &>/dev/null && apt-get install -y -qq cxl-utils || true

# ── PMDK ─────────────────────────────────────────────────────
apt-get install -y -qq \
  libpmem-dev libpmem2-dev 2>/dev/null || \
  apt-get install -y -qq libpmem-dev 2>/dev/null || true

# ── DAX filesystem + VM image tools ─────────────────────────
apt-get install -y -qq \
  e2fsprogs xfsprogs util-linux kmod \
  cloud-image-utils libguestfs-tools

# ── CXLMemSim build deps ─────────────────────────────────────
apt-get install -y -qq \
  libspdlog-dev libfmt-dev

# ── Debug / inspection tools ─────────────────────────────────
apt-get install -y -qq \
  strace ltrace pciutils lshw \
  numactl hwloc \
  linux-perf 2>/dev/null || true

# ── Python data stack ────────────────────────────────────────
apt-get install -y -qq \
  python3-matplotlib python3-numpy \
  python3-pandas python3-scipy

echo ""
echo "=== apt installs complete ==="
REMOTE

echo ""
echo "=== Verification ==="
ssh $SSH_OPTS "${GCP_USER}@${INSTANCE_IP}" 'bash -s' << 'VERIFY'
ok() { printf "  [OK]  %-28s %s\n" "$1" "$2"; }
err(){ printf "  [!!]  %-28s MISSING\n" "$1"; }

grep -c vmx /proc/cpuinfo | grep -q '^[1-9]' && ok "VMX (nested KVM)" "" || err "VMX"
[ -e /dev/kvm ]                              && ok "KVM /dev/kvm"     "" || err "KVM"
grep -q avx2 /proc/cpuinfo                   && ok "AVX2"             "" || err "AVX2"
command -v qemu-system-x86_64 >/dev/null     && ok "qemu" "$(qemu-system-x86_64 --version | head -1)" || err "qemu"
command -v bpftool >/dev/null                && ok "bpftool"          "$(bpftool version 2>/dev/null | head -1)" || err "bpftool"
command -v clang >/dev/null                  && ok "clang"            "$(clang --version | head -1)" || err "clang"
command -v cmake >/dev/null                  && ok "cmake"            "$(cmake --version | head -1)" || err "cmake"
command -v numactl >/dev/null                && ok "numactl"          "" || err "numactl"
command -v daxctl >/dev/null                 && ok "daxctl"           "" || err "daxctl"
command -v virt-customize >/dev/null         && ok "virt-customize"   "" || err "virt-customize"
clinfo 2>/dev/null | grep -q "Device Name"  && ok "OpenCL" "$(clinfo 2>/dev/null | grep 'Device Name' | head -1 | sed 's/.*: //')" || err "OpenCL"
pkg-config --exists OpenCL 2>/dev/null       && ok "OpenCL dev"       "" || err "OpenCL dev headers"
dpkg -l libpmem-dev  2>/dev/null | grep -q '^ii' && ok "libpmem-dev"  "" || err "libpmem-dev"
dpkg -l libnuma-dev  2>/dev/null | grep -q '^ii' && ok "libnuma-dev"  "" || err "libnuma-dev"
dpkg -l libfftw3-dev 2>/dev/null | grep -q '^ii' && ok "libfftw3-dev" "" || err "libfftw3-dev"
VERIFY

echo ""
echo "install_deps.sh complete — next: ./rsync_source.sh && ./build_tools.sh"
