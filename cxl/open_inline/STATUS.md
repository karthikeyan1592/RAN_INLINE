# Open Inline v3 — Project Status Tracker

Single source of truth for "what phase are we in, what's actually done, what's deferred, what's a
known gap." Each phase section below carries a JSON status block (machine-checkable) plus prose
for context a JSON blob can't hold. Update this file whenever a phase's status changes — don't let
it drift from the per-feature `README.md`/`VERIFICATION.md` files, which remain the detailed
source for any one slice.

Convention: `status` is one of `done`, `partial`, `not_started`, `spec_only`. `done` means built
and verified against a real oracle/real build, not just written.

```json
{
  "last_updated": "2026-07-26",
  "phases": [
    {"id": "p0-rig-scaffold", "status": "partial"},
    {"id": "p1-ran-baseline", "status": "done", "note": "P1-G1 + P1-G2 both fully green for real on a GCP n2-standard-16 VM (run-id 20260725T180323Z): {restarts:0, new_error_lines:0, ngap_stable:true, counters_monotonic:true}, P1-R10 pcap corpus (840,783 frames) archived + pulled back locally; ~15 real bugs found+fixed this session including the pipefail/grep-q SIGPIPE root cause behind the 'docker-logs race' and the real WebSocket-based metrics transport + missing-byte-counter finding that led to switching counters_monotonic to fronthaul NIC sysfs counters"},
    {"id": "p2-phy-kernels", "status": "done", "note": "all 6 children (p2a-p2f) done; real end-to-end CRC pass + TB bit-exact for all 3 MVP MCS points"},
    {"id": "p2a-scaffold", "status": "done", "note": "stub kernel chain replaced with the real 8-stage pipeline 2026-07-23"},
    {"id": "p2b-k5-k6", "status": "done"},
    {"id": "p2c-k1", "status": "done"},
    {"id": "p2d-k2-k3", "status": "done"},
    {"id": "p2e-k4", "status": "done"},
    {"id": "p2f-integration", "status": "done", "note": "LDPC hookup + CB segmentation + real pipeline wiring + oracle TX generator + pipeline_test.py all done, real CRC pass + TB bit-exact end-to-end for all 3 MVP MCS points; class-a structural gate now also run for real against p1's live-captured corpus (2026-07-25), 29/29 assertions PASS (pipeline_runner.cpp gained a required <udcomphdr_bytes> CLI arg 2026-07-26, see p2a-scaffold's udCompHdr fix)"},
    {"id": "p3-live-tap-ul-inject", "status": "partial", "note": "M1-M6 all implemented; 157/157 local assertions PASS (grew from 134 across two GCP sessions this date); real GCP session 2026-07-26: ru_emulator patch built+run for real (1 real compile bug found+fixed), gpu-phy/oracle images rebuilt, p1 soak re-verified green; found+fixed a real live throughput bug (BPF filter now also checks src-MAC, dropping DL flood in-kernel), a real config-conversion bug in the patch itself, AND (second pass, same date) two more real bugs chasing the resulting calibration failure -- pcap_comparator had no direction filter (hub-mode capture let it calibrate against a downlink frame), and underneath that a cross-feature udCompHdr compression-header offset bug shared with p2a-scaffold/p2c-k1 (confirmed against 163,268/163,268 real frames in one corpus). Both fixed, both locally regression-tested; P3-U1/U2/I1 full live runs (all fixes applied together) still deferred, exact resume runbook in DEFERRED_LIVE_GATES.md"},
    {"id": "p4-phy-l2-seam", "status": "partial", "note": "oi_seam ring library + producer + L2 stub all implemented; 81/81 local assertions PASS (P4-G1/G2/G3 all real, incl. real 2nd-thread concurrency for G2); gpu_phy_seam_bridge.c gained a required <udcomphdr_bytes> CLI arg 2026-07-26 (see p2a-scaffold's udCompHdr fix); P4-G4 full integration deferred (needs live rig + gpu-phy's own drain-call-site wiring, LLD Q1 still open)"},
    {"id": "p5-one-command-rig", "status": "partial", "note": "runner+ledger+lint+comparator all implemented; 63/63 local assertions PASS incl. real docker-compose P5-G1; all 9 real gates/suite.yml addenda shipped for p1/p2a-f/p3/p4 and validated; WSL2 half of P5-G2 run for real 2026-07-27 -- overall: PASS across all 29 real gates (3 real runner bugs found+fixed along the way: compose_env schema addition, missing p0 base overlay, silent teardown failure -- see VERIFICATION.md), archived at artifacts/p5/20260727T021153Z-8a0b784/; GCP half + cross-comparison still deferred (VM unreachable that session, see DEFERRED_LIVE_GATES.md)"},
    {"id": "p6-physical-m1-ingest", "status": "spec_only"}
  ]
}
```

---

## p0-rig-scaffold

```json
{
  "id": "p0-rig-scaffold",
  "status": "partial",
  "path": "features/p0-rig-scaffold",
  "gates_verified": ["P0-R1", "P0-R2", "P0-R3", "P0-R4", "P0-R5", "P0-R6", "P0-R7"],
  "gates_written_not_ci_run": ["P0-R8"],
  "gates_partially_verified": ["P0-R9/P0-G2"],
  "real_results": {
    "ldpc_suite_bit_mismatches": "0 / 4,096,000 (BG1/BG2 x LS=384/256, 200 msgs each)",
    "images_built": ["oi/gpu-phy:dev (253MB)", "oi/oracle:dev"]
  },
  "known_gaps": [
    "SCTP unavailable on this host (no CONFIG_IP_SCTP) — check_sctp.sh correctly short-circuits smoke_up.sh before attempting compose up; this is an environment limitation, not a code bug",
    "upstream gnb image (full OCUDU gNB w/ UHD/DPDK/ROHC) never finished building from scratch in this session — was left running in background (/tmp scratchpad), status unknown/likely lost to a scratchpad wipe",
    "P0-R9/P0-G2 full '5gc+gnb stable 60s + NG-setup log' success path not observed on this host — blocked by the two gaps above, not by a code defect",
    "P0-R8 CI job script written but never run in an actual CI runner (no CI provider chosen — open question)"
  ],
  "resolved_2026_07_23": [
    "bg_tables.h provenance contradiction (file header said 'extracted from srsRAN_Project', MODIFICATIONS.md said 'our own, no external dependency') — regenerated the BG1/BG2 shift tables + LS_TO_IDX from OCUDU's real, public BSD-3 API (ldpc_graph_impl/get_lifting_index), verified bit-identical to the pre-regeneration baseline (0/4,096,000 mismatches, same as before), replaced the file, corrected MODIFICATIONS.md, and rebuilt oi/gpu-phy:dev from scratch (--no-cache) to ensure the shipped image reflects the fix — re-verified Gate 1 PASS inside the freshly built container."
  ],
  "next_step": "On a host with Docker + SCTP (e.g. GCP n2-standard-16 per SIM SS4 DoD): let gnb/5gc build finish, run helpers/smoke_up.sh for the full P0-G2 pass."
}
```

---

## p1-ran-baseline

