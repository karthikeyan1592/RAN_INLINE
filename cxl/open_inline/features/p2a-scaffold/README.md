# p2a-scaffold

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — the shared host/tooling
scaffolding every other p2 sub-feature depends on. No new spec here; the requirements below are
p2-phy-kernels' own (SPEC.md/HLD.md/LLD.md), split out for tractable, independently-verifiable
implementation.

## Scope (from parent LLD §1 module breakdown)

- `src/host/oi_p2_host.h/.cpp` — the `setup/feed/launch_slot/drain/tap/teardown` API (LLD §2),
  stable per **P2-R17**. Kernel implementations plug in as sub-features land (p2b–p2f); this slice
  builds the API surface + orchestration (queue/event chain, HLD §6) against dummy/no-op kernel
  stages so the API itself is testable before any real kernel exists.
- `src/host/oi_p2_config.h/.cpp` — YAML load + MVP-config validator, **P2-R11**.
- `src/host/oi_p2_buffers.h/.cpp` — buffer pool (RE grid ×2, chan-est, eq-out, LLR, CB-LLR),
  zero-device-allocation-after-setup rule (LLD §5).
- `src/kernels/oi_kernel_compat.h` — the single abstraction header (HLD §2); MVP: empty of vendor
  branches. Every later kernel includes this and nothing else vendor-specific.
- `helpers/lint_portability.py` — static lint for **P2-R2** (warp-width/asm/intrinsic grep + WG-size
  scan); runs against whatever `.cl`/SYCL sources exist at the time, so it's live from commit 1.
- `helpers/provenance_check.py` + `src/host/oi_p2_provenance.h` — **P2-R12**: every kernel source
  must carry a `provenance.json` entry (OCUDU repo URL, tag `release_26_04`, clone SHA, port
  source paths). This slice builds the checker; later slices populate the entries.
- `helpers/agpl_denylist.py` — **P2-R13**: packaging/lint deny-list for AGPL artifacts.
- `helpers/lint_no_perf.sh` — **P2-R16**: no timing/throughput threshold anywhere in gating code.

## Gates this slice owns

Cross-cutting rows from the parent gate-mapping table: **R2, R11, R12, R13, R16**.

**R14 (dual-oracle) is explicitly NOT claimed here** — this slice only builds the harness
*structure* (`oracle_compare.py`'s CLI shape, config plumbing for both vector paths). Actually
running a kernel's outputs through both oracles and passing is each kernel slice's own
responsibility: **p2b/p2d/p2e each claim P2-R14/R14a for their own kernels**; **p2c explicitly
does not** (K1 has no golden vectors — see its README); **p2f** confirms the harness held together
across all of them at the pipeline level. Don't treat p2a as "done" for R14 — it never can be,
by design.

## Explicitly NOT in this slice

Any real kernel (`k*.cl`) — those are p2b (K5/K6), p2c (K1), p2d (K2/K3), p2e (K4). This slice's own
tests use dummy/stub kernels only, to prove the host API and tooling independent of kernel
correctness.
