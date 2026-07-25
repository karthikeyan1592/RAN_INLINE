#!/usr/bin/env bash
# ci_p1_static.sh — bundles every P1 check that does NOT require a live, SCTP-capable bring-up:
# P1-R1 (rendered-config diff), P1-R2 (fronthaul network shape), P1-R3 (network membership),
# P1-R5 (rigcfg_crosscheck, incl. negative test), P1-R6 (check_sctp.sh distinct exit-3 message),
# P1-R8 classifier unit test (P1-R11's own zero-project-code audit), P1-R11 (no compiled sources),
# P1-R12 (lint_no_perf.sh). What's NOT covered here: P1-R7/R9/G2 (need a live, SCTP-capable rig --
# environment-blocked on this host, see VERIFICATION.md, same status as p0's P0-R9/P0-G2).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P0_DOCKER="${ROOT}/../p0-rig-scaffold/docker"

FAIL=0
check() {
  if [ "$1" = "0" ]; then echo "PASS: $2"; else echo "FAIL: $2" >&2; FAIL=1; fi
}

export GNB_CONFIG_PATH="${ROOT}/docker/configs/gnb_ofh_testmode.yml"
export P1_RU_EMU_CONFIG_PATH="${ROOT}/docker/configs/ru_emu.yml"

BASE_CONFIG="$(docker compose -f "${P0_DOCKER}/upstream/docker/docker-compose.yml" config 2>/dev/null)"
FULL_CONFIG="$(docker compose -f "${P0_DOCKER}/upstream/docker/docker-compose.yml" \
                             -f "${P0_DOCKER}/compose.sim.yml" \
                             -f "${ROOT}/docker/compose.p1.yml" config 2>/dev/null)"

echo "=== P1-R1: upstream services unchanged except additive gnb.networks/command ==="
python3 - << PYEOF
import yaml, sys
base = yaml.safe_load("""$BASE_CONFIG""")
full = yaml.safe_load("""$FULL_CONFIG""")

ok = True
# 5gc must be byte-identical (structurally) between base and full-stack renderings.
if base["services"]["5gc"] != full["services"]["5gc"]:
    print("FAIL: 5gc service definition changed"); ok = False
else:
    print("PASS: 5gc service definition unchanged")

# gnb must differ ONLY by the added fronthaul network key and the MAC-resolution `command`
# override (config file CONTENT is expected to differ since GNB_CONFIG_PATH intentionally points
# elsewhere -- P1-R1 is about compose structure). The `command` addition is itself additive: it
# overrides how THIS overlay invokes the upstream binary, not the upstream image/default command
# (SPEC.md P1-R1's 2026-07-25 rationale note has the full story -- found necessary by the first
# real bring-up, not a design preference: gnb's eth0/eth1/eth2 network-to-device-name mapping is
# empirically unstable across container recreations on this host).
base_gnb, full_gnb = dict(base["services"]["gnb"]), dict(full["services"]["gnb"])
base_gnb.pop("networks", None); full_gnb.pop("networks", None)
base_gnb.pop("command", None); full_gnb.pop("command", None)
if base_gnb != full_gnb:
    print("FAIL: gnb service definition changed beyond its networks/command keys"); ok = False
else:
    print("PASS: gnb service definition unchanged outside its networks/command keys")
if "command" not in full["services"]["gnb"]:
    print("FAIL: expected gnb.command override (MAC-based interface resolution) missing"); ok = False
else:
    print("PASS: gnb.command override present (MAC-based fronthaul interface resolution)")

base_nets = set(base["services"]["gnb"].get("networks", {}).keys())
full_nets = set(full["services"]["gnb"].get("networks", {}).keys())
if full_nets - base_nets == {"fronthaul"} and base_nets <= full_nets:
    print("PASS: gnb.networks gained exactly {fronthaul}, nothing removed")
else:
    print(f"FAIL: gnb.networks diff unexpected: base={base_nets} full={full_nets}"); ok = False

sys.exit(0 if ok else 1)
PYEOF
check "$?" "P1-R1 rendered-config diff"

echo "=== P1-R2: fronthaul network shape ==="
python3 - << PYEOF
import yaml, sys
full = yaml.safe_load("""$FULL_CONFIG""")
fh = full.get("networks", {}).get("fronthaul", {})
ok = (fh.get("driver") == "bridge" and
      fh.get("driver_opts", {}).get("com.docker.network.driver.mtu") == "9000")
print("PASS" if ok else "FAIL", "fronthaul network: driver=bridge, mtu=9000:", fh)
sys.exit(0 if ok else 1)
PYEOF
check "$?" "P1-R2 fronthaul network driver+MTU"

echo "=== P1-R3: network membership (ru-emu only fronthaul, 5gc never on fronthaul) ==="
python3 - << PYEOF
import yaml, sys
full = yaml.safe_load("""$FULL_CONFIG""")
ru_nets = set(full["services"]["ru-emu"].get("networks", {}).keys())
gc_nets = set(full["services"]["5gc"].get("networks", {}).keys())
ok = (ru_nets == {"fronthaul"}) and ("fronthaul" not in gc_nets)
print("PASS" if ok else "FAIL", f"ru-emu networks={ru_nets}, 5gc networks={gc_nets}")
sys.exit(0 if ok else 1)
PYEOF
check "$?" "P1-R3 fronthaul/backhaul membership"

echo "=== P1-R5: rigcfg_crosscheck ==="
"${ROOT}/helpers/rigcfg_crosscheck.sh" >/dev/null 2>&1
check "$?" "rigcfg_crosscheck.sh passes on the real config pair"

echo "=== P1-R6: check_sctp.sh distinct exit-3 message ==="
MSG="$("${ROOT}/helpers/check_sctp.sh" 2>&1)"
RC=$?
if [ "$RC" = "0" ]; then
  check 0 "check_sctp.sh (SCTP available on this host)"
elif [ "$RC" = "3" ] && echo "$MSG" | grep -q "CONFIG_IP_SCTP"; then
  check 0 "check_sctp.sh exits 3 with the actionable CONFIG_IP_SCTP message (expected on this host)"
else
  check 1 "check_sctp.sh unexpected exit ($RC): $MSG"
fi

echo "=== P1-R8: classifier unit test ==="
bash "${ROOT}/tests/classifier_test.sh" >/tmp/p1_classifier_out.txt 2>&1
check "$?" "classifier_test.sh (see /tmp/p1_classifier_out.txt for detail)"

echo "=== P1-R11: zero project code ==="
COMPILED_SRC="$(find "$ROOT" -name "*.cl" -o -name "*.cu" 2>/dev/null | grep -v /build/ | grep -v tools/)"
if [ -z "$COMPILED_SRC" ]; then
  check 0 "no OpenCL/CUDA kernel sources in the feature tree (tools/*.cpp is host-side test tooling only, never runs in a rig container)"
else
  check 1 "unexpected kernel sources found: $COMPILED_SRC"
fi

echo "=== P1-R12: lint_no_perf.sh ==="
bash "${ROOT}/helpers/lint_no_perf.sh" >/dev/null 2>&1
check "$?" "lint_no_perf.sh"

echo
if [ "$FAIL" -eq 0 ]; then
  echo "ci_p1_static: ALL PASS"
  exit 0
else
  echo "ci_p1_static: FAILURES ABOVE"
  exit 1
fi
