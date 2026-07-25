#!/usr/bin/env bash
# archive_pcap.sh — P1-R10, IF-P1-PCAPS. Archives an already-captured fronthaul pcap into the
# corpus layout (artifacts/p1/pcaps/<run-id>/fronthaul_000.pcap + manifest.json), rotating into
# multiple files if --max-mb is exceeded. The replay input contracted to p2-phy-kernels
# (p2f-integration's class-(a) structural gate).
#
# Usage: archive_pcap.sh --run-id ID --pcap-in FILE [--max-mb 512] [--out-root DIR] [--iface IFACE]
# Exit 0 on complete archive; verdict: {"check":"pcap_corpus","run_id":"...","files":k,"frames":N}
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ID=""
PCAP_IN=""
MAX_MB=512
OUT_ROOT="${ROOT}/../../artifacts/p1/pcaps"
IFACE="unknown"

while [ $# -gt 0 ]; do
  case "$1" in
    --run-id) RUN_ID="$2"; shift 2 ;;
    --pcap-in) PCAP_IN="$2"; shift 2 ;;
    --max-mb) MAX_MB="$2"; shift 2 ;;
    --out-root) OUT_ROOT="$2"; shift 2 ;;
    --iface) IFACE="$2"; shift 2 ;;
    *) echo "usage: archive_pcap.sh --run-id ID --pcap-in FILE [--max-mb 512] [--out-root DIR] [--iface IFACE]" >&2; exit 2 ;;
  esac
done

if [ -z "$RUN_ID" ] || [ -z "$PCAP_IN" ]; then
  echo "error: --run-id and --pcap-in are required" >&2
  exit 2
fi
if [ ! -f "$PCAP_IN" ]; then
  echo "error: pcap not found: $PCAP_IN" >&2
  exit 2
fi

OUT_DIR="${OUT_ROOT}/${RUN_ID}"
mkdir -p "$OUT_DIR"

CLASSIFY_RESULT="$("${ROOT}/helpers/assert_ecpri.sh" --pcap-in "$PCAP_IN" 2>/dev/null || true)"

PINS_JSON="${ROOT}/../p0-rig-scaffold/pins.json"
PINS_DIGEST="null"
if [ -f "$PINS_JSON" ]; then
  PINS_DIGEST="\"sha256:$(sha256sum "$PINS_JSON" | cut -d' ' -f1)\""
fi

GNB_CFG="${ROOT}/docker/configs/gnb_ofh_testmode.yml"
RU_CFG="${ROOT}/docker/configs/ru_emu.yml"
RIGCFG_DIGEST="$(python3 - "$GNB_CFG" "$RU_CFG" << 'PYEOF'
import sys, yaml, json, hashlib
canon = []
for path in sys.argv[1:]:
    with open(path) as f:
        canon.append(json.dumps(yaml.safe_load(f), sort_keys=True))
print(hashlib.sha256("".join(canon).encode()).hexdigest())
PYEOF
)"

HOST_KIND="wsl2"
if grep -qi microsoft /proc/version 2>/dev/null; then HOST_KIND="wsl2"; else HOST_KIND="linux"; fi
HOST_KERNEL="$(uname -r)"

# Rotate: split into <=MAX_MB-sized files if the pcap exceeds that bound (mechanical size-based
# rotation, not time-based -- P1-R10 only requires a bounded size, not a specific rotation policy).
FILES=()
MAX_BYTES=$((MAX_MB * 1024 * 1024))
SIZE=$(stat -c%s "$PCAP_IN")
if command -v tcpdump >/dev/null 2>&1 && [ "$SIZE" -gt "$MAX_BYTES" ]; then
  # Real bug found + fixed (2026-07-25, first time a capture actually exceeded MAX_MB and this
  # branch ran for real): tcpdump's `-C` rotation does NOT do printf-style filename substitution --
  # it just appends a raw, non-zero-padded integer suffix to the exact `-w` filename given (first
  # file keeps the literal name, then `.1`, `.2`, ...). The old `-w fronthaul_%03d.pcap` produced
  # files literally named `fronthaul_%03d.pcap`, `fronthaul_%03d.pcap1`, `fronthaul_%03d.pcap2` --
  # valid pcaps, just with a confusing literal "%03d" in every filename. Fixed to a plain name; glob
  # below picks up the base file plus tcpdump's own `.N` suffixes (sorted lexically, which is only
  # correct up to 9 fragments -- acceptable for this project's capture sizes, not a general solution).
  tcpdump -r "$PCAP_IN" -w "${OUT_DIR}/fronthaul.pcap" -C "$MAX_MB" 2>/dev/null
  for f in "${OUT_DIR}"/fronthaul.pcap*; do [ -f "$f" ] && FILES+=("$f"); done
fi
if [ "${#FILES[@]}" -eq 0 ]; then
  cp "$PCAP_IN" "${OUT_DIR}/fronthaul_000.pcap"
  FILES=("${OUT_DIR}/fronthaul_000.pcap")
fi

CAPTURED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
FRAMES="$(python3 -c "
import json, sys
try:
    d = json.loads(sys.argv[1]) if sys.argv[1].strip() else {}
except Exception:
    d = {}
print(d.get('frames', 0))
" "$CLASSIFY_RESULT")"

python3 - "$OUT_DIR/manifest.json" "$RUN_ID" "$CAPTURED_UTC" "$PINS_DIGEST" "$RIGCFG_DIGEST" \
         "$IFACE" "$HOST_KIND" "$HOST_KERNEL" "$CLASSIFY_RESULT" << 'PYEOF'
import sys, json

out_path, run_id, captured_utc, pins_digest_raw, rigcfg_digest, iface, host_kind, host_kernel, classify_raw = sys.argv[1:10]
pins_digest = None if pins_digest_raw == "null" else pins_digest_raw.strip('"')

try:
    classify = json.loads(classify_raw) if classify_raw.strip() else {}
except Exception:
    classify = {}

manifest = {
    "schema": "oi-p1-pcap/1",
    "run_id": run_id,
    "captured_utc": captured_utc,
    "pins_digest": pins_digest,
    "rigcfg_digest": f"sha256:{rigcfg_digest}",
    "iface": iface,
    "filter": "ether proto 0xaefe or (vlan and ether proto 0xaefe)",
    "mtu": 9000,
    "vlan_tagged": classify.get("vlan_tagged", False),
    "counts": {
        "c_dl": classify.get("c_dl", 0), "c_ul": classify.get("c_ul", 0),
        "u_dl": classify.get("u_dl", 0), "u_ul": classify.get("u_ul", 0),
    },
    "cell": {
        "band": "n78", "bw_mhz": 20, "scs_khz": 30, "nof_ant": 1,
        "eaxc": {"ul": [0], "dl": [0], "prach": [4]},
    },
    "host": {"kind": host_kind, "kernel": host_kernel},
}
with open(out_path, "w") as f:
    json.dump(manifest, f, indent=2)
PYEOF

NOF_FILES="${#FILES[@]}"
echo "{\"check\":\"pcap_corpus\",\"run_id\":\"${RUN_ID}\",\"files\":${NOF_FILES},\"frames\":${FRAMES}}"
exit 0
