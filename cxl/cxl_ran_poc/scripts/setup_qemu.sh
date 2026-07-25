#!/bin/bash
# Launch the CXL RAN PoC QEMU VM.
#
# QEMU 9.x runs inside the Docker image (cxl-qemu-builder) so that its
# Ubuntu 24.04 runtime libs stay isolated from the host's Ubuntu 22.04.
# KVM, the VM disk, CXL backing file and PoC source are all passed through.
#
# Prerequisites (run once before this script):
#   1. scripts/build_qemu.sh     — builds Docker image cxl-qemu-builder
#   2. scripts/provision_vm.sh   — downloads Ubuntu 24.04 qcow2 + seed.iso
#
# Usage:
#   ./scripts/setup_qemu.sh [--vm-dir DIR] [--mem 8G] [--cpus 4] [--ssh-port 2222]
#
# After the VM boots (first boot ~20 min for provisioning):
#   ssh -p 2222 ubuntu@localhost
#   sudo /opt/cxl_ran_poc/scripts/in_vm_setup.sh   # CXL bring-up
#   /opt/cxl_ran_poc/scripts/in_vm_run.sh           # run pipeline

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# ── defaults ─────────────────────────────────────────────────────────────────
VM_DIR="/mnt/devdata/cxl_vm"
VM_MEM="8G"
VM_CPUS=4
SSH_PORT=2222
CXL_SIZE="2G"
CXL_MEM_ALIGN="256M"
DOCKER_IMAGE="cxl-qemu-builder"

# ── parse args ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --vm-dir)      VM_DIR="$2";        shift 2 ;;
        --mem)         VM_MEM="$2";        shift 2 ;;
        --cpus)        VM_CPUS="$2";       shift 2 ;;
        --ssh-port)    SSH_PORT="$2";      shift 2 ;;
        --cxl-size)    CXL_SIZE="$2";      shift 2 ;;
        --image)       DOCKER_IMAGE="$2";  shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── pre-flight: Docker image ──────────────────────────────────────────────────
if ! command -v docker &>/dev/null; then
    echo "ERROR: docker not found." >&2; exit 1
fi

if ! docker image inspect "${DOCKER_IMAGE}" &>/dev/null; then
    echo "ERROR: Docker image '${DOCKER_IMAGE}' not found." >&2
    echo "       Run scripts/build_qemu.sh first." >&2
    exit 1
fi

QEMU_VER=$(docker run --rm "${DOCKER_IMAGE}" /out/qemu-system-x86_64 --version \
           2>/dev/null | head -1 || echo "unknown")
echo "QEMU: ${QEMU_VER}"

# Verify CXL / q35 support (QEMU exits non-zero on -machine help; || true absorbs it)
MACHINE_HELP=$(docker run --rm "${DOCKER_IMAGE}" /out/qemu-system-x86_64 -machine help 2>&1 || true)
if ! echo "${MACHINE_HELP}" | grep -q 'q35'; then
    echo "ERROR: This QEMU build does not support the q35 machine type." >&2
    exit 1
fi
echo "PASS: q35 machine type available"

# ── pre-flight: VM artefacts ──────────────────────────────────────────────────
DISK="${VM_DIR}/ubuntu-24.04-server.qcow2"
SEED="${VM_DIR}/seed.iso"
CXL_FILE="${VM_DIR}/cxl_mem_file"

for f in "$DISK" "$SEED"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: Missing artefact: ${f}" >&2
        echo "       Run scripts/provision_vm.sh first." >&2
        exit 1
    fi
done

if [[ ! -f "$CXL_FILE" ]]; then
    echo "Creating CXL backing file ${CXL_FILE} (${CXL_SIZE})..."
    truncate -s "${CXL_SIZE}" "$CXL_FILE"
fi

# ── SSH port availability check ───────────────────────────────────────────────
if ss -tln 2>/dev/null | grep -q ":${SSH_PORT} "; then
    echo "WARN: port ${SSH_PORT} already in use; trying $((SSH_PORT + 1))"
    SSH_PORT=$((SSH_PORT + 1))
fi

echo ""
echo "=== Launching QEMU VM inside Docker ==="
echo "    Image   : ${DOCKER_IMAGE}"
echo "    Disk    : ${DISK}"
echo "    CXL     : ${CXL_FILE} (${CXL_SIZE})"
echo "    Memory  : ${VM_MEM}"
echo "    CPUs    : ${VM_CPUS}"
echo "    SSH     : localhost:${SSH_PORT} → guest:22"
echo "    PoC src : ${ROOT} → /mnt/poc_src (9p)"
echo ""
echo "First boot installs all software (~20 min). Monitor inside VM:"
echo "    tail -f /var/log/cxl_provision/provision.log"
echo ""
echo "SSH in with:  ssh -p ${SSH_PORT} ubuntu@localhost"
echo "Then run:"
echo "    sudo /opt/cxl_ran_poc/scripts/in_vm_setup.sh"
echo "    /opt/cxl_ran_poc/scripts/in_vm_run.sh"
echo ""

# ── launch QEMU via docker run ────────────────────────────────────────────────
# --net host: lets QEMU's slirp hostfwd bind directly on the host interface.
# --privileged: required for KVM + shared memory-backend-file.
# Volumes: VM disk, CXL file, and PoC source (9p share) are bind-mounted.
exec docker run \
    --rm \
    --name cxl-qemu-vm \
    --net host \
    --privileged \
    --device /dev/kvm \
    -v "${VM_DIR}:/vm" \
    -v "${ROOT}:/poc:ro" \
    "${DOCKER_IMAGE}" \
    /out/qemu-system-x86_64 \
        -enable-kvm \
        -cpu host,hypervisor=off \
        -smp "${VM_CPUS}" \
        -m "${VM_MEM},slots=8,maxmem=32G" \
        -M "q35,accel=kvm,cxl=on" \
        \
        -drive "file=/vm/ubuntu-24.04-server.qcow2,id=hd0,format=qcow2,cache=writeback,if=none" \
        -device "virtio-blk-pci,drive=hd0,bus=pcie.0,bootindex=1" \
        -drive "file=/vm/seed.iso,id=seed0,format=raw,if=none,readonly=on" \
        -device "virtio-blk-pci,drive=seed0,bus=pcie.0" \
        \
        -object "memory-backend-file,id=cxl-mem0,share=on,mem-path=/vm/cxl_mem_file,size=${CXL_SIZE},align=${CXL_MEM_ALIGN}" \
        -device "pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52" \
        -device "cxl-rp,id=rp0,bus=cxl.0,chassis=0,port=0" \
        -device "cxl-type3,bus=rp0,volatile-memdev=cxl-mem0,id=cxl-type3-0" \
        -M "cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G" \
        -L "/usr/share/seabios" \
        -L "/usr/share/qemu" \
        \
        -fsdev "local,id=poc_src,path=/poc,security_model=mapped-xattr" \
        -device "virtio-9p-pci,fsdev=poc_src,mount_tag=poc_src,bus=pcie.0" \
        \
        -netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22" \
        -device "virtio-net-pci,netdev=net0,bus=pcie.0,rombar=0" \
        \
        -nographic \
        -serial "mon:stdio"
