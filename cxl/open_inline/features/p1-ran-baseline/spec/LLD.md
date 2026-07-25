# p1-ran-baseline — LLD

Companion to [`SPEC.md`](SPEC.md) / [`HLD.md`](HLD.md). Design document; paths are the layout the
implementation must produce. Upstream paths verified against `/root/linux_env/cxl/third_party/ocudu`.

## Module breakdown

```
features/p1-ran-baseline/
  docker/
    compose.p1.yml            # fronthaul net + ru-emu service + additive gnb extension (P1-R1..R3)
    configs/
      gnb_ofh_testmode.yml    # ru_ofh cell + cell_cfg + test_mode (P1-R5)
      ru_emu.yml              # ru_emulator socket-mode config (P1-R4)
  helpers/
    check_sctp.sh             # single source, shared with p0 (P1-R6)
    assert_ecpri.sh           # capture + classify 0xAEFE C/U-plane per direction (P1-R8)
    soak_stability.sh         # 600 s stability checker (P1-R9)
    archive_pcap.sh           # corpus + manifest writer (P1-R10)
    kpi_snapshot.sh           # reads gnb stdout-JSON metrics + ru-emu KPI table (P1-G1, IF-P1-KPI)
    lint_no_perf.sh           # greps feature tree for forbidden threshold keys (P1-R12)
    rigcfg_crosscheck.sh      # ru_emu.yml ⟷ gnb_ofh_testmode.yml consistency (P1-R5)
  tests/                      # CI wrappers: static rendered-config checks + gate scripts
```

No `src/` content: P1 is zero project code (P1-R11); everything above is config or shell.

## Public APIs (script CLI contracts — IF-P1-ASSERT)

Exit-code convention (shared with p0): `0` pass · `1` fail · `2` setup/IO error · `3`
precondition-blocked. Last stdout line of every script is one JSON object (verdict line).

```text
check_sctp.sh
  exit 0 | 3; verdict: {"check":"sctp","present":true|false}
  detection order: grep -q '^SCTP' /proc/net/protocols → modprobe sctp → re-grep.

assert_ecpri.sh [--iface IFACE] [--seconds 30] [--pcap-out FILE]
  captures on the fronthaul bridge host interface (default: resolved from
  `docker network inspect <project>_fronthaul` → br-<id>), filter:
  'ether proto 0xaefe or (vlan and ether proto 0xaefe)'.
  Classifies per frame: {plane: c|u, dir: dl|ul} (see Data structures).
  exit 0 iff all four required classes nonzero per P1-R8 (c/dl, u/dl, u/ul; c/ul counted
  if present but not required — see Q3).
  verdict: {"check":"ecpri","frames":N,"c_dl":n1,"u_dl":n2,"u_ul":n3,"c_ul":n4,"vlan_tagged":bool}

soak_stability.sh [--seconds 600] [--sample-every 60]
  exit 0 iff P1-R9(a–d) all hold; verdict:
  {"check":"soak","seconds":600,"restarts":0,"new_error_lines":0,
   "counters_monotonic":true,"ngap_stable":true}

archive_pcap.sh --run-id ID [--max-mb 512]
  rotates capture into corpus layout (below), writes manifest; exit 0 on complete archive.
  verdict: {"check":"pcap_corpus","run_id":"...","files":k,"frames":N}

kpi_snapshot.sh [--format json]
  emits IF-P1-KPI snapshot (below); exit 2 only (it never judges — judgment is in soak/gate
  scripts, keeping "clean" criteria in one place).

rigcfg_crosscheck.sh
  exit 0 iff all cross-consistency rules (below) hold between the two YAMLs.
```

## Data structures & formats

### Fronthaul plan (IF-P1-FRONTHAUL — normative values)

| Item | Value | Note |
|---|---|---|
| network name | `fronthaul` | compose bridge, `com.docker.network.driver.mtu: "9000"` |
| subnet | `10.60.1.0/24` | disjoint from upstream `ran` 10.53.1.0/24 and `metrics` 172.19.1.0/24; IP unused by OFH (raw L2) but compose requires IPAM |
| ru-emu MAC | `02:6f:69:00:01:01` | locally-administered; compose `mac_address` on the fronthaul endpoint |
| gnb (DU) MAC | `02:6f:69:00:01:02` | idem |
| VLAN TCI | `1` (both `vlan_tag_cp` and `vlan_tag_up`; ru-emu `vlan_tag: 1`) | see Q1 |
| eAxC ports | `ul_port_id: [0]`, `dl_port_id: [0]`, `prach_port_id: [4]` | 1 cell, 1×1; ru-emu defaults trimmed to match |
| cell | TDD n78, 20 MHz, SCS 30 kHz, 1 antenna | HLD D5; `ru_emulator` `bandwidth: MHz20` |
| UL compression | `bfp`, 9 bit (upstream default on both sides) | must match on both YAMLs |

