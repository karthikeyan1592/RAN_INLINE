# p2-phy-kernels — SPEC

> **PIN:** OCUDU `release_26_04` (`gitlab.com/ocudu/ocudu`, BSD-3-Clause-Open-MPI, per-file SPDX
> verified on every port source). Local port tree: `third_party/ocudu` (shallow clone, HEAD
> `4e9f8d65a0277be4101fdcfb31910eaa0c365b6c`, 2026-07-17). Read
> [`../../../research/ocudu_repin.md`](../../../research/ocudu_repin.md) (dual-oracle rule, §2) and
> [`../../../research/use_case_classification.md`](../../../research/use_case_classification.md)
> (§0.1: the kernels are a **T3 port**, not a fresh build) before implementing.
> **⚠ LEGAL RULE:** port ONLY from `third_party/ocudu` (BSD). NEVER copy from
> `third_party/srsRAN_Project` (same code lineage, **AGPLv3** distribution — reference/oracle only).

**Feature:** SIM phase P2 — the portable GPU UL PUSCH kernel pipeline.
**Authority:** [`ARCHITECTURE_v3_SIM.md`](../../../ARCHITECTURE_v3_SIM.md) §3 (backend contract +
kernel rules, normative), §3.1 (verification ladder), §4 P2 row (gates).
Master: [`ARCHITECTURE_v3.md`](../../../ARCHITECTURE_v3.md) §2 (data path), §3 (split 7.2x).
Validation constraints: [`../../../../SRSRAN_LIMITATIONS.md`](../../../../SRSRAN_LIMITATIONS.md)
§1 (DEV-044), §2 (DEV-043).

## Purpose

Deliver the six portable GPU kernels of the UL PUSCH receive chain and the host pipeline that
strings them together — the bulk of the SIM build (SIM §6.1). Five kernels are
**algorithm-preserving ports** of OCUDU's production CPU PHY (T3); the depacketizer is **fresh
(T4)** — no CPU-to-GPU port equivalent exists anywhere, public literature is empty (classification
§0.2). The LDPC decoder is **reused** from prior bit-exact work (BG1/BG2) — a dependency, not a
deliverable. CRC24A runs on CPU after handoff. DL stays on OCUDU's CPU PHY (hybrid DU; UL-only
pipeline, ARCHITECTURE_v3 §2).

Pipeline (one slot, UL): eCPRI/O-RAN U-plane depacketize + RE-grid reassembly → DMRS LS channel
estimation + interpolation → single-layer MMSE equalization → soft demapping (LLRs) →
descrambling → rate-dematching → **[reused LDPC decode]** → host CRC24A → TB record.

**Backend statement (SIM §3):** this feature provides the P2 payload of `compute_backend`
(kernels run on PoCL in SIM; identical source on vendor ICDs in PHYSICAL). It *consumes*
`ingest_backend` (SIM: af_packet-captured/canned frames staged to a device buffer; PHYSICAL:
frames already in VRAM via dmabuf) behind one packet-arena ABI (HLD §4). It *feeds*
`handoff_backend` (plain readback in SIM) with decoded TBs. It implements none of the backends'
transport itself.

## In scope

- Six kernels, **one sub-phase each, landed separately** (SIM §4 P2 row), in pipeline order:

| # | Kernel | Type | Port source (namespace `ocudu`) |
|---|---|---|---|
| K1 | eCPRI/O-RAN U-plane depacketizer + RE-grid reassembly | **T4 fresh** — highest-uncertainty kernel | none (O-RAN CUS-plane spec; cuPHY read-only; OCUDU `lib/ofh/serdes` CPU decoder as field-semantics reference) |
| K2 | DMRS LS chan-est + interpolation (TS 38.211 §6.4.1.1) | T3 port | `dmrs_pusch_estimator_impl`, `port_channel_estimator_average_impl`, `interpolator_linear_impl` |
| K3 | Single-layer MMSE equalizer (MIMO deferred) | T3 port | `channel_equalizer_generic_impl` (1×N path) |
| K4 | Soft demapper → int8 LLRs | T3 port | `demodulation_mapper_impl` + per-QAM scalar paths |
| K5 | Descrambler (TS 38.211 §6.3.1.1, Gold seq §5.2.1) | T3 port | `pseudo_random_generator_impl::apply_xor` |
| K6 | Rate-dematcher, single-shot (TS 38.212 §5.4.2; HARQ deferred) | T3 port | `ldpc_rate_dematcher_impl` (generic) |

- Host pipeline orchestration (slot batching, queues/events, buffer pools), modeled on
  `pusch_processor_impl`/`pusch_demodulator_impl` sequencing; host-side CB segmentation
  parameters (TS 38.212 §5.2.2) and CRC24A/CRC24B verdicts.
