#!/usr/bin/env bash
# rigcfg_crosscheck.sh — P1-R5. Cross-checks gnb_ofh_testmode.yml <-> ru_emu.yml consistency
# (IF-P1-RIGCFG's cross-consistency rules, LLD "Configuration" section): MACs equal
# pairwise-swapped, VLAN tags equal, bandwidth/SCS equal, eAxC port lists equal, UL compression
# method+bitwidth equal, no dpdk_config on the ru-emu side.
#
# Exit 0 iff all rules hold. Exit 1 with a field-by-field diff otherwise. Exit 2 on a YAML
# load/shape error (config bug, not a rig failure — LLD's own error-handling table).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GNB_CFG="${1:-${ROOT}/docker/configs/gnb_ofh_testmode.yml}"
RU_CFG="${2:-${ROOT}/docker/configs/ru_emu.yml}"

python3 - "$GNB_CFG" "$RU_CFG" << 'PYEOF'
import sys, json
try:
    import yaml
except ImportError:
    print(json.dumps({"check": "rigcfg_crosscheck", "error": "PyYAML not available"}))
    sys.exit(2)

gnb_path, ru_path = sys.argv[1], sys.argv[2]
try:
    with open(gnb_path) as f:
        gnb = yaml.safe_load(f)
    with open(ru_path) as f:
        ru = yaml.safe_load(f)
except Exception as e:
    print(json.dumps({"check": "rigcfg_crosscheck", "error": f"YAML load failed: {e}"}))
    sys.exit(2)

try:
    cell = gnb["ru_ofh"]["cells"][0]
    ru_cell = ru["ru_emu"]["cells"][0]
except (KeyError, IndexError, TypeError) as e:
    print(json.dumps({"check": "rigcfg_crosscheck", "error": f"unexpected YAML shape: {e}"}))
    sys.exit(2)

diffs = []

def rule(name, cond, detail):
    if not cond:
        diffs.append({"rule": name, "detail": detail})

# MACs equal pairwise-swapped: gnb's ru_mac == ru-emu's own MAC, gnb's du_mac == ru-emu's du_mac.
rule("mac_pairwise_swap",
    cell["ru_mac_addr"].lower() == ru_cell["ru_mac_addr"].lower() and
    cell["du_mac_addr"].lower() == ru_cell["du_mac_addr"].lower(),
    f"gnb.ru_ofh.cells[0]=({cell.get('ru_mac_addr')},{cell.get('du_mac_addr')}) vs "
    f"ru_emu.cells[0]=({ru_cell.get('ru_mac_addr')},{ru_cell.get('du_mac_addr')})")

# VLAN: gnb has separate cp/up tags, ru-emu has one combined tag; both must equal it.
rule("vlan_tag",
    cell["vlan_tag_cp"] == cell["vlan_tag_up"] == ru_cell["vlan_tag"],
    f"gnb vlan_tag_cp={cell.get('vlan_tag_cp')} vlan_tag_up={cell.get('vlan_tag_up')} vs "
    f"ru_emu vlan_tag={ru_cell.get('vlan_tag')}")

# Bandwidth/SCS: gnb's cell_cfg carries these; ru-emu only carries bandwidth (numeric MHz).
cell_cfg = gnb.get("cell_cfg", {})
rule("bandwidth",
    cell_cfg.get("channel_bandwidth_MHz") == ru_cell.get("bandwidth"),
    f"gnb cell_cfg.channel_bandwidth_MHz={cell_cfg.get('channel_bandwidth_MHz')} vs "
    f"ru_emu bandwidth={ru_cell.get('bandwidth')}")

# eAxC port lists.
for port in ("ul_port_id", "dl_port_id", "prach_port_id"):
    rule(f"eaxc_{port}", cell.get(port) == ru_cell.get(port),
        f"gnb {port}={cell.get(port)} vs ru_emu {port}={ru_cell.get(port)}")

# UL compression method+bitwidth.
ru_ofh = gnb.get("ru_ofh", {})
rule("ul_compression",
    ru_ofh.get("compr_method_ul") == ru_cell.get("compr_method_ul") and
    ru_ofh.get("compr_bitwidth_ul") == ru_cell.get("compr_bitwidth_ul"),
    f"gnb ru_ofh.compr_method_ul/bitwidth_ul={ru_ofh.get('compr_method_ul')}/"
    f"{ru_ofh.get('compr_bitwidth_ul')} vs ru_emu={ru_cell.get('compr_method_ul')}/"
    f"{ru_cell.get('compr_bitwidth_ul')}")

# No dpdk_config on the ru-emu side (D2: socket transceiver only).
rule("no_dpdk_config", "dpdk" not in ru, "ru_emu.yml unexpectedly contains a 'dpdk' section")

result = {"check": "rigcfg_crosscheck", "ok": len(diffs) == 0, "diffs": diffs}
print(json.dumps(result))
sys.exit(0 if not diffs else 1)
PYEOF
