#!/usr/bin/env bash
# build_images.sh — builds gpu-phy + oracle images, emits pins.json (P0-R7, IF-P0-PINS).
# Run from this feature's root (features/p0-rig-scaffold/).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_DIR="${ROOT}/docker"
UPSTREAM="${DOCKER_DIR}/upstream"          # symlink -> pinned third_party/ocudu checkout
OI_OCUDU_SRC="${OI_OCUDU_SRC:-${UPSTREAM}}"
OUT="${1:-${ROOT}/pins.json}"

echo "== [1/4] Verifying upstream pin =="
OCUDU_SHA="$(git -C "$UPSTREAM" rev-parse HEAD)"
OCUDU_TAG="$(git -C "$UPSTREAM" describe --tags --exact-match 2>/dev/null || echo 'UNPINNED')"
if [ "$OCUDU_TAG" != "release_26_04" ]; then
  echo "error: docker/upstream is not checked out at tag release_26_04 (got: ${OCUDU_TAG})" >&2
  echo "       run: git -C '$UPSTREAM' fetch --depth 1 origin tag release_26_04 && git -C '$UPSTREAM' checkout release_26_04" >&2
  exit 1
fi
echo "  OCUDU pinned: ${OCUDU_TAG} @ ${OCUDU_SHA}"

LDPC_SHA="$(cd "${DOCKER_DIR}/gpu-phy/ldpc_suite" && sha256sum bg_tables.h ldpc_decode.cl bit_diff_test.cpp.orig | sha256sum | cut -d' ' -f1)"
BUILT_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# "Pins core": the input-provenance subset known BEFORE either image is built (no image digests --
# those are only knowable after the build, and a label can't self-reference its own image's
# digest). Baked into both images identically as LABEL org.openinline.pins (P0-R7 test: both
# labels parse to the same content). The fuller pins.json file (below) adds output digests.
OI_PINS_CORE=$(cat <<JSON
{"schema":"oi-pins/1","ocudu":{"repo":"https://gitlab.com/ocudu/ocudu","tag":"${OCUDU_TAG}","sha":"${OCUDU_SHA}"},"base_image":{"name":"ubuntu:24.04"},"pocl":{"source":"apt","version":"5.0-2.1build3"},"adaptivecpp":{"repo":"https://github.com/AdaptiveCpp/AdaptiveCpp","tag":"v25.10.0"},"ldpc_suite":{"origin":"cxl_ran_poc/gpu_daemon/ldpc_cl","sha":"${LDPC_SHA}"}}
JSON
)

echo "== [2/4] Building gpu-phy =="
docker buildx build \
  --build-context ocudu="${OI_OCUDU_SRC}" \
  --build-arg OI_PINS_CORE="${OI_PINS_CORE}" \
  -f "${DOCKER_DIR}/gpu-phy/Dockerfile" \
  -t oi/gpu-phy:dev \
  --load \
  "${DOCKER_DIR}/gpu-phy"

echo "== [3/4] Building oracle =="
docker buildx build \
  --build-arg OI_PINS_CORE="${OI_PINS_CORE}" \
  -f "${DOCKER_DIR}/oracle/Dockerfile" \
  -t oi/oracle:dev \
  --load \
  "${ROOT}"

echo "== [4/4] Generating pins.json =="
GPUPHY_LABEL="$(docker image inspect oi/gpu-phy:dev --format '{{index .Config.Labels "org.openinline.pins"}}')"
ORACLE_LABEL="$(docker image inspect oi/oracle:dev --format '{{index .Config.Labels "org.openinline.pins"}}')"
if [ "$GPUPHY_LABEL" != "$ORACLE_LABEL" ]; then
  echo "error: org.openinline.pins label differs between gpu-phy and oracle images (P0-R7)" >&2
  exit 1
fi
echo "  label parity confirmed: both images carry identical org.openinline.pins"

# Note: `docker image inspect` prints a stray blank line to stdout even when it errors (image not
# found) -- an `A 2>/dev/null || B` fallback would concatenate that blank line with B's output.
# Capture into a variable and check $? explicitly instead.
image_id_or_digest() {
  local ref="$1" out
  out="$(docker image inspect "$ref" --format '{{index .RepoDigests 0}}' 2>/dev/null)"
  if [ $? -eq 0 ] && [ -n "$out" ] && [ "$out" != '<no value>' ]; then
    printf '%s' "$out"
    return
  fi
  out="$(docker image inspect "$ref" --format '{{.Id}}' 2>/dev/null)"
  if [ $? -eq 0 ] && [ -n "$out" ]; then
    printf '%s' "$out"
  else
    printf 'unknown'
  fi
}

GPUPHY_DIGEST="$(image_id_or_digest oi/gpu-phy:dev)"
ORACLE_DIGEST="$(image_id_or_digest oi/oracle:dev)"
BASE_DIGEST="$(image_id_or_digest ubuntu:24.04)"

cat > "$OUT" <<JSON
{
  "schema": "oi-pins/1",
  "built_utc": "${BUILT_UTC}",
  "ocudu": {"repo": "https://gitlab.com/ocudu/ocudu", "tag": "${OCUDU_TAG}", "sha": "${OCUDU_SHA}"},
  "base_image": {"name": "ubuntu:24.04", "digest": "${BASE_DIGEST}"},
  "pocl": {"source": "apt", "version": "5.0-2.1build3"},
  "adaptivecpp": {"repo": "https://github.com/AdaptiveCpp/AdaptiveCpp", "tag": "v25.10.0"},
  "ldpc_suite": {"origin": "cxl_ran_poc/gpu_daemon/ldpc_cl", "sha": "${LDPC_SHA}"},
  "images": [
    {"name": "oi/gpu-phy", "digest": "${GPUPHY_DIGEST}"},
    {"name": "oi/oracle", "digest": "${ORACLE_DIGEST}"}
  ]
}
JSON

echo "pins.json written to ${OUT}"
cat "$OUT"