- Dual-oracle validation harness + per-kernel gates; growing-pipeline integration gate on canned
  eCPRI pcaps (both P1-captured and oracle-packed classes, see P2-R15).
- Fixed MVP configuration (below) and its YAML schema; clean rejection of anything else.
- Provenance + AGPL-hygiene machinery (P2-R12/R13).

## Out of scope

- **Performance** — any latency/throughput requirement or claim (SIM tier rule; PHYSICAL measures).
- **HARQ** (combining, soft buffers over retransmissions) — K6 is single-shot, `rv=0`, new-data only.
- **MIMO / multi-layer / multi-port** — K3 is 1 layer × 1 rx port; `MAX_PORTS` paths not ported.
- **DL PHY** in any form; PRACH, PUCCH, SRS, CSI; UCI-on-PUSCH (no `ulsch_demultiplex` port —
  SCH-data-only codeword, see HLD D7).
- Transform precoding (DFT-s-OFDM), CFO/TA compensation, EVM, post-eq SINR reporting.
- LDPC decoder work (reused as-is); CRC on GPU.
- Live tap / UL injection (→ `p3-live-tap-ul-inject`); seam IPC (→ `p4-phy-l2-seam`);
  real NIC/dmabuf ingest (→ `p6-physical-m1-ingest`).
- O-RAN C-plane processing (ru_emulator C-plane is consumed by the DU, not by us; K1 handles
  U-plane only) and BFP compression (MVP pins uncompressed 16-bit IQ; BFP-9 is a stretch).

## Fixed MVP configuration (bounds all P2 scope)

| Parameter | Value | Why |
|---|---|---|
| Duplex / band | TDD, n78 | matches the p1 rig's upstream gnb sample config |
| Numerology / BW | µ=1 (30 kHz SCS), 20 MHz → **51 PRB, 612 subcarriers** | OCUDU docker default; small grids; one numerology only |
| UEs / layers / rx ports | 1 / 1 / 1 | single-layer MMSE scope; ru_emulator socket-mode single eAxC |
| PUSCH allocation | fixed: `rb_start=0`, 51 PRB, symbols 0–13, mapping type A | full-band, no scheduler variability |
| DMRS | type 1, single-symbol, positions {2,7,11} (TS 38.211 Table 6.4.1.1.3-4, l₀=2, add-pos 2), 2 CDM groups w/o data | matches OCUDU defaults; 11 data symbols |
| MCS set | indices **{4, 13, 21}** of TS 38.214 Table 5.1.3.1-1 (Qm = 2/4/6) | one MCS per mandatory modulation; 256QAM stretch |
| HARQ / RV | new-data always, `rv=0` | HARQ out of scope |
| UCI on PUSCH | none | demux collapses to identity (HLD D7) |
| Scrambling / cell | `n_ID = PCI = 1`, C-RNTI `0x4601` | p1 rig defaults |
| OFH U-plane | 1 eAxC (0), **static, uncompressed 16-bit IQ**, no VLAN | removes BFP decompressor from K1 MVP |

Rationale: this is exactly the configuration the p1 rig runs, so P1 captures, P3 live taps and P2
gates all share one config; it exercises every kernel's algorithm without exercising any deferred
dimension (layers, HARQ, UCI, compression).

## Requirements