```json
{
  "id": "p1-ran-baseline",
  "status": "done",
  "path": "features/p1-ran-baseline",
  "files_present": ["spec/SPEC.md", "spec/HLD.md", "spec/LLD.md", "README.md", "VERIFICATION.md",
                   "docker/compose.p1.yml", "docker/configs/gnb_ofh_testmode.yml", "docker/configs/ru_emu.yml",
                   "helpers/check_sctp.sh", "helpers/rigcfg_crosscheck.sh", "helpers/assert_ecpri.sh",
                   "helpers/soak_stability.sh", "helpers/archive_pcap.sh", "helpers/kpi_snapshot.sh",
                   "helpers/lint_no_perf.sh", "helpers/bring_up.sh", "helpers/remote_provision.sh",
                   "helpers/deploy_and_bring_up.sh", "tools/synth_ecpri_gen.cpp",
                   "tests/classifier_test.sh", "tests/ci_p1_static.sh", "Makefile"],
  "implementation_started": true,
  "blocks": [],
  "unblocks": ["p2f-integration's class-(a) structural pcap gate — DONE (2026-07-25): re-run for real against the corpus at artifacts/p1/pcaps/20260725T180323Z/, see p2f-integration's done_2026_07_25_class_a entry"],
  "done_2026_07_24": [
    "compose.p1.yml: 3rd compose layer, real docker compose config render verified — gnb additively gains networks.fronthaul only, 5gc byte-identical, ru-emu/5gc network membership correct (P1-R1/R2/R3, tests/ci_p1_static.sh)",
    "ru-emu reuses ocudu/gnb's own image (no separate build) — real finding: no COMPONENT=ru-emulator build arg exists in the monolithic Dockerfile, it always builds ru_emulator too",
    "gnb_ofh_testmode.yml + ru_emu.yml: grounded in real OCUDU schemas (ru_emulator_appconfig.h, ru_emulator_cli11_schema.cpp, gnb_ru_picocom_scb_tdd_n78_20mhz.yml) after finding the LLD's normative shapes were approximate/wrong in several ways (see VERIFICATION.md corrections 1-4)",
    "rigcfg_crosscheck.sh: real cross-consistency check + negative test, PASS",
    "check_sctp.sh: real exit-3 with exact message on this host (same SCTP gap as p0)",
    "synth_ecpri_gen.cpp: real, self-consistent eCPRI+O-RAN CUS pcap fixture (2 c_dl, 3 u_dl [1 vlan-tagged], 2 u_ul, 0 c_ul) via real OCUDU builders (ecpri::packet_builder, uplane_message_builder, cplane_message_builder)",
    "assert_ecpri.sh classifier: 7/7 assertions PASS vs the real fixture incl. negative test (RU-only pcap)",
    "bring_up.sh (GCP-VM counterpart to p0's smoke_up.sh): runs for real on this host, correctly exits 3 at the SCTP precondition",
    "full CI-static sweep (tests/ci_p1_static.sh): P1-R1/R2/R3/R5/R6/R8/R11/R12 all PASS"
  ],
  "done_2026_07_25_local_bringup": [
    "First real local docker compose up on this (non-SCTP) host, using already-built images (no rebuild cost). Found + fixed 4 real bugs: (1) relative config path resolved against the wrong compose file's directory -- fixed with a required absolute-path env var; (2) ru_emulator binary is actually at /usr/local/bin not /opt/ocudu/bin in the already-built image -- fixed to PATH-relative command; (3) ru-emu needs CAP_NET_ADMIN (HLD D2's 'no cap_add' claim was wrong) -- ethernet_transmitter_impl.cpp unconditionally calls ioctl(SIOCSIFMTU), fatal without it even when MTU is already correct; (4) Q6 container interface naming is NOT just unconfirmed, it's genuinely non-deterministic across recreations of the identical stack (confirmed: eth0=fronthaul on first up, eth1=fronthaul after --force-recreate, no config change) -- fixed for real with a MAC-based runtime resolution wrapper in gnb's command (P1-R1/SPEC.md amended to allow this additive command override)",
    "5gc+gnb+ru-emu brought up together for real: 0 restarts, all running. MAC/PHY test-mode traffic confirmed live (RNTI 0x44) but this alone doesn't prove wire traffic (it's synthesized in du_high's MAC test-mode PDU injection)",
    "REAL eCPRI captured on the fronthaul bridge (individual veth, not bridge master -- bridges don't reliably surface port-forwarded traffic on their own master device, a real capture-methodology finding) and classified with the UNMODIFIED assert_ecpri.sh: {frames:495, c_dl:37, u_dl:373, u_ul:85, c_ul:0, vlan_tagged:true}, exit 0 -- P1-R8 ACHIEVED FOR REAL, no GCP needed for this",
    "Q1 (VLAN tagging) ANSWERED not just handled: real captured frames ARE 802.1Q-tagged (vlan 1), confirming the ru_emulator CLI::Range(1,65536)-no-untagged-option finding's prediction -- validates the SAME-DAY p2a/p2c VLAN fix against real captured traffic, not just synthetic fixtures",
    "Q2 (compose per-endpoint mac_address) CONFIRMED working; Q3 (UL C-plane absent) CONFIRMED matching the fixture's assumption; Q6 (interface naming) CONFIRMED and RESOLVED (see bug 4 above) -- no longer open",
    "NEW real limitation found (not previously anticipated at this severity): gnb's OFH TX thread bursts heavily at startup then FULLY STOPS (not just runs late) once real CPU contention on this shared 4-core host catches up -- confirmed via /sys/class/net/eth0/statistics/tx_packets freezing for 6+ seconds; gnb alone measured 137% CPU. This blocks the sustained 30s capture window and the 10-min soak on THIS host specifically -- now the ONE genuinely open question for the GCP VM to answer (does a bigger, non-shared CPU budget keep TX running continuously), not a protocol/config gap"
  ],
  "environment_blocked": [],
  "environment_blocked_note": "FULLY RESOLVED on GCP: P1-G1 and P1-G2 both green for real on n2-standard-16 (run-id 20260725T180323Z). All P1-R9 legs proven: restarts:0, new_error_lines:0, ngap_stable:true, counters_monotonic:true. P1-R10 pcap corpus (840,783 frames) archived on the remote AND correctly pulled back into this repo at artifacts/p1/pcaps/20260725T180323Z/. See resolved_2026_07_25_gcp and resolved_2026_07_25_gcp_metrics_chase for the full bug list and the counters_monotonic root-cause chase.",
  "deployment_automation_2026_07_25": "remote_provision.sh + deploy_and_bring_up.sh: RUN FOR REAL against a GCP n2-standard-16 VM (asia-south2-a) for the first time -- see resolved_2026_07_25_gcp for the ~10 real bugs found and fixed on this first real run (image transfer, SCTP false-negative, log routing/levels, force-recreate, tcpdump capability, exec bit, docker-logs visibility race).",
  "resolved_2026_07_25_gcp": [
    "deploy_and_bring_up.sh was compiling OCUDU from source on the fresh VM instead of transferring already-built local images -- explicit user-flagged waste. Fixed: real docker image inspect probe on both ends + docker save|gzip|ssh|gunzip|docker load, only for images actually missing remotely.",
    "check_sctp.sh had 2 real bugs invisible on every host used before (all root shells): non-root modprobe needs privilege (silently swallowed by 2>/dev/null, mis-reported as 'absent'), and /proc/net/protocols reports 'SCTP' uppercase against a lowercase-only grep. Fixed: sudo -n retry + case-insensitive match. This GCP VM genuinely HAS SCTP -- the earlier 'SCTP-blocked' status was a check bug, not a real host limitation.",
    "gnb_ofh_testmode.yml log config: filename changed /tmp/gnb.log -> stdout (app log was invisible to docker logs entirely); ngap_level: info added narrowly (all_level stayed at default warning) so NG-Setup evidence is visible without a full-volume log flood; ofh_level: error added to suppress a 'missed incoming User-Plane' warning flood (32,648/32,753 = 99.7% of lines) -- a likely benign SIM-tier timing artifact, root cause not chased further, real capture separately proves traffic is genuinely on the wire regardless.",
    "bring_up.sh: added --force-recreate to compose up (was silently reusing stale/log-bloated containers from earlier manual debugging); widened NG-setup log check from a single immediate grep to a 15x3s retry (a docker-logs visibility race was reproducible -- NGAP evidence present moments later or on manual re-check with no config change -- root cause not fully confirmed, retried around, not claimed solved).",
    "remote_provision.sh: added setcap cap_net_raw,cap_net_admin+eip on tcpdump (this Ubuntu 24.04 image does NOT auto-grant it, contrary to common assumption) + libcap2-bin package, fixing a real 'Operation not permitted' non-root capture failure.",
    "soak_stability.sh: restored missing +x bit (genuine pre-existing local bug, never triggered before since no earlier run had reached this step) + same 15x3s docker-logs retry pattern applied to its own independent NGAP_UP_0 check.",
    "Real P1-R8 result at scale (30s capture, dedicated VM, no contention): {frames: 851004, c_dl: 59220, u_dl: 627732, u_ul: 164052, c_ul: 0, vlan_tagged: true} -- ~28K frames/sec sustained, zero TX stall. Frames on this VM are untagged/tagged depending on run (observed both true across runs); dual-branch p2a/p2c fix already handles it -- not a new issue.",
    "P1-R9 soak REAL root cause + fix + CONFIRMED: the 'docker-logs visibility race' was actually a pipefail/grep-q SIGPIPE bug -- `docker logs C | grep -q PATTERN` under `set -o pipefail` can return 141 even when grep genuinely matches, because grep -q's early exit SIGPIPEs the still-writing docker logs upstream of it; deterministic once the log is large enough that the match isn't near the tail (confirmed via direct reproduction), not a timing flake. Fixed in bring_up.sh + soak_stability.sh (capture-then-grep, no live pipe) and proactively in p0-rig-scaffold/helpers/smoke_up.sh (same pattern, never yet triggered there). Also found+fixed: ru_emu.yml's log.level:info flooded its container log to 5.09GB in 26 minutes (docker logs on it wouldn't return) -- fixed to warning; kpi_snapshot.sh had a dead unconditional full-log read of ru_emu's log (unused variable) -- removed."
  ],
  "resolved_2026_07_25_gcp_metrics_chase": [
    "counters_monotonic root-caused for real, same night, VM kept warm rather than deferred (empirical archaeology needs a live rig). Traced through the real pinned OCUDU source: gnb's JSON metrics are pushed over a WebSocket the app itself hosts (apps/services/remote_control/remote_server.cpp, built on uWebSockets) -- NOT stdout, a file, or UDP -- and only to clients that first send {\"cmd\":\"metrics_subscribe\"}. metrics.enable_json:true in YAML is bound to THREE different struct fields via OCUDU's CLI11 'shared metrics: section' merging (gnb top-level, du_high's common_metrics_cfg.enable_json_metrics, remote_control's enable_metrics_subscription).",
    "Even after solving the transport, the real payload has NO byte/throughput counters at all: mac_dl_cell_metric_report (include/ocudu/mac/mac_metrics.h) carries only average/min/max_latency_us and cpu_usage_percent -- confirmed by reading apps/helpers/metrics/json_generators/du_high/mac.cpp directly. RLC/PDCP (which normally carry byte counts) aren't active in this MAC-test-mode config. Not a mapping gap -- a real, confirmed absence.",
    "REAL FIX: switched counters_monotonic's data source to the fronthaul NIC's own kernel sysfs counters (/sys/class/net/<iface>/statistics/{tx,rx}_bytes inside the gnb container, read via docker exec + the same MAC-based interface resolution compose.p1.yml's command wrapper already uses). Genuinely monotonic, zero new dependencies. kpi_snapshot.sh + soak_stability.sh updated; mac_ul_bytes/mac_dl_bytes kept as explicitly-null fields with a comment on WHY (confirmed nonexistent), not removed.",
    "Bonus: found+fixed 2 more real bugs while manually archiving a pcap (this investigation's own guardrail, done regardless of hunt outcome to decouple p3/p2f): deploy_and_bring_up.sh's pull-back rsync pointed at ~/oi-rig/artifacts/p1/ but the real path is one directory deeper (~/oi-rig/open_inline/artifacts/p1/) -- P1-R10's corpus was NEVER actually pulled back locally by this script on any prior run, silently masked by rsync's own 'not fatal' fallback message. archive_pcap.sh's tcpdump -C rotation used a %03d filename pattern tcpdump doesn't support (no printf substitution), producing files with a literal '%03d' in the name -- fixed to a plain base name.",
    "FULL RE-CONFIRMATION, ALL FOUR P1-R9 CONDITIONS REAL: {\"restarts\": 0, \"new_error_lines\": 0, \"counters_monotonic\": true, \"ngap_stable\": true}. bring_up: PASS (P1-G1 + P1-G2 green, run-id=20260725T180323Z). P1-R10 pcap corpus (840,783 frames) archived AND correctly pulled back into artifacts/p1/pcaps/20260725T180323Z/ -- verified the pull-back path fix works on this exact run. No condition weakened to reach this -- the real data source was fixed, per this investigation's own explicit no-silent-null guardrail."
  ],
  "known_gaps_deferred": [
    "Q7 (gnb ERROR-log baseline) remains open -- new_error_lines:0 held regardless on the GCP soak, so it hasn't blocked anything in practice yet, but the tolerated-pattern-list capture itself still hasn't been done.",
    "ru_emu's stdout KPI table parse (kpi_snapshot.sh's ru_emu.eaxc field) remains unimplemented -- honest empty [], never fabricated -- but was never gated by any P1 requirement, so it's not blocking."
  ]
}
```

