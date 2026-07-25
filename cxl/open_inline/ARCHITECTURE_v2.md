# Open Inline — Architecture v2

**Status:** Working plan (draft 2026-07-07). **v1 (`ARCHITECTURE.md`, frozen 2026-07-05) is kept as-is
for history; v2 is the active document.**
**Author:** Karthi (Nokia BSP Engineer, Bangalore) · **Target:** Prof. Arkaprava Basu, IISc CSA
**Diagrams:** [`DIAGRAMS.md`](DIAGRAMS.md)

---

## 1. Goal & thesis

**Goal: prove the architecture works** — functionally first, then on hardware. *Not* to produce
performance numbers from emulation.

**Thesis:** An open, vendor-portable, **CXL-backed** GPU FEC for O-RAN. Uplink LLRs are placed in a
**CXL(-emulated) shared region**; a **portable OpenCL/SYCL LDPC decoder** on *any* GPU
(NVIDIA/AMD/Intel) reads them and decodes, plugged in through the open **O-RAN AAL / DPDK-BBDev**
seam. The open-standards alternative to NVIDIA Aerial (CUDA + GPUDirect + NVLink).

**CXL is the crux** — the research question: *can CXL serve as the open CPU↔GPU (and multi-vDU
pooling) memory fabric for GPU RAN FEC?* We answer it in **three tiers of proof** (§3).

**Honest framing:** CXL Type-3 here is a **streaming + poolable** fabric (the GPU DMAs from it),
**not** a zero-copy coherent NVLink substitute (that is not achievable with CXL Type-3 even on real
hardware — CCCL). Its wins are **openness, pooling, disaggregation, capacity** — not raw latency.

---

## 2. Architecture (data path)

```
 Fronthaul ─► CPU (OAI UL PHY): chan-est/eq/demap ─► LLRs
                                    │ write
                                    ▼
                        ┌───────── CXL shared region ─────────┐
                        │ Type-3 → system-ram NUMA            │
                        │ (CXL 3.0: one pool ↔ N vDUs)        │
                        └───────┬───────────────┬─────────────┘
              GPU reads LLR (DMA)│               │ hard bits (write-back)
                                 ▼               ▲
                     GPU (NVIDIA/AMD/Intel): OpenCL/SYCL LDPC decode
                                 ▲
                    plugged in via O-RAN AAL / DPDK-BBDev seam (OAI nrLDPC_coding)
                                 │
                    result ─► CPU: CRC / HARQ / MAC
```

The **CXL region is the CPU↔GPU hand-off**. At CXL 3.0 it becomes a pool one GPU serves for N vDUs
— the pooling advantage NVLink (per-chip) cannot replicate.

---

## 3. The three-tier proof strategy (the spine)

We prove the architecture works in three escalating, independently-valid tiers. Each is honestly
labeled; the performance story lives only in the top tier.

| Tier | Setup | **Proves** | Does NOT prove | Research status |
|---|---|---|---|---|
| **T1 — Functional** | QEMU CXL (Type-3 → system-ram NUMA) + PoCL (OpenCL on CPU) | software architecture correct · **bit-exact LDPC** · **CXL region in the data path** · OAI/BBDev integration | no real GPU · no performance | **valid functional validation** (accepted; the Linux CXL subsystem itself is developed on QEMU) |
| **T2 — GPU mechanism** | Real GPU (GCP NVIDIA; AMD Dev Cloud) + **NUMA-as-CXL** region (no CXL device needed) | **GPU DMAs from the CXL-like region and decodes** · resolves the **AMD/OpenCL** pin/dmabuf unknown · multi-vendor portability | not CXL-specific latency | real-GPU mechanism proof |
| **T3 — CXL hardware** | Real CXL Type-3 module + GPU on one box (IISc / a CXL facility) | **CXL-specific latency + performance** · pooling | — | the strong result (hardware follow-on) |

**Key:** **T1 + T2 together prove the architecture *works*** — software correct *and* the GPU↔CXL
mechanism real, incl. AMD/OpenCL — **without needing a CXL device.** T3 adds the performance story.
"Take it to IISc to show it runs on real CXL" = **T3**.

**The honesty line (non-negotiable):** **no performance number ever comes from T1 (QEMU) or the
NUMA-as-CXL stand-in.** Latency/bandwidth claims come only from **T3** (or a clearly-labeled
CXLMemSim/Mess *model*). Frame T1/T2 as functional + mechanism, never as timing.

---

## 4. What we prove vs defer (honesty ledger)

- ✅ Architecture correctness & data path — **T1**
- ✅ Portable, bit-exact GPU decode across vendors — **T1** (PoCL) + **T2** (NVIDIA/AMD)
- ✅ GPU-DMA-from-CXL-region mechanism incl. AMD/OpenCL — **T2**
- ⏳ CXL-hardware latency / performance / pooling — **T3** (hardware)
- ❌ Zero-copy coherent CPU↔GPU — **not achievable with CXL Type-3** (streaming DMA only). Retire
  this claim; pitch CXL as a **streaming/pooling** fabric.

---

## 5. Build phases (mapped to proof tiers)

- **Phase 0 — de-risking spikes** (before building):
  - **0-A** dmabuf→OpenCL import on a real GPU (T2 enabler). Fail → **HIP fallback** for the datapath.
  - **0-B** batched single-launch slot-decode latency on a real GPU (AdaptiveCpp).
  - **0-C** locate a **CXL+GPU box** for T3 (cloud/vendor/academic facility) — decides when T3 is reachable.
- **Phase 1 — GPU BBDev PMD (the artifact):** self-contained **BSD-3** OpenCL LDPC PMD via OAI
  `nrLDPC_coding` / AAL. Proven at **T1** (QEMU, bit-exact) and **T2** (real GPU). CB-mode, **no
  internal HARQ** first.