| ID | Requirement (each testable) |
|---|---|
| **P2-R1** | The pipeline comprises exactly K1–K6 + reused LDPC + host CRC24A, in the order above; each kernel lands in its own sub-phase with its own gate, and the pipeline is buildable/testable at every prefix (K1 alone, K1–K2, … K1–K6+LDPC). |
| **P2-R2** | **Portability (SIM §3, hard):** every kernel is OpenCL C source-JIT and/or AdaptiveCpp generic-SSCP SYCL; no warp/wavefront-width assumptions, no inline asm, no vendor intrinsics outside the single abstraction header; no work-group-size dependence beyond queried limits. Enforced by static lint (LLD §7 T-R2) + PoCL/Oclgrind runs. |
| **P2-R3** | **K1 (fresh, T4):** parses eCPRI msg-type 0 (IQ data) framing + O-RAN U-plane section headers for the MVP config and scatters IQ into the slot RE grid; emits a per-symbol completeness bitmap; tolerates loss/reorder/duplication per LLD error rules. Gate is vs a *structural* oracle (OCUDU's CPU `uplane_message_decoder` output on the same frames), not golden vectors — no vectors exist for this stage. |
| **P2-R4** | **K2:** LS estimation at DMRS REs (gen. per TS 38.211 §6.4.1.1.1/.2) + frequency interpolation + time-domain hold, algorithm-preserving vs the named OCUDU sources; float stage, tolerance-gated (tolerance recorded per P2-R14). |
| **P2-R5** | **K3:** per-RE single-layer MMSE (1×1) incl. per-RE post-eq noise variance, algorithm-preserving vs `channel_equalizer_generic_impl`; float, tolerance-gated. |
| **P2-R6** | **K4:** soft demap QPSK/16QAM/64QAM to int8 LLRs with OCUDU's exact quantization (`LLR_MAX=120`; range limit 24 for QPSK, 20 for 16/64QAM) and saturation semantics; output gate is **bit-exact int8** vs oracle when fed identical float inputs, tolerance-gated end-of-chain. |
| **P2-R7** | **K5:** descrambler is **bit-exact** (integer stage): Gold-sequence x1/x2 generation (TS 38.211 §5.2.1, `c_init = RNTI·2¹⁵ + n_ID` per §6.3.1.1) + sign-flip on LLRs identical to `apply_xor(span<log_likelihood_ratio>)`, incl. ±`LLR_INFTY` handling. |
| **P2-R8** | **K6:** rate-dematcher is **bit-exact** (integer stage): bit-selection revert (k₀ for rv=0) + de-interleave per Qm (TS 38.212 §5.4.2, Table 5.4.2.1-2), filler-bit LLR = +`LLR_INFTY`, saturated accumulate path present but exercised only with `new_data=true`. |
| **P2-R9** | **LDPC reuse:** the prior bit-exact BG1/BG2 OpenCL decoder is invoked unmodified (source SHA pinned); its existing bit-exact suite stays green *inside this pipeline's buffers* (same LLR ABI, no re-quantization between K6 and LDPC). |
| **P2-R10** | **CPU tail:** host computes CB segmentation params (TS 38.212 §5.2.2), CRC24B per CB (>1 CB) and CRC24A over the TB (§5.1), and emits the TB+CRC record (LLD §5.6) consumed later by `p4-phy-l2-seam`. |
| **P2-R11** | The pipeline accepts exactly the MVP configuration; any other config in the YAML is rejected at setup with a structured error (no silent fallback, no partial support). |
| **P2-R12** | **Port provenance:** every ported kernel records, in its source header and in a machine-readable `provenance.json`, the OCUDU origin: repo URL, tag `release_26_04`, clone commit SHA, and per-file relative path(s) of the ported implementation(s). CI fails if a kernel source lacks a provenance entry. |
| **P2-R13** | **AGPL hygiene:** no code, header, test vector, or generated fixture derived from `third_party/srsRAN_Project` appears in any shipped/committed artifact. srsRAN golden vectors are consumed **read-only from the local checkout path at CI runtime** and are excluded (lint + packaging deny-list) from images, tarballs and the repo. Sionna-generated vectors are the only redistributable oracle data (ocudu_repin §2). |
| **P2-R14** | **AMENDED 2026-07-23 (adopted, not just disclosed — see below).** Original wording: dual oracle per kernel — (a) bit-exact or recorded-tolerance comparison vs srsRAN AGPL golden vectors (CI-only), where a matching vector set exists and is verified applicable to the OCUDU 26.04 algorithm; (b) tolerance comparison vs Sionna-generated vectors (shippable) for float stages and as algorithm-independent cross-check. **Adopted interpretation, effective for K1/K5/K6/K2/K3/K4 and p2f-integration going forward:** a single oracle — the real, currently-linked OCUDU library itself (not a static vector file from either source) — satisfies this gate's intent. Rationale: every kernel slice this project actually built (p2b/p2c/p2d/p2e) used this approach in practice and disclosed it as a substitution each time; neither the srsRAN-AGPL CI-only vector pipeline nor the Sionna-generated shippable vector pipeline was ever built, and linking directly against OCUDU's real, currently-built implementation is at least as strong a correctness signal as either static vector file would be (it can't silently drift from the algorithm it's supposed to represent the way a stale vector file could). Each float kernel's tolerance (metric + threshold) is still recorded in LLD §7 and in the results ledger; integer kernels still admit no tolerance. The CI-only-vs-shippable *packaging* distinction (P2-R13's AGPL-hygiene boundary) remains real and enforced (see P2-R13) — this amendment is about which oracle source is compared against, not about relaxing the AGPL-artifact hygiene rule. |
| **P2-R15** | **Integration gate (growing pipeline):** the pipeline decodes canned eCPRI pcaps end-to-end. Two pcap classes: **(a) P1-captured** (protocol-real, data-synthetic — ru_emulator static IQ, *no TB ground truth*, per SRSRAN_LIMITATIONS §4/DEV-044): gate = structural (reassembly matches CPU OFH decoder, pipeline runs to completion, stable outputs); **(b) oracle-packed** (harness packs known oracle RE grids into valid U-plane frames): gate = CRC24A pass + decoded TB bit-exact vs the oracle TB. Class (b) is the pass/fail decode gate. |
| **P2-R16** | **No-perf rule:** no requirement, gate, or CI assertion in this feature contains a timing/throughput threshold. Kernel-time printouts are permitted as unasserted debug output only. |
| **P2-R17** | The host pipeline API (`setup/feed/drain`, LLD §3) is the stable surface consumed by `p3-live-tap-ul-inject` (live frames in place of pcap frames) and `p4-phy-l2-seam` (TB records out); switching pcap→live requires no kernel or API change. |

