# p1-ran-baseline — SPEC

**Feature:** SIM phase P1 — pure-upstream RAN baseline rig.
**Tier:** SIM (T1). **Authority:** [`ARCHITECTURE_v3_SIM.md`](../../../ARCHITECTURE_v3_SIM.md)
§1/§2/§4 (P1 row of §4 is the gate source); master: [`ARCHITECTURE_v3.md`](../../../ARCHITECTURE_v3.md)
§6.3(b). **Pins:** OCUDU `release_26_04` (`gitlab.com/ocudu/ocudu`, BSD-3; re-pin rationale:
[`research/ocudu_repin.md`](../../../research/ocudu_repin.md)). Local verified clone:
`/root/linux_env/cxl/third_party/ocudu`.

**Backend statement (SIM §3):** P1 implements **no backend**. It is deliberately zero-project-code:
every process is unmodified OCUDU or Open5GS. What P1 delivers to the backend contract is the
**wire** — a real fronthaul bridge carrying real eCPRI — that the SIM `ingest_backend`
(`p3-live-tap-ul-inject`) later taps, and the pcap corpus the `compute_backend` pipeline
(`p2-phy-kernels`) replays.

## Purpose

Bring up the full containerized RAN with **zero project code**: OCUDU `ru_emulator`
(`apps/examples/ofh/`) ↔ OCUDU gNB (CU+DU, split 7.2x OFH, test-mode MAC-emulated UEs) ↔ Open5GS
5GC, on two compose networks (`fronthaul` bridge with MTU 9000 — the eCPRI wire — and the
backhaul network for NGAP/GTP-U). Prove, by capture on the fronthaul bridge, that **real eCPRI
(ethertype `0xAEFE`) C-plane and U-plane frames** flow, and that the rig is stable for 10 minutes.
Everything downstream (P2 pcap replay, P3 live tap/injection, P4 seam, P5 automation) assumes this
rig exists and behaves exactly as specified here.

## In scope

- Compose overlay (`compose.p1.yml`) on the p0-vendored **monolithic** OCUDU
  `docker/docker-compose.yml` (base-compose decision: HLD §Design decisions D1) adding:
  the `fronthaul` network (bridge, MTU 9000), the `ru-emu` service, and additive-only extensions
  to the upstream `gnb` service (join `fronthaul`, config selection via upstream-provided env vars).
- `ru-emu` image built from the **upstream OCUDU Dockerfile** with `COMPONENT=ru-emulator`
  (no project Dockerfile), running the **socket transceiver** (transceiver decision: HLD D2).
- Rig configuration files (pure YAML, no code): gNB config = split-7.2x `ru_ofh` cell +
  `test_mode` test UEs (upstream `configs/testmode.yml` pattern); matching `ru_emulator` config.
- Assertion tooling (scripts only, run from the host or a tooling container): SCTP precondition
  check (shared with p0), eCPRI capture/classifier on the fronthaul bridge, KPI/stability checker,
  pcap corpus archiver.
- Definition of the P1 KPI-clean criteria (functional counters only — see P1-R9 and Honesty notes).

## Out of scope

- Any patched upstream source (→ `p3-live-tap-ul-inject`); any project kernel or pipeline
  (→ `p2-phy-kernels`); seam IPC (→ `p4-phy-l2-seam`); orchestration/ledger (→ `p5-one-command-rig`).
- `gpu-phy`/`oracle` participation — the p0 services stay defined but idle in P1 sessions.
- DPDK anywhere at SIM tier (HLD D2); hugepages; device mounts; real NICs (→ `p6-physical-m1-ingest`).
- Protocol-attached UEs performing NAS registration — test-mode UEs stimulate PHY/MAC only
  (upstream-documented limitation; see Honesty notes and the gate wording in P1-R9).
- Any latency/throughput requirement — **forbidden at SIM tier** (SIM preamble rule).

## Requirements

Every requirement is testable; test mapping in LLD §Test plan.