---

## p2-phy-kernels (parent spec)

```json
{
  "id": "p2-phy-kernels",
  "status": "partial",
  "path": "features/p2-phy-kernels",
  "children": ["p2a-scaffold", "p2b-k5-k6", "p2c-k1", "p2d-k2-k3", "p2e-k4", "p2f-integration"],
  "children_done": 6,
  "children_total": 6,
  "lld_corrections_made_this_project": [
    {"section": "SS4.1 IQ conversion constant", "was": "/ 32768.0f", "now": "/ 32767.0f", "found_by": "p2c-k1, cross-validated against real OCUDU quantizer + encoder/decoder round-trip"},
    {"section": "K3 MMSE formula", "was": "1/(|h|^2+sigma^2)", "now": "y*conj(h)/(tx_scaling*|h|^2), matches ZF exactly for 1 Tx layer", "found_by": "p2d-k2-k3, OCUDU's own code comment confirms MMSE==ZF for nof_tx_layers==1"},
    {"section": "SS7 float-kernel tolerance table", "was": "T_K2/T_K3/T_K3n all TBD (Q1 open)", "now": "T_K2=0.05, T_K2n=0.05 (new row), T_K3=0.005, T_K3n=0.005 — all measured, Q1 resolved", "found_by": "p2d-k2-k3"},
    {"section": "K2 kernel prototype", "was": "single k2_chanest kernel, scalar dmrs_symbol_idx arg", "now": "split into k2a_chanest_symbol (per-DMRS-symbol) + k2b_chanest_combine (cross-symbol time-domain combine)", "found_by": "p2d-k2-k3 — original prototype couldn't produce its own committed time-domain hold rule"}
  ],
  "open_questions_status": {
    "Q1_float_tolerances": "RESOLVED (p2d-k2-k3)",
    "Q2_oi_frame_desc_wire_layout": "PARTIALLY RESOLVED — eCPRI+O-RAN CUS layers resolved for real (p2c-k1), Ethernet II framing (14B, no VLAN) still an inferred placeholder, not pcap-confirmed",
    "Q3_tb_record_byte_widths": "still open, provisional",
    "Q4_slot_timeout_value": "still open, not decided",
    "Q5_dc_subcarrier_fields": "still open — p2d's K2a/K2b kernels consistently assume no dc_position handling, but this is not a new resolution",
    "Q6_oclgrind_k1_atomics": "still open, recommendation only, not implemented"
  }
}
```

---

## p2a-scaffold

```json
{
  "id": "p2a-scaffold",
  "status": "done",
  "path": "features/p2a-scaffold",
  "verified": true,
  "test_assertions_passing": 57,
  "test_breakdown": {"host_api_test.cpp": 26, "preparse_test.cpp": 31},
  "real_bugs_fixed": [
    "find operator-precedence bug in lint_no_perf.sh silently skipped src/ and tests/",
    "oi_p2_host.h claimed C-compatibility it didn't have (<cstddef>/<cstdint> instead of C headers)",
    "forward-reference bug: oi_p2_tb_record_c used before its own typedef",
    "oi_frame_desc.h couldn't compile as OpenCL C (used <stdint.h>, which doesn't exist in OpenCL C's device dialect) — fixed with __OPENCL_C_VERSION__-gated type aliases",
    "oi_p2_feed's original 4-scalar-arg signature was circular under the parse-inside-feed reading and incompatible with PHYSICAL's dmabuf path — reconciled to oi_p2_feed(pipeline, const oi_frame_desc* desc), parsing moved to a new shared oi_oran_preparse() helper called by ingest backends",
    "oi_oran_preparse.cpp's byte layout was an explicit placeholder — rewritten with the real bit-packed eCPRI+O-RAN CUS layout during p2c-k1, 15/15 preparse_test assertions re-verified",
    "2026-07-23: K6's full_length arg confused with segment_length (K vs codeword N) while rewiring the real pipeline — see p2f-integration VERIFICATION.md",
    "2026-07-23: missing data-RE compaction step between K1's full 14x612 grid and K3's compact 6732-entry expectation — fixed with device-side clEnqueueCopyBuffer, no kernel changes"
  ],
  "resolved_2026_07_24": [
    "Q2 Ethernet-layer VLAN handling: oi_frame_desc gains eth_hdr_len (14/18, from a reserved byte, 32-byte layout unchanged); oi_oran_wire_layout.h's offset macros parameterized on eth_len; oi_oran_preparse_frame now detects the real 802.1Q tag per-frame (EtherType at 12, or at 16 if 0x8100 sits at 12) instead of assuming untagged. Triggered by p1's ru_emulator finding (--vlan_tag is CLI::Range(1,65536), no untagged option). preparse_test.cpp extended with 6 new tagged/untagged/negative cases, all pass (24/24 total)."
  ],
  "resolved_2026_07_26": [
    "udCompHdr compression-header offset (cross-feature, driven by p3's live P3-I1 gate hitting a real GCP capture): oi_frame_desc gains payload_byte_off (another reserved byte, 32-byte layout unchanged); oi_oran_preparse_frame gains a required udcomphdr_bytes parameter (0 or 2, explicit/caller-supplied, never content-sniffed -- the 2 candidate bytes read as 0x00 0x00 for the real rig's none/16 config, indistinguishable from 'absent' by content alone). Root cause: OCUDU's static-compression U-plane builder writes 0 bytes for udCompHdr+reserved (what this project always assumed); the dynamic-compression builder writes 2; this project's real RU emulator (apps/examples/ofh/ru_emulator.cpp, upstream OCUDU example code) unconditionally uses the 2-byte layout regardless of config. Confirmed byte-for-byte against two independent real corpora (163,268/163,268 real UL frames matched in one of them). K1's kernel (p2c-k1) now reads desc.payload_byte_off directly instead of re-deriving it. preparse_test.cpp extended with 2 new cases (ABSENT/PRESENT, the latter matching the exact real 36-byte offset found on the wire), 31/31 total."
  ],
  "known_gaps_deferred": [
    "oi_frame_desc::section_id is uint8_t but the real wire field is 12 bits (0-4095) — latent truncation risk, harmless under MVP's single-section-per-frame scope, not fixed (would change the struct's frozen 32-byte layout)",
    "p3/p6 ingest control-flow inversion (push vs pull) — flagged twice, explicitly NOT resolved, deferred to whichever of p3/p6 is implemented first",
    "kernel .cl source paths in oi_p2_host.cpp are hardcoded CWD-relative (only resolve correctly run from p2a-scaffold/tests/) — same class of fragility as other slices' test harnesses, but now inside production orchestration code; not urgent at SIM tier",
    "K6+LDPC+CB-desegment tail is host-orchestrated with fresh per-CB device allocations inside oi_p2_drain — a documented, scoped deviation from HLD SS5's 'zero device allocations after setup' (K1-K5 fully honor it); see p2f-integration/VERIFICATION.md item 3"
  ],
  "real_pipeline_stages": 8,
  "real_pipeline_note": "2026-07-23: stub kernel chain fully replaced in-place with real K1/K2a(x3)/K2b/K3/K4/K5/K6+LDPC+CRC orchestration per user direction; oi_p2_setup/feed/launch_slot/drain signatures kept frozen except oi_p2_launch_slot gained an mcs_index parameter (user-directed, see oi_p2_host.h doc comment)"
}
```

