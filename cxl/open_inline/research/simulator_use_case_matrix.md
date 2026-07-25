# Simulator Selection — Use-Case Matrix + GPU-L1-Open Gap Deep Dive

**Date:** 2026-07-19 · **Supersedes nothing, extends** [`simulator_tool_selection.md`](simulator_tool_selection.md)
(RAN-stack tool choice, unchanged: srsRAN/OCUDU) **and** [`../SRSRAN_LIMITATIONS.md`](../SRSRAN_LIMITATIONS.md).
**Method requested:** enumerate every distinct simulator use case first, pick the best tool per use
case (not force one tool where it doesn't fit), then chase the one open lead that could change the
GPU-L1 gap finding — NVIDIA Sionna.

---

## 1. Use-case list (every place something stands in for real hardware/RF/data)

| # | Use case | What "simulator" means here |
|---|---|---|
| UC1 | eCPRI/O-RU traffic generation (RF-free, split 7.2x) | synthetic C/U-plane frames on a real or virtual NIC |
| UC2 | vDU/vCU protocol stack (MAC/RLC/PDCP/SDAP/RRC/NGAP) | real L2/L3 processing, standards-compliant |
| UC3 | UE emulation (control-plane attach, no real PHY) | MAC-level emulated UEs |
| UC4 | Core network (5GC: AMF/SMF/UPF) | already decided, not re-litigated |
| UC5 | **GPU L1 PHY execution** (depacketize→chanest→eq→demap→LDPC), open, vendor-portable | **the empty intersection — this doc's main focus** |
| UC6 | PHY correctness oracle / golden vectors | ground truth to validate our GPU kernels |
| UC7 | GPU compute backend stand-in (no real silicon, SIM tier) | OpenCL-on-CPU execution of our own kernels |
| UC8 | NIC ingest mechanism stand-in (SIM tier) | af_packet/socket RX before real dmabuf exists |
| UC9 | CXL fabric stand-in (Phase 2 handoff leg, functional-only) | shared-region semantics before real CXL hardware |
| UC10 | CXL *performance-sensitivity* characterization (post-Phase-2, stretch) | latency/topology/coherency modeling across configs |
| UC11 | PHY↔L2 seam testing | our own custom IPC — not an external-tool use case |

## 2. Best tool per use case

| # | Use case | Best tool | Status |
|---|---|---|---|
| UC1 | eCPRI traffic-gen | **srsRAN/OCUDU `ru_emulator`** | settled (`simulator_tool_selection.md` §2.1; OAI confirmed to have no equivalent) |
| UC2 | vDU/vCU | **srsRAN/OCUDU `gnb`** | settled |
| UC3 | UE emulation | **srsRAN/OCUDU gNB test mode** | settled |
| UC4 | Core network | **Open5GS** | settled, unchanged |
| UC5 | GPU L1 execution | **none — ours to build** (confirmed again this pass, §3) | thesis gap, unchanged in kind, changed in *quality of reference material* (§3) |
| UC6 | PHY oracle/golden vectors | **srsRAN/OCUDU in-repo golden vectors** (primary) **+ Sionna as a second, richer, algorithmically-independent oracle** (new this pass) | upgraded |
| UC7 | GPU compute stand-in | **PoCL + AdaptiveCpp-OMP + Oclgrind** | settled (`ARCHITECTURE_v3_SIM.md` §3.1) |
| UC8 | NIC ingest stand-in | **af_packet/socket RX, our own `ingest_backend` impl** | settled, not a tool-selection question |
| UC9 | CXL fabric stand-in (functional) | **QEMU CXL `persistent-memdev`** (already proven in `cxl_ran_poc`, v6–v8 runs) | settled, carried over from prior work |
| UC10 | CXL sensitivity characterization | **CXLMemSim** (already in `third_party/`, unused by `open_inline` until now — §4) | newly connected, stretch-goal only |
| UC11 | PHY↔L2 seam | **custom IPC** (feature `p4-phy-l2-seam`) | settled — real packed-FAPI is commercially gated in srsRAN/OCUDU (per `SRSRAN_LIMITATIONS.md`) |

---

## 3. The Sionna question — can we port "full L1 on GPU" and make it open?

### 3.1 What Sionna actually is (verified)

- **License: genuinely Apache-2.0, contributions welcomed** (github.com/NVlabs/sionna) — a real,
  material difference from cuPHY ("Apache-2.0... not accepting contributions"). This makes Sionna's
  source legally the *cleanest* reference we've found, better than cuPHY for that purpose.
- **PHY completeness:** yes — full 5G-NR-compliant chain including 5G LDPC, Polar, convolutional
  codes, multiple decoders, QAM/custom modulation, 3GPP 38.901 channel models. Used *with* NVIDIA
  cuPHY specifically to generate "fully 5G NR compliant PUSCH/PDSCH datasets" — i.e. Sionna's own
  authors treat it as PHY-complete enough to validate cuPHY against.
- **Execution model: TensorFlow-graph, batch-oriented link-level simulation** — built for
  research/training-data generation (run N waveforms through a channel model, differentiate through
  it, evaluate BER/BLER curves), not for receiving one real packet at a time under a slot deadline.
  This is a different job than our `ingest_backend`→GPU-PHY pipeline needs to do.

### 3.2 Portability — the disqualifying finding

**Confirmed via `NVlabs/sionna` issue #464 ("Support of non-CUDA GPU"): as of Sionna 0.14+ (Mitsuba
ray-tracing dependency), Sionna cannot run on AMD GPUs or TensorFlow-Metal — CUDA-only, full stop.**
TensorFlow itself has a ROCm build, but Sionna doesn't target it. Porting Sionna's actual runtime
into our pipeline would trade one vendor lock (would-be cuPHY/CUDA) for a different one
(TensorFlow/CUDA) — it does not get us closer to the vendor-portable thesis; it moves the same
problem sideways.

