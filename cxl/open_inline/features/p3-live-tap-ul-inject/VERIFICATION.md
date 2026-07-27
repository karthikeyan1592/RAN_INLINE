# p3-live-tap-ul-inject — VERIFICATION

## What's implemented and verified for real

All six LLD modules (M1-M6) are implemented. Everything locally runnable was run for real, no
hand-computed expected values anywhere — every test compares against a real linked OCUDU
component, a real captured corpus fragment, or a real kernel-level mechanism (veth pair, BPF
filter, PACKET_STATISTICS).

| Module | What it is | Real test | Result |
|---|---|---|---|
| M1/M2 (patch) | `patches/0001-oracle-grid-ul-injection.patch` against pinned OCUDU `release_26_04` | `git apply --check` against the pristine checkout; 3-file syntax-check (`ru_emulator.cpp`, `ru_emulator_cli11_schema.cpp`, `ru_emulator_oracle_grid.cpp`) with real OCUDU headers | PASS (exit 0); 0 compile errors |
| M2 loader (compiled) | `patches/files/ru_emulator_oracle_grid.{h,cpp}` | `osg_loader_crosscheck_test` — compiles the patch's own loader file directly, cross-checks vs the project-side writer | 24/24 PASS |
| Shared packer | `../p2f-integration/src/host/oi_oracle_pack.{h,cpp}` (LLD Q1's resolution) | p2f's `pipeline_test.py` regression (no drift after extraction) + `osg_gen`'s own self-check | 27/27 PASS (pipeline_test.py), self-check clean for MCS 4/13/21 |
| `.osg` format | `src/host/oi_osg_format.{h,cpp}` | `osg_format_test` — round-trip, CRC32 (real zlib) corruption detection, magic/version/iq_format/length validation | 27/27 PASS |
| M3 (ingest_backend) | `src/host/oi_ingest_af_packet.{h,cpp}` | `ingest_af_packet_test` — REAL veth pair, REAL p1 captured corpus (200 frames, VLAN-tagged), REAL `oi_p2_pipeline` (PoCL) | 19/19 PASS |
| M4 (comparator) | `tools/pcap_comparator.cpp` | `pcap_comparator_test` — hand-built wire frames carrying real oracle-grid bytes, corruption injection | 8/8 PASS |
| M5 (harness) | `tools/bit_exact_harness.cpp` | Compile-verified against the real pipeline + M3; calibration algorithm unit-tested separately | 43/43 PASS (`harness_calibrate_test`); full live run deferred (no rig) |
| M6 (DU checker) | `helpers/run_du_undisturbed_check.sh` | Thin pass-through to p1's own `soak_stability.sh`, unmodified (LLD's own stated design) | syntax-checked; real run deferred (no rig) |
| Schema regression | patched CLI11 schema | `patch_schema_regression_test` — compiles the real patched schema, parses configs with/without `oracle_injection` | 13/13 PASS |

**Total local assertions across this feature's own test suite: 134 PASS, 0 FAIL.**
Full project regression sweep (p1 `ci_p1_static.sh`, p2a-p2f including `pipeline_test.py`, all 4
lint tools) re-run clean after every change in this feature — see STATUS.md for the exact PASS
tallies per feature.

## LLD Q1 resolved: shared packer library

Per LLD's own recommendation, `oi_oracle_pack.{h,cpp}` (in `p2f-integration/src/host/`, since
p2f's `oracle_tx_gen.cpp` was the first mover) now holds the TB->RE-grid packing logic (steps
1-6 of the original recipe: random TB, real OCUDU segment/LDPC-encode/rate-match, scramble,
modulate, RE-map) and the real per-symbol grid->wire-IQ-bytes conversion (via OCUDU's actual
`iq_compression_none_impl::compress()`, not a hand-rolled byte packer). `oracle_tx_gen.cpp`
(pcap front end) and `osg_gen.cpp` (`.osg` front end) both call it. Verified no drift: p2f's own
`pipeline_test.py` re-run clean (27/27) after the extraction, byte-identical CB counts for all 3
MVP MCS points.

## Real bugs found and fixed (chronological)

