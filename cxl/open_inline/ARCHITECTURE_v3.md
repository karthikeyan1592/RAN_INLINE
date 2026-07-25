# Open Inline — Architecture v3: Complete L1 on GPU, Phase 1 + Phase 2

**Status:** Working plan (draft 2026-07-07). **Locked structure: Phase 1 (below) is what we build
now; Phase 2 (§11) is locked-in scope for later — not built yet, not elaborated beyond its defined
role.**
v1 (`ARCHITECTURE.md`) and v2 (`ARCHITECTURE_v2.md`) are kept as-is for history; v3 supersedes both.
**Author:** Karthi (Nokia BSP Engineer, Bangalore) · **Target:** Prof. Arkaprava Basu, IISc CSA
**Diagrams:** [`DIAGRAMS.md`](DIAGRAMS.md)

> **Changelog from v2:** CXL is **not retired** — it's **sequenced**. **Phase 1** (the architecture
> below) uses standard pinned-memory DMA for the GPU→CPU decoded-bits handoff — no CXL, no exotic
> hardware, buildable now. **Phase 2** (§11) swaps *that one leg* for a CXL-backed shared region,
> once Phase 1 is working — validating the original research question ("can CXL serve as an open
> CPU↔GPU fabric") on real hardware, against a working baseline. Everything else (NIC→GPU ingest,
> GPU-resident full PHY, split choice, PHY↔L2 seam) is shared by both phases, unchanged.
> The lookaside GPU-BBDev-PMD idea (v1/v2's near-term deliverable) remains **retired** — not a
> stepping stone.

---

## 1. Thesis

> **An open, vendor-portable, complete-L1-on-GPU RAN PHY.** Fronthaul (eCPRI, O-RAN split 7.2x)
> lands **directly in GPU memory** via the open Linux `dma-buf` + `rdma-core` mechanism (no NVIDIA
> proprietary software required, even on Mellanox NIC hardware). The GPU (NVIDIA/AMD/Intel, portable
> **OpenCL/SYCL**) runs the **entire DU-side PHY**: eCPRI depacketize → channel-est → equalize →
> demap → LDPC decode. The open-standards alternative to NVIDIA Aerial/cuPHY + GPUDirect + DOCA.
>
> **Phase 1:** decoded bits cross back to the CPU via a plain pinned-memory copy — prove the whole
> architecture works, cheaply, with no exotic hardware dependency.
> **Phase 2:** that same leg is re-implemented over **CXL**, answering the original "is CXL a viable
> open NVLink-alternative fabric" question on a real, bounded, already-working piece of the system.

**Why Phase 1 first:** the lookaside design (v1/v2) proved a *component* works (portable GPU LDPC).
This design proves the *system-level claim* that matters for AI-RAN: an open GPU can be the inline
PHY, the way Aerial is — without NVIDIA's stack anywhere in the loop. Proving that doesn't need CXL.
**Why CXL still matters (Phase 2):** once Phase 1 works, CXL is introduced against a known-good
baseline — a controlled, honest way to measure it, instead of betting the whole architecture on it.

---
---

# PHASE 1 — Complete L1 on GPU, no CXL

## 2. Architecture (data path)

```
UE (real, or SDR-loopback / synthetic injector)
   │  RF
   ▼
O-RU (real SDR-based, or OAI's software O-RU: FHI library + xran, DPDK-based)
   │  O-RAN split 7.2x: RU does RF + FFT; sends FREQUENCY-DOMAIN resource elements
   ▼
Fronthaul: eCPRI / UDP over Ethernet
   │
   ▼
NIC (Mellanox/ConnectX — mlx5; RoCE)
   │  ibv_reg_dmabuf_mr()  ← OPEN verb (rdma-core), NOT NVIDIA proprietary
   │  NIC DMAs packet payload directly into GPU VRAM (CPU never touches packet data)
   ▼
┌──────────────────────────── GPU (NVIDIA / AMD / Intel) ────────────────────────────┐
│  OpenCL / SYCL, portable kernels — the ENTIRE DU-side PHY:                          │
│   1. eCPRI depacketize + RE-grid reassembly (headers, reorder, symbol placement)    │
│   2. channel estimation                                                             │
│   3. equalization                                                                    │
│   4. soft-demap → LLR                                                                │
│   5. LDPC decode (min-sum, our bit-exact kernel, BG1/BG2 auto-detect)               │
│   6. hard-decision → TB bits                                                         │
│  (RU already did FFT under split 7.2x — GPU does not repeat it; see §3)             │
└───────────────────────────────────┬──────────────────────────────────────────────────┘
                                     │  decoded TB bits (small, not latency-critical)
                                     │  PHASE 1: standard pinned-memory copy (hipMemcpy /
                                     │  clEnqueueReadBuffer) — NO CXL, no exotic HW
                                     │  PHASE 2: same leg, CXL-backed region instead (§11)
                                     ▼
CPU: CRC24 check → PHY↔L2 seam (custom IPC now; FAPI-style later) → MAC/RLC/PDCP → 5GC
```

**Control plane note:** the CPU still posts receive buffers / manages completion queues via open
`rdma-core`/DPDK — that's fine and still fully open software. What's NOT needed is NVIDIA's DOCA
GPUNetIO/GDAKI (GPU autonomously ringing the NIC doorbell) — see §4.

---

## 3. Split choice: 7.2x, not split 8 (be precise about this)

"Complete L1 on GPU" needs a scoping decision, because O-RAN split 7.2x and split 8 disagree about
where FFT happens:

| Split | RU does | GPU/DU does | Fronthaul bandwidth |
|---|---|---|---|
| **7.2x (chosen)** | RF + **FFT** (+ CP removal) | chan-est, eq, demap, LDPC (**"complete L1" = everything the DU owns**) | lower (frequency-domain REs) |
| 8 (stretch/future) | RF only | **FFT** + everything else | much higher (raw time-domain IQ) |

**We target split 7.2x.** "GPU does the complete L1" means *the entire DU-side workload* — this
matches how real Aerial/cuPHY deployments are typically configured (7.2x is the standard, most
widely deployed O-RAN split) and keeps compatibility with commodity/existing O-RU hardware. Split 8
(FFT-on-GPU too) is a valid future extension but multiplies fronthaul bandwidth requirements — out
of scope unless specifically motivated later. OAI's fronthaul stack (§6) supports both.

---

## 4. The open NIC→GPU mechanism (corrected framing from v1/v2)

v1/v2 said "NIC-locked to mlx5" in a way that implied a software lock-in. **Corrected:** mlx5 is a
**hardware/driver-maturity** choice, not a proprietary-software one. Every layer below is open:

| Layer | Openness |
|---|---|
| ConnectX NIC silicon | hardware — purchase it, no NVIDIA software license needed |
| `mlx5_core` / `mlx5_ib` kernel drivers | ✅ open — upstream Linux kernel |
| `DMA-BUF` (kernel framework) | ✅ open — mainline Linux, vendor-neutral |
| `rdma-core` + `libibverbs` + mlx5 userspace provider | ✅ open — github.com/linux-rdma/rdma-core |
| `ibv_reg_dmabuf_mr()` | ✅ open — the verb that does the actual pull |
| DPDK mlx5 PMD | ✅ open — BSD license |
| GPU-side dmabuf export (`hsa_amd_portable_export_dmabuf` / CUDA equivalent) | ✅ open (ROCm) |
| **DOCA GPUNetIO / GDAKI** (GPU *itself* rings the NIC doorbell, no CPU) | ⚠️ historically NVIDIA-only; NVIDIA has since released a **"limited features" open-source cut** (`NVIDIA-DOCA/gpunetio` on GitHub) — **not yet verified**: license terms, whether it's usable with non-NVIDIA GPUs, whether it's needed at all for our design (we don't require GPU-autonomous doorbell-ringing; CPU-driven receive-posting via open rdma-core is sufficient) |

**Net:** the entire *data plane* (packets landing in GPU VRAM) is achievable with zero proprietary
software, on Mellanox hardware. The only historically-closed piece (GDAKI) is a control-plane
optimization we don't need for correctness — CPU-posted receives are fine.

---

## 5. GPU→CPU result handoff — PHASE 1: pinned memory (Phase 2 swaps this leg to CXL, §11)

Decoded TB bits are small (≤1056 bytes/CB) and not latency-critical relative to the LDPC decode
itself. A **standard pinned-memory DMA copy** is the correct, simple, already-solved mechanism for
Phase 1:

```
GPU VRAM (decoded TB) ──clEnqueueReadBuffer/hipMemcpyAsync──► pinned host buffer ──► CPU reads
```

- Allocate a pinned host buffer up front (`clCreateBuffer(CL_MEM_ALLOC_HOST_PTR)` / `hipHostMalloc`).
- One async copy on decode completion; CPU waits on the transfer's **completion event** — this
  synchronization comes free from the GPU API (queue/stream events), no hand-rolled status-flag
  polling needed (contrast: raw CXL memory has no such primitive — see §11).
- **Same process vs. separate L2 process:** default to **same process** for now (the pinned buffer
  is just a pointer, zero extra IPC). If a separate L2 process is later required (e.g. real
  unmodified OAI MAC), use POSIX shared memory or a Unix socket — standard, no new hardware. See §7.

No CXL, no special memory fabric, no NUMA tricks required for Phase 1. This mechanism is fully open
(OpenCL/HIP are open standards/open-source runtimes) — dropping CXL here introduces no new
dependency, proprietary or otherwise.

---

## 6. Hardware — what's real, what's a spike, what's a gap

### 6.1 GPU vendor availability (verified)
| Vendor | Where | Note |
|---|---|---|
| NVIDIA | **GCP** (L4/A100/H100/H200/B200…) | easy, cheap dev/test |
| AMD | **AMD Developer Cloud**, Azure **ND-MI300X-v5**, OCI | GCP has **no AMD** — confirmed, unchanged |
| Intel | Intel Tiber/Dev Cloud (unverified specifics — check before relying on it) | optional 3rd vendor |

### 6.2 NIC (mlx5/RDMA) + GPU pairing — the mechanism enabler
- **GCP A3-Ultra (H200) / A4 (B200):** real **ConnectX-7** NICs shipped with real NVIDIA GPUs.
  Genuine candidate. **But:** built/tuned for GPU-to-GPU NCCL cluster fabric — **unverified**
  whether `ibv_reg_dmabuf_mr` works for an *arbitrary external sender* (our eCPRI traffic pattern)
  rather than intra-cluster peer traffic. Also expensive (8-GPU nodes for a 1-GPU test).
- **Recommended for the mechanism-proof stage: a single cheap bare-metal box** — one ConnectX-5/6
  card (inexpensive secondary market) + any GPU (NVIDIA/AMD) on the same PCIe root complex. This is
  the standard, low-cost way RDMA/GPUDirect mechanism work is actually done — no hyperscale SKU
  needed. Check whether IISc HACC or a Nokia lab box can host a ConnectX card + GPU.
- **AMD + RDMA NIC:** Azure ND-MI300X-v5 ships InfiniBand NICs — same cluster-fabric caveat as GCP.
  A bare-metal AMD-GPU + ConnectX box is likely the more controllable path here too.
- **Freeze-breaker:** if `ibv_reg_dmabuf_mr` → GPU VRAM cannot be made to work for external-sender
  traffic in a bounded spike (≤2 weeks) on the chosen box, fall back to **CPU-staged ingest**
  (NIC→CPU→pinned-buffer→GPU, one extra copy) while keeping the rest of the architecture (GPU-resident
  full PHY) intact — a real but honest degradation, not a blocker to the whole project.

### 6.3 eCPRI live traffic generation — solved at the component level
**Good news, verified:** OAI ships a real, production-grade, **DPDK-based** split-7.2/8 fronthaul
stack (O-RAN-SC **FHI library** + **`xran`**), used in an actual MWC Barcelona over-the-air demo.
This produces genuine eCPRI/UDP traffic over a real NIC — not a toy, not something to build from
scratch, and its DPDK basis dovetails directly with the DPDK mlx5 ingest path in §4.

**What's not yet verified:** whether the O-RU side can run **without real RF hardware** (pure
lab/no-radio testing). `xran`/FHI is the network-side protocol implementation; the RF frontend is a
separate concern. Two options:
- **(a) SDR loopback** — a real or low-cost SDR (or two) providing an actual RF path, UE↔RU over the
  air or cabled loopback. Most realistic; needs hardware.
- **(b) Synthetic eCPRI injector** — inject known frequency-domain REs directly into the U-plane
  stream (bypassing RF entirely), matching the earlier project's "Gate 1: synthetic labeled IQ"
  approach. Cheapest, fully controllable, no RF hardware — **recommended starting point.**

### 6.4 Direct answer to "GCP + Intel/AMD GPU + live UE traffic in one step"
**Not achievable as a single step.** GCP has no AMD/Intel GPU. The realistic combined plan:
1. **Mechanism spike** on a cheap bare-metal NVIDIA-or-AMD box + ConnectX card (§6.2).
2. **PHY correctness** using OAI's real FHI/xran eCPRI generator (§6.3, starting with the synthetic
   injector) feeding the GPU-resident PHY (§2), validated against the existing bit-exact oracle.