### 3.3 Sionna Research Kit (`sionna-rk`) — a second Aerial-shaped reference, not a shortcut

Checked directly: `sionna-rk` is **real-time**, not batch — "software-defined 5G RAN and core
network for end-to-end experimentation running in real-time," supporting "simulated, cabled, or
over-the-air" operation. But: **built on OpenAirInterface** (not srsRAN/OCUDU) and **explicitly
"powered by the NVIDIA DGX Spark"** — i.e. hardware-specific, NVIDIA-only, same shape as Aerial+OAI.
This is a *second* independent confirmation (Aerial being the first) that "open, real-time, GPU
L1, vendor-portable" doesn't exist anywhere yet, including from the team that would most want to
have already built it. Treat as reference-only, same rule as Aerial: never link/copy, read for
design ideas.

### 3.4 The one genuinely portable sub-path: neural_rx via ONNX (unchanged from prior finding, now better-evidenced)

`NVlabs/neural_rx` (the joint chanest+eq+demap neural replacement, Sionna-trained) exports to ONNX
and is deployed via TensorRT for sub-ms inference inside live OAI PHY processing — confirming a
*real*, live, slot-deadline-capable deployment exists, just on NVIDIA's runtime (TensorRT). ONNX
itself is portable (execution providers exist for other vendors — ROCm EP, MIGraphX), but an
AMD/Intel-path deployment of `neural_rx` at slot-deadline latency is **not evidenced anywhere** —
this remains exactly what memory already flagged: a **Phase-3, research-grade candidate**, not
something to build Phase-1 GPU L1 on top of. No change to that earlier conclusion; now grounded in
a directly-verified live-deployment precedent (previously only inferred).

### 3.5 Verdict on Sionna