### eCPRI classifier (frame parse used by assert_ecpri.sh)

```
offset 12 (or 16 if 802.1Q tag 0x8100 present): ethertype — must be 0xAEFE
eCPRI common header (first byte after ethertype):
  byte0 = revision/reserved/concat ; byte1 = message type
  msg type 0x00 → U-plane (IQ data) ; 0x02 → C-plane (real-time control)
direction: src MAC == DU MAC → dl ; src MAC == RU MAC → ul   (plan MACs above)
```
Preferred implementation: `tshark` with its `ecpri`/`oran_fh_cus` dissectors when available;
the raw-offset fallback above is normative so the check also runs from busybox-grade tooling.

### Pcap corpus layout + manifest (IF-P1-PCAPS)

```
artifacts/p1/pcaps/<run-id>/
  fronthaul_000.pcap …        # rotated, --max-mb bound
  manifest.json
```

```json
{"schema":"oi-p1-pcap/1","run_id":"...","captured_utc":"...",
 "pins_digest":"sha256:...","rigcfg_digest":"sha256:...",
 "iface":"br-...","filter":"ether proto 0xaefe or (vlan and ether proto 0xaefe)",
 "mtu":9000,"vlan_tagged":true,
 "counts":{"c_dl":0,"c_ul":0,"u_dl":0,"u_ul":0},
 "cell":{"band":"n78","bw_mhz":20,"scs_khz":30,"nof_ant":1,
         "eaxc":{"ul":[0],"dl":[0],"prach":[4]}},
 "host":{"kind":"wsl2|gcp","kernel":"..."}}
```

`rigcfg_digest` = sha256 over the two rig YAMLs (canonicalized) — p2/p3 refuse a corpus whose
digest mismatches their pinned rig configuration.

### IF-P1-KPI snapshot

```json
{"schema":"oi-p1-kpi/1","t_utc":"...",
 "gnb":{"ng_setup":true,"testmode_ues":1,
        "mac_ul_bytes":0,"mac_dl_bytes":0,"pusch_crc_ok":0,"pusch_crc_nok":0},
 "ru_emu":{"eaxc":[{"id":0,"rx_total":0,"rx_on_time":0,"rx_early":0,"rx_late":0}]},
 "note":"rx_on_time/early/late are timing observations — SIM, not gated, not quotable"}
```
Sources: gnb stdout JSON metrics (upstream compose already sets `autostart_stdout_metrics: true`,
`enable_json: true`); ru_emulator periodic stdout KPI table (parsed; exact upstream column set
recorded at implementation — Q4).

## Configuration (YAML/env schema)

### compose.p1.yml (normative shape)

```yaml
services:
  gnb:                          # additive extension of upstream service only (P1-R1)
    networks:
      fronthaul: {mac_address: "02:6f:69:00:01:02"}
  ru-emu:
    image: ocudu/ru-emulator:${OI_TAG:-release_26_04}
    build:
      context: ../../../third_party/ocudu     # actual path fixed at implementation; upstream tree, unmodified
      dockerfile: docker/Dockerfile
      args: {COMPONENT: ru-emulator, OS_VERSION: "24.04"}
    networks:
      fronthaul: {mac_address: "02:6f:69:00:01:01"}
    configs: [ru_emu_config.yml]
    command: ru_emulator -c /ru_emu_config.yml
    depends_on: {gnb: {condition: service_started}}
configs:
  ru_emu_config.yml: {file: ./configs/ru_emu.yml}
networks:
  fronthaul:
    driver: bridge
    driver_opts: {com.docker.network.driver.mtu: "9000"}
    ipam: {config: [{subnet: 10.60.1.0/24}]}
```

