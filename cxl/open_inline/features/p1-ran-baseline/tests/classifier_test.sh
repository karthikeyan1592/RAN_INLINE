#!/usr/bin/env bash
# classifier_test.sh — P1-R8 unit test: assert_ecpri.sh's classifier vs synth_ecpri_gen.cpp's own
# known-composition fixture (a REAL eCPRI+O-RAN CUS pcap built from real OCUDU builders — see
# tools/synth_ecpri_gen.cpp). Also covers the negative case (only the RU MAC's frames present ->
# c_dl/u_dl absent -> exit 1).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_PCAP="${ROOT}/tests/fixtures/synth_ecpri.pcap"
FIXTURE_JSON="${ROOT}/tests/fixtures/synth_ecpri.json"

FAIL=0
check() {
  if [ "$1" = "0" ]; then echo "PASS: $2"; else echo "FAIL: $2" >&2; FAIL=1; fi
}

if [ ! -f "$FIXTURE_PCAP" ]; then
  echo "error: fixture missing ($FIXTURE_PCAP) -- run 'make run-synth-ecpri-gen' first" >&2
  exit 2
fi

RESULT="$("${ROOT}/helpers/assert_ecpri.sh" --pcap-in "$FIXTURE_PCAP")"
RC=$?
check "$([ $RC -eq 0 ] && echo 0 || echo 1)" "assert_ecpri.sh exits 0 on the known-good fixture"

EXP_C_DL="$(python3 -c "import json; print(json.load(open('$FIXTURE_JSON'))['c_dl'])")"
EXP_U_DL="$(python3 -c "import json; print(json.load(open('$FIXTURE_JSON'))['u_dl'])")"
EXP_U_UL="$(python3 -c "import json; print(json.load(open('$FIXTURE_JSON'))['u_ul'])")"
EXP_C_UL="$(python3 -c "import json; print(json.load(open('$FIXTURE_JSON'))['c_ul'])")"

GOT_C_DL="$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin)['c_dl'])")"
GOT_U_DL="$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin)['u_dl'])")"
GOT_U_UL="$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin)['u_ul'])")"
GOT_C_UL="$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin)['c_ul'])")"
GOT_VLAN="$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin)['vlan_tagged'])")"

check "$([ "$GOT_C_DL" = "$EXP_C_DL" ] && echo 0 || echo 1)" "c_dl count matches fixture ground truth ($EXP_C_DL)"
check "$([ "$GOT_U_DL" = "$EXP_U_DL" ] && echo 0 || echo 1)" "u_dl count matches fixture ground truth ($EXP_U_DL)"
check "$([ "$GOT_U_UL" = "$EXP_U_UL" ] && echo 0 || echo 1)" "u_ul count matches fixture ground truth ($EXP_U_UL)"
check "$([ "$GOT_C_UL" = "$EXP_C_UL" ] && echo 0 || echo 1)" "c_ul count matches fixture ground truth ($EXP_C_UL, observed-only per Q3)"
check "$([ "$GOT_VLAN" = "True" ] && echo 0 || echo 1)" "classifier detects the VLAN-tagged frame (802.1Q handling)"

# Negative test: a pcap containing only RU-sourced frames (no DU C-plane/U-plane) must fail the
# gate distinctly (missing class, not a crash).
python3 - "$FIXTURE_PCAP" << 'PYEOF' > /tmp/ru_only.pcap
import sys, struct
with open(sys.argv[1], "rb") as f:
    data = f.read()
out = data[:24]
off = 24
RU_MAC = bytes.fromhex("026f69000101")
while off + 16 <= len(data):
    incl_len = struct.unpack_from("<I", data, off + 8)[0]
    frame = data[off + 16:off + 16 + incl_len]
    if frame[6:12] == RU_MAC:
        out += data[off:off + 16 + incl_len]
    off += 16 + incl_len
sys.stdout.buffer.write(out)
PYEOF

"${ROOT}/helpers/assert_ecpri.sh" --pcap-in /tmp/ru_only.pcap > /tmp/ru_only_result.json
RC=$?
check "$([ $RC -ne 0 ] && echo 0 || echo 1)" "negative test: RU-only pcap (no c_dl/u_dl) -> assert_ecpri.sh exits nonzero"

echo
if [ "$FAIL" -eq 0 ]; then
  echo "classifier_test: ALL PASS"
  exit 0
else
  echo "classifier_test: FAILURES ABOVE"
  exit 1
fi
