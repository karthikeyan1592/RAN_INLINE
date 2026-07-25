#!/usr/bin/env bash
# Open Inline SIM tier — bootstrap the GCP VM (run ON the VM after first ssh).
# Installs the pinned SIM-0 stack: srsRAN Project (gNB + ru_emulator), Open5GS,
# PoCL (CPU OpenCL), OpenCL tooling, and the veth "virtual fronthaul wire".
# Idempotent-ish; ~20 min on n2-standard-16. UNTESTED SCAFFOLD — fix forward on first run
# and commit the corrections (SIM-0 exit = this script runs clean end-to-end).
set -euo pipefail
sudo() { command sudo -n "$@"; }

echo "== [1/6] base packages =="
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  libfftw3-dev libmbedtls-dev libsctp-dev lksctp-tools libyaml-cpp-dev libgtest-dev \
  libzmq3-dev libdw-dev binutils-dev \
  pocl-opencl-icd ocl-icd-opencl-dev opencl-headers clinfo \
  linux-tools-common numactl

echo "== [2/6] record pinned environment (SIM<->PHYSICAL must match) =="
mkdir -p ~/openinline/pins
uname -a | tee ~/openinline/pins/kernel.txt
lsb_release -a 2>/dev/null | tee ~/openinline/pins/distro.txt
clinfo -l | tee ~/openinline/pins/opencl.txt   # must list PoCL

echo "== [3/6] Open5GS (5GC) via official PPA =="
sudo add-apt-repository -y ppa:open5gs/latest
sudo apt-get update && sudo apt-get install -y open5gs mongodb-org || \
  sudo apt-get install -y open5gs   # mongodb pulled as dep on 24.04 PPA

echo "== [4/6] srsRAN Project (gNB + ru_emulator), pinned release =="
SRSRAN_TAG="${SRSRAN_TAG:-release_26_04}"   # OCUDU (ex-srsRAN)   # re-pin deliberately, record in pins/
cd ~/openinline
[ -d srsRAN_Project ] || git clone https://gitlab.com/ocudu/ocudu.git srsRAN_Project
cd srsRAN_Project && git fetch --tags && git checkout "$SRSRAN_TAG"
echo "$SRSRAN_TAG" > ~/openinline/pins/srsran.txt
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
ninja -C build gnb ru_emulator
sudo ninja -C build install || true   # apps also runnable from build/apps/

echo "== [5/6] virtual fronthaul wire: veth pair on a bridge =="
sudo tee /usr/local/sbin/oi-fhwire >/dev/null <<'EOF'
#!/bin/sh
# creates fh-du <-> fh-ru veth "wire" for OFH socket-mode / af_packet DPDK
set -e
ip link show fh-du >/dev/null 2>&1 && exit 0
ip link add fh-du type veth peer name fh-ru
ip link set fh-du up mtu 9000
ip link set fh-ru up mtu 9000
EOF
sudo chmod +x /usr/local/sbin/oi-fhwire && sudo /usr/local/sbin/oi-fhwire

echo "== [6/6] hugepages (for optional af_packet-DPDK mode) =="
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep -q hugetlbfs /proc/mounts || { sudo mkdir -p /dev/hugepages; sudo mount -t hugetlbfs none /dev/hugepages; }

echo "DONE. SIM-1 next: gNB test mode + ru_emulator configs over fh-du/fh-ru (see ARCHITECTURE_v3_SIM.md §4)."