---

## p2b-k5-k6

```json
{
  "id": "p2b-k5-k6",
  "status": "done",
  "path": "features/p2b-k5-k6",
  "verified": true,
  "test_assertions_passing": 37,
  "test_breakdown": {"k5_test.cpp": 25, "k6_test.cpp": 12},
  "oracle": "real linked libocudu_sequence_generators.a / libocudu_ldpc.a (single strong oracle, not the srsRAN-AGPL+Sionna dual pair R14/R14a literally specifies)",
  "real_bugs_fixed": [
    "K6 test vector exceeded single-pass rate-dematch capacity (invalid test data, not a kernel bug)",
    "K6 test vector not Qm-aligned (invalid test data, not a kernel bug)",
    "p2b had no build script — Makefile + bootstrap-ocudu target added after user flagged the reproducibility gap; verified clean rebuild from a fully wiped OCUDU_BUILD"
  ],
  "known_gaps_deferred": [
    "K6 has no wraparound/repetition pass for rm_length beyond full_length-nof_filler_bits — believed out of MVP scope (HARQ/rv>0 deferred) but whether the MVP's actual fixed MCS set can ever trigger this has not been checked against real TBS/RE-mapping arithmetic",
    "P2-R14/R14a literal dual-oracle (srsRAN-AGPL CI-only + Sionna shippable) structure not built — single strong real-library oracle substituted, disclosed in README"
  ]
}
```

---

## p2c-k1

```json
{
  "id": "p2c-k1",
  "status": "done",
  "path": "features/p2c-k1",
  "verified": true,
  "test_assertions_passing": 73,
  "oracle": "real linked OCUDU ecpri::packet_decoder + ofh::uplane_message_decoder + ofh::uplane_message_builder (encoder+decoder round-trip)",
  "real_bugs_fixed": [
    "oi_frame_desc.h OpenCL-C incompatibility (see p2a entry, found while implementing K1)",
    "parent LLD IQ conversion constant wrong (/32768.0f -> /32767.0f)",
    "OCUDU's real U-plane builder hardcodes direction=downlink regardless of input — worked around in test with a precisely-targeted bit flip (not a guess), not a kernel or OCUDU bug",
    "test comparison bug: compared K1's exact fp32 output against bf16-rounded real-decoder output using exact float equality — fixed to compare via to_bf16(kernel_value)==decoder_stored_value"
  ],
  "resolved_2026_07_24": [
    "Q2 Ethernet-layer VLAN question: triggered by p1's ru_emulator finding (--vlan_tag is CLI::Range(1,65536), no untagged option, so the real wire is expected to always carry a tag). Rather than flip the placeholder guess, K1's kernel now reads desc.eth_hdr_len (14 or 18, set by oi_oran_preparse_frame's new real per-frame VLAN detection — see p2a entry) instead of a hardcoded constant. k1_test.cpp extended with a full VLAN-tagged round-trip (real OCUDU encoder+decoder, real PoCL kernel run): bit-exact RE-grid match. p3's LLD updated with a two-branch BPF filter spec note + af_packet PACKET_AUXDATA caveat (not yet implemented — p3 hasn't started). Still open: which branch the live rig actually exercises — that's what the GCP capture confirms, not a code dependency anymore."
  ],
  "resolved_2026_07_26": [
    "udCompHdr compression-header offset (cross-feature, see p2a entry for the shared-code root cause + citations): K1's kernel now reads desc.payload_byte_off directly instead of computing OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len) itself, which silently assumed the static-compression (0-byte) builder layout. Real ru_emulator-sourced frames use the dynamic-compression (2-byte) layout unconditionally. k1_test.cpp extended with a new Test 6: a full round-trip using the REAL OCUDU dynamic-compression builder AND decoder (authentic OCUDU round-trip, not hand-inserted padding), bit-exact K1-kernel RE-grid match, plus a negative control. All pre-existing Tests 1-5 (static-compression, VLAN-tagged) still pass unchanged — additive, not a replacement. 73/73 total."
  ],
  "known_gaps_deferred": [
    "oi_frame_desc::section_id truncation risk (see p2a entry)",
    "P2-R14/R14a does not literally apply to K1 (no golden-vector oracle exists for T4-fresh eCPRI framing) — structural oracle (D9) is the correct and only gate, not a gap"
  ]
}
```

---

## p2d-k2-k3

```json
{
  "id": "p2d-k2-k3",
  "status": "done",
  "path": "features/p2d-k2-k3",
  "verified": true,
  "test_results": {
    "k2_test.cpp": {"ch_est_NRMSE_max_across_sweep": 0.0226, "noise_var_rel_err_max": 0.0236, "epre_rel_err_max": 0.00015, "gate": "NRMSE < 0.05 and rel_err < 0.05, both PASS"},
    "k3_test.cpp": {"assertions_passing": 19, "eq_symbols_NRMSE": 0.0000985, "eq_noise_var_rel_err_max": 0.00055, "gate": "NRMSE < 0.005 and rel_err < 0.005, both PASS"}
  },
  "oracle": "real linked OCUDU dmrs_pusch_estimator + channel_equalizer_generic_impl (mmse=zf path)",
  "thresholds_set_in_parent_lld": {"T_K2": 0.05, "T_K2n": 0.05, "T_K3": 0.005, "T_K3n": 0.005},
  "real_bugs_fixed": [
    "parent LLD K3 formula wrong: 1/(|h|^2+sigma^2) never existed in real code; MMSE==ZF for 1 Tx layer per OCUDU's own comment",
    "K2's committed single-launch prototype couldn't produce its own time-domain hold rule (needs two DMRS symbols at once) — split into K2a+K2b",
    "self-caught citation error: K2b's time-combine was initially (wrongly) attributed to interpolator_linear_impl; corrected to apply_td_domain_strategy/simd_vector_interpolate before landing",
    "K3 test: ch_symbols/ch_estimates stored as cbf16_t (bf16-rounded) in the real oracle path — fixed by rounding test inputs through bf16 before feeding either implementation",
    "K3 test: OCUDU's SIMD equalizer path uses an approximate reciprocal instruction (ocudu_simd_f_rcp) causing ~1.5e-4 relative mismatches — understood, non-bug source; kernel uses exact division and is arguably more precise; test tolerance set accordingly",
    "independent re-verification caught test thresholds (0.1/0.01/0.01) looser than the LLD's recorded values (0.05/0.005/0.005) — tightened the code to match the LLD, not the reverse (LLD's margin math was the one that reconciled correctly)"
  ],
  "known_gaps_deferred": [
    "K2b's noise-variance formula operates at 306-pilot-RE granularity rather than replicating an unresolved 612-wide/306-wide dimensional detail in the real estimate_noise() — validated empirically (max 2.36% error, well within the 5% threshold), not asserted correct by construction",
    "P2-R14/R14a literal dual-oracle structure not built — same substitution as K5/K6/K1",
    "beta_scaling (PUSCH-to-DMRS power ratio) fixed at 1.0 (0dB) throughout testing, not swept"
  ]
}
```

---

## p2e-k4

```json
{
  "id": "p2e-k4",
  "status": "done",
  "path": "features/p2e-k4",
  "verified": true,
  "test_assertions_passing": 17,
  "test_result": "bit-exact, 0 mismatches (P2-R6's gate has no tolerance column)",
  "oracle": "real linked OCUDU demodulation_mapper (scalar dispatch)",
  "real_bugs_fixed": [
    "OpenCL C rejects __constant arrays declared inside a non-kernel function (first draft had 64QAM lookup tables declared locally) — hoisted to file/program scope, caught by PoCL's own build error"
  ],
  "known_gaps_deferred": [
    "P2-R14/R14a literal dual-oracle structure not built — same substitution as other kernels, though this slice has no tolerance threshold left to set (bit-exact gate already fully met)"
  ]
}
```