1. **Missing includes when extracting `oi_oracle_pack.h`**: `cbf16_t` isn't declared in
   `ocudu/adt/bf16.h` alone — it needs `ocudu/adt/complex.h` too (the original file included
   both; the extraction dropped one). Also dropped `compression_factory.h` from
   `oracle_tx_gen.cpp`'s own includes, needed for `main()`'s own `create_iq_compressor` call.
   Found via real compile errors, not guessed.
2. **`osg_gen.cpp`'s DMRS-safety design constraint** (not in the LLD, found while designing the
   generator, confirmed by reading `oi_dmrs_ref_seq.cpp:31`'s real `c_init` formula): K2a's
   channel estimation computes `nslot = slot_id % 20` from the REAL wire `slot_id`, and DMRS
   `c_init` is a direct function of `nslot`. If an `.osg` file's baked-in DMRS grid used a
   different `nslot` than the real within-frame slot it gets injected at, channel estimation
   would silently use the wrong reference sequence. Fixed by making `osg_gen` always produce
   exactly `slots_per_frame` (20) files — one per `nslot` 0..19 — which, combined with
   `N == slots_per_frame` in the P3-R6 schedule, guarantees `file_idx == slot mod 20` for every
   `sfn`, matching each file's own baked-in `nslot` by construction.
3. **`oi_frame_desc.slot_id` vs the wire's real (sfn, slot)** (real reconciliation finding, not
   previously flagged in the LLD): `slot_id` is the HOST's own monotonic counter, starting at 0
   when ingest's stream state is constructed (confirmed via `oi_oran_preparse.cpp`'s
   `running_slot_id++`), with no guaranteed relationship to the wire's actual cumulative slot
   count if the tap joins a live stream mid-flight. SPEC.md's own "harness derives sfn = slot_id
   / slots_per_frame" text implicitly assumes they're the same counter. Resolved: since
   `N == slots_per_frame` makes `file_idx` depend only on `slot mod slots_per_frame`, the harness
   only needs to recover a constant phase offset once (search all N candidates against the first
   decoded TB/frame, lock the one that matches) — implemented in `oi_harness_calibrate.h`, used
   by both M4 and M5, unit-tested for all 20 possible true offsets plus the failure path.
4. **`generate_test_data()` regression from the header-size extraction** (caught by direct
   compile against real OCUDU headers, not by inspection): the first patch draft tried to make
   `generate_test_data()` call the new `ul_uplane_headers_size()` helper too, removing its local
   `ofh_header_size`/`ecpri_iq_data_header_size`/`ether_header_size` variables — but that function
   ALSO uses `ofh_header_size` directly later (`params.payload_size`), so the removal broke a
   real compile. Fixed by leaving `generate_test_data()` completely untouched (upstream, byte-
   identical) and making the new helper a fully independent duplicate, accepting a small constant
   duplication in exchange for zero regression risk in code this patch doesn't otherwise touch
   (P3-R5's own "patch touches only the identified path" spirit).
5. **`SO_RCVBUF` silently capped, `ingest_af_packet_test`'s real socket_drops** (found via the
   test itself, not assumed): a burst of 200 real captured frames produced real nonzero
   `PACKET_STATISTICS.tp_drops`. Requesting a larger `SO_RCVBUF` (8MB) had ZERO effect —
   root-caused via direct measurement (`sysctl net.core.rmem_max` = 212992 bytes on this host,
   below the ~480KB the burst needed) to `SO_RCVBUF` being silently capped at `rmem_max`
   regardless of the requested value. Fixed with `SO_RCVBUFFORCE` (bypasses the cap, needs
   `CAP_NET_ADMIN` — already true for every environment this project targets), with a graceful
   fallback to plain `SO_RCVBUF` if not privileged. Re-ran the test after the fix:
   `socket_drops` dropped from 7 to 0, `ethertype_matched` went from 193 to the full 200 sent.
   This is a real production concern, not just a test artifact: P1 measured ~28K frames/sec
   sustained on a live rig.
6. **`frames_seen` vs `ethertype_matched` degenerate-equal bug** (found while implementing M3,
   before any test ran): with a genuine kernel-level `SO_ATTACH_FILTER`, a `ret 0` reject drops
   the frame before it ever reaches userspace — there is no way for a single filtered socket to
   observe pre-filter traffic. The original single-socket design would have made `frames_seen`
   always equal `ethertype_matched`, contradicting the LLD's own distinct-counter semantics.
   Fixed with a second, unfiltered, never-drained companion socket on the same interface, used
   only for its own `PACKET_STATISTICS.tp_packets` to source `frames_seen`.