**Don't port the runtime. Do use it as a second oracle, and as reference source for kernel
algorithms — legally cleaner than cuPHY.** Concretely:
- **UC6 upgrade:** generate additional validation waveforms (arbitrary MCS/channel/SNR combos, richer
  than srsRAN's fixed golden-vector set) with Sionna, run them through our own portable kernels,
  compare. Same bit-exact/tolerance rules as the srsRAN oracle (`p2-phy-kernels` spec, once written).
- **Algorithm reference, not copied code:** when writing our own OpenCL/SYCL chan-est/equalizer/
  demapper/LDPC kernels, Sionna's Apache-2.0, contribution-welcoming Python/TF source is safe and
  legible to consult as a spec — same "build fresh, don't port" rule already applied to cuPHY
  (`phase1_feasibility_cloud_hw.md` §6), just with a better-licensed reference available now.
- **Not a shortcut to "done."** The actual kernel-writing work (feature `p2-phy-kernels`) is
  unchanged in scope; Sionna makes the reference material better, not the workload smaller.

---

## 4. CXLMemSim — checked against UC9/UC10, not a substitute for existing Phase-2 plan

Local checkout inspected (`third_party/CXLMemSim`, BSD-3-Clause, UC Santa Cruz Sluglab):

- **What it is:** a CXL memory-system *characterization* simulator — latency, bandwidth, topology,
  coherency/directory modeling — plus a QEMU-integrated CXL Type-2/Type-3 device-model stack.
  `qemu_integration/guest_libcuda/` is a **CUDA Driver API shim** used to generate GPU-shaped memory
  *access patterns* (bias/HITM/p2p/alloc-policy research questions — `rq1_graph_bfs.c`,
  `rq4_devfrac_sweep.c`, etc.) for studying CXL coherency policy under GPU-like workloads. **It does
  not execute real CUDA/HIP/OpenCL kernels through a simulated CXL path** — the shim intercepts and
  models, it doesn't run our GPU-PHY code.
- **Relation to what Phase 2 actually needs (UC9):** functional proof of a real GPU-PHY process
  writing decoded TB bits into a CXL-backed region — already solved, already proven, by QEMU CXL
  `persistent-memdev` in `cxl_ran_poc` (v6–v8 runs). CXLMemSim is a different tool for a different
  job; it doesn't replace that.
- **Where it *does* fit:** `cxl_ran_poc/GAPS.md` gap 3 already names "NUMA sweep + CXLMemSim/Mess
  sensitivity" as **not started** — that's the correct home for CXLMemSim, as a **post-Phase-2
  stretch goal** (UC10: characterizing latency/coherency sensitivity across configurations), not a
  Phase-2 functional dependency. Newly connected here; wasn't previously cross-referenced from
  `open_inline`.

---

## 5. Gap ledger (updated)

| Gap | Before this pass | After this pass |
|---|---|---|
| Open, vendor-portable, GPU-resident L1 | Doesn't exist; ours to build | **Unchanged — still doesn't exist**, including checked against Sionna/sionna-rk specifically. Two independent confirmations now (Aerial, Sionna-RK) that even NVIDIA's own research tooling hasn't done this in a vendor-neutral way. |
| Quality of oracle/reference material for building it | srsRAN golden vectors + cuPHY (read-only, contribution-hostile license) | **Better:** srsRAN golden vectors (primary, unchanged) + Sionna (Apache-2.0, contribution-welcoming, richer scenario coverage) as a second oracle and cleaner algorithm reference |
| AI-native receiver portability (chanest+eq+demap via neural_rx) | Inferred untested on AMD/Intel | Same conclusion, now backed by a confirmed real NVIDIA-only live deployment (TensorRT) to contrast against — Phase-3 candidate, unchanged priority |
| CXL Phase-2 functional proof | Solved via QEMU CXL persistent-memdev (`cxl_ran_poc`) | Unchanged — confirmed CXLMemSim is not an alternative path to this |
| CXL sensitivity/characterization tooling | Not connected to `open_inline` | Now explicitly mapped: CXLMemSim, as a stretch goal, cross-referenced with `cxl_ran_poc/GAPS.md` gap 3 |

## 6. Action items

1. No change to the RAN-stack tool decision (srsRAN/OCUDU) or the GPU-kernel build plan
   (`p2-phy-kernels`) — this research strengthens the reference material, doesn't shrink the work.
2. When `p2-phy-kernels` spec is (re)written, add Sionna as a second named oracle source alongside
   srsRAN golden vectors, with the same bit-exact/tolerance rules.
3. Note in `p2-phy-kernels` / wherever licensing hygiene is documented: Sionna source may be
   consulted directly (Apache-2.0, contribution-welcoming) under the same "reference only, write our
   own" rule already applied to cuPHY — record this so future contributors don't need to re-derive
   the license comfort level.
4. Leave CXLMemSim connection as a documented stretch goal (UC10) — no action until Phase 2 has a
   working baseline (per `ARCHITECTURE_v3.md` §10 exit criterion), consistent with existing sequencing.

## Sources

Sionna: github.com/NVlabs/sionna (LICENSE, issue #464 non-CUDA GPU), developer.nvidia.com/sionna,
developer.nvidia.com/blog/jumpstarting-link-level-simulations-with-sionna,
nvlabs.github.io/sionna/rk (Research Kit docs + LDPC CUDA tutorial) ·
Sionna Research Kit: github.com/NVlabs/sionna-rk ·
neural_rx / ONNX+TensorRT live OAI deployment: github.com/NVlabs/neural_rx ·
CXLMemSim: local checkout `third_party/CXLMemSim` (README, LICENSE, `qemu_integration/guest_libcuda/`) ·
cross-reference: `cxl_ran_poc/GAPS.md` gap 3 (NUMA sweep + CXLMemSim/Mess sensitivity, not started) ·
academic context: arxiv.org/pdf/2602.04652 (Sionna-LDPC5G-derived GPU benchmark, Clemson/NVIDIA) ·
portable FFT precedent (not adopted, context only): arxiv.org/pdf/2203.09384 (SYCL-FFT).