---

## p2f-integration

```json
{
  "id": "p2f-integration",
  "status": "done",
  "path": "features/p2f-integration",
  "files_present": ["README.md", "VERIFICATION.md", "src/host/oi_p2_crc.{h,cpp}", "src/host/oi_p2_cb_segment.{h,cpp}", "src/host/oi_p2_ldpc_decode.{h,cpp}", "tools/oracle_tx_gen.cpp", "tools/pipeline_runner.cpp", "helpers/pcap_packer.py", "tests/cb_segment_test.cpp", "tests/ldpc_decode_test.cpp", "tests/integration/pipeline_test.py", "Makefile"],
  "depends_on": ["p2a-scaffold", "p2b-k5-k6", "p2c-k1", "p2d-k2-k3", "p2e-k4"],
  "dependencies_satisfied": true,
  "scope": ["LDPC hookup (wire K6 cb_llr_out to existing prior-work LDPC decoder, no re-quantization)", "CPU tail (CB segmentation, CRC24B/24A, oi_p2_tb_record)", "pcap_packer.py oracle-vector packer", "pipeline_test.py growing-pipeline integration gate (P2-R1, P2-R15, P2-R16, P2-R17)", "pipeline-level P2-R14 closure sweep across K1-K6"],
  "known_upfront_risk_RESOLVED_2026_07_23": "p2f's README flagged that every kernel slice's one-oracle-only P2-R14 practice would block P2-R15's integration gate unless explicitly decided. User directed: adopt the single-real-library-oracle approach as P2-R14's actual interpretation (parent SPEC.md/LLD.md amended). No longer a blocker.",
  "done_2026_07_23": [
    "CRC16/24A/24B bit-by-bit LFSR port (oi_p2_crc)",
    "CB segmentation sizing (nof_segments, lifting_size, segment_length, codeword_length, filler/CRC/zero-pad bits) + rm_length + desegmentation/TB-CRC self-check, verified vs real linked OCUDU ldpc_segmenter_tx across all 3 MVP MCS points: cb_segment_test.cpp 30/30 assertions PASS",
    "LDPC decode hookup (K6 output -> p0's decoder, incl. 2*Z zero-pad size bridge), verified bit-exact vs real OCUDU ldpc_encoder across all 3 MVP points + p0's baseline: ldpc_decode_test.cpp 18/18 assertions PASS",
    "bg_tables.h provenance fix + oi/gpu-phy:dev rebuild (see p0-rig-scaffold resolved_2026_07_23)",
    "p2a-scaffold's stub 8-stage chain replaced in-place with these real components, wired into oi_p2_host.cpp (see p2a-scaffold entry) — host_api_test.cpp 24/24 assertions PASS",
    "full re-verification sweep after all changes: lint_portability PASS, provenance_check PASS, every p2a-p2f test suite re-run and passing"
  ],
  "done_2026_07_23_pipeline_test": [
    "oi_p2_write_arena additive API (fills a real gap: no way for a caller to place frame bytes in the arena oi_p2_feed's descriptors point to) — see p2a-scaffold entry",
    "tools/oracle_tx_gen.cpp: real OCUDU TX chain (segment/LDPC-encode/rate-match/scramble/modulate, PDSCH's real TX chain consulted read-only since OCUDU has no PUSCH encoder) + real eCPRI/O-RAN wire frames + JSON ground-truth sidecar, self-verified via real-decoder round-trip for all 3 MVP MCS points",
    "real finding + fix: 64-QAM's peak constellation amplitude (~1.08) got silently clipped by the real wire codec's implicit unit full-scale assumption — fixed with a 0.9 TX amplitude scale in the oracle only (no pipeline-side change needed, equalizer is blind to a common data/DMRS gain)",
    "tools/pipeline_runner.cpp: feeds a pcap through the real oi_p2_host pipeline exactly as a production ingest_backend would (preparse -> write_arena -> feed -> launch_slot -> drain -> tap), emits a JSON TB record",
    "helpers/pcap_packer.py + tests/integration/pipeline_test.py: P2-R15's class-b (oracle-packed) gate — CRC24A pass + TB bit-exact vs the oracle's own known TB, verified for real across all 3 MVP MCS points (C=1/2/4, QPSK/16QAM/64QAM); class-a (P1-captured) implemented, at the time SKIPPED pending p1-ran-baseline's real pcap corpus (see done_2026_07_25_class_a below — no longer skipped); P2-R17 API-surface check implemented as a grep-based substitute (p3/p4 don't exist yet to literally test against)",
    "pipeline_test.py: 21/21 assertions PASS; full p2 re-verification sweep (lint_no_perf, lint_portability, provenance_check, every p2a-p2f test suite) re-run clean"
  ],
  "done_2026_07_25_class_a": [
    "class-a (P1-captured) gate run for real for the first time, against p1-ran-baseline's real GCP-archived corpus (840,783 frames). Found + fixed 2 real path/scope mismatches: (1) pipeline_test.py's P1_PCAP_DIR pointed at p1-ran-baseline/captures/, a location archive_pcap.sh never writes to -- repointed at the real corpus root artifacts/p1/pcaps/<latest-run-id>/, also fixed to pick up all rotated pcap fragments (fronthaul.pcap, .pcap1, .pcap2, ...) not just literal .pcap-suffixed files; (2) pipeline_runner.cpp's frame-feed loop was implicitly single-slot (reads slot_id from the first frame, never re-checks it) but never stopped at a slot boundary -- a real 30s/850K-frame multi-slot capture just overflowed the arena (OI_P2_ERR_ARENA_OVERFLOW), not an arena-sizing bug. Fixed by bounding the feed to the first slot_id observed (slot demuxing across a live stream is p3's job, not this MVP tool's). Real result: all 6 checks PASS across 3 pcap fragments (pipeline completes + I2-I5 taps readable, no CRC/TB assertion per DEV-044). Full suite re-run: pipeline_test: ALL PASS, 27/27 assertions, exit 0. lint_no_perf.sh re-run clean."
  ],
  "done_2026_07_26": [
    "udCompHdr compression-header offset fix (cross-feature, see p2a-scaffold entry for the shared root cause): pipeline_runner.cpp gains a required <udcomphdr_bytes> CLI arg, threaded to oi_oran_preparse_frame. pipeline_test.py passes 0 for class-b (oracle_tx_gen.cpp uses OCUDU's static-compression builder) and 2 for class-a (real ru_emulator-sourced P1 captures always use the dynamic-compression layout). pipeline_test: ALL PASS, 29/29 assertions (21 class-b + 6 class-a across 3 real pcap fragments + 2 P2-R17 -- this feature's own check count was already 29 pre-fix, the '27/27' figure recorded in done_2026_07_25_class_a above was already stale before this session, not a regression this fix caused)"
  ],
  "not_started": [],
  "known_gaps_deferred": [
    "P2-R1's original 'independent prefix build' design (K1-only, K1-K2, ...) no longer applies now that all 7 kernels wire into one oi_p2_setup call — pipeline_test.py checks the OBSERVABLE contract (I2-I5 taps independently readable) instead, a disclosed scope evolution, not a silent gap"
  ]
}
```

---

## p3-live-tap-ul-inject