7. **This feature's own LLD.md YAML example was wrong** (caught by
   `patch_schema_regression_test` actually compiling the real patched schema and parsing the
   LLD's own example verbatim): the example used a flat `ru_emu: [...]` list and field names
   (`ru_mac_address`, `ul_compr_method`, `ru_ul_port_id`, ...) — the EXACT same two mistakes
   `p1-ran-baseline/docker/configs/ru_emu.yml`'s own header comment already found and corrected
   months earlier (real shape: `ru_emu: cells: [...]`, real field names `ru_mac_addr`/
   `compr_method_ul`/`ul_port_id`/...). Fixed in LLD.md §Configuration, with a note citing the
   real precedent so the next reader doesn't repeat the mistake a third time.
8. **`compose.p3.yml`'s `command` override missing** (caught by testing `docker compose config`
   rendering locally, matching P0/P1's own precedent of verifying compose layering before
   bring-up): compose's `configs:` list merges additively across layers (confirmed:
   `ru-emu`'s rendered config correctly showed BOTH p1's `ru_emu_config.yml` and this feature's
   `ru_emu_oracle_injection.yml`), but `command` is a full-scalar override, not merged — without
   an explicit override, `ru-emu` would have kept running p1's own `-c /ru_emu_config.yml`
   (injection silently absent) despite the new config being mounted right next to it. Fixed by
   explicitly overriding `command` in `compose.p3.yml`.
