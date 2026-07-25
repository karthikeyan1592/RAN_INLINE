#!/usr/bin/env bash
# kpi_snapshot.sh — IF-P1-KPI. Emits a KPI snapshot; NEVER judges pass/fail (per LLD: "judgment is
# in soak/gate scripts, keeping 'clean' criteria in one place"). Exit 2 only, on a setup error
# (container not found) — never exit 0/1, since this script makes no pass/fail claim of its own.
#
# LLD Q4 RESOLVED FOR REAL (2026-07-25, GCP VM, real OCUDU source read + live verification --
# superseding the old "unverified mapping" note this header used to carry):
#
# 1. gnb's `metrics.enable_json` JSON metrics do NOT go to stdout, a file, or any passive log at
#    all -- confirmed by reading apps/gnb/gnb.cpp + apps/services/remote_control/remote_server.cpp:
#    they're pushed over a WebSocket the app itself hosts (`remote_control: {enabled, bind_addr,
#    port}`), and ONLY to clients that first send a `{"cmd":"metrics_subscribe"}` text frame. This
#    fully explains every earlier "JSON metrics never appear anywhere" finding -- nothing was ever
#    supposed to reach stdout/log, ever, regardless of log level or filename.
# 2. Even after subscribing, the real payload (apps/helpers/metrics/json_generators/du_high/
#    mac.cpp + include/ocudu/mac/mac_metrics.h, read directly from the pinned OCUDU checkout) has
#    NO byte/throughput counters at the MAC layer at all -- only per-cell latency
#    (average/min/max_latency_us) and cpu_usage_percent. RLC/PDCP (which normally carry SDU/PDU
#    byte counts in a real stack) aren't even active in this MAC-test-mode config (traffic is
#    injected directly at the MAC layer -- see compose.p1.yml's own note on this). So there is
#    NO cumulative counter available from gnb's self-reported metrics, by ANY route (JSON, log
#    table, or stdout) -- not a mapping gap, a real absence confirmed by reading the source that
#    generates the JSON.
# 3. Real, working fix: P1-R9's "functional counters strictly increase" is satisfied instead by
#    the fronthaul NIC's own kernel-level counters (/sys/class/net/<iface>/statistics/{tx,rx}_
#    bytes) inside the gnb container -- genuinely monotonic, already the exact mechanism used
#    earlier this session to detect the WSL2 TX stall via the same sysfs path. Resolves the
#    fronthaul interface the same MAC-based way compose.p1.yml's own command wrapper already does
#    (interface naming is non-deterministic across recreations -- Q6). Verified live: 50,476,981,884
#    tx_bytes / 13,066,925,502 rx_bytes on a running rig (2026-07-25).
#
# gnb's JSON metrics blob itself is left unconsumed (no counters worth extracting for THIS gate;
# a WebSocket subscribe client would be real extra complexity for latency/cpu stats this schema
# doesn't need) -- ru_emu's stdout KPI table parse (Q4's other half) also remains unimplemented,
# same as before, emitted as `null`, never fabricated.
set -uo pipefail

GNB_CONTAINER="${1:-ocudu_gnb}"
RU_CONTAINER="${2:-ocudu_ru_emu}"
GNB_FRONTHAUL_MAC="02:6f:69:00:01:02"   # pinned in compose.p1.yml -- same constant, not re-derived

if ! docker inspect "$GNB_CONTAINER" >/dev/null 2>&1; then
  echo "{\"check\":\"kpi_snapshot\",\"error\":\"container ${GNB_CONTAINER} not found\"}" >&2
  exit 2
fi

FRONTHAUL_STATS="$(docker exec "$GNB_CONTAINER" sh -c "
  IFACE=\$(ip -o link show | grep -i '${GNB_FRONTHAUL_MAC}' | awk -F': ' '{print \$2}' | cut -d'@' -f1)
  if [ -z \"\$IFACE\" ]; then exit 1; fi
  cat /sys/class/net/\$IFACE/statistics/tx_bytes /sys/class/net/\$IFACE/statistics/rx_bytes
" 2>/dev/null || true)"
FRONTHAUL_TX_BYTES="$(echo "$FRONTHAUL_STATS" | sed -n '1p')"
FRONTHAUL_RX_BYTES="$(echo "$FRONTHAUL_STATS" | sed -n '2p')"

python3 - "${FRONTHAUL_TX_BYTES:-}" "${FRONTHAUL_RX_BYTES:-}" << 'PYEOF'
import sys, json
from datetime import datetime, timezone

def to_int_or_none(s):
    try:
        return int(s)
    except (ValueError, TypeError):
        return None

tx_bytes = to_int_or_none(sys.argv[1])
rx_bytes = to_int_or_none(sys.argv[2])

snapshot = {
    "schema": "oi-p1-kpi/1",
    "t_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "gnb": {
        "ng_setup": None,  # derived from log-pattern match, not this snapshot -- see soak_stability.sh
        "fronthaul_tx_bytes": tx_bytes,  # real kernel sysfs counter, monotonic -- see header item 3
        "fronthaul_rx_bytes": rx_bytes,
        "mac_ul_bytes": None,  # confirmed NOT to exist in OCUDU's JSON metrics schema -- see header item 2
        "mac_dl_bytes": None,
    },
    "ru_emu": {"eaxc": []},  # ru_emulator's KPI table parse -- still not implemented, real gap, not fabricated
    "note": "rx_on_time/early/late are timing observations -- SIM, not gated, not quotable. "
           "mac_ul_bytes/mac_dl_bytes are null because those fields don't exist in OCUDU's real "
           "metrics schema (confirmed via source), not because of an extraction failure -- use "
           "fronthaul_tx_bytes/fronthaul_rx_bytes for counters_monotonic instead (see this script's header).",
}
print(json.dumps(snapshot))
PYEOF