```json
{
  "id": "p3-live-tap-ul-inject",
  "status": "partial",
  "path": "features/p3-live-tap-ul-inject",
  "files_present": ["spec/SPEC.md", "spec/HLD.md", "spec/LLD.md", "README.md", "VERIFICATION.md",
                   "src/host/oi_osg_format.{h,cpp}", "src/host/oi_osg_schedule.h",
                   "src/host/oi_ingest_af_packet.{h,cpp}", "src/host/oi_harness_calibrate.h",
                   "tools/osg_gen.cpp", "tools/pcap_comparator.cpp", "tools/bit_exact_harness.cpp",
                   "patches/0001-oracle-grid-ul-injection.patch",
                   "patches/files/ru_emulator_oracle_grid.{h,cpp}",
                   "docker/compose.p3.yml", "docker/configs/ru_emu_oracle_injection.yml",
                   "helpers/run_du_undisturbed_check.sh", "helpers/lint_no_perf.sh",
                   "tests/osg_format_test.cpp", "tests/osg_loader_crosscheck_test.cpp",
                   "tests/ingest_af_packet_test.cpp", "tests/harness_calibrate_test.cpp",
                   "tests/pcap_comparator_test.cpp", "tests/patch_schema_regression_test.cpp",
                   "Makefile"],
  "implementation_started": true,
  "spec_updates_made_this_project": [
    "LLD updated during the oi_p2_feed ABI reconciliation (pre-implementation): ingest wrapper calls the shared oi_oran_preparse_frame() (p2a) instead of bespoke parsing, then calls oi_p2_feed(pipeline, &desc)",
    "2026-07-26 (real build): LLD §Configuration's own YAML example was wrong (same 2 mistakes p1-ran-baseline's ru_emu.yml already found+corrected: flat ru_emu:[...] instead of ru_emu:cells:[...], and wrong field names ru_mac_address/ul_compr_method/ru_ul_port_id instead of the real ru_mac_addr/compr_method_ul/ul_port_id) -- caught by patch_schema_regression_test actually compiling the real patched schema and parsing the LLD's own example verbatim; fixed in LLD.md with the real precedent cited"
  ],
  "done_2026_07_26": [
    "LLD Q1 resolved: shared oi_oracle_pack library (p2f-integration/src/host/, TB->grid->wire-IQ-bytes, real OCUDU iq_compression_none_impl::compress() for the byte conversion, not hand-rolled) -- oracle_tx_gen.cpp (p2f) and osg_gen.cpp (p3) both call it; p2f's pipeline_test.py re-run clean (27/27) after extraction, zero drift",
    ".osg file format (oi_osg_format.{h,cpp}) byte-precise per LLD §3.1, real zlib CRC32; osg_format_test 27/27 PASS incl. real corruption detection + magic/version/iq_format/length validation",
    "osg_gen.cpp: real design finding (not in the LLD) -- K2a's DMRS c_init depends on the real wire nslot (oi_dmrs_ref_seq.cpp:31), so oracle files MUST be generated one-per-within-frame-slot (N=slots_per_frame=20 exactly), not an arbitrary count, or DMRS-based channel estimation would silently use the wrong reference sequence on every file reuse after the first",
    "M1/M2 ru_emulator oracle-injection patch (patches/0001-oracle-grid-ul-injection.patch): git apply --check PASS against the pristine pinned checkout (P3-R1, real, re-confirmed multiple times); 3 touched/new files syntax-check clean against real OCUDU headers; loader (ru_emulator_oracle_grid.cpp) compiled as real standalone code + cross-checked against the project-side writer (osg_loader_crosscheck_test, 24/24 PASS, incl. two independent CRC32 implementations agreeing); real CLI11 schema compiled+tested directly (patch_schema_regression_test, 13/13 PASS) proving both P3-R4 (absent block -> disabled, byte-identical to upstream) and P3-R2 (explicit block parses correctly)",
    "M3 gpu-phy ingest_backend (oi_ingest_af_packet.{h,cpp}): real two-branch VLAN-aware classic BPF filter, PACKET_AUXDATA handling, ring-buffer arena wraparound for continuous multi-slot demux, real counters. ingest_af_packet_test: REAL veth pair + REAL p1 captured corpus (200 frames, VLAN-tagged) + REAL oi_p2_pipeline (PoCL) -- 19/19 PASS. 2 real bugs found+fixed: SO_RCVBUF silently capped by net.core.rmem_max (fixed with SO_RCVBUFFORCE), and a real design bug where frames_seen would always == ethertype_matched with a single filtered socket (fixed with an unfiltered companion socket)",
    "M4 pcap comparator (tools/pcap_comparator.cpp) + M5 live bit-exact harness (tools/bit_exact_harness.cpp) + M6 DU-undisturbed checker (thin pass-through to p1's soak_stability.sh, unmodified, per LLD's own design): pcap_comparator_test 8/8 PASS (real corruption detection); harness_calibrate_test 43/43 PASS (real reconciliation finding: oi_frame_desc.slot_id is a host-derived counter, NOT the wire's real sfn/slot -- see VERIFICATION.md); bit_exact_harness compile-verified against the real pipeline+ingest, full live run deferred (no rig)",
    "docker/compose.p3.yml + docker/configs/ru_emu_oracle_injection.yml: real, locally-verified via `docker compose config` layered on top of upstream+p0+p1 -- 1 real bug found+fixed (command override missing, so ru-emu would have silently kept running p1's own config despite the oracle config being mounted)",
    "Full local test suite: 134/134 assertions PASS across 6 test binaries. Full project regression sweep (p1 ci_p1_static.sh, p2a-p2f incl. pipeline_test.py, all 4 lint tools) re-run clean, no regressions",
    "RETROACTIVE FIX (found while building p4): the 'all 4 lint tools' regression sweep above had been calling p2a-scaffold's own lint_no_perf.sh, whose find pattern is hardcoded to */p2*/ paths -- it silently scanned ZERO files under p3-live-tap-ul-inject/ every time. Every feature owns its own scoped copy (p1's is the correct precedent); p3 now has helpers/lint_no_perf.sh, run for real: 0 hits across src/docker/helpers/tests/tools/patches",
    "Live GCP session (same date, later): src-MAC BPF filter fix (oi_ingest_af_packet.h/.cpp, real live CPU-overhead bug -- see VERIFICATION.md), busy-loop-backoff fix (bit_exact_harness.cpp/gpu_phy_seam_bridge.c), and a real config-conversion bug in the OCUDU patch itself (emu_cfg.oracle_injection never copied from the CLI11-parsed struct -- see patch_conversion_regression_test.sh) all found+fixed. ingest_af_packet_test extended to replay the FULL 840,783-frame archived corpus (not a 200-frame sample).",
    "Live GCP session (same date, second pass): TWO more real bugs found+fixed chasing a calibration failure that survived all fixes above -- a direct raw-byte search proved injection was already correct at the source. (1) pcap_comparator had no direction filter, so a hub-mode-flooded capture let it calibrate against a downlink frame no oracle file could ever match -- fixed with a required <ru_mac> CLI arg + a source-MAC skip before preparse/calibration. (2) the deeper, cross-feature bug underneath (1): the shared oi_oran_wire_layout.h assumed OCUDU's static-compression (0-byte) U-plane builder layout, but this project's real RU emulator unconditionally uses the dynamic-compression (2-byte udCompHdr+reserved) layout -- confirmed byte-for-byte against two independent real corpora (163,268/163,268 frames matched in one). Fixed at the shared-code layer (oi_frame_desc gains payload_byte_off, oi_oran_preparse_frame gains a required udcomphdr_bytes parameter, K1's kernel reads the resolved field directly) -- see p2a-scaffold/p2c-k1 entries for the cross-feature half. pcap_comparator.cpp/bit_exact_harness.cpp/gpu_phy_seam_bridge.c all gained a required <udcomphdr_bytes> CLI arg (2 for the real rig). Full local sweep after both fixes: 157/157 assertions PASS across this feature's own test binaries (grew from 134 -- +23 net new assertions across the src-MAC BPF filter fix, the corpus-replay extension of ingest_af_packet_test, the patch_conversion_regression_test.sh new suite, and this session's direction-filter + udCompHdr-gap regression cases in pcap_comparator_test.cpp). Still deferred: the actual >=1000-slot P3-I1 live re-run with all fixes applied together -- see DEFERRED_LIVE_GATES.md's 'second session log'."
  ],
  "environment_blocked": ["P3-U1 (live-capture half)", "P3-U2 (live-rig regression half)", "P3-I1 (full integration)"],
  "environment_blocked_note": "Every piece each deferred gate exercises has its own real, passing local test (see VERIFICATION.md's module table) -- only the live-rig orchestration itself is deferred (no SCTP/rig on this host). Full runbook with exact commands + pass criteria: DEFERRED_LIVE_GATES.md at repo root.",
  "known_gaps_deferred": [
    "push-based (p3) vs pull-based (p6) ingest_backend control-flow inversion — flagged, not resolved, still open (unrelated to this session's work, p6 not built)",
    "Full patched ru_emulator binary never built locally (needs ocudu_ofh/ocudu_phy_support bootstrap beyond what this session's local testing judged worth the time, since it can't be functionally run without the live rig anyway) -- git apply --check, per-file syntax-check, and the loader/schema modules compiled+tested standalone all real; only the full linked binary is untested locally",
    "deploy_and_bring_up.sh has no image-override variable yet for the oracle-injection-patched ru-emu image -- flagged directly in DEFERRED_LIVE_GATES.md's P3-U2 entry"
  ]
}
```

---

## p4-phy-l2-seam

