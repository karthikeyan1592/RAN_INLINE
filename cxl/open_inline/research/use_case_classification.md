# Use-Case Classification — Reuse / Reference / Port / Fresh

**Date:** 2026-07-19 · **Extends:** [`simulator_use_case_matrix.md`](simulator_use_case_matrix.md)
(UC numbering reused) · [`simulator_tool_selection.md`](simulator_tool_selection.md) ·
[`../SRSRAN_LIMITATIONS.md`](../SRSRAN_LIMITATIONS.md)

**Framework (user-defined):** every use case falls into one of four types —
**T1 reuse** an existing open tool as-is · **T2 reference** — no adoptable implementation exists,
but reference material (papers/specs/readable source) does · **T3 port** from an existing
tool/codebase · **T4 fresh** — no end-to-end reference exists anywhere.
T1/T3 are solved by existing open source; T2/T4 needed this pass's paper-level research.

---

## 0. Headline findings of this pass

1. **The OCUDU BSD relicense moves the GPU-L1 kernels from T4/T2 into T3.** Verified directly:
   `gitlab.com/ocudu/ocudu` LICENSE is repo-wide **BSD-3-Clause, no PHY carve-out** — meaning the
   complete, production-tested CPU reference implementations of *exactly our kernel list*
   (`dmrs_pusch_estimator_impl`, `pusch_demodulator_impl` [equalizer+demapper], descrambler,
   rate-dematcher, `pusch_decoder_impl`) are now legally portable into our Apache/BSD kernels.
   Under srsRAN's old AGPLv3 this was impossible. This collapses the largest "fresh implementation"
   risk in the project into an "algorithm port + parallelization" task.
   **⚠ Legal footgun:** our local checkout (`third_party/srsRAN_Project`, tag `release_24_10_1`)
   is the **AGPLv3 distribution**. The BSD grant attaches to the OCUDU distribution. **Port only
   from the OCUDU tree, never from the local srsRAN checkout** — same lineage, different license.
   This makes the re-pin to OCUDU a legal prerequisite for kernel work, not just hygiene.
