#!/bin/bash
# Build QEMU 9.x inside Docker and extract the binary to /mnt/devdata/cxl_vm/.
# Run once; reuse the binary for all subsequent VM launches.
#
# Usage:
#   ./scripts/build_qemu.sh [--jobs N] [--tag v9.2.3] [--out-dir /mnt/devdata/cxl_vm]
#
# Requirements:  docker (rootful or rootless with --privileged not needed here)
# Time estimate: 20-30 min on a 4-core machine

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

QEMU_TAG="v9.2.3"
JOBS=$(nproc)
OUT_DIR="/mnt/devdata/cxl_vm"
IMAGE_TAG="cxl-qemu-builder"
CONTAINER_NAME="qemu-extract-$$"

# ── parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)   JOBS="$2";    shift 2 ;;
        --tag)    QEMU_TAG="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ── pre-flight ───────────────────────────────────────────────────────────────
if ! command -v docker &>/dev/null; then
    echo "ERROR: docker not found. Install Docker first." >&2
    exit 1
fi

AVAIL_KB=$(df -k "${OUT_DIR%/*}" 2>/dev/null | awk 'NR==2{print $4}' || echo 0)
NEED_KB=$((200 * 1024))  # ~200 MB for binary + pc-bios
if [[ "$AVAIL_KB" -lt "$NEED_KB" ]]; then
    echo "WARNING: Less than 200 MB free on $(dirname "$OUT_DIR"). Proceeding anyway."
fi

mkdir -p "$OUT_DIR"

# ── check if already built ───────────────────────────────────────────────────
if [[ -x "${OUT_DIR}/qemu-system-x86_64" ]]; then
    EXISTING_VER=$("${OUT_DIR}/qemu-system-x86_64" --version 2>/dev/null | head -1)
    echo "QEMU already built: ${EXISTING_VER}"
    echo "Delete ${OUT_DIR}/qemu-system-x86_64 to force a rebuild."
    exit 0
fi

echo "=== Building QEMU ${QEMU_TAG} (${JOBS} jobs) ==="
echo "    Source:  Docker multi-stage (${IMAGE_TAG})"
echo "    Output:  ${OUT_DIR}/"
echo ""

# ── build ────────────────────────────────────────────────────────────────────
docker build \
    --build-arg QEMU_TAG="${QEMU_TAG}" \
    --build-arg JOBS="${JOBS}" \
    -f "${ROOT}/docker/Dockerfile.qemu" \
    -t "${IMAGE_TAG}" \
    "${ROOT}"

# ── extract binary ───────────────────────────────────────────────────────────
docker create --name "${CONTAINER_NAME}" "${IMAGE_TAG}" >/dev/null

docker cp "${CONTAINER_NAME}:/out/qemu-system-x86_64" "${OUT_DIR}/"
docker cp "${CONTAINER_NAME}:/out/qemu-img"            "${OUT_DIR}/"
docker cp "${CONTAINER_NAME}:/out/pc-bios"             "${OUT_DIR}/"

docker rm "${CONTAINER_NAME}" >/dev/null

chmod +x "${OUT_DIR}/qemu-system-x86_64" "${OUT_DIR}/qemu-img"

# ── verify ───────────────────────────────────────────────────────────────────
echo ""
echo "=== Verification ==="
"${OUT_DIR}/qemu-system-x86_64" --version

# Check CXL machine type is present
if "${OUT_DIR}/qemu-system-x86_64" -machine help 2>/dev/null | grep -q q35; then
    echo "PASS: q35 machine type available"
else
    echo "WARN: q35 machine type not found — check QEMU build"
fi

echo ""
echo "QEMU binary: ${OUT_DIR}/qemu-system-x86_64"
echo "Done. Run scripts/provision_vm.sh next."