## Acceptance gates

Per-kernel (unit) gates — each runs on PoCL every commit, Oclgrind nightly (SIM §3.1):

| Sub-phase | Gate | Oracle(s) |
|---|---|---|
| P2.1 K1 | RE grid + bitmap identical to CPU `uplane_message_decoder` replay over the same frames (uncompressed 16-bit: integer-exact after fixed-point→float conversion) | structural (OCUDU CPU decoder as executable spec); pcap class (a)+(b) frames |
| P2.2 K2 | channel-estimate tolerance ≤ recorded threshold (LLD §7) on all vectors | srsRAN vectors (CI-only) + Sionna (shippable) |
| P2.3 K3 | eq symbols + noise vars within recorded tolerance | srsRAN (CI-only) + Sionna |
| P2.4 K4 | int8 LLRs bit-exact on identical float inputs; tolerance-gated from float chain | srsRAN (CI-only) + Sionna |
| P2.5 K5 | bit-exact, all vectors, all three MCS | srsRAN (CI-only) + Sionna-derived integer check |
| P2.6 K6 | bit-exact, BG1+BG2, all vectors (rv=0) | srsRAN (CI-only) + Sionna-derived integer check |
| LDPC (dep) | existing suite green in-pipeline, 0 mismatches | own prior suite |

Integration gate (P2 exit, = SIM §4 P2 row): growing `gpu-phy` pipeline decodes canned eCPRI
pcaps end-to-end — class (b) oracle-packed pcaps decode with CRC pass + TB bit-exact for all
three MCS; class (a) P1 pcaps pass the structural gate. Green on WSL2 **and** the GCP SIM VM.

## Dependencies

- `p0-rig-scaffold`: `gpu-phy` image (PoCL + AdaptiveCpp), `oracle` image + verdict CLI, pins
  manifest, CI skeleton.
- `p1-ran-baseline`: canned eCPRI pcaps (class a) and the MVP cell config values.
- Prior LDPC kernel + bit-exact suite (own work, pinned).
- `third_party/ocudu` (port source, BSD) and `third_party/srsRAN_Project` (CI-only oracle, AGPL —
  never shipped); Sionna (Apache-2.0) as offline vector generator.
- Downstream consumers: `p3-live-tap-ul-inject` (P2-R17 API), `p4-phy-l2-seam` (TB record).

## Honesty-ledger notes (what P2 does NOT prove)

1. **No performance evidence.** PoCL on CPU proves function only; nothing here says the pipeline
   meets any slot deadline on any GPU (PHYSICAL-only, and even there P2 kernels are measured, not
   thresholded).
2. **Float tolerance ≠ bit-exactness.** K2/K3 (and K4's float input) are tolerance-gated; only
   K5/K6/LDPC/CRC are bit-exact claims. A tolerance pass on two oracles is strong but not identity.
3. **Fixed config only.** One numerology, one BW, one allocation, three MCS, 1×1, no HARQ, no UCI,
   no compression. Nothing is claimed about any other configuration.
4. **K1 is fresh (T4) and the highest-uncertainty kernel** — no golden vectors, no prior art;
   its oracle is another implementation (OCUDU CPU OFH decoder), i.e. consistency, not conformance.
5. **Decoding P1 pcaps proves plumbing, not correctness** — their payload is ru_emulator's static
   synthetic IQ with no ground-truth TB (DEV-044 pattern, SRSRAN_LIMITATIONS §4/§5). Ground-truth
   decode is proven only on oracle-packed pcaps (P2-R15b) and later on p3's injected live path.
6. **Same-lineage oracle caveat:** srsRAN vectors are `release_24_10_1`, port source is OCUDU
   `release_26_04` — algorithm drift between the two must be verified per kernel before a vector
   failure is treated as a port bug (P2-R14a "verified applicable"; DEV-043 rule: read the source
   before discarding odd data).
7. **PoCL/Oclgrind cannot prove vendor-GPU behavior** (warp width, vendor float rounding, JIT) —
   rung 2.5 (RTX 2050) is optional/indicative; vendor proof is PHYSICAL.
