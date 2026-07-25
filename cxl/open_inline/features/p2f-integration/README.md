# p2f-integration

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — wiring all six kernels + the
reused LDPC decoder + the CPU tail into one real pipeline, and the growing-pipeline integration
gate. This is the slice that proves K1→K2→K3→K4→K5→K6→LDPC→CRC actually chains correctly, not just
that each kernel is individually correct.

## Scope

- **LDPC hookup** (`src/host/...`) — invoke the existing prior-work bit-exact BG1/BG2 OpenCL
  decoder (own prior work, source SHA pinned) unmodified, wired to K6's `cb_llr_out` with **no
  re-quantization** (same `int8` buffer, no copy-and-rescale) — **P2-R9**. ABI-mismatch is the
  first thing to check on any regression here (parent LLD §6 error table), not the LDPC kernel
  itself (already proven).
- **CPU tail** (`src/host/oi_p2_cb_segment.h/.cpp`, `oi_p2_crc.h/.cpp`) — CB segmentation (TS
  38.212 §5.2.2), CRC24B per CB (>1 CB), CRC24A over the TB, emitting the `oi_p2_tb_record` (LLD
  §4.7) consumed by `p4-phy-l2-seam`. **P2-R10**.
- `helpers/pcap_packer.py` — packs oracle RE grids into valid U-plane frames (class-b pcaps,
  **P2-R15b**), the harness tool that makes the integration gate's pass/fail half possible.
- `tests/integration/pipeline_test.py` — the full growing-pipeline gate, **P2-R15**:
  - class (a) P1-captured pcaps → **structural gate only** (K1 bitmap/grid matches CPU decoder
    replay, pipeline runs to completion, stable across repeat runs — no CRC/TB assertion; DEV-044
    rationale: no ground truth exists in ru_emulator's static IQ);
  - class (b) oracle-packed pcaps → **CRC24A pass + TB bit-exact** vs the packer's own known
    oracle TB, all three MCS — this is the actual pass/fail decode gate.
  - Also validates **P2-R1** (every pipeline prefix K1-only, K1–K2, … buildable/testable) and
    **P2-R17** (p3/p4 stub linkage against `oi_p2_host.h` unchanged, pcap→live feed swap needs no
    kernel/API change).

## Gates this slice owns

LDPC-dep={R9}; integration/P2-exit={R1, R15, R16, R17} from the parent gate-mapping table.

**P2-R14's pipeline-level closure is this slice's job**, distinct from p2b/p2d/p2e's per-kernel
ownership: confirm that every kernel slice actually landed with both oracles wired (not just its
own README's claim — re-run `oracle_compare.py` across K1–K6 as a single sweep before declaring
P2 exit) and that the **R13** AGPL-hygiene boundary held throughout integration — i.e. no srsRAN
CI-only artifact leaked into the pipeline_test.py fixtures that ship or get packaged (re-run
`agpl_denylist.py` at this level too, not just per-kernel). If any prior slice's R14 claim turns
out to be one-oracle-only, that's a blocker for P2-R15's integration gate, not a separate bug.

## Depends on

**All of p2a–p2e** — this is the slice that requires every other kernel to exist. Sequence this
last.

## Note on p1 dependency

Class-(a) pcaps require `p1-ran-baseline`'s captured corpus to exist. If p1 isn't built yet when
this slice starts, class-(b) (the actual correctness gate, self-contained via `pcap_packer.py`) can
still be fully implemented and gated — class-(a) is a structural-only, no-ground-truth check that
can be added once p1's pcaps are available, per the earlier finding that p2's core correctness work
doesn't block on p1.

## Design decisions (recorded ahead of implementation, 2026-07-23)

Two calls made for this slice, captured here now so the reasoning survives even though the code
hasn't landed yet — same convention as `bg_tables.h`'s provenance entry in
`p0-rig-scaffold/docker/gpu-phy/ldpc_suite/MODIFICATIONS.md`, just written before rather than after
the change. Update this section (or fold into a `VERIFICATION.md`) once implemented.

1. **Pipeline wiring: replace p2a's 8-stage stub chain in `oi_p2_host.cpp` in place**, rather than
   building a separate p2f-owned orchestration binary. `P2-R17` already frames `oi_p2_host.h` as
   *the* stable API surface p3/p4 build against, and p2a's own README stated the stub existed
   specifically to be filled in as kernels land — a second orchestrator wouldn't satisfy either of
   those, it would just relocate the real wiring work to later, after p3/p4 exist and the change
   becomes riskier. Matches the precedent already set when p2d bumped the stub 7→8 stages in place
   for the K2a/K2b split. `host_api_test.cpp`'s stage-marker assertions become real per-stage
   correctness checks as each stage is wired.
2. **`oi_p2_launch_slot` gains an `mcs_index` parameter**: `oi_p2_launch_slot(pipeline, slot_id,
   mcs_index)`. K4's Qm and K6/LDPC's base_graph/lifting_size/rm_length are all MCS-derived, and
   nothing upstream of this PHY-only pipeline carries that information otherwise (it's a real
   DCI/scheduler-grant value in a full system). The config schema's `mcs_set: [4, 13, 21]` being a
   *list* rather than a single `mcs_index` scalar was the deciding signal — it already reads as
   "must handle any of these per transmission," not "pick one for the pipeline's lifetime," which
   is also just how MCS actually works (a per-slot scheduler decision, not a pipeline-lifetime
   constant). Same caller-supplied-per-launch pattern already used for `tx_scaling` on K3. Cheap
   now for the same reason the `oi_p2_feed` ABI amendment was: no real caller depends on the
   current two-argument signature yet.