| ID | Requirement |
|---|---|
| **P1-R1** | The P1 rig SHALL be expressed as a compose overlay set on the p0 base (`IF-P0-COMPOSE`, monolithic `docker-compose.yml`). `docker compose config` over base+overlays SHALL show every upstream service definition unchanged except **additive** keys on `gnb` (new network attachment; env-var values the upstream file already parameterizes; and, **as of 2026-07-25**, a `command` override — see below). No upstream file is edited. |
| | **`command` addition rationale (2026-07-25, found by the first real local bring-up, not a design preference):** `gnb` is dual-/triple-homed (fronthaul/ran/metrics); empirically, which network attaches as `eth0`/`eth1`/`eth2` inside the container is **not stable** across `docker compose up` invocations of the identical stack (observed both orderings on this host without any config change). A static `network_interface` value in `gnb_ofh_testmode.yml` silently binds the OFH transmitter to the wrong network with no error — frames are sent, just never reach `ru-emu`. `gnb`'s `command` is overridden to a small shell wrapper that resolves the correct interface by MAC address (the one value that IS stable, since this overlay pins it) before launching the real upstream `gnb` binary against a patched, writable copy of the config (Docker `configs:` mounts are read-only). This is still a compose-file-only change (P1-R11: no project source compiles into any image) and still additive (upstream's default `command` is untouched anywhere `gnb` is used without this overlay). |
| **P1-R2** | The overlay SHALL define a `fronthaul` network: compose bridge driver, `com.docker.network.driver.mtu: 9000`. Verified: the bridge and both member veth endpoints report MTU 9000; membership is exactly {`ru-emu`, `gnb`}. |
| **P1-R3** | The backhaul role SHALL be served by the upstream `ran` network, unmodified (subnet `10.53.1.0/24`); membership for RAN traffic is exactly {`gnb`, `5gc`}. `ru-emu` SHALL NOT be attached to it, and `5gc` SHALL NOT be attached to `fronthaul` (rendered-config assertion). |
| **P1-R4** | The `ru-emu` service SHALL run the unmodified OCUDU `release_26_04` `ru_emulator` binary built via the upstream `docker/Dockerfile` `COMPONENT=ru-emulator` target, configured with **no `dpdk_config`** (socket transceiver selected, per upstream selection logic), `network_interface` = the container's fronthaul interface. Image provenance (tag+SHA) recorded in the p0 pins manifest. |
| **P1-R5** | The `gnb` service SHALL run with a config composed of: split-7.2x `ru_ofh` section (one cell; MAC/VLAN/eAxC values matching the `ru-emu` config per the LLD fronthaul plan) plus `test_mode.test_ue` with `nof_ues ≥ 1`, `pusch_active: true`, `pdsch_active: true`. Bandwidth/numerology values SHALL be identical between the `ru_ofh` cell and the `ru_emulator` config (startup cross-check by the assertion tooling). |
| **P1-R6** | **SCTP precondition (WSL2):** a day-1 check script SHALL verify SCTP support (`/proc/net/protocols` contains `SCTP`, else attempt `modprobe sctp`) before any bring-up, and on failure exit with the distinct code 3 and the actionable message: NGAP needs `CONFIG_IP_SCTP`; stock WSL2 kernels may lack it — rebuild the WSL2 kernel with `CONFIG_IP_SCTP=m` or run on the GCP VM. Single source shared with p0 (`check_sctp.sh`; p0-R9 references these semantics). |
| **P1-R7** | After bring-up, `gnb` logs SHALL show a successful NG Setup toward Open5GS (NGSetupResponse or upstream equivalent success line) within 60 s of the `5gc` service reaching healthy. |
| **P1-R8** | **eCPRI on the wire:** a capture on the fronthaul bridge over a ≥ 30 s window SHALL contain frames with ethertype `0xAEFE` (VLAN-tagged or untagged both acceptable — classifier handles 802.1Q), classified by eCPRI common-header message type into **both** planes: ≥ 1 C-plane frame (msg type 2, Real-Time Control) sourced from the DU MAC **and** ≥ 1 U-plane frame (msg type 0, IQ data) in **each** direction (DU→RU DL and RU→DU UL). Zero-frame classes fail the gate. |
| **P1-R9** | **10-minute stability:** over a 600 s soak with capture sampling: (a) no container exits/restarts (`RestartCount` delta 0 for all rig services); (b) no new ERROR-level gnb log lines relative to the post-bring-up baseline pattern list (LLD); (c) functional counters advance monotonically — gnb test-mode UL/DL MAC traffic counters (stdout JSON metrics) and ru_emulator per-eAxC rx totals both strictly increase between the 1st and 10th minute; (d) NGAP association remains established (no SCTP re-association in logs). "Attach" at P1 = NG Setup up + test-mode UE traffic flowing (see Honesty notes). |
| **P1-R10** | **Pcap corpus artifact:** each passing integration run SHALL archive a fronthaul pcap (rotated, bounded size, LLD schema) plus a manifest (rig config digest, pins digest, capture filter, frame counts per plane/direction) — the replay input contracted to `p2-phy-kernels` (SIM §4 P2 integration gate: "canned eCPRI pcaps (from P1 capture)"). |
| **P1-R11** | **Zero project code:** the P1 rig SHALL contain no project-authored source in any running container — only upstream binaries, YAML configs, compose files, and host-side assertion scripts. Verified: image digests for `ru-emu`/`gnb`/`5gc` are pure upstream builds recorded in pins; assertion scripts never execute inside rig containers except read-only `docker exec` inspections. |
| **P1-R12** | No requirement, gate, script, or config in this feature SHALL contain a latency or throughput threshold. Timing-flavored upstream KPIs (ru_emulator rx-window early/on-time/late) are logged as observations but SHALL NOT participate in any pass/fail decision (grep-able assertion over the feature tree). |

