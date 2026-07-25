#!/bin/bash
# Download Ubuntu 24.04 minimal cloud image and create a cloud-init seed ISO.
# Output artefacts land in OUT_DIR (default: /mnt/devdata/cxl_vm/).
#
# After this script:
#   • ubuntu-24.04-server.qcow2  — VM boot disk (sparse, grows on write)
#   • seed.iso                   — cloud-init NoCloud metadata + first-boot script
#   • cxl_mem_file               — 2 GB CXL Type-3 backing file (pre-allocated)
#
# Then run:   scripts/setup_qemu.sh
#
# Requirements: wget (or curl), qemu-img, cloud-image-utils or genisoimage/xorriso

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

OUT_DIR="/mnt/devdata/cxl_vm"
DISK_SIZE="20G"          # logical max; qcow2 is sparse — physical use starts ~2 GB
CXL_SIZE="2G"
UBUNTU_RELEASE="noble"   # Ubuntu 24.04
UBUNTU_IMAGE="noble-server-cloudimg-amd64.img"
UBUNTU_URL="https://cloud-images.ubuntu.com/${UBUNTU_RELEASE}/current/${UBUNTU_IMAGE}"

# ── parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir)    OUT_DIR="$2";    shift 2 ;;
        --disk-size)  DISK_SIZE="$2";  shift 2 ;;
        --cxl-size)   CXL_SIZE="$2";   shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── pre-flight ───────────────────────────────────────────────────────────────
for cmd in wget qemu-img; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: '${cmd}' not found. Install with: apt install qemu-utils wget" >&2
        exit 1
    fi
done

AVAIL_KB=$(df -k "${OUT_DIR%/*}" 2>/dev/null | awk 'NR==2{print $4}' || echo 0)
NEED_KB=$((4500 * 1024))
if [[ "$AVAIL_KB" -lt "$NEED_KB" ]]; then
    echo "WARNING: Only $(( AVAIL_KB / 1024 )) MB free on $(dirname "$OUT_DIR")."
    echo "         Need ~4.5 GB (qcow2 + CXL backing). Free space before proceeding."
fi

mkdir -p "$OUT_DIR"

# ── 1. download base cloud image ─────────────────────────────────────────────
QCOW2="${OUT_DIR}/ubuntu-24.04-server.qcow2"
if [[ -f "${QCOW2}" ]]; then
    echo "Disk image already exists: ${QCOW2}"
else
    echo "=== Downloading Ubuntu 24.04 cloud image (~600 MB) ==="
    TMPIMG="${OUT_DIR}/${UBUNTU_IMAGE}"
    wget --show-progress -c -O "${TMPIMG}" "${UBUNTU_URL}"

    echo "=== Converting to qcow2 and resizing to ${DISK_SIZE} ==="
    qemu-img convert -f qcow2 -O qcow2 "${TMPIMG}" "${QCOW2}"
    qemu-img resize "${QCOW2}" "${DISK_SIZE}"
    rm -f "${TMPIMG}"
    echo "Disk image: ${QCOW2}"
fi

# ── 2. create CXL backing file ───────────────────────────────────────────────
CXL_FILE="${OUT_DIR}/cxl_mem_file"
if [[ -f "${CXL_FILE}" ]]; then
    echo "CXL backing file already exists: ${CXL_FILE}"
else
    echo "=== Creating ${CXL_SIZE} CXL backing file ==="
    truncate -s "${CXL_SIZE}" "${CXL_FILE}"
    echo "CXL backing file: ${CXL_FILE}"
fi

# ── 3. build cloud-init seed ISO ─────────────────────────────────────────────
SEED_ISO="${OUT_DIR}/seed.iso"
SEED_DIR="${OUT_DIR}/seed_staging"
mkdir -p "${SEED_DIR}"

# meta-data (required by cloud-init NoCloud)
cat > "${SEED_DIR}/meta-data" <<'META'
instance-id: cxl-ran-poc-vm
local-hostname: cxl-board
META

# user-data: copy from scripts/cloud-init/user-data
USERDATA_SRC="${ROOT}/scripts/cloud-init/user-data"
if [[ ! -f "${USERDATA_SRC}" ]]; then
    echo "ERROR: ${USERDATA_SRC} not found. Run after cloud-init/user-data is created." >&2
    exit 1
fi
cp "${USERDATA_SRC}" "${SEED_DIR}/user-data"

# build the ISO — try cloud-localds first, then genisoimage, then xorriso
SEED_ISO="${OUT_DIR}/seed.iso"
if command -v cloud-localds &>/dev/null; then
    cloud-localds "${SEED_ISO}" "${SEED_DIR}/user-data" "${SEED_DIR}/meta-data"
elif command -v genisoimage &>/dev/null; then
    genisoimage -output "${SEED_ISO}" -volid cidata -joliet -rock \
        "${SEED_DIR}/user-data" "${SEED_DIR}/meta-data"
elif command -v xorriso &>/dev/null; then
    xorriso -as mkisofs -output "${SEED_ISO}" -volid cidata -joliet -rock \
        "${SEED_DIR}/user-data" "${SEED_DIR}/meta-data"
else
    echo "ERROR: none of cloud-localds / genisoimage / xorriso found." >&2
    echo "       Install with: apt install cloud-image-utils" >&2
    exit 1
fi

rm -rf "${SEED_DIR}"
echo "Seed ISO: ${SEED_ISO}"

# ── summary ──────────────────────────────────────────────────────────────────
echo ""
echo "=== VM artefacts ready in ${OUT_DIR}/ ==="
ls -lh "${OUT_DIR}/"
echo ""
USED_KB=$(du -sk "$OUT_DIR" | awk '{print $1}')
FREE_KB=$(df -k "$OUT_DIR" | awk 'NR==2{print $4}')
echo "Disk used by artefacts : $(( USED_KB / 1024 )) MB"
echo "Disk free on partition : $(( FREE_KB / 1024 )) MB"
echo ""
echo "Next step: scripts/setup_qemu.sh"