9. **`ocudu_yaml_util` never built locally** (needed for the schema regression test, not
   previously built by any earlier feature's bootstrap): `cmake --build . --target
   ocudu_yaml_util` — a small, fast, single-file target (`lib/support/config_yaml.cpp`); also
   needed `-lyaml-cpp` at final link (missing on the first attempt, real undefined-reference
   errors from `YAML::` symbols).
10. **This feature's own "full regression sweep" had never actually run `lint_no_perf.sh` against
    p3's own tree** (found retroactively, while building p4): the regression sweep reported
    earlier in this session called `p2a-scaffold/helpers/lint_no_perf.sh`, whose own `find`
    pattern is hardcoded to `*/p2*/...` paths — no path segment under
    `p3-live-tap-ul-inject/` ever matches `p2*`, so it silently scanned zero files here every
    time it was run, PASS or not. Every feature's `lint_no_perf.sh` is its own copy, scoped to
    its own tree (p1's own copy is the correct precedent, not p2a's) — this feature now has its
    own (`helpers/lint_no_perf.sh`), created and run for real: clean, 0 hits, across
    `src/docker/helpers/tests/tools/patches`.
11. **`pcap_comparator` calibrating against the wrong-direction frame** (2026-07-26, found live on
    GCP after a real captured wire session, DEFERRED_LIVE_GATES.md's "second session log" has the
    full narrative). Symptom: `pcap_comparator` reported `"calibration failed on first U-plane
    frame (slot_id=8, symbol_id=0)"` against a live oracle-injection capture, even though a direct,
    independent raw-byte search (plain Python, no project code) proved the exact 2448-byte oracle
    payload for that exact (slot, symbol) WAS present, byte-for-byte, 1495 times in the capture —
    injection was correct at the source. Root cause: the bridge tap runs in hub mode
    (`ageing_time=0`, needed for UL visibility — see p1's own workaround), so the pcap contains
    BOTH directions of fronthaul traffic; `pcap_comparator` had no direction filter and calibrated
    against whichever eCPRI frame it saw first in capture order — confirmed directly (the first
    `ethertype=0xAEFE` frame in the capture had `src=DU_MAC`, a downlink frame no oracle file could
    ever match). `oi_frame_desc`/`oi_oran_preparse_frame` have no direction field (O-RAN U-plane
    section headers are direction-symmetric at the wire-format level), so this had to be filtered
    at the tool layer, not the shared parser. Fixed: `pcap_comparator.cpp` now requires a `<ru_mac>`
    CLI arg and skips any non-RU-sourced frame (Ethernet bytes 6..11) before preparse/calibration.
    New regression test (`pcap_comparator_test.cpp`): a stream with a leading DU-sourced frame whose
    payload matches no oracle file must still calibrate correctly and report 0 mismatches once the
    filter is in place; a negative control re-runs the SAME stream without the fix's effective
    behavior (`udcomphdr_bytes` mismatch, see item 12) to prove the test actually exercises the bug.
12. **The real bug underneath item 11, and why fixing it alone still didn't pass** (2026-07-26,
    same session, immediately after item 11's fix): re-running `pcap_comparator` with the direction
    filter in place STILL failed calibration, now on the correct (RU-sourced) first frame. Direct
    byte-offset investigation (hex-dumping the real captured frame at the position the tool assumed
    payload started) found the real IQ payload begins 2 bytes later than
    `OI_WIRE_TOTAL_HEADER_BYTES(eth_hdr_len)` computes — bytes `[34]=0x00 [35]=0x00` sit between the
    O-RAN section header and the real IQ data on every real RU-sourced frame checked (8 consecutive
    symbols, then 163,268/163,268 frames in an independent second corpus). Root-caused by reading
    the real OCUDU source (not guessed): `ofh_uplane_message_builder_static_compression_impl::
    serialize_compression_header` (`ofh_uplane_message_builder_static_compression_impl.cpp:9-14`)
    writes 0 bytes ("the udCompHdr and reserved fields are absent"); `..._dynamic_compression_impl::
    serialize_compression_header` (`ofh_uplane_message_builder_dynamic_compression_impl.cpp:10-23`)
    writes 2 (`data_width<<4|type` + reserved). `apps/examples/ofh/ru_emulator.cpp`'s own hand-rolled
    frame construction (upstream OCUDU example code, `ru_emulator.cpp:195-244`, not this project's
    patch) unconditionally uses the 2-byte layout regardless of `ul_compr_method` config — confirmed
    byte-for-byte against two independent real corpora (this session's none/16 capture, gap bytes
    `0x00 0x00`; `artifacts/p1/pcaps/20260725T180323Z`'s bfp/9 capture, 163,268/163,268 frames
    matched, gap bytes `0x91 0x00` = `(9<<4)|1` exactly as predicted). This is a shared,
    cross-feature bug (p2a's `oi_oran_preparse_frame`/`oi_frame_desc` and p2c's K1 kernel both
    silently assumed the 0-byte layout) — fixed at the shared-code layer, not as a p3-local
    workaround: see p2a-scaffold/VERIFICATION.md and p2c-k1/VERIFICATION.md for the cross-feature
    fix account, and `oi_oran_wire_layout.h`'s header comment for the full citation trail. p3's own
    share of the fix: `pcap_comparator.cpp`/`bit_exact_harness.cpp` (and p4's
    `gpu_phy_seam_bridge.c`) now take a required `<udcomphdr_bytes>` CLI arg (2 for the real rig,
    never sniffed/assumed), threaded through `oi_ingest_open_af_packet`. New regression tests:
    `pcap_comparator_test.cpp` (a stream with the real 2-byte gap present, correct with
    `udcomphdr_bytes=2`, and a negative control proving `udcomphdr_bytes=0` against the SAME stream
    fails exactly the way the real bug did).

    **This is the SECOND time a synthetic-test-validated wire assumption failed on first contact
    with real traffic** (the first was the VLAN `eth_hdr_len` fix, 2026-07-24) — same root pattern:
    the synthetic frame generator (`k1_test.cpp`'s own static-builder fixtures, `oracle_tx_gen.cpp`)
    and the parser shared an unstated assumption (0-byte comp-header), so agreement between them
    proved internal consistency, not correctness against the real wire. This is exactly why the
    live P3-I1 gate exists — a synthetic-only test suite, however thorough, cannot catch a bug where
    the test fixture and the code under test are wrong in the same way.

## Real, disclosed API deviations from the LLD's literal text

- **`oi_ingest_open_af_packet(iface, pipeline, arena_bytes)`**: LLD shows `(const char* iface)`
  only, with no way for `oi_ingest_poll`'s own documented `oi_p2_feed(pipeline, ...)` call to
  reach a pipeline pointer. Genuinely underspecified (not a conflict with an already-frozen
  cross-feature contract — `oi_ingest` is this feature's own API surface) — resolved by adding
  the pipeline pointer (and the arena's byte budget, needed for ring-buffer wraparound) as
  construction-time parameters, stored in the opaque handle, consistent with the LLD's own
  "own one `oi_oran_preparse_state` per stream" ownership pattern for the same handle.
- **`oi_ingest_last_slot_id`/`oi_ingest_has_last_slot_id`** (additive, not in the LLD's
  `oi_ingest_counters` struct): needed so M5 can detect a slot boundary and drive
  `oi_p2_launch_slot`/`oi_p2_drain` itself — the LLD's M3 text only ever describes `feed()`,
  never launch/drain, which must live somewhere for continuous multi-slot operation. Same
  additive-not-breaking precedent as `oi_p2_write_arena`'s own earlier addition to `oi_p2_host.h`.
- **Ring-buffer arena wraparound** (not literally specified, but implied by "continuous
  multi-slot demux" being explicitly this feature's own scope, distinct from
  `pipeline_runner.cpp`'s one-shot bounded feed): `oi_ingest_poll` wraps `arena_offset` to 0
  whenever the next frame wouldn't fit before the configured `arena_bytes` budget, never
  splitting a frame across the boundary.
- **`oi_ingest_open_af_packet`'s 4th parameter, `ru_mac`** (2026-07-26, added after a real,
  live-diagnosed bug — full account in the "src-MAC BPF filter fix" entry above): the LLD's BPF
  filter pattern was ethertype-only. On a real bridge, gpu-phy is a 3rd promiscuous listener, not
  UL traffic's real destination, so DL frames only reach it via hub-mode flooding
  (`ageing_time=0`) needed for UL visibility in the first place — at ~4x the relevant traffic
  volume. Measured live on the GCP rig: a real 2:1 system/user CPU-time penalty, ~2.4 min/real-
  slot instead of real-time. Fix: the kernel filter now also requires `src_mac == ru_mac`, so the
  DL flood is dropped before any syscall, not just before feed(). `ru_mac` is caller-supplied (6
  raw bytes), read from the same `ru_mac_addr` config value every other rig config already uses —
  never hardcoded in this module. All three real callers (the ingest test, `bit_exact_harness`,
  and p4's `gpu_phy_seam_bridge`) updated to pass it; `compose.p3.yml`/`compose.p4.yml`'s own
  command arrays pass the real pinned value (`02:6f:69:00:01:01`), matching how `rnti`/MAC
  addresses are already threaded through those same files.
- **`oi_ingest_open_af_packet`'s 5th parameter, `udcomphdr_bytes`, and `pcap_comparator`'s new
  `<udcomphdr_bytes>` CLI arg** (2026-07-26, added after the shared udCompHdr-offset bug — full
  account in "Real bugs found and fixed" item 12 above): a cross-feature signature change
  (`oi_oran_preparse_frame` in p2a-scaffold gained the same parameter) needed because the
  udCompHdr+reserved field's presence (0 or 2 bytes) can't be reliably autodetected from a frame's
  own bytes — it reads as `0x00 0x00` for the real rig's none/16 config, indistinguishable from
  "absent" by content alone. Every real caller (`bit_exact_harness`, `pcap_comparator`,
  `gpu_phy_seam_bridge`, the ingest test) now passes it explicitly; the real rig's value is always
  `OI_WIRE_UDCOMPHDR_BYTES_PRESENT` (2), since `ru_emulator.cpp`'s own frame construction always
  uses that layout regardless of config. `compose.p3.yml`/`compose.p4.yml` updated accordingly.

## Known-open items (real, not hidden)

- **UPDATE 2026-07-26 (GCP live session): the patched `ru_emulator` binary WAS built for real**,
  on the GCP VM, from a fresh `release_26_04` checkout with the patch applied (`docker build -t
  ocudu/gnb:oracle-injection`) — superseding the earlier "never built locally" note below (kept
  for its own real history, not deleted). One real bug was found and fixed in the process (see
  `DEFERRED_LIVE_GATES.md`'s p3 section): `generate_test_data()` still referenced a local
  `ofh_header_size` variable the patch had removed — the earlier session's claimed fix for this
  exact issue was never actually present in the saved patch file; re-fixed and re-verified
  (`git apply --check` clean, `patch_schema_regression_test` still 13/13, the image builds and
  the container runs for real). The image is real, tagged, and was run against the live rig with
  real injected oracle traffic (P3-U1 in progress — see below).
- *(historical, superseded by the above)* Full patched `ru_emulator` binary was never built
  locally. Building the whole CMake target needs `ocudu_ofh`/`ocudu_phy_support`/
  `ocudu_yaml_util` (only the last was built, for the schema test) — a substantial additional
  bootstrap this session judged out of proportion to local-testing value, since the binary
  can't be functionally exercised without the live rig anyway. What WAS verified locally
  instead: `git apply --check` (real), 3-file syntax-check against real OCUDU headers (real),
  the loader module compiled+tested as real standalone code (real), and the CLI11 schema
  compiled+tested as real standalone code (real).
- **P3-U1 (live-capture half): real progress, not yet a full pass.** The live rig was brought up
  on GCP with real oracle injection (patched `ru-emu` + real `.osg` grid files + `gpu-phy` running
  `bit_exact_harness` against real fronthaul traffic). A real, live throughput problem was found
  and fixed mid-session (the src-MAC BPF filter fix, documented above) — the original attempt at
  ≥1000 slots was taking ~2.4 min/slot (hub-mode flooding gpu-phy with ~4x irrelevant DL traffic);
  a reduced 20-slot run was then attempted and was still in progress (>45 min elapsed, real CPU
  time accumulating, real traffic being received and processed, no crash) when stopped for the
  fix. Honest accounting of what that specific run does and does NOT prove: `bit_exact_harness`
  only emits its one JSON verdict line on successful completion (by design, matching every other
  gate script's one-line-verdict convention) — since it was stopped mid-loop, no intermediate
  slot-by-slot count or mismatch tally was ever printed, so this run contributes "the mechanism
  runs against real live traffic without crashing or erroring" as evidence, not a specific N-slots-
  bit-exact data point. The actual bit-exactness of this decode chain is independently proven by
  already-passing local tests exercising the identical code path against known oracles
  (`pipeline_test.py`, k1-k6 kernel tests) — this live run's job is proving the *live-traffic
  integration*, not re-proving decode correctness from scratch. Full re-run at the specced ≥1000
  slots with the fix applied is the next live-session step.
- **UPDATE 2026-07-26 (same date, second GCP session): the calibration-failure symptom is now
  root-caused and fixed, not just worked around.** After the busy-loop-backoff and
  `emu_cfg.oracle_injection` conversion-line fixes (see items above), a live wire capture proved
  injection was genuinely correct at the source (a direct raw-byte search found the exact expected
  oracle payload, byte-for-byte, in the RU-sourced traffic) — the remaining calibration failure was
  two further real, distinct bugs in the comparator/parser layer, not the patch: `pcap_comparator`
  had no direction filter (see "Real bugs found and fixed" item 11) and the shared wire-layout code
  assumed the wrong (0-byte) comp-header layout (item 12, a cross-feature fix — see p2a-scaffold/
  p2c-k1's own VERIFICATION.md). Both fixed, both locally regression-tested (see item 12's account).
  **Still not done**: the actual ≥1000-slot live re-run with all four fixes applied together has not
  yet been executed to completion — this session's VM time was spent on capture+diagnosis, not a
  full timed run. Direct next step.
- **P3-U2's live-rig regression half, P3-I1's full ≥1000-slot integration**: still deferred, now
  with a real, fixed, and locally-proven ingest fast path underneath them. Full runbook:
  `DEFERRED_LIVE_GATES.md` at the repo root.
- **`deploy_and_bring_up.sh` has no image-override variable yet** for pointing at the
  oracle-injection-patched `ru-emu` image instead of the base one — this session's GCP work
  brought the rig up with direct `docker compose` commands instead (documented in
  `DEFERRED_LIVE_GATES.md`), so this gap is unchanged, still real, still open.
- **`ru_emu_config.yml`/`ru_emu_oracle_injection.yml` are two separate, complete files, not one
  base file plus an additive fragment** — docker compose's `configs:` has no YAML-merge
  mechanism, so `compose.p3.yml` mounts a second, complete config file rather than attempting to
  layer YAML the way compose layers compose files themselves. Documented in
  `compose.p3.yml`'s own comment; not a hidden inconsistency.
