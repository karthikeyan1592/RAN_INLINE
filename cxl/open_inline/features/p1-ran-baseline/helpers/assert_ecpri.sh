#!/usr/bin/env bash
# assert_ecpri.sh — P1-R8. Captures on the fronthaul bridge (or classifies an already-captured
# pcap via --pcap-in, used by tests/classifier_test.sh) and asserts real eCPRI 0xAEFE C-plane AND
# U-plane frames flow in both directions.
#
# Classifier (LLD "eCPRI classifier" section, raw-offset fallback — normative so this also runs
# from busybox-grade tooling without tshark's ecpri/oran_fh_cus dissectors):
#   offset 12 (or 16 if 802.1Q tag 0x8100 present): ethertype -- must be 0xAEFE
#   eCPRI common header (first byte after ethertype): byte1 = message type
#     0x00 -> U-plane (IQ data) ; 0x02 -> C-plane (real-time control)
#   direction: src MAC == DU MAC -> dl ; src MAC == RU MAC -> ul (IF-P1-FRONTHAUL plan)
#
# Usage:
#   assert_ecpri.sh --pcap-in FILE [--du-mac MAC] [--ru-mac MAC]      # classify an existing pcap
#   assert_ecpri.sh [--iface IFACE] [--seconds 30] [--pcap-out FILE]  # live capture + classify
#
# Exit 0 iff c_dl, u_dl, u_ul are all nonzero (P1-R8; c_ul is observed-and-recorded only, Q3).
# Exit 2 on a setup/capture error (missing tcpdump, bad pcap, etc).
set -uo pipefail

IFACE=""
SECONDS_CAP=30
PCAP_OUT=""
PCAP_IN=""
DU_MAC="02:6f:69:00:01:02"
RU_MAC="02:6f:69:00:01:01"
PROJECT="${COMPOSE_PROJECT_NAME:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --iface) IFACE="$2"; shift 2 ;;
    --seconds) SECONDS_CAP="$2"; shift 2 ;;
    --pcap-out) PCAP_OUT="$2"; shift 2 ;;
    --pcap-in) PCAP_IN="$2"; shift 2 ;;
    --du-mac) DU_MAC="$2"; shift 2 ;;
    --ru-mac) RU_MAC="$2"; shift 2 ;;
    --project) PROJECT="$2"; shift 2 ;;
    *) echo "usage: assert_ecpri.sh [--iface IFACE] [--seconds N] [--pcap-out FILE] [--project NAME] | --pcap-in FILE [--du-mac M] [--ru-mac M]" >&2; exit 2 ;;
  esac
done

if [ -z "$PCAP_IN" ]; then
  if [ -z "$IFACE" ]; then
    # NOTE: relying on COMPOSE_PROJECT_NAME (or --project) rather than a CWD-basename guess --
    # compose derives its default project name from the FIRST -f file's directory (here,
    # p0-rig-scaffold/docker/ -> project "docker"), which will not generally match whatever
    # directory a script happens to be invoked from. bring_up.sh always sets/passes this
    # explicitly; this fallback exists only for ad-hoc manual invocation.
    if [ -z "$PROJECT" ]; then
      echo "error: --project (or COMPOSE_PROJECT_NAME) required to resolve the fronthaul bridge without --iface" >&2
      exit 2
    fi
    NETID="$(docker network inspect "${PROJECT}_fronthaul" --format '{{.Id}}' 2>/dev/null | cut -c1-12)"
    if [ -z "$NETID" ]; then
      echo "error: could not resolve fronthaul bridge (docker network inspect ${PROJECT}_fronthaul failed)" >&2
      exit 2
    fi
    IFACE="br-${NETID}"
  fi
  if ! command -v tcpdump >/dev/null 2>&1; then
    echo "error: tcpdump not found (host-side prerequisite)" >&2
    exit 2
  fi
  PCAP_OUT="${PCAP_OUT:-$(mktemp --suffix=.pcap)}"
  echo "capturing on ${IFACE} for ${SECONDS_CAP}s -> ${PCAP_OUT}" >&2
  if ! timeout "$((SECONDS_CAP + 5))" tcpdump -i "$IFACE" -w "$PCAP_OUT" \
      'ether proto 0xaefe or (vlan and ether proto 0xaefe)' -G "$SECONDS_CAP" -W 1 2>/dev/null; then
    echo "error: tcpdump capture failed on ${IFACE}" >&2
    exit 2
  fi
  PCAP_IN="$PCAP_OUT"
fi

python3 - "$PCAP_IN" "$DU_MAC" "$RU_MAC" << 'PYEOF'
import sys, json, struct

pcap_path, du_mac_str, ru_mac_str = sys.argv[1], sys.argv[2].lower(), sys.argv[3].lower()

def mac_str(b):
    return ":".join(f"{x:02x}" for x in b)

def read_pcap(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        return []
    frames = []
    off = 24
    while off + 16 <= len(data):
        incl_len = struct.unpack_from("<I", data, off + 8)[0]
        off += 16
        if off + incl_len > len(data):
            break
        frames.append(data[off:off + incl_len])
        off += incl_len
    return frames

frames = read_pcap(pcap_path)

counts = {"c_dl": 0, "c_ul": 0, "u_dl": 0, "u_ul": 0}
vlan_tagged = False
total_matched = 0

for frame in frames:
    if len(frame) < 14:
        continue
    dst, src = frame[0:6], frame[6:12]
    ethertype_off = 12
    if frame[12:14] == b"\x81\x00":
        ethertype_off = 16
        vlan_tagged = True
    if ethertype_off + 2 > len(frame) or frame[ethertype_off:ethertype_off + 2] != b"\xae\xfe":
        continue
    ecpri_off = ethertype_off + 2
    if ecpri_off + 2 > len(frame):
        continue
    msg_type = frame[ecpri_off + 1]
    src_str = mac_str(src)
    if src_str == du_mac_str:
        direction = "dl"
    elif src_str == ru_mac_str:
        direction = "ul"
    else:
        continue  # not from a known plan MAC -- not counted (LLD D4 keys classifier on plan MACs)

    total_matched += 1
    if msg_type == 0x02:
        counts[f"c_{direction}"] += 1
    elif msg_type == 0x00:
        counts[f"u_{direction}"] += 1

required_nonzero = counts["c_dl"] > 0 and counts["u_dl"] > 0 and counts["u_ul"] > 0
result = {
    "check": "ecpri", "frames": total_matched,
    "c_dl": counts["c_dl"], "u_dl": counts["u_dl"], "u_ul": counts["u_ul"], "c_ul": counts["c_ul"],
    "vlan_tagged": vlan_tagged,
}
print(json.dumps(result))
sys.exit(0 if required_nonzero else 1)
PYEOF
