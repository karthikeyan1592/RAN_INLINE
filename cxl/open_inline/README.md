# open_inline — Complete-L1-on-GPU RAN PHY (open, vendor-portable; CXL sequenced into Phase 2)

Research base.
- **[`ARCHITECTURE_v3.md`](ARCHITECTURE_v3.md)** — **the working plan** (2026-07-07). Phase 1 (build
  now, no CXL) + Phase 2 (locked scope: CXL for the GPU→CPU leg, not yet built).
- **[`DIAGRAMS.md`](DIAGRAMS.md)** — v3 PlantUML diagrams (rendered SVGs embedded; sources in `docs/`).
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — v1, 🔒 FROZEN baseline (2026-07-05), history only.
- **[`ARCHITECTURE_v2.md`](ARCHITECTURE_v2.md)** — v2, CXL-crux/three-tier draft (2026-07-07), history only.
- **[`research/feasibility_research.md`](research/feasibility_research.md)** — feasibility study (risks carried into v3 §9; CXL findings carried into §11).

## One line
Fronthaul (eCPRI, O-RAN split 7.2x) lands **directly in GPU memory** via the open Linux
`dma-buf`/`rdma-core` mechanism (no NVIDIA proprietary software, even on Mellanox NIC hardware).
The GPU (NVIDIA/AMD/Intel, portable **OpenCL/SYCL**) runs the **entire DU-side PHY**: eCPRI
depacketize → chan-est → equalize → demap → LDPC decode. Open alternative to NVIDIA Aerial/cuPHY +
GPUDirect + DOCA.

**Phase 1:** decoded bits cross back to the CPU via a plain pinned-memory copy — prove the whole
architecture works, no exotic hardware. **Phase 2 (locked scope, not built yet):** that one leg —
and *only* that leg — moves to a **CXL-backed shared region**, validating "CXL as an open NVLink
alternative" against a working Phase-1 baseline. See ARCHITECTURE_v3 §11.

## What changed from v2 (see ARCHITECTURE_v3 changelog)
**CXL is not retired — it's sequenced.** Phase 1 is exactly what was "v3 no-CXL" before. Phase 2
reintroduces CXL, scoped to one leg only (GPU→CPU handoff), once Phase 1 has a working baseline to
measure against. The former "Phase 3" (complete-L1-on-GPU) is promoted to Phase 1 of this doc — the
lookaside GPU-BBDev-PMD idea remains retired, not a stepping stone.

## Key corrected facts (v3)
- **mlx5 (Mellanox) is a hardware/driver-maturity choice, not a proprietary-software lock-in** — the
  entire NIC→GPU data plane (`mlx5_core`, `DMA-BUF`, `rdma-core`, `ibv_reg_dmabuf_mr`, DPDK mlx5 PMD)
  is open source. Only GPU-autonomous doorbell-ringing (GDAKI) was NVIDIA-only — and we don't need it.
- **Split 7.2x, not split 8** — RU keeps FFT; "complete L1 on GPU" = everything the DU owns
  (chan-est/eq/demap/LDPC). Matches real Aerial deployments; split 8 is a future stretch.
- **eCPRI live traffic is a solved component** — OAI's real DPDK-based FHI/xran fronthaul stack
  (MWC-demoed) generates genuine eCPRI traffic; a synthetic injector (no RF hardware) is the
  recommended starting point over needing an SDR immediately.
- **GCP is NVIDIA-only** (unchanged fact) — AMD needs AMD Developer Cloud / Azure MI300X / OCI.
  GCP A3-Ultra/A4 do have real ConnectX-7 NICs, but tuned for GPU-cluster fabric — **unverified**
  for our external-sender traffic pattern. A cheap bare-metal box (1 GPU + 1 ConnectX card) is the
  recommended mechanism-proof environment.
- **PHY↔L2 seam changed** — AAL/DPDK-BBDev (FEC-lookaside-only) no longer applies since GPU owns the
  whole PHY. New seam: simple custom IPC now, FAPI-style later if production interop is needed.

## Hardware staging plan (see ARCHITECTURE_v3 §6, DIAGRAMS §4)
1. **Mechanism spike** — bare-metal box, dmabuf NIC→GPU ingest proof.
2. **PHY correctness** — synthetic eCPRI injector → GPU full PHY → bit-exact vs oracle.
3. **Multi-vendor portability** — repeat on GCP (NVIDIA) + AMD box/cloud.
4. **Live demo (optional)** — swap injector for SDR loopback / real UE.

"GCP + AMD/Intel + live UE traffic in one step" is **not achievable** — GCP has no AMD/Intel GPU.
The staged plan above is the honest path.

## Phase 2 (locked scope, deferred — see ARCHITECTURE_v3 §11)
Once Phase 1 hits its exit criterion (M1–M4 bit-exact on one vendor), the GPU→CPU handoff swaps from
pinned memory to a CXL Type-3 shared region — nothing else in the architecture changes. This answers
the original research question honestly, against a known-good baseline, instead of betting the whole
system's first working version on unproven fabric. Carries forward v2's three-tier (T1/T2/T3) honesty
method: no performance claims from QEMU/emulated CXL, real numbers only from real CXL hardware.

## Layout
```
open_inline/
  ARCHITECTURE_v3.md   ← working plan (read first)
  ARCHITECTURE_v2.md   ← v2 (history)
  ARCHITECTURE.md      ← v1 frozen (history)
  DIAGRAMS.md          ← v3 diagrams (+ docs/diagrams/*.svg,*.png, docs/puml/*.puml)
  research/            ← feasibility study
  src/ bench/ scripts/ ← code · results · build/render harnesses
```