2. **Both residual T4 items confirmed literature-empty** (searched this pass): no academic papers
   on GPU eCPRI/O-RAN-fronthaul depacketization (only USPTO patents + cuPHY's own implementation),
   and no literature at all on slot-deadline GPU baseband on AMD/Intel (nearest neighbors are
   NVIDIA-only: InterfO-RAN arXiv:2507.23177, Zak-OTFS GPU receiver arXiv:2604.02266). **The
   publishable novelty of this project lives exactly at these two points + the M1 ingest assembly.**

---

## 1. Classification matrix

### T1 — Reuse as-is (existing open tool, no or trivial modification)

| Component | Tool | License | Notes |
|---|---|---|---|
| eCPRI traffic-gen (UC1) | OCUDU `ru_emulator` | BSD-3 | needs the small UL-inject patch → listed again under T3 |
| vDU/vCU L2/L3 (UC2) | OCUDU `gnb` | BSD-3 | unmodified |
| UE emulation (UC3) | OCUDU gNB test mode | BSD-3 | MAC-level, unmodified |
| 5GC (UC4) | Open5GS | AGPLv3 | separate process, no linking — AGPL irrelevant to our code |
| SIM GPU stand-in (UC7) | PoCL / AdaptiveCpp-OMP / Oclgrind | MIT/BSD | per SIM §3.1 ladder |
| CXL functional stand-in (UC9) | QEMU CXL `persistent-memdev` | GPL (tool) | proven in cxl_ran_poc v6–v8; tool-use only |
| Oracle vectors (UC6a) | OCUDU in-repo golden vectors (`tests/unittests/phy`) | BSD-3 | primary oracle |
| Oracle vectors, extended (UC6b) | **Sionna as generator** | Apache-2.0 | arbitrary MCS/SNR/channel scenarios; run offline, outputs are data |
| PHYSICAL day-1 probes | perftest (`--use_cuda_dmabuf`/ROCm forks), l2fwd-nv | BSD/GPL dual; Apache | verification harnesses, run as-is |
| LDPC decode kernel | **our own prior bit-exact OpenCL kernel** | ours | reuse of own prior work |

### T2 — Reference-only (consult, never adopt the runtime)

| Reference | For which component | Why not adoptable | License status |
|---|---|---|---|
| **cuPHY** (aerial-cuda-accelerated-ran, open since Dec 2025) | all GPU kernels, esp. depacketizer + pipeline orchestration | warp-32 intrinsics/PTX/DOCA entanglement (feasibility §6) — technical, not legal, barrier | Apache-2.0, "not accepting contributions" — copying legal but against our clean-thesis rule; **read-only** |
| **Sionna source** | kernel algorithms (chanest/eq/demap/LDPC) | TF-graph + CUDA-only (issue #464), batch-oriented | Apache-2.0, contributions welcome — cleanest reference we have |
| **Aerial cuBB architecture docs** (RU-emu ↔ cuPHYController ↔ TestMAC) | pipeline/orchestration shape, FAPI seam semantics | closed DOCA GPUNetIO coupling; NVIDIA-only | docs public |
| **SCF FAPI 222 spec** | `p4-phy-l2-seam` message semantics | we build custom IPC (packed FAPI commercially gated in srsRAN/OCUDU) | public spec |
| **Rivermax / DOCA GPUNetIO docs** | M1 ingest architecture | proprietary runtimes | docs public |
| Papers — receiver chain baseline: arXiv:2605.26157 (LS-at-DMRS + 2D interp + per-SC MMSE + Gauss-MMSE LLR — matches our MVP chain) | p2 kernels | paper, not code | — |
| Papers — GPU LDPC: IEEE 9336349, arXiv:2602.04652 (Sionna-derived GPU benchmark) | LDPC context/benchmarks | our kernel already exists | — |
| Papers — real-time GPU RAN (NVIDIA-only): InterfO-RAN arXiv:2507.23177; Zak-OTFS arXiv:2604.02266 | slot-deadline orchestration (T4 item) | closest existing work; none on AMD/Intel | — |
| CCCL arXiv:2602.22457, prior CXL findings | Phase-2 CXL handoff | carried from v2 research | — |

### T3 — Port from existing (legal source + bounded adaptation)

| Component | Port source | License | Adaptation scope |
|---|---|---|---|
| **GPU kernels: chan-est, equalizer, demapper, descrambler, rate-dematch** (UC5, the p2 bulk) | **OCUDU `lib/phy/upper/...` CPU implementations** | **BSD-3 (from OCUDU tree ONLY — not local AGPL checkout)** | algorithm-preserving port C++→OpenCL C/SYCL + parallelization (per-RE/per-CB work-items, no warp assumptions); bit-exact/tolerance gates vs the same repo's golden vectors — oracle and source now share lineage, strongest possible validation loop |
| ru_emulator UL-inject patch (p3) | OCUDU `ru_emulator` | BSD-3 | small; upstream-contributable (OCUDU accepts contributions) |
| dmabuf-MR registration probe code (p6 day-1) | perftest source | BSD/GPL dual — use BSD | extract registration/setup patterns |
| CXL region mapping + `e2e_slot_t` polling/status scheme (Phase-2 `handoff_backend`) | **own cxl_ran_poc artifacts** (v6–v8) | ours | direct carry-over |
| nFAPI (only if FAPI-compliance stretch activates) | OAI nFAPI | OAI Public License (Apache-based) | deferred, not in plan |

### T4 — Fresh, no end-to-end reference (== the thesis contributions)

| Component | What exists / what doesn't | Fresh scope |
|---|---|---|
| **M1: raw-eth-QP + dmabuf-MR NIC→VRAM ingest** | every link individually documented (deep-feasibility §1); **assembly has zero public examples**; silicon capability proven only via closed software (Rivermax/Aerial) | `p6-physical-m1-ingest` — probe ladder already specced in deep-feasibility §1.1 |
| **Portable GPU eCPRI depacketizer + RE-grid reassembly** | **zero academic literature** (this pass: patents + cuPHY only); cuPHY's exists but warp/DOCA-entangled — T2 reference at most | wavefront-agnostic kernel, fresh; the O-RAN CUS-plane spec + cuPHY-as-spec are the only guides |
| **Slot-deadline GPU pipeline orchestration on AMD/Intel** | **zero literature on any non-NVIDIA GPU** (this pass); NVIDIA-only neighbors exist | queue/event orchestration, HIP/L0 streams under real-time budget — *the* unpublished territory; Phase-1 claims stay functional+measured (not carrier-grade RT), per existing honesty rule |
| PHY↔L2 seam shm-ring (p4) | standard engineering; FAPI spec = semantic reference; low-risk | fresh but not novel — excluded from "contribution" claims |

---

## 2. What changed vs. prior docs (deltas to carry into specs)

1. `phase1_feasibility_cloud_hw.md` §6 said "re-implement the UL PUSCH slice as fresh portable
   kernels." **Now: port-not-fresh** (T3 via OCUDU BSD), with effort estimate expected to drop —
   revise when `p2-phy-kernels` spec is written. Validation methodology (golden vectors) unchanged
   and actually strengthened (same-lineage oracle).
2. Licensing-hygiene rule updates: (a) OCUDU source = copyable (BSD), but **only from the OCUDU
   distribution**; local `third_party/srsRAN_Project` stays reference-only (AGPL); (b) Sionna =
   readable reference, Apache, contribution-friendly; (c) cuPHY = read-only by our own rule
   (unchanged).
3. Re-pin srsRAN→OCUDU is now a **legal prerequisite** for p2 kernel work (was: hygiene/currency).
4. The three T4 rows are the project's novelty claims — `ARCHITECTURE_v3.md` thesis wording and any
   future paper outline should cite exactly these three (M1 assembly, portable depacketizer,
   non-NVIDIA slot-deadline orchestration), each now backed by a documented literature-absence check
   (this doc + `simulator_use_case_matrix.md` + `physical_deep_feasibility.md`).

## Sources (this pass)

OCUDU repo-wide BSD-3 LICENSE: gitlab.com/ocudu/ocudu/-/raw/main/LICENSE (fetched, no scope
limitation) · receiver-chain baseline paper: arxiv.org/pdf/2605.26157 · GPU-LDPC: ieeexplore
9336349, arxiv 2602.04652 · real-time GPU RAN (NVIDIA-only): arxiv 2507.23177 (InterfO-RAN),
arxiv 2604.02266 (Zak-OTFS) · GPU-fronthaul-depacketization literature search: **negative result**
(USPTO patents + NVIDIA cuPHY/Aerial docs only) · AMD/ROCm slot-deadline literature search:
**negative result** · Aerial PUSCH receiver docs: docs.nvidia.com/aerial (pyAerial receiver
algorithms, Aerial Framework PUSCH lowering tutorial) · prior: `simulator_use_case_matrix.md`,
`physical_deep_feasibility.md`, `phase1_feasibility_cloud_hw.md`.
