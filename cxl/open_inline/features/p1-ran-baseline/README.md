# p1-ran-baseline

Implementation of [`spec/`](spec/) — the SIM-tier, zero-project-code RAN baseline rig: OCUDU
`ru_emulator` (split-7.2x OFH) &lt;-&gt; OCUDU `gnb` (CU+DU, test-mode UEs) &lt;-&gt; Open5GS `5gc`,
proving real eCPRI (`0xAEFE`) flows on a dedicated `fronthaul` bridge and that the rig is stable
for 10 minutes. See [`VERIFICATION.md`](VERIFICATION.md) for what's actually been built and run,
including several real corrections this LLD's approximate normative shapes needed once checked
against the actual OCUDU source.

## Layout

```
docker/
  compose.p1.yml              # 3rd compose layer: {p0 upstream+compose.sim.yml} + this
  configs/
    gnb_ofh_testmode.yml       # ru_ofh cell + cell_cfg + test_mode (P1-R5)
    ru_emu.yml                 # ru_emulator socket-mode config (P1-R4)
helpers/
  check_sctp.sh                # thin wrapper -> p0's own script (single source, P1-R6)
  rigcfg_crosscheck.sh         # the two configs' cross-consistency rules (P1-R5)
  assert_ecpri.sh              # capture/classify 0xAEFE C/U-plane per direction (P1-R8)
  soak_stability.sh            # 600s stability checker (P1-R9)
  archive_pcap.sh              # pcap corpus + manifest writer (P1-R10, IF-P1-PCAPS)
  kpi_snapshot.sh               # IF-P1-KPI snapshot (never judges; UNVERIFIED field schema, see its header)
  lint_no_perf.sh               # P1-R12
  bring_up.sh                   # full P1-G1+P1-G2 sequence for an SCTP-capable host (GCP VM)
  remote_provision.sh           # runs ON a target host: installs Docker/Compose/tcpdump, clones OCUDU
  deploy_and_bring_up.sh        # runs LOCALLY: rsyncs this tree + drives the two scripts above over SSH
tools/
  synth_ecpri_gen.cpp           # builds the classifier's real, self-consistent test fixture
tests/
  fixtures/synth_ecpri.{pcap,json}
  classifier_test.sh             # P1-R8 unit test against the fixture
  ci_p1_static.sh                 # bundles every check buildable WITHOUT a live SCTP-capable rig
```

## What's been verified on this host (no SCTP)

`tests/ci_p1_static.sh` — P1-R1/R2/R3 (real `docker compose config` rendering, not just written),
P1-R5 (rigcfg cross-check + negative test), P1-R6 (real exit-3 on this actual host), P1-R8
(classifier vs. a real, self-verified eCPRI+O-RAN CUS fixture), P1-R11, P1-R12. All PASS.

## First local bring-up (2026-07-25): P1-R8 achieved for real

Before provisioning anything remote, ran a real local `docker compose up` here (bypassing only
the SCTP gate manually — `bring_up.sh` itself was not touched and still correctly refuses to
start without SCTP). Found and fixed 4 real bugs (relative config path resolving against the
wrong directory, `ru_emulator`'s actual install path, a missing `CAP_NET_ADMIN` grant, and
non-deterministic `eth0`/`eth1` container interface naming — fixed with MAC-based runtime
resolution). Captured real, live eCPRI traffic on the fronthaul bridge and classified it with the
unmodified `assert_ecpri.sh`: `{c_dl:37, u_dl:373, u_ul:85, c_ul:0, vlan_tagged:true}`, exit 0.
**P1-R8 is proven for real**, including Q1 (VLAN tagging — confirmed real) and Q6 (interface
naming — confirmed and resolved). Full story in `VERIFICATION.md`.

## What's still environment-blocked (needs a GCP VM)

P1-R7 (NG Setup) needs real SCTP — confirmed still absent on this host. P1-R9 (10-minute soak) /
P1-G2 also need a host where `gnb`'s OFH transmit thread doesn't stall under CPU contention — a
**new** finding from the local bring-up: on this shared 4-core host, `gnb` bursts real traffic at
startup then fully stops once contention catches up (confirmed by watching
`/sys/class/net/eth0/statistics/tx_packets` freeze). Whether a dedicated `n2-standard-16`'s larger
CPU budget keeps it running continuously is the one thing actually left to confirm remotely.

## Deploying to a remote host (e.g. a freshly-provisioned GCP VM)

This repo's git tree has no remote configured, so deployment is rsync-based, not `git clone`-based
(`third_party/ocudu` *does* have a real upstream remote and is cloned fresh on the target instead
of transferred). One command from this dev box, once the target VM is up and SSH-reachable:

```bash
helpers/deploy_and_bring_up.sh <user>@<external-ip> [--soak-seconds 600] [--identity ~/.ssh/id_ed25519]
```

This rsyncs `open_inline/` to `~/oi-rig/open_inline` on the target (excluding local build
artifacts, which are host-specific and rebuilt fresh), runs `remote_provision.sh` there (Docker +
Compose v2 + tcpdump + python3-yaml + the pinned OCUDU checkout, idempotent), then runs
`bring_up.sh` there and pulls back the resulting pcap corpus into this repo's `artifacts/p1/`.
Exit code mirrors `bring_up.sh`'s own (0 iff P1-G1+P1-G2 both green).

## Real corrections vs this feature's own LLD (see VERIFICATION.md for full detail)

1. No `COMPONENT=ru-emulator` build arg exists in the monolithic `docker/Dockerfile` — it always
   builds `ru_emulator` alongside `gnb`. `ru-emu` reuses `ocudu/gnb`'s own image; no separate build.
2. `ru_emu.yml`'s real top-level shape is `log:` / `ru_emu: {cells: [...]}` (not a flat `ru_emu:`
   list), with real field names (`t2a_*`, `compr_method_ul`, `enable_promiscuous`, `prach_format`)
   read directly off `ru_emulator_cli11_schema.cpp` — not the LLD's approximate guesses.
3. Upstream's `gnb` build args do not actually reference an `${OS_VERSION}` env var (hardcoded to
   `24.04` already) — the LLD's Q5 "pin OS_VERSION=24.04" note doesn't apply to anything upstream
   actually parameterizes for `gnb`.