```json
{
  "id": "p4-phy-l2-seam",
  "status": "partial",
  "path": "features/p4-phy-l2-seam",
  "files_present": ["spec/SPEC.md", "spec/HLD.md", "spec/LLD.md", "README.md", "VERIFICATION.md",
                   "src/oi_seam_ring.h", "src/oi_seam.{h,c}", "src/oi_seam_producer.{h,c}",
                   "src/oi_l2_validate.{h,c}", "src/l2_stub_main.c",
                   "docker/compose.p4.yml", "docker/Dockerfile.l2stub",
                   "helpers/gate_p4_ordering.sh", "helpers/gate_p4_wrap.sh", "helpers/gate_p4_restart.sh",
                   "helpers/gate_p4_integration.sh", "helpers/lint_no_perf.sh",
                   "tests/struct_layout_test.c", "tests/producer_test.c", "tests/ordering_test.c",
                   "tests/wrap_test.c", "tests/restart_test.c", "Makefile"],
  "implementation_started": true,
  "done_2026_07_26": [
    "oi_seam_ring.h byte-precise per LLD §Data structures, _Static_assert-checked at every compile (offsetof/sizeof for every field); OI_SEAM_TB_MAX_BYTES=3457 real, computed value (MCS 21's 27656 tbs_bits / 8, no rounding needed) -- LLD Q3 resolved",
    "oi_seam.{h,c}: real reserve/publish/wait_status/release/epoch API, real mmap'd MAP_SHARED regular-file backing (P4-R10), bounded spin->yield->nanosleep backoff, ring-buffer wraparound. Two real API deviations from the LLD's literal signatures, both disclosed: oi_seam_open() takes a create flag + returns a status code (LLD didn't specify error surfacing), all real per-file additions documented in VERIFICATION.md",
    "oi_seam_producer.{h,c}: P4-R12's 1:1 field-mapping from a synthetic p2 record view, sfn=slot_id/slots_per_frame + slot=slot_id%slots_per_frame derivation, tb_size_bytes>OI_SEAM_TB_MAX_BYTES refusal (P4-R14, never truncates)",
    "oi_l2_validate.{h,c} + l2_stub_main.c: per-key (rnti,harq_id) (sfn,slot) monotonicity validation + CRC verdict counting + epoch-change detection/reset; 0 FAPI/SCF-222 symbols (grep-verified)",
    "struct_layout_test 33/33 PASS (real memcpy round-trip + every field offset), producer_test 13/13 PASS, ordering_test (P4-G1) 4/4 PASS (real ring, 2-key deliberate cross-key out-of-order completion, per-key monotonicity holds; negative case caught), wrap_test (P4-G2) 13/13 PASS (real 2nd thread, ring genuinely blocks past capacity, resumes correctly), restart_test (P4-G3) 17/17 PASS (both producer-restart epoch-bump and consumer-restart persisted-tail-resume scenarios) -- 80/80 total local assertions",
    "P4-R4 static check: 0 GPU-API symbols in oi_seam.*/oi_seam_ring.h (real grep). P4-R11 static check: 0 real FAPI/SCF-222 symbols (only this project's own comments mention 'FAPI' to document its absence). P4-R13 lint_no_perf.sh (feature-scoped copy): 0 hits",
    "Real finding: P4-R3's cited precedent ('CXL PoC's e2e_slot_t') doesn't exist anywhere in cxl_ran_poc/ -- closest real precedent is desc_ring.h's desc_ring_t (same release/acquire discipline, no per-slot status). LLD's own byte layout still authoritative, implemented as specified",
    "Real finding: dynamic race detectors (ThreadSanitizer: fails outright, known mmap limitation; Helgrind: reports races even after manual ANNOTATE_HAPPENS_BEFORE/AFTER experiments) cannot verify this ring's release/acquire discipline on x86_64, because acquire/release atomics compile to plain load/store instructions on x86 (no fence needed, x86 TSO already provides the ordering) -- indistinguishable from an unsynchronized access to an instruction-level instrumentation tool. Verified instead via static C11 memory-model code review (every non-atomic field write precedes the single release-store; every read follows the matching acquire-load) -- see VERIFICATION.md for the full account",
    "docker/compose.p4.yml + Dockerfile.l2stub: additive gpu-phy volume mount + new l2-stub service, named/volume-persisted ring (P4-R10)"
  ],
  "done_2026_07_26": [
    "udCompHdr compression-header offset fix (cross-feature, see p2a-scaffold entry for the shared root cause): gpu_phy_seam_bridge.c gains a required <udcomphdr_bytes> CLI arg, threaded through oi_ingest_open_af_packet (p3's module, whose own signature also grew this parameter). Real ru_emulator-sourced frames always need udcomphdr_bytes=2. This binary has no dedicated unit test of its own (correctness is exercised via p3's ingest_af_packet_test + this binary's own compile); local suite unaffected: 81/81 assertions still PASS (struct_layout_test/producer_test/ordering_test/wrap_test/restart_test), 0 regressions -- corrects this entry's own previously-recorded '80/80' figure, which was already stale before this session"
  ],
  "environment_blocked": ["P4-G4 (full integration)"],
  "environment_blocked_note": "Every piece P4-G4 exercises (ring library, producer field-mapping, L2 stub validation) has its own real, passing local test -- only the end-to-end wiring (gpu-phy's own event loop actually calling this feature's producer at its real drain call site, LLD Q1, still open) is untested locally, and needs the live rig regardless. Full runbook: DEFERRED_LIVE_GATES.md.",
  "known_gaps_deferred": [
    "LLD Q1 (drain call-site ownership) not resolved -- gpu-phy's own main loop doesn't yet call oi_p2_drain + this feature's producer together; flagged directly in DEFERRED_LIVE_GATES.md's P4-G4 entry as the real first step that run needs",
    "OI_SEAM_RING_CAPACITY=64 is the LLD's own stated MVP placeholder (Q2), not re-derived from real buffering-depth numbers (p2's HLD §5 double-buffering strategy isn't implementation-fixed yet)"
  ]
}
```

---

## p5-one-command-rig

```json
{
  "id": "p5-one-command-rig",
  "status": "partial",
  "path": "features/p5-one-command-rig",
  "files_present": ["spec/SPEC.md", "spec/HLD.md", "spec/LLD.md", "README.md", "VERIFICATION.md",
                   "schemas/suite.schema.json", "schemas/ledger.schema.json",
                   "helpers/discover_suites.py", "helpers/run_gate.sh", "helpers/ledger_build.py",
                   "helpers/ledger_render_md.py", "helpers/simtest_runner.py",
                   "helpers/lint_ledger_no_perf.sh", "helpers/compare_ledgers.sh",
                   "helpers/lint_no_perf.sh", "Makefile",
                   "tests/test_discover_suites.py", "tests/test_ledger_build.py",
                   "tests/test_compare_ledgers.py", "tests/test_p5_g1.py", "tests/mock_suites/**"],
  "implementation_started": true,
  "done_2026_07_26": [
    "IF-P5-SUITE (oi-p5-suite/1) + IF-P5-LEDGER (oi-p5-ledger/1) schemas written and enforced via jsonschema at discovery/build time",
    "discover_suites.py: globs pX-*/gates/suite.yml (or suite.physical.yml under --tier physical), schema-validates, plus LLD's extra rules (compose_overlays exist, gate ids unique, script exists+executable) -- an invalid/missing manifest is discovered:false with validation_error attached, never a crash (P5-R2/R3/R14)",
    "run_gate.sh + ledger_build.py: external timeout wrapper (D3, zero change to existing scripts) + PASS/FAIL/ERROR/BLOCKED/TIMEOUT classification, incl. the 'invalid JSON last line forces ERROR regardless of exit code' rule (P5-R4)",
    "simtest_runner.py: real discover -> merge compose_overlays (root-relative to --root, fixed a bug where it was hardcoded to this repo's own p0 base, which would have broken --root-pointed test runs and silently brought up the real 5GC/gNB stack during unit testing) -> real docker compose up (one merge, one up, one down) -> invoke every discovered gate in phase order -> aggregate -> render -> teardown. Zero feature-specific assertion logic (P5-R15)",
    "compute_overall: unconditional BLOCKED > FAIL/ERROR > INCOMPLETE > PASS precedence, exactly per LLD Data structures, unit-tested across 8 constructed combinations",
    "lint_ledger_no_perf.sh (P5-R8 rollup) + compare_ledgers.sh (P5-R9, pins_digest/rigcfg_digest NOT exempted, only host/timestamps/run_id ignored) both implemented and real-tested",
    "P5-G1 (tests/test_p5_g1.py): full run against tests/mock_suites/ with REAL docker compose (busybox services, real pull, real containers) -- real 4-overlay merge into ONE up, real teardown exactly once, real TIMEOUT from an actual sleep past timeout_s, real overall:BLOCKED via the precedence rule, real --only-phase (Q3(a): overlay still merged, only selected phase's gates invoked) and --keep-up behavior, real lint_ledger_no_perf.sh pass-then-catch. 63/63 total local assertions PASS across all 4 test files (14+20+7+22, grew from 59 after the 2026-07-27 oi-p5-suite/2 schema bump -- see done_2026_07_27 below)",
    "All 9 real gates/suite.yml addenda shipped: p1-ran-baseline (4 gates, incl. 2 new thin JSON-wrapper scripts for check_sctp.sh/soak_stability.sh which predate the JSON-verdict-line contract and write their precondition JSON to stderr not stdout), p2a-scaffold/p2b-k5-k6/p2c-k1/p2d-k2-k3/p2e-k4/p2f-integration (each phase = one real p2 sub-feature, since p2-phy-kernels itself holds only specs -- a disclosed, additive glob-scope decision, not a spec conflict), p3-live-tap-ul-inject (7 gates incl. a new patch-apply-check wrapper), p4-phy-l2-seam (5 gates) -- all 9 validate discovered:true against the real repo root, and every gate script was also smoke-tested directly against this host's real state (see DEFERRED_LIVE_GATES.md's P5-G2 entry for the itemized real results)",
    "Retroactive fix: p1's/p3's/p4's own lint_no_perf.sh (and the shared p2a-scaffold copy) didn't scan the new gates/ directories these addenda added -- all 4 extended one line each and re-verified clean (same class of gap already found for p0 and p3's original copy earlier this session)",
    "Real finding: check_sctp.sh reports sctp=available on this WSL2 host as of this build (contradicting DEFERRED_LIVE_GATES.md's prior 'no SCTP' framing, now corrected there) -- does not change any deferral, since every deferred gate needs the live rig genuinely UP, not just SCTP kernel support"
  ],
  "done_2026_07_27": [
    "make simtest run FOR REAL for the first time (WSL2), after building the missing local images (oi/gpu-phy:dev, oi/oracle:dev, ocudu/gnb:oracle-injection, oi/l2-stub:dev) and p3's real 20-file oracle grid set -- found+fixed 3 real, previously-latent bugs in simtest_runner.py/discover_suites.py itself (none of P5-G1's mock-suite tests could catch them: no real env vars, no real base compose file, no real teardown needed there). (1) oi-p5-suite/2 schema addition: optional compose_env map, each suite.yml declares its own overlays' required env vars via {root}/{feature_root} templates, runner generically merges them -- fixes 'docker compose up FAILED' for every real overlay with a hard-required ${VAR:?...}. (2) p0_base_overlay() -> p0_base_overlays() (plural): was missing the upstream docker-compose.yml that actually defines gnb/5gc's image/build blocks. (3) tear_down() silently swallowed its own failure (no env vars threaded through, same class as (1)) -- a run reporting overall:PASS still left every container running; now logs to stderr on nonzero exit. All 9 gates/suite.yml bumped to schema oi-p5-suite/2; mock-suite test fixtures (test_discover_suites.py, tests/mock_suites/) updated to match, full local suite re-verified 63/63 clean.",
    "WSL2 half of P5-G2 achieved for real: overall:PASS across all 29 real gates spanning all 9 phases (p1-p4), including p1-soak-stability at its own suite-specced 60s window -- verified via the real JSON output (counters_monotonic:true), not assumed from exit code. Final clean run (artifacts/p5/20260727T021153Z-8a0b784/) also confirmed docker ps -a empty afterward, verifying fix (3) end-to-end. Real finding on the same host, same night (see p1-ran-baseline's own entry): a separately-run, LONGER 600s soak on the same rig DID show a genuine TX stall (ru-emu's own TX_TOTAL/RX_TOTAL stuck at 0) -- the 60s gate genuinely passes, it isn't a shorter window hiding a real failure; the stall is real but appears to develop after roughly a minute, not present at t=0."
  ],
  "environment_blocked": ["P5-G2 (GCP half + WSL2/GCP cross-comparison)"],
  "environment_blocked_note": "The WSL2 half is now genuinely done (see done_2026_07_27 above) -- not blocked on rig readiness anymore. What remains is running the identical make simtest on the GCP VM and cross-comparing via compare_ledgers.sh. Blocked for a different, operational reason as of 2026-07-27: the GCP VM (oi-p1-rig) was stopped (standard cost discipline) at the end of the prior session, and this environment has no gcloud authentication to start it back up (gcloud auth list -> no credentialed accounts; direct SSH to its static IP times out, consistent with it being off). Needs the user to start the VM. Full runbook + exact commands: DEFERRED_LIVE_GATES.md.",
  "known_gaps_deferred": [
    "gates/suite.yml only exists for p1/p2a-f/p3/p4 today, matching what's built -- p6's PHYSICAL-tier manifests are p6's own future deliverable, correctly out of scope here",
    "rigcfg_digest is a real, reproducible SHA-256 over the resolved compose-overlay set's contents but isn't cross-checked against any other value defined elsewhere in the codebase -- its only consumer is compare_ledgers.sh's cross-host equality check"
  ]
}
```