Env (via `.env`, consumed by upstream parameterization — no upstream edits):
`GNB_CONFIG_PATH=…/gnb_ofh_testmode.yml`, `OS_VERSION=24.04` (pins the upstream compose's
`${OS_VERSION:-26.04}` default down to the project's Ubuntu 24.04 pin — see Q5/flagged gap).

### gnb_ofh_testmode.yml (normative keys; upstream schema)

```yaml
ru_ofh:
  cells:
    - network_interface: eth1          # container's fronthaul iface (Q6: name resolution)
      ru_mac_addr: 02:6f:69:00:01:01
      du_mac_addr: 02:6f:69:00:01:02
      vlan_tag_cp: 1
      vlan_tag_up: 1
      ul_port_id: [0]
      dl_port_id: [0]
      prach_port_id: [4]
  compr_method_ul: bfp
  compr_bitwidth_ul: 9
cell_cfg: {dl_arfcn: 625000, band: 78, channel_bandwidth_MHz: 20,
           common_scs: 30, nof_antennas_dl: 1, nof_antennas_ul: 1, plmn: "00101", tac: 7}
test_mode:
  test_ue: {rnti: 0x44, ri: 1, cqi: 15, nof_ues: 1, pusch_active: true, pdsch_active: true}
cu_cp: {}   # AMF addrs come from the upstream compose-injected config (gnb_compose_config.yml)
```

### ru_emu.yml (normative keys; upstream `ru_emulator_appconfig` schema)

```yaml
log: {level: info, filename: stdout}
ru_emu:
  - network_interface: eth0
    ru_mac_address: 02:6f:69:00:01:01
    du_mac_address: 02:6f:69:00:01:02
    vlan_tag: 1
    bandwidth: 20
    ul_compr_method: bfp
    ul_compr_bitwidth: 9
    ru_ul_port_id: [0]
    ru_dl_port_id: [0]
    ru_prach_port_id: [4]
# NO dpdk_config key — its absence selects the socket transceiver (upstream selection logic)
```

### Cross-consistency rules (rigcfg_crosscheck.sh)

MACs equal pairwise-swapped; VLAN tags equal; bandwidth/SCS equal; eAxC port lists equal;
UL compression method+bitwidth equal; `dpdk_config` absent; any violation → exit 1 with a
field-by-field diff.

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| Host lacks SCTP | `check_sctp.sh` | exit 3 + actionable WSL2 message (P1-R6); soak/gate scripts refuse to start |
| Bridge MTU ≠ 9000 (host quirk) | `assert_ecpri.sh` pre-check via `ip link` | exit 2 (rig defect, not protocol fail) with observed MTU |
| No 0xAEFE frames at all | classifier count 0 | exit 1; dump last 100 gnb + ru-emu log lines; hint list: MAC plan mismatch, iface name, VLAN mismatch |
| One plane/direction class zero | classifier | exit 1; verdict shows which class; hint: C-only ⇒ ru-emu not answering (config mismatch); U-dl-only ⇒ test_mode inactive |
| gnb never reaches NG Setup | 60 s log-pattern timeout | exit 1; distinguish SCTP connect-refused (5gc unhealthy → exit 2) from timeout |
| Container restart during soak | `docker inspect` RestartCount delta | exit 1; identify service; attach its logs |
| Counter stalls (not monotonic) | consecutive kpi_snapshot diffs | exit 1; name the stalled counter |
| Capture tool missing / not permitted | tcpdump probe at start | exit 2 with install/permission hint (host-side prerequisite, documented in helpers README) |
| Rig YAML cross-check fails | `rigcfg_crosscheck.sh` at bring-up | exit 2 before any container starts (config bug, not a RAN failure) |

## Test plan (per requirement)

| Req | Test |
|---|---|
| P1-R1 | CI static: `docker compose config` (base) vs (base+overlay): upstream service diff contains only the added `gnb.networks.fronthaul` key and env-resolved values; upstream files byte-identical to p0 vendored tree. |
| P1-R2 | `docker network inspect` → mtu option 9000; `ip link` on bridge + both veths reports 9000; member set exactly {ru-emu, gnb}. |
| P1-R3 | Rendered config: `ru-emu.networks` = {fronthaul} only; `5gc.networks` ∌ fronthaul; `ran` definition identical to upstream. |
| P1-R4 | Image label/pins: built from pinned OCUDU SHA with `COMPONENT=ru-emulator`; `ru_emu.yml` contains no `dpdk_config` (crosscheck rule); runtime log shows socket transceiver (no EAL init lines). |
| P1-R5 | `rigcfg_crosscheck.sh` exit 0; gnb accepts config (clean startup, no config-parse errors); negative test: perturb one field → crosscheck exit 1. |
| P1-R6 | On SCTP host: exit 0. On a namespace/kernel without SCTP (CI mock: bind-mount empty `/proc/net/protocols` stub or assert message text via unit harness): exit 3 + exact documented message. |
| P1-R7 | Bring-up run: NG-setup success pattern within 60 s of 5gc healthy; negative: stop 5gc → pattern absent, correct error class. |
| P1-R8 | `assert_ecpri.sh --seconds 30` exit 0 with all required classes nonzero; classifier unit-tested against a canned pcap with known composition (tagged and untagged variants). |
| P1-R9 | `soak_stability.sh --seconds 600` exit 0 on the live rig; injected-fault rehearsals: kill ru-emu mid-soak → restart detected; pause traffic → monotonicity failure detected. |
| P1-R10 | After a passing soak: corpus files + manifest exist; manifest counts equal classifier counts; `rigcfg_digest` recomputes identically; p2-facing check: manifest validates against `oi-p1-pcap/1` schema. |
| P1-R11 | Pins audit: rig image digests ∈ upstream-built set; `grep -r` over feature tree finds no compiled-language sources; assertion scripts run only host-side or read-only `docker exec`. |
| P1-R12 | `lint_no_perf.sh`: grep feature tree for threshold-bearing keys (`*_max_us`, `latency`, `throughput`, `gbps`, `mbps` as gate operands) → zero hits in gating logic; manual review that timing KPIs appear only in the `note`-tagged snapshot fields. |

Gate mapping: **P1-G1** = {R4, R5, R6, R7} + kpi_snapshot nonzero-traffic check ·
**P1-G2** = {R8, R9, R10} (R1–R3, R11, R12 are CI-static preconditions to both).

## Open questions

1. **Q1 — VLAN on the bridge:** upstream OFH configs always carry VLAN tags; whether
   `release_26_04` socket ether emits 802.1Q on the wire in all cases (and whether tag 1 vs
   upstream-example 3 matters) is confirmed at implementation. The classifier and MTU plan handle
   both tagged/untagged; the chosen TCI is recorded in the pcap manifest either way.
2. **Q2 — compose `mac_address` per-network support:** requires a compose version supporting
   per-endpoint `mac_address`. If the pinned docker/compose on WSL2 or GCP lacks it, fall back to
   service-level `mac_address` (single-homed ru-emu is safe; dual-homed gnb needs verification) or
   `enable_promiscuous: true` + bridge flooding (HLD D4 fallback). Decision recorded in pins.
3. **Q3 — UL C-plane presence:** whether the DU emits UL C-plane distinctly and whether ru-emu
   emits any C-plane at all in this rig shape. P1-R8 requires C-plane from the DU + U-plane both
   directions; `c_ul` is observed-and-recorded only. Tighten after first capture if upstream
   reliably produces it.
4. **Q4 — exact ru_emulator KPI table format:** column set/format of the periodic stdout KPI dump
   is parsed at implementation; `oi-p1-kpi/1` reserves the fields shown and versions up if the
   table differs.
5. **Q5 — `OS_VERSION` pin:** upstream monolithic compose defaults `26.04`, split compose `24.04`,
   project pin is Ubuntu 24.04 (features README). This LLD pins `OS_VERSION=24.04` via `.env`;
   flagged upward as an upstream-inconsistency note (see spec report).
6. **Q6 — container iface naming:** mapping of compose networks to `eth0/eth1` inside dual-homed
   `gnb` is attachment-order dependent. Resolution strategy (query by MAC via
   `ip -o link | grep <mac>` in an entrypoint-free way, i.e. resolve at config-generation time on
   the host, or pin attachment order) decided at implementation; the chosen mechanism must not
   modify upstream images (P1-R11).
7. **Q7 — gnb ERROR-log baseline list:** the tolerated post-bring-up pattern list (e.g. benign
   late-frame warnings on non-RT hosts) is captured empirically on first bring-up and versioned
   with the rig configs; the soak checker compares against it (P1-R9(b)).
