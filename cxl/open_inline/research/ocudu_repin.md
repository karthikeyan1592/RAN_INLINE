# OCUDU Re-Pin Verification (srsRAN Project → OCUDU)

**Date:** 2026-07-19 · **Decision executed:** all `open_inline` pins move
`srsRAN Project release_24_10_1` → **`OCUDU release_26_04`** (`gitlab.com/ocudu/ocudu`,
tag verified via `git ls-remote`). Trigger: BSD-3 port-source requirement
([`use_case_classification.md`](use_case_classification.md) §0.1). Method: shallow clone to
`third_party/ocudu` (96 MB, HEAD 2026-07-17) + file-level diff against the srsRAN checkout.

## 1. Verified carry-overs (file-level, not migration-guide hearsay)

| Asset | srsRAN path | OCUDU path | Verdict |
|---|---|---|---|
| `ru_emulator` | `tests/integrationtests/ofh/` | **`apps/examples/ofh/`** (moved — promoted test→example) | ✅ all files incl. **both DPDK and socket transceivers** (socket mode = our SIM-tier fronthaul option) |
| docker compose | `docker/docker-compose.yml` | `docker/docker-compose.yml` + **new** `docker-compose.split.yml` (CU/DU split), `docker-compose.ui.yml`, grafana/metrics | ✅ improved; services incl. `5gc`, `gnb` |
| PUSCH kernel port sources | `lib/phy/upper/channel_processors/pusch/`, `lib/phy/upper/signal_processors/dmrs_pusch_estimator_impl.*` | same, except DMRS estimator moved to `lib/phy/upper/signal_processors/pusch/` | ✅ all present; **per-file SPDX headers `BSD-3-Clause-Open-MPI` directly on the PHY sources** — per-file license confirmation, stronger than repo LICENSE |
| FAPI adaptor | `lib/fapi_adaptor` | `lib/fapi_adaptor` | ✅ |
| Namespace | `namespace srsran` | **`namespace ocudu`**, headers `include/srsran/` → `include/ocudu/` | ⚠ mechanical rename — affects our patches/specs |
| Release | `release_24_10_1` | `release_26_04` (initial OCUDU release; UL MIMO, NRPPa/SRS, Xn+cond-HO, FR2-120kHz, new PRACH formats, RoHC) | pin to `release_26_04` |

## 2. The one regression: golden conformance vectors are GONE from OCUDU

Verified three ways: (a) zero `*_test_data*` / `*_vectortest*` files anywhere in OCUDU's
`tests/unittests/phy` (srsRAN had `pusch_{decoder,demodulator,processor}_test_data.h` +
`*_vectortest.cpp`); (b) unit-test count dropped 65→43 (the vector-driven conformance tests are
exactly what's missing); (c) GitLab group API lists no companion vector/matlab repo
(`ocudu`, `ocudu_docs`, `ocudu_packaging`, `ocudu_test_report`, `ocudu_infra_srs`,
`ocudu_ai_playground` — none carries vectors). srsRAN_matlab remains GitHub/srsran-only.

**Oracle plan adjustment (dual-oracle):**
1. **Bit-exact gates for ported kernels: keep using the srsRAN `release_24_10_1` vectors from the
   local AGPL checkout — as CI fixtures only, never redistributed.** Rationale: AGPL obligations
   attach to distribution/network service, not to private CI use; and these vectors match the
   ported algorithms line-for-line (same lineage), so they remain the strongest bit-exact oracle.
   Do **not** vendor them into any repo we publish.
2. **Redistributable oracle: Sionna-generated vectors** (Apache-2.0 outputs, UC6b in
   [`simulator_use_case_matrix.md`](simulator_use_case_matrix.md)) — elevated from "richer second
   oracle" to "the oracle we can actually ship with the project," used for float-stage tolerance
   gates and algorithm-independent cross-checks (not bit-exactness of ported integer stages, where
   algorithm variants differ).
3. Watch item: vectors may reappear in a later OCUDU release (26.04 is the first cut) — recheck at
   next release before investing in more Sionna generator tooling than p2 needs.

## 3. Code/spec impact executed with this re-pin

- `features/README.md` pin line → OCUDU `release_26_04` + path/namespace notes.
- `p0-rig-scaffold` spec set + `p3-live-tap-ul-inject` SPEC: tag strings updated, OCUDU-rename
  banner added (upstream URL, ru_emulator path move, `srsran`→`ocudu` namespace). p3's patch
  target path changed to `apps/examples/ofh/`.
- `sim/gcp/setup_sim.sh`: clone URL + tag updated (still untested scaffold).
- **Update (2026-07-21):** all remaining features (`p1`, `p2`, `p3` HLD/LLD, `p4`, `p5`, `p6`)
  written after this doc was first drafted correctly carry the OCUDU pin from the start — no
  retrofit needed for those. `p6-physical-m1-ingest` in particular already cites OCUDU/`research/
  physical_deep_feasibility.md` throughout its SPEC/HLD/LLD.
- Local `third_party/srsRAN_Project` checkout **retained deliberately** for: (a) the AGPL golden
  vectors (CI-only oracle), (b) `cxl_ran_poc` reproducibility (its uprobe offsets/benchmarks bind
  to that exact build). It is no longer a port source or a pin target for `open_inline`.
- `third_party/ocudu` (new, shallow) is the **only legal port source** for kernel work.

## 4. Bonus finding

`docker-compose.split.yml` — OCUDU now ships an official CU/DU **split deployment** compose.
Directly useful for the SIM-tier topology (our `gpu-phy` container slots into a rig that already
practices multi-container DU deployment) and for the eventual PHY↔L2 seam work; the p1/p5 spec
agents should evaluate basing the rig on the split compose instead of the monolithic one.