## Acceptance gates

Traceability: SIM §4, row **P1**. Definition of done (SIM §4 footer): both gates green on WSL2
**and** the GCP `n2-standard-16` VM; results logged to the honesty ledger (manually until p5).

| Gate | Type | Statement | SIM §4 source |
|---|---|---|---|
| **P1-G1** | unit | "gnb/ru-emu KPIs clean", defined functionally: bring-up succeeds (P1-R6 precondition, P1-R7 NG Setup), gnb test-mode metrics report nonzero UL and DL MAC traffic, ru_emulator reports nonzero rx totals on every configured eAxC, no ERROR-level logs during a 60 s check window. Covers P1-R4/R5/R7. | P1 "Test gate (unit)" |
| **P1-G2** | integration | tcpdump on the fronthaul bridge asserts real eCPRI `0xAEFE` C-plane **and** U-plane flow (P1-R8) **and** attach/traffic counters stable over 10 minutes (P1-R9); pcap corpus archived (P1-R10). | P1 "Integration gate" |

## Dependencies on other features

- **`p0-rig-scaffold`** — provides `IF-P0-COMPOSE` (vendored upstream compose base), the pins
  manifest (`IF-P0-PINS`) that P1 extends with the `ru-emu` image entry, and `check_sctp.sh`.
- Consumers: **`p2-phy-kernels`** (pcap corpus, `IF-P1-PCAPS`), **`p3-live-tap-ul-inject`**
  (running rig, KPI baseline, fronthaul network contract `IF-P1-FRONTHAUL`),
  **`p5-one-command-rig`** (P1 assertion scripts wrapped as a phase suite per `IF-P5-SUITE`).
- External: OCUDU `release_26_04`; Open5GS as pinned by the upstream compose (`v2.7.6`, Ubuntu
  22.04 base — upstream's choice, recorded in pins, not overridden).

## Honesty-ledger notes (what P1 does NOT prove)

- **Test-mode UEs do not attach in the NAS/5GC sense.** Upstream documents `test_mode` as creating
  UEs "in RRC connected state" that "mainly stimulate PHY and MAC" — no UE NAS registration reaches
  Open5GS. The SIM §4 phrase "attach/traffic counters" is therefore interpreted (and flagged upward
  as a doc ambiguity) as: NGAP association up + test-mode MAC traffic counters advancing. No
  end-to-end user-plane (UE↔DN) claim is made.
- The UL IQ on the wire is the ru_emulator's **static generated IQ** — real protocol, meaningless
  data. Ground truth arrives only with P3 injection (srsRAN-limitations pattern acknowledged).
- Nothing about any backend is proven: no project kernel runs, no tap exists, no seam exists.
- The fronthaul is a Linux bridge with veth — functionally real eCPRI, mechanically nothing like a
  ConnectX/DPDK path (PHYSICAL-only, SIM §5 gaps 1–2).
- No performance evidence: rx-window timing KPIs are recorded as observations, never gated, never
  quoted (SIM preamble rule; P1-R12). WSL2/GCP hosts are non-realtime; OFH timing windows are not
  expected to be met and are not claimed.