- **Phase 2 — CXL data path:** LLR placed in the **CXL region** (T1 QEMU system-ram NUMA) and in
  **NUMA-as-CXL** (T2 real GPU); the GPU reads LLRs from it via the PMD. Proves CXL is in the loop.
- **Phase 3 — hardware & beyond:** **T3** CXL-hardware performance + pooling; then inline full-L1
  (port more open cuPHY kernels → OpenCL/SYCL) and CXL 3.0 multi-vDU pooling.

---

## 6. Component decisions & known-good config

| Layer | Choice | Note |
|---|---|---|
| GPU compute | **OpenCL / SYCL** (portable), not CUDA/HIP as primary | reuse our bit-exact OpenCL LDPC kernel; **pin to OpenCL-1.2-safe features** (NVIDIA lags); **verify per vendor** |
| SYCL runtime | **AdaptiveCpp** (`hip:gfx90a` on MI-class) | **not DPC++** (~7× kernel-launch overhead on AMD) |
| Offload seam | **O-RAN AAL / DPDK-BBDev** (OAI `nrLDPC_coding` `_t2`-style) | NOT eBPF; **do NOT build on `gpudev`** (CUDA-only) |
| PMD | **self-contained BSD-3**; **do NOT link cuPHY** (Apache-2.0, no contributions) | cuPHY = reference oracle only |
| CXL region | Type-3 → **system-ram NUMA** (`cxl create-region` + `daxctl`); NUMA-as-CXL for T2 | **never `/dev/dax` mmap** |
| GPUs | **GCP NVIDIA** (dev + NVIDIA data) · **AMD Dev Cloud / Azure MI300X** (AMD proof) · Intel optional; **IISc optional (T3)** | cloud covers T1+T2; T3 needs CXL+GPU |
| HARQ | start **no internal HARQ**, single-shot | on-card HARQ (ACC100) doesn't map to a lookaside GPU |

---

## 7. Risks & landmines (ranked, from `research/feasibility_research.md`)

1. **HARQ soft-buffer combining (#1)** → Phase-1 ships single-shot, no internal HARQ.
2. **dmabuf→OpenCL interop uneven** (Gate 0-A) → prototype first; HIP fallback.
3. **DPC++ ~7× launch overhead on AMD** → AdaptiveCpp only.
4. **`gpudev` CUDA-only / useless** → self-contained PMD.
5. **cuPHY licensing** (Apache-2.0, "no contributions") → BSD-3 original; never link.
6. **mlx5 NIC-lock** — only relevant if doing real NIC→GPU ingress; **T1/T2/T3 can use CPU-fed LLRs**
   (LLRs already sit in the CXL region), sidestepping the NIC question for the core proof.
7. **QEMU / PoCL = functional only** → **no performance claims** (the honesty line).

---

## 8. Publication

- **Near-term — architecture demonstration:** functionally-validated (T1) + GPU-mechanism (T2) open,
  portable, **CXL-backed** GPU RAN FEC via the O-RAN AAL/BBDev seam; **bit-exact, multi-vendor**;
  performance honestly deferred. A legitimate contribution (the artifact + the proven architecture).
- **Strong follow-on — T3:** CXL-hardware latency + pooling → systems venue (NSDI/ATC/EuroSys/MobiCom).
- **O-RAN WG6 (AAL):** "AAL FEC GPU profile" contribution (Nokia membership).

---

## 9. Frozen / honesty lines (carry forward)

- **CXL = the crux** (research question), framed as a **streaming/pooling** fabric — not zero-copy coherent.
- **No performance number from QEMU / PoCL / NUMA-as-CXL** — ever. Perf only from T3 (or a labeled model).
- **Portable OpenCL/SYCL** compute (reuse our kernel); **AAL/BBDev seam**; **self-contained BSD-3 PMD**.
- **"Open" = open standards + open source + multi-vendor**, not "vendorless."
- **23.4× CPU anchor unchanged** (11,703 µs/slot). Real measurements + real bit-exact oracle only.

---

## 10. References (verified) — full characterization in `research/feasibility_research.md`

Open cuPHY (Apache-2.0): github.com/NVIDIA/aerial-cuda-accelerated-ran ·
CCCL/CXL-CCL arXiv:2602.22457 (GPU↔CXL = streaming DMA, ~20 GB/s/658 ns, NVIDIA/CUDA) ·
Six Times to Spare arXiv:2602.04652 (GPU LDPC 6×, within 6–24% of slot; integrated GB10) ·
DecodeX arXiv:2511.02952 · CXL-DMSim arXiv:2411.02282 (QEMU CXL = functional-only) ·
DPDK BBDev (o-ran-sc/o-du-phy; doc.dpdk.org bbdev) · OAI `nrLDPC_coding` (`_t2`, MR !3344) ·
ROCm ROCmRDMA / `hsa_amd_portable_export_dmabuf` · Khronos `cl_khr_external_memory` · SYCL
AdaptiveCpp · GCP GPUs (NVIDIA-only) · AMD Developer Cloud · Azure ND-MI300X-v5 ·
emucxl arXiv:2404.08311 (NUMA-as-CXL) · Pond arXiv:2203.00241 · 3GPP TS 38.212.

---

## 11. Related documents
- `ARCHITECTURE.md` (v1, FROZEN) — earlier baseline, kept as-is.
- `research/feasibility_research.md` — the study behind the risks/landmines and the tier honesty.
- `DIAGRAMS.md` — per-phase + three-tier PlantUML diagrams.
- Superseded (history): `../Evolved Arch/Final_Arch*.md`, `../E2E_*`, `../EBPF_OFFLOAD_ARCH.md`, `../IMPLEMENTER_PROMPT.md`.