---

## p6-physical-m1-ingest

```json
{
  "id": "p6-physical-m1-ingest",
  "status": "spec_only",
  "implementation_started": false,
  "note": "PHYSICAL-tier ingest; shares the same push/pull control-flow open item as p3. No implementation work done; needs the OCI GPU box, out of scope for this session except the optional P6-R1/R2/R3 standalone probe programs, added this session as build-verified-only (no hardware here).",
  "optional_tail_2026_07_26": {
    "files_present": ["src/probe01_nic_ident.c", "src/probe02_dmabuf_mr.c",
                      "src/probe03_rawqp_hostmem.c", "src/Makefile"],
    "done": [
      "probe01_nic_ident.c (P6-R1) and probe03_rawqp_hostmem.c (P6-R3): written against this host's REAL installed libibverbs/mlx5dv headers (infiniband/verbs.h, infiniband/mlx5dv.h -- confirmed present, real struct layouts used: ibv_flow_spec_eth, mlx5dv_qp_init_attr, MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC). Both compile with zero warnings (-Wall -Wextra -std=c11), link against real -libverbs/-lmlx5, and RUN for real on this host -- honestly reporting fail (no RDMA hardware present: MLX5_DEVICES=0, QP_CREATE/FLOW_RULE/LOOPBACK_RX=fail), not faked as passing",
      "probe02_dmabuf_mr.c (P6-R2): the ibverbs half (ibv_reg_dmabuf_mr, real signature) is real and compiles clean; the GPU-side dmabuf export half is written against the real, documented NVIDIA CUDA driver API / AMD ROCm HSA API function names, guarded behind OI_P6_HAVE_CUDA/OI_P6_HAVE_ROCM (neither defined by the Makefile, since neither SDK is installed here -- no cuda.h/hsa_ext_amd.h found on this host). Disclosed, not hidden: that branch has never been compiled against a real vendor header in this environment; the default (only locally-verified) path is an honestly-labeled stub that reports fail, matching this session's 'build-verified-only, cannot run' scope exactly",
      "All 3 probes: zero-warning compile + link verified via src/Makefile (make all), each probe's CLI/output contract (RESULT: KEY=value lines, exit 0 iff all its fields are ok) matches LLD's probe CLI table exactly"
    ],
    "known_gaps_deferred": [
      "probe02's GPU-export branch is unbuilt against any real vendor SDK in this environment -- needs a real box with CUDA or ROCm installed to actually build-verify (not just run) that half",
      "No other p6 work attempted, per this session's explicit instruction to do only P6-R1/R2/R3 and nothing else"
    ]
  }
}
```

---

## Cross-cutting open items (not owned by a single phase)

```json
{
  "cross_cutting_open_items": [
    {
      "item": "P2-R14/R14a literal dual-oracle structure (srsRAN-AGPL CI-only + Sionna shippable) not built for ANY kernel",
      "affects": ["p2b-k5-k6", "p2c-k1", "p2d-k2-k3", "p2e-k4", "p2f-integration"],
      "substituted_with": "real linked OCUDU library as a single strong oracle, disclosed in every affected README/VERIFICATION.md",
      "status": "RESOLVED 2026-07-23 — user-directed decision to adopt the single-real-library-oracle approach as P2-R14's actual interpretation going forward, rather than build the literal dual-artifact structure. Parent SPEC.md/LLD.md P2-R14 wording amended to record this. No longer a blocker for p2f-integration's P2-R15 gate."
    },
    {
      "item": "p3/p6 ingest control-flow inversion (push vs pull)",
      "affects": ["p3-live-tap-ul-inject", "p6-physical-m1-ingest"],
      "status": "open, flagged twice across sessions, needs explicit decision"
    },
    {
      "item": "oi_frame_desc::section_id truncation risk (uint8_t field, 12-bit real wire value)",
      "affects": ["p2a-scaffold", "p2c-k1"],
      "status": "open, harmless under current MVP scope (single section per frame), flagged for reconciliation if that scope ever changes"
    },
    {
      "item": "Q2 Ethernet II framing: code now handles BOTH untagged (14B) and 802.1Q-tagged (18B) — no longer a single-guess assumption. Which one the live rig actually uses is still unconfirmed.",
      "affects": ["p2a-scaffold", "p2c-k1", "p3-live-tap-ul-inject (spec updated, not yet implemented)", "p1-ran-baseline (whenever its captures exist)"],
      "status": "RESOLVED 2026-07-24 as a code dependency — oi_frame_desc.eth_hdr_len + oi_oran_preparse_frame's real per-frame VLAN detection + K1 kernel consuming the descriptor's eth_hdr_len (not a hardcoded constant), triggered by p1's ru_emulator finding (--vlan_tag has no untagged option). p3's LLD amended with a two-branch BPF filter spec note + af_packet PACKET_AUXDATA caveat. Still open as an EMPIRICAL question (which branch the live rig exercises) — resolving that no longer requires any code change, only a GCP capture to confirm."
    },
    {
      "item": "p0's gnb+5gc background build status unknown, SCTP unavailable on this host",
      "affects": ["p0-rig-scaffold"],
      "status": "open, needs a host with Docker+SCTP to fully close P0-R9/P0-G2"
    }
  ]
}
```
