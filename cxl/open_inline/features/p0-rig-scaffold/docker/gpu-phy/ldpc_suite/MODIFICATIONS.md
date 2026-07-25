# Vendored LDPC suite — provenance and modification ledger

Source: `cxl_ran_poc/gpu_daemon/ldpc_cl/` (prior project work, this repo). Checksums of the
byte-for-byte originals: `PRIOR_WORK.sha256`.

| File | Status | Notes |
|---|---|---|
| `bg_tables.h` | **REGENERATED 2026-07-23** | No longer the vendored copy. See "bg_tables.h provenance correction" below — this file's own original header comment ("extracted verbatim from srsRAN_Project") contradicted this ledger's original "our own... no external dependency" claim. Resolved by regenerating the tables from OCUDU's real, public, BSD-3 API (`ldpc_graph_impl`) instead of trusting either claim. Bit-exactness re-verified: 0/4,096,000 mismatches, identical to the pre-regeneration baseline. |
| `ldpc_decode.cl` | **Unmodified** | Byte-for-byte vendored copy. The bit-exact kernel under test (P0-R6). Our own OpenCL, no srsRAN/OCUDU dependency. |
| `bit_diff_test.cpp` | **Mechanically ported** | Original preserved as `bit_diff_test.cpp.orig`. Change scope: (1) `#include "srsran/..."` → `#include "ocudu/..."` (+ `srsvec`→`ocuduvec`, `srsran_test.h`→`ocudu_test.h` renames) and `using namespace srsran[::ldpc]` → `using namespace ocudu[::ldpc]`; (2) `ldpc_encoder::encode()`'s 2nd-argument type changed upstream from the shared `codeblock_metadata::tb_common_metadata` to a dedicated `ldpc_encoder::configuration` struct (verified: same two fields we set, `base_graph`/`lifting_size`; `Nref` defaults to 0/unlimited, matching the original's unset behavior) — a real OCUDU API-surface change, tracked here, not a test-logic change; (3) `#define RESULTS_CSV` wrapped in `#ifndef` so the build-time `-DRESULTS_CSV=...` override actually takes effect (plain `-D` cannot win against a later unconditional `#define` in the same translation unit — a preprocessing mechanics fix, not a behavior change). **Zero test-case/algorithm/assertion change.** Verified: `diff bit_diff_test.cpp.orig bit_diff_test.cpp` — every hunk is one of the three items above. |
| | | **Build/link verified natively** (2026-07-21, before Dockerization): configured OCUDU with `-DBUILD_TESTING=OFF -DENABLE_{UHD,SIDEKIQ,ZEROMQ,FFTW,MKL,FFTZ,ARMPL,DPDK,LIBNUMA}=OFF`, built target `ocudu_channel_coding` (pulls in `ocudu_ldpc`/`ocudu_polar`/`ocudu_short_block`/`ocudu_crc_calculator`/`log_likelihood_ratio`/`ocuduvec`/`ocudu_support`/`ocudulog`/`fmt` transitively — all static, all BSD-3), linked `bit_diff_test.cpp` against them + `libOpenCL`. **Result: BG1/BG2 × LS=384/LS=256, 0 mismatches** on the host's native PoCL (`Portable Computing Language` / `cpu-haswell`), confirming the port preserves bit-exactness — see `helpers/build_images.sh` / Dockerfile for the containerized equivalent. |

## Why the harness was ported and the kernel wasn't

`bit_diff_test.cpp` is test-driver code: it uses an LDPC *encoder* (from srsRAN/OCUDU) purely to
generate ground-truth messages, then checks our own OpenCL *decoder* reproduces them exactly. The
encoder dependency is incidental to what's being proven (our kernel's bit-exactness), so re-pinning
its include paths to track the project's OCUDU re-pin (`research/ocudu_repin.md`) is exactly the
same mechanical, non-semantic change already applied throughout this project's specs — not a
modification to the thing P0-R6 requires stay unmodified (the kernel and its behavior).

Linking against OCUDU's `ocudu_channel_coding` library (BSD-3-Clause-Open-MPI) instead of the old
srsRAN Project's AGPLv3 build also means this image carries no copyleft obligation — a strictly
better outcome than porting for its own sake.

## bg_tables.h provenance correction (2026-07-23)

**The discrepancy.** This file's own top-of-file comment said its BG1/BG2 shift-matrix values were
"extracted verbatim from srsRAN_Project lib/phy/upper/channel_coding/ldpc/ldpc_luts_impl.cpp,"
while this ledger (the document the project's whole AGPL-hygiene system —
`helpers/agpl_denylist.py`, every kernel's `provenance.json`, P2-R13 — treats as authoritative)
called the same file "our own... no external dependency." Discovered while reusing this file in
p2f-integration's LDPC hookup, i.e. right as its exposure was about to extend into a second
feature.

**Why this wasn't resolved by picking the more convenient reading.** The underlying shift-matrix
*values* are almost certainly not srsRAN's copyrightable expression — they're TS 38.212 Table
5.3.2-2/5.3.2-3's own standard-mandated numbers, which every conformant LDPC implementation must
reproduce identically to interoperate. But this project built `agpl_denylist.py`/
`provenance.json`/the dual-oracle CI-only rule specifically so no single contested case has to be
settled by a confident-but-unverified argument under time pressure. Accepting "no external
dependency" on that reasoning here would have made this provenance system's first real contested
case exactly the kind of judgment call it exists to avoid making informally.

**Resolution: regenerate, not argue.** `bg_tables.h` was regenerated from OCUDU's real, public
BSD-3 API (`ldpc_graph_impl`, `get_lifting_index`) instead of from srsRAN's file or from a
hand-transcription of the 3GPP spec tables (the latter was considered and rejected too — a
51-lifting-size x 2-base-graph table transcribed from memory is exactly the kind of unverified
guess this project's discipline avoids). Method: for each of the 8 standard lifting-size sets
(TS 38.212 Table 5.3.2-1), `ldpc_graph_impl(bg, max_Zc_in_set)` was constructed via OCUDU's real
API and queried via `get_lifted_node(m,n)` for every check-node/variable-node pair. TS 38.212's own
table construction guarantees each set's raw shift values are already `< max_Zc` for that set, so
this recovers the exact raw (un-reduced) table value losslessly — `ldpc_decode.cl`'s own runtime
`(r+shift) % Z` reduction is unchanged and still correct, since these are the same *raw* values
that step always expected. This makes the srsRAN-provenance question moot by construction, not by
argument: srsRAN's file was never consulted to produce the new table.

**Verification.** The regenerated table was built into an isolated test copy of
`bit_diff_test.cpp`/`ldpc_decode.cl` and run at the same 200-message scale as the original
baseline: **0/4,096,000 bit mismatches**, identical to the pre-regeneration result recorded above.
`PRIOR_WORK.sha256`'s `bg_tables.h` entry is retained for historical reference (the checksum of the
file *before* this regeneration) but no longer describes the file currently in this directory.

**Follow-up required, not yet done as part of this correction**: `oi/gpu-phy:dev` (the Docker image
built and run earlier this project) has the pre-regeneration file baked in. That image needs a
rebuild for the shipped artifact to reflect this fix — tracked in `STATUS.md`.