3. **Multi-vendor portability** — repeat on GCP (NVIDIA) and an AMD box/cloud (AMD) once the
   mechanism and PHY are proven on one vendor.
4. **Live/full demo** (optional, later) — swap the synthetic injector for an SDR loopback.

---

## 7. PHY↔L2 integration seam (structural correction from v1/v2)

Because GPU now owns the **entire PHY** (not just LDPC), the v1/v2 seam (O-RAN AAL / DPDK-BBDev —
a FEC-*lookaside*-only interface) **no longer applies.** The real seam is a **PHY↔L2 split**
(how Aerial/cuPHY actually talks to OAI's L2, typically FAPI-style).

- **For our functional/mechanism testing:** define a **simple custom IPC** (shared-memory ring or
  socket carrying decoded TB + CRC status from the GPU-PHY process to a CPU-L2 process). Sufficient
  for proving the architecture; avoids the much larger scope of full FAPI compliance.
- **Production-compliance stretch goal:** a real **SCF FAPI** (or OAI's internal equivalent) split,
  only if interop with an unmodified L2/OAI-MAC is later required.
- **Be honest about scope:** this is a materially bigger integration surface than the retired
  BBDev-PMD plan (one op vs. a full PHY interface). Budget for it accordingly.

---

## 8. What Phase 1 proves vs. defers (honesty ledger)

- ✅ Open NIC→GPU data-plane mechanism (`dma-buf`/`ibv_reg_dmabuf_mr`, no proprietary software)
- ✅ Portable, bit-exact GPU LDPC (reused from prior work) inside a GPU-resident full PHY
- ✅ Standard GPU→CPU result handoff (no CXL needed for Phase 1)
- ⏳ External-sender dmabuf ingest at scale (GCP A3-Ultra/A4 or bare-metal) — **needs the §6.2 spike**
- ⏳ eCPRI live traffic without RF hardware — **needs the §6.3(b) synthetic injector, or an SDR**
- ⏳ GPU-autonomous NIC control (GDAKI-equivalent) — **not required**; CPU-posted receive is sufficient
- ➡️ CXL — **not in Phase 1; locked in as Phase 2 scope** (§11), not retired

---

## 9. Risks & landmines (Phase 1; carried forward + new)

1. **External-sender dmabuf ingest unverified** on any specific box/cloud (§6.2) — the top open risk.
2. **PHY↔L2 seam is now a bigger build** than the retired BBDev PMD (§7).
3. **eCPRI-without-RF needs the synthetic injector built** (§6.3) — not yet confirmed to exist off-the-shelf.
4. **HARQ soft-buffer state** — now lives inside the GPU-resident PHY process; still needs a
   correct combining strategy across retransmissions. Start single-shot (no HARQ) as before.
5. **dmabuf→OpenCL/SYCL import unevenness** across GPU vendors (`cl_khr_external_memory` support) —
   prototype early, per vendor.
6. **AdaptiveCpp, not DPC++**, on AMD (∼7× kernel-launch overhead on DPC++'s AMD backend).
7. **Licensing:** keep all original code BSD/Apache as appropriate; do not link cuPHY (Apache-2.0,
   "not accepting contributions") — reference oracle only.
8. **NVIDIA-DOCA/gpunetio's new open release** — not yet characterized; verify before assuming it's
   relevant (we may not need it at all per §4).

---

## 10. Milestones — Phase 1

| M | Deliverable | Depends on |
|---|---|---|
| 1 | Mechanism spike: synthetic packets → NIC → dmabuf → GPU VRAM, verify correct bytes land | §6.2 box |
| 2 | GPU-resident PHY kernels (eCPRI depacketize → chan-est → eq → demap → LDPC), tested standalone against synthetic RE grids | reuse existing LDPC kernel |
| 3 | Wire M1+M2: synthetic eCPRI injector → NIC → GPU → decoded TB, bit-exact vs ground truth | §6.3(b) |
| 4 | Simple PHY↔L2 custom IPC → CRC/MAC stub, end-to-end slot processing | §7 |
| 5 | Multi-vendor repeat (NVIDIA + AMD) | §6.1, §6.2 on 2nd vendor |
| 6 (stretch) | Real OAI FHI/xran O-RU (still synthetic-fed or SDR), live demo | §6.3(a) |

**Phase 1 exit criterion (gates Phase 2):** M1–M4 pass, bit-exact, on at least one vendor. Phase 2
does not start until Phase 1 has a working baseline to measure against.

---
---

# PHASE 2 — CXL for the GPU→CPU handoff (locked scope, not yet built)

## 11. Scope (locked)

**What changes:** the *one* leg identified in §5 — the GPU→CPU decoded-bits handoff — moves from
Phase 1's pinned-memory copy to a **CXL-backed shared region**. Nothing else changes: NIC→GPU
ingest (§4), the GPU-resident full PHY (§2), the split choice (§3), and the PHY↔L2 seam (§7) are
**identical in Phase 2** to Phase 1.

```
Phase 1:  GPU VRAM ──pinned-memory DMA copy──► CPU RAM
Phase 2:  GPU VRAM ──DMA──► CXL shared region ◄──CPU reads (same region, no copy into a 2nd buffer)
```

**Why this leg, specifically:** it's the smallest, most isolated, already-working piece of the
system — swapping it lets Phase 2 test *only* the CXL question, against a known-good Phase-1
baseline, instead of betting the whole architecture's first working version on unproven fabric.

**What Phase 2 is for (the research question, restated honestly):** *does an open CXL Type-3 region
work as the CPU↔GPU shared-memory fabric for this handoff, and how does it compare — functionally
and in latency — to the Phase-1 pinned-memory baseline?* This is the same question the earlier v2
work (three-tier T1/T2/T3 proof, `research/feasibility_research.md`) investigated; that analysis
carries forward unchanged and applies directly once Phase 2 starts:

- CXL Type-3 is a **streaming DMA fabric, not zero-copy coherent** (CCCL: ~20 GB/s, ~658 ns vs
  214 ns local DRAM, on NVIDIA/CUDA — **AMD/OpenCL path unproven**, a Phase-2 spike item).
- No performance claim from QEMU/emulated CXL — only from real CXL hardware.
- The synchronization primitive Phase 1 gets for free (GPU-API completion events, §5) does **not**
  exist for raw CXL memory — Phase 2 will need to reintroduce an explicit status-flag/polling scheme
  (e.g. the earlier `e2e_slot_t`-style atomic status field) for the CPU to know when the GPU's write
  is visible. This is real added complexity Phase 2 accepts in exchange for testing CXL itself.

**What Phase 2 is explicitly NOT (yet):** not multi-GPU pooling, not a NIC-ingest change, not a
PHY↔L2 seam change. Those remain future/deferred beyond Phase 2 unless separately scoped.

**Hardware for Phase 2:** needs a real CXL Type-3 + GPU pairing (or QEMU-CXL for functional-only
proof first, per the three-tier method) — not yet located; this is Phase 2's own hardware question,
separate from Phase 1's dmabuf/ConnectX search in §6.2.

**Status:** scope is locked as above. Detailed milestones, hardware search, and implementation are
deferred until Phase 1's exit criterion (§10) is met — intentionally not elaborated further here.

---

## 12. References

Open cuPHY (Apache-2.0, reference only): github.com/NVIDIA/aerial-cuda-accelerated-ran ·
`rdma-core` (open, mlx5 provider): github.com/linux-rdma/rdma-core ·
`ibv_reg_dmabuf_mr` / DMA-BUF GPUDirect-RDMA-without-DOCA: NVIDIA GPU Operator RDMA docs ·
**NVIDIA-DOCA/gpunetio** (new, open-source limited GDAKI — unverified relevance): github.com/NVIDIA-DOCA/gpunetio ·
OAI **O-RAN FHI7.2 tutorial** (real eCPRI, DPDK/xran-based, MWC-demoed): `openairinterface5g/doc/ORAN_FHI7.2_Tutorial.md` ·
ProtO-RU (open-source software O-RU, split-7.2, SDR-based): ResearchGate ·
GCP A3-Ultra/A4 GPUDirect-RDMA (ConnectX-7) docs: docs.cloud.google.com/compute/docs/gpus/gpudirect ·
AMD Developer Cloud · Azure ND-MI300X-v5 · ROCm `hsa_amd_portable_export_dmabuf` ·
Khronos `cl_khr_external_memory` · AdaptiveCpp · 3GPP TS 38.212.
**Phase 2 (CXL):** CCCL/CXL-CCL arXiv:2602.22457 (GPU↔CXL streaming DMA, ~20 GB/s/658 ns, CUDA-only —
AMD unproven) · CXL-DMSim arXiv:2411.02282 (QEMU CXL functional-only, no perf claims) ·
emucxl arXiv:2404.08311 · Pond arXiv:2203.00241.

---

## 13. Related documents
- **`ARCHITECTURE_v3_SIM.md`** — T1 realization of Phase 1 (functional proof on GCP KVM, PoCL GPU
  stand-in, veth fronthaul); **`ARCHITECTURE_v3_PHYSICAL.md`** — T3 realization (cloud bare metal,
  M1 mechanism + performance). Both specialize this doc; the backend contract lives in SIM §3.
- `research/phase1_feasibility_cloud_hw.md` — Phase-1 feasibility study + cloud hardware search
  (2026-07-16/17): srsRAN ru_emulator solves §6.3, NIC-vendor fork, hardware ladder, build-vs-port.
- `ARCHITECTURE.md` (v1) and `ARCHITECTURE_v2.md` (v2) — earlier baselines, kept as history.
  v2's three-tier (T1/T2/T3) proof method and CXL honesty framing carry forward into Phase 2 (§11).
- `research/feasibility_research.md` — prior feasibility study (HARQ, licensing, dmabuf-unevenness
  risks carried into §9; CXL findings carried into §11).
- `DIAGRAMS.md` — v3 diagrams (Phase 1: data path, split choice, hardware staging, PHY↔L2 seam;
  Phase 2: the CXL handoff swap).
