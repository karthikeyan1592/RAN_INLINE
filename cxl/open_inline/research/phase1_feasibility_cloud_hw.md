# Phase 1 Feasibility Study & Cloud Hardware Search

**Date:** 2026-07-16 · **Scope:** ARCHITECTURE_v3.md Phase 1 (complete L1 on GPU, no CXL)
**Question answered:** can Phase 1 be run **end-to-end in the cloud** — UE sim + RAN sim +
real NIC + real AMD/Intel GPU + real kernel stack + real CPU running L2/L3 — and on what hardware?

---

## 0. Verdict (TL;DR)

**Feasible, with one honest scope correction and one hardware caveat.**

1. **The eCPRI-without-RF problem is already solved** — better than v3 §6.3 assumed. srsRAN
   Project ships an open-source, DPDK-based **`ru_emulator`** (split 7.2, real eCPRI/OFH packets
   on a real NIC) *and* a **gNB test mode** that emulates attached UEs at the MAC, so the whole
   CU/DU/core stack runs with no radio and no physical UE. The §6.3(b) "synthetic injector we'd
   have to build" exists off-the-shelf.
2. **Scope correction (be honest in the pitch):** no open-source tool gives a *fully attached,
   protocol-live UE* whose signal traverses eCPRI without RF hardware. What is achievable in cloud:
   **protocol-real / data-synthetic** — real L2/L3/core + real eCPRI U/C-plane on a real NIC +
   bit-exact replayed UE waveforms through the GPU PHY. A live attached UE needs the SDR path
   (v3 §6.3(a)), which no cloud provides. This matches M1–M4; it is not a weaker claim than v3
   already makes.
3. **Hardware caveat: the NIC vendor decides the ingest mechanism.** eCPRI is plain Ethernet, so
   NIC→GPU-VRAM ingest needs either **mlx5 raw-packet QPs + `ibv_reg_dmabuf_mr`** or **DPDK's
   mlx5 dmabuf path** — both effectively **ConnectX-only**. The cheap AMD-GPU clouds (Vultr,
   Hot Aisle) ship **Broadcom** NICs: `bnxt_re` has dmabuf verbs support (kernel ≥6.3) but no
   raw-Ethernet-QP / DPDK-dmabuf path, so on those boxes the §6.2 freeze-breaker (CPU-staged
   ingest, one extra copy) is the realistic day-1 mode.
4. **Exactly one cloud SKU satisfies every requirement simultaneously** (bare metal + AMD GPU +
   mlx5/ConnectX Ethernet NIC + custom kernel): **OCI `BM.GPU.MI300X.8`** at ~$6/GPU/hr ($48/hr
   node). It's the "graduation box", not the dev box. Develop cheap elsewhere; burst on it.
5. **Recommended budget path:** ~US$0.9k–2.8k total for a 6-week Phase 1 (see §5).

---

## 1. What "Phase 1 end-to-end" concretely maps to

| Requirement (user) | Concrete component | Status |
|---|---|---|
| UE sim | srsRAN **gNB test mode** (MAC-level emulated UEs) + **replayed bit-exact UE waveforms** (RE grids from the existing oracle) injected as U-plane payload | exists / reuse prior oracle |
| RAN sim | srsRAN CU/DU (`gnb`) + Open5GS or OAI 5GC — real MAC/RLC/PDCP/SDAP/NGAP on host CPU | exists, open-source |
| Real CPU running L2/L3 | same as above — DU-high + CU + core are ordinary CPU processes; bare metal gives isolated cores, hugepages, RT tuning | solved by bare metal |
| Real NIC + real eCPRI | srsRAN **`ru_emulator`** (DPDK, split 7.2 OFH: C-plane + U-plane with dummy/injectable UL IQ) on one port/VF ↔ DU on another | exists ([docs](https://docs.srsran.com/projects/project/en/latest/tutorials/source/oranRU/source/index.html), [discussion #517](https://github.com/srsran/srsRAN_Project/discussions/517)) |
| Real GPU (AMD/Intel), full L1 | our OpenCL/SYCL/HIP PHY kernels (depacketize→chan-est→eq→demap→LDPC), LDPC reused bit-exact | prior work + new kernels |
| NIC→GPU direct DMA | `ibv_reg_dmabuf_mr` on raw-packet QP (mlx5) with GPU buffer exported via `hsa_amd_portable_export_dmabuf` (ROCm) / dmabuf export (CUDA open modules, Level Zero) | mechanism verified in ecosystem (§2.1); our traffic pattern = the M1 spike |
| Real kernel stack | upstream kernel ≥6.3 (amdgpu/xe dmabuf export, mlx5/bnxt_re dmabuf MR, DPDK, rdma-core) | needs bare metal or full-passthrough VM |

**Wiring without physical cabling (cloud reality):** you cannot plug a cable between two ports in
a cloud datacenter. Three workarounds, in order of preference:
(a) **same-NIC internal loopback** — SR-IOV VFs / two functions on one ConnectX port switch traffic
in the NIC eswitch, no external link needed (ru_emulator on VF0, DU on VF1); (b) **two hosts on a
provider L2 segment** (OCI VCNs support real L2 VLANs; Vultr VPC 2.0); (c) UDP-encapsulated eCPRI
over L3 if the provider only routes IP. (a) is the cleanest and is itself part of the M1 spike.

---

## 2. Feasibility findings (verified July 2026)

### 2.1 AMD GPU ⇄ RDMA-NIC dmabuf path: REAL
The exact workflow v3 §4 relies on is the ecosystem-standard one: `hipMalloc()` →
`hsa_amd_portable_export_dmabuf()` → `ibv_reg_dmabuf_mr()`; AMD's own ROCm XIO does precisely this
([ROCm XIO memory-modes doc](https://rocm.docs.amd.com/projects/rocm-xio/en/beta-0.1.0/conceptual/memory-modes.html),
[DatenLord GPUDirect-RDMA writeup](https://medium.com/@datenlord/the-evolution-and-implementation-of-gpudirect-rdma-19751f7b9413)).
No NVIDIA software anywhere. **What remains ours to prove (M1):** using it with a *raw-packet QP*
(plain Ethernet eCPRI frames, not RDMA sends) — mlx5 supports `IBV_QPT_RAW_PACKET` receive into any
registered MR, including dmabuf MRs; this combination is exactly the M1 spike.

### 2.2 The NIC-vendor fork in the road
- **ConnectX / mlx5:** raw-packet QPs ✅, dmabuf MR ✅, DPDK mlx5 dmabuf ✅ → full open NIC→VRAM path.
- **Broadcom / bnxt_re:** dmabuf MR ✅ in kernel & rdma-core
  ([rdma-core bnxt_re](https://github.com/linux-rdma/rdma-core/blob/master/providers/bnxt_re/verbs.c),
  auxiliary-driver support since kernel 6.3 — [cks blog](https://utcc.utoronto.ca/~cks/space/blog/linux/BroadcomNetworkDriverAndRDMA));
  but **no raw-Ethernet QP path and no DPDK dmabuf RX** → eCPRI frames can't land in VRAM by open
  means today. On Broadcom boxes, run **CPU-staged ingest** (NIC→CPU DPDK→pinned→GPU, the §6.2
  freeze-breaker) — architecture intact, one honest extra copy.
- **Intel E810/irdma:** no dmabuf MR support → not a candidate NIC.

### 2.3 eCPRI without RF: solved by srsRAN (stronger than v3 §6.3 assumed)
- [`ru_emulator`](https://github.com/srsran/srsRAN_Project/discussions/517): open-source, YAML-configured,
  DPDK-bound to a real NIC, parses/emits split-7.2 OFH (eCPRI) packets, generates UL U-plane
  (static-IQ today — injecting *our* oracle RE grids is a small patch, giving bit-exact ground truth).
- [gNB test mode](https://docs.srsran.com/projects/project/en/latest/tutorials/source/testmode/source/index.html):
  emulates attached UEs at MAC level so DU-high/CU/core all run realistically with zero radio.
- OAI's FHI7.2/xran path (v3 §6.3) remains the alternative; O-RAN-SC's `o-du/phy` sample apps can
  also emulate the O-RU side (used for FlexRAN testing). srsRAN's is the most turnkey.
- **Honest limit:** neither gives a protocol-attached UE *through* the fronthaul without RF.
  End-to-end claim = real protocol stack + real eCPRI + bit-exact replayed UE data. A live UE
  attach (M6 stretch) needs SDRs → lab hardware, not cloud.

### 2.4 GPU portability plumbing (risk §9.5 refined)
`cl_khr_external_memory` is **import-only and still unevenly supported** (AMD ROCm OpenCL support
unconfirmed). Correction to the plan: don't import NIC buffers *into* OpenCL — **allocate with the
native runtime (HIP / Level Zero / CUDA), export dmabuf to the NIC, and run kernels via
HIP/AdaptiveCpp-SYCL over those same pointers (USM)**. Every vendor has a native dmabuf *export*:
ROCm `hsa_amd_portable_export_dmabuf`, Intel Level Zero external-memory export, CUDA
`cuMemGetHandleForAddressRange` (needs open kernel modules). This sidesteps the OpenCL-import
unevenness entirely; keep OpenCL only for the pure-compute LDPC portability story.

### 2.5 CPU side
srsRAN DU + `ru_emulator` want isolated cores, hugepages, and DPDK-capable NICs — all trivial on
bare metal, painful/impossible on container clouds (RunPod, AMD Developer Cloud droplets are
excluded: no kernel control, virtio NICs).

---

## 3. Cloud hardware survey (verified July 2026)

### 3.1 The one box that has everything: OCI `BM.GPU.MI300X.8`
- 8× AMD MI300X, 2× Xeon 8480+ (112 cores), 2 TB RAM, **bare metal**, frontend NIC is
  **mlx5/ConnectX** (`mlx5_1`/eth0 + `rdma*` RoCE devices)
  ([Oracle GA blog](https://blogs.oracle.com/cloud-infrastructure/announcing-ga-oci-compute-amd-mi300x-gpus),
  [oci-gpu-quickstarts MI300X README](https://github.com/oracle-quickstart/oci-gpu-quickstarts/blob/main/amd/MI300X/README-MI300X.md)).
- **$6/GPU/hr → $48/hr/node** ([price list](https://www.oracle.com/cloud/price-list/)). Needs a
  service-limit/quota request (possibly capacity reservation) — start that request early; unverified
  how fast MI300X quota is granted in a fresh tenancy.
- Role: **graduation box** — full open path AMD-GPU + mlx5 + raw-packet-QP + dmabuf, M1→M4 final runs.
- Plenty of CPU for DU/CU/core + DPDK; custom kernel fine (bare metal).

### 3.2 Cheap dev grind (NVIDIA, mlx5): OCI `BM.GPU.A10.4`
- 4× A10, 64 OCPU Ice Lake, 1 TB RAM, 2×50G NIC, bare metal, **$2/GPU/hr → $8/hr**
  ([shapes doc](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm)).
  OCI bare-metal GPU family NICs are Mellanox (confirm `lspci` day 1 — high confidence, not 100%).
- Role: build & debug everything (M1 mechanism, ru_emulator wiring, PHY kernels via HIP-on-CUDA or
  SYCL, seam/IPC, srsRAN integration) at 1/6 the MI300X price. Same OCI tenancy/tooling → the
  final AMD runs are a re-run, not a re-port.

### 3.3 Cheap AMD compute (PHY-only or CPU-staged e2e)
| Provider | Offer | NIC | Fit |
|---|---|---|---|
| **Hot Aisle** | 1× MI300X **VM $2.99/hr**, by the minute ([pricing](https://hotaisle.xyz/pricing/)) | virtualized (no CX) | PHY kernel port/perf on real MI300X, no NIC path — cheapest real-AMD iteration loop |
| **Hot Aisle** | 8× MI300X bare metal $3.39/GPU/hr, **1-month min** (~$19.5k) | 8× Broadcom 57608 400G (RoCEv2, `bnxt_re`) | e2e with CPU-staged ingest; month-commit kills it for us |
| **Vultr** | 8× MI300X bare metal **$14.80/hr on-demand** ([Vultr MI300X](https://www.vultr.com/products/cloud-gpu/amd-mi325x-mi300x/), [Thunder pricing survey](https://www.thundercompute.com/blog/amd-mi300x-pricing)) | **Broadcom** (+ Juniper fabric — [Vultr blog](https://blogs.vultr.com/Lisle-data-center)) | best-value AMD **e2e with CPU-staged ingest**; try `bnxt_re` dmabuf experiments as stretch |
| Azure ND MI300X v5 | 8× MI300X VM | CX-7 but **InfiniBand** link layer + AccelNet VF | IB can't carry raw-Ethernet eCPRI; VM; misfit — skip |
| AMD Developer Cloud | MI300X droplets | virtio | containers/VMs, no kernel/NIC control — PHY dev only |

### 3.4 Intel GPU
**Intel Tiber AI Cloud**: Max 1100 from **$0.39/hr**, bare-metal Max 1100/1550 exists
([pricing](https://ai.cloud.intel.com/pricing/), [instances](https://console.cloud.intel.com/docs/reference/gpu_instances.html)).
No ConnectX pairing → **M5 portability leg only** (PHY kernels via SYCL/Level Zero). Cheapest
3rd-vendor checkbox; availability/roadmap wobbly post-restructuring — treat as opportunistic.

### 3.5 Other NVIDIA options (context)
- **Voltage Park**: H100 bare metal **from $1.99/GPU/hr on-demand, no minimum**, CX-7 on
  Quantum-2 **InfiniBand** ([pricing](https://www.voltagepark.com/pricing)). Cheapest mlx5+GPU metal,
  *but* IB link layer: eCPRI needs the port in Ethernet mode or NIC-internal VF loopback with link
  down — plausible (eswitch loopback is internal) but **unverified**; only worth a 1-day probe
  because it's so cheap. Ask support about Ethernet-mode ports before committing.
- **GCP A3-Ultra/A4**: CX-7 + NVIDIA, but 8-GPU nodes (~$60–90/hr), fabric-tuned, external-sender
  question unresolved (v3 §6.2) — poor value for this project despite existing GCP tenancy.
- **Latitude.sh / OVH / Hetzner**: Hetzner GPU boxes have no ConnectX option; OVH/Latitude GPU
  metal NIC models unverified and mostly monthly-commit — none beats OCI A10.4 for this purpose.

---

## 4. Recommended execution plan

**Stage A — dev grind (weeks 1–4): OCI `BM.GPU.A10.4`, $8/hr, start/stop daily.**
Day-1 checklist: `lspci | grep -i mell`, `ibv_devinfo`, confirm dmabuf MR
(`ib_write_bw --use_cuda_dmabuf` analogue / small C probe), SR-IOV VF creation + eswitch loopback,
hugepages + DPDK bind, srsRAN build. Then: M1 spike (raw-packet QP → dmabuf VRAM landing, byte
check) → ru_emulator ↔ DU wiring on VF pair → M2/M3 (GPU PHY on replayed oracle grids, bit-exact)
→ M4 (PHY↔L2 IPC, gNB test-mode UEs, Open5GS).

**Stage B — AMD portability (weeks 3–5, overlaps): Hot Aisle 1× MI300X VM, $2.99/hr.**
Port/validate PHY kernels (HIP + AdaptiveCpp), bit-exact vs oracle. No NIC work here.

**Stage C — AMD end-to-end graduation (week 6): OCI `BM.GPU.MI300X.8`, $48/hr, short bursts.**
Re-run the full Stage-A pipeline on AMD + mlx5: this is the headline result ("complete L1 on an
AMD GPU, open NIC→VRAM path, real L2/L3/core on CPU, real eCPRI"). Automate setup on Stage A so
Stage C is ≤3–5 sessions. **File the OCI MI300X quota request in week 1.**
*Fallback if MI300X quota stalls:* Vultr 8× MI300X on-demand ($14.80/hr) with CPU-staged ingest —
weaker headline (ingest not zero-copy on AMD leg), still a complete e2e.

**Stage D (optional): Intel Max 1100 on Tiber ($0.39/hr) — M5 third-vendor PHY checkbox.**

### Budget estimate (6 weeks)
| Item | Rate | Hours | Cost |
|---|---|---|---|
| OCI A10.4 dev | $8/hr | ~100 | $800 |
| Hot Aisle MI300X VM | $2.99/hr | ~40 | $120 |
| OCI MI300X bursts | $48/hr | ~30 | $1,440 |
| (fallback: Vultr MI300X) | $14.80/hr | (~30) | ($444) |
| Intel Tiber (optional) | $0.39/hr | ~30 | $12 |
| **Total** | | | **≈ $0.9k (fallback path) – $2.4k (full)** |

*Cheaper alternative kept on the table (v3 §6.2):* one used ConnectX-5 (~$100–250) + any ROCm dGPU
in a lab/desktop box beats all cloud pricing for the mechanism spike if IISc/Nokia can host it —
cloud is for what a desk can't provide (MI300X-class parts and repeatable rented environments).

---

## 5. Risk register (delta vs v3 §9)

| # | Risk | Δ vs v3 | Mitigation |
|---|---|---|---|
| 1 | Raw-packet-QP + dmabuf combo unproven (external Ethernet sender → VRAM) | unchanged, still top risk | M1 spike ≤2 weeks; CPU-staged fallback intact |
| 2 | Cloud L2 wiring (no cables) | **new** | NIC-internal VF loopback first; OCI L2 VLAN / UDP-eCPRI as fallbacks |
| 3 | Broadcom NICs on cheap AMD clouds | **new** | plan around it (OCI MI300X for the real path; Vultr = CPU-staged) |
| 4 | OCI MI300X quota/capacity latency | **new** | request week 1; Vultr fallback |
| 5 | `cl_khr_external_memory` unevenness | refined | native-runtime allocate/export + HIP/SYCL USM kernels (§2.4); OpenCL demoted to compute-only |
| 6 | "Live UE" expectation management | **new** | state clearly: protocol-real/data-synthetic in cloud; live UE = SDR lab (M6) |
| 7 | ru_emulator UL IQ is static by default | **new** | small patch to inject oracle RE grids (source is open); budget 1–2 weeks |

---

## 6. GPU L1: build vs port (added 2026-07-17)

**Landscape (verified):** a complete GPU L1 exists only as CUDA (NVIDIA Aerial cuPHY, open-sourced
Dec 2025, Apache-2.0, github.com/NVIDIA/aerial-cuda-accelerated-ran — "not accepting contributions");
vendor-portable L1 exists only on CPU (srsRAN, OAI). The intersection — GPU-resident, portable — is
empty. That gap is the thesis.

**Porting cuPHY to AMD/Intel: rejected.** Warp-32 cooperative kernels vs CDNA wavefront-64, inline
PTX + WMMA tensor-core paths (no auto-map to MFMA/XMX), entanglement with CUDA graphs/DOCA/NGC, and
a fork of a moving upstream. Multi-engineer-year scale — not a Phase-1 project.

**Reference material update (2026-07-19):** [`simulator_use_case_matrix.md`](simulator_use_case_matrix.md)
checked NVIDIA Sionna as a possible source of an already-built open GPU L1 — verdict: genuinely
Apache-2.0/contribution-welcoming (better license than cuPHY) but CUDA-only (confirmed, no AMD/ROCm
support) and batch/offline execution model, not real-time. Not portable, not adoptable as our
runtime — but now used as a *second, better-licensed* algorithm reference alongside cuPHY (read
only, same rule as below) and as an additional oracle source alongside srsRAN golden vectors.
Sionna Research Kit (real-time, but OAI-based + NVIDIA-DGX-Spark-specific) independently reconfirms
the same gap. No change to the decision below.

**Chosen: re-implement the DU-side UL PUSCH slice as fresh portable kernels** (Apache/BSD, HIP +
AdaptiveCpp-SYCL, USM pointers, no warp-width assumptions / inline asm / vendor intrinsics):
eCPRI depacketize + RE-grid (moderate) · DMRS/LS chan-est + interpolation (moderate) · single-layer
MMSE equalizer (easy; MIMO later) · soft demapper (easy) · descrambler (easy) · rate-dematch, HARQ
deferred (moderate) · **LDPC decode = already done, bit-exact (the hardest kernel)** · CRC24/TB on CPU.
**DL stays on srsRAN CPU PHY** — hybrid DU, matches the v3 §2 UL-only data path.

**Oracle:** srsRAN ships per-block golden test vectors (srsRAN_matlab / MATLAB 5G Toolbox, vectors
in-repo, `tests/unittests/phy`) + a PUSCH processor benchmark harness — block-by-block bit-exact
validation, same methodology as the prior LDPC/CXL PoC. Aerial cuPHY/pyAerial = second,
algorithm-level cross-check (read/re-derive only, never link/copy).

**Effort estimate (this team's background):** MVP fixed-config UL chain bit-exact ~2–3 months
part-time; configurable (any MCS/PRB, multi-UE) +2–3 months; real-time slot-deadline performance is
the long tail — Phase-1 claim is functional bit-exactness + measured latency, not carrier-grade.

### 6.1 AI-native L1 clarification (added 2026-07-17)
Checked: NVIDIA ARC-Pro / Nokia anyRAN "full L1 on GPU" runs **cuPHY = classical DSP** (FFT,
MMSE chan-est, min-sum LDPC) — neural nets do NOT replace FFT/LDPC even in NVIDIA's stack. The
neural piece is **NRX** (github.com/NVlabs/neural_rx, arXiv:2409.02912): one NN jointly replacing
*chan-est + equalization + demapping only*, trained on **synthetic Sionna data** (open, Apache-2.0 —
training data is NOT a gap), inference via TensorRT (NVIDIA-locked), still research/trial-stage.
Implication: Phase-1 classical portable L1 is exactly what ships commercially AND the baseline/
platform for AI-native work. Our chanest→eq→demap kernel cluster boundary = the NRX drop-in seam →
**Phase-3 candidate:** Sionna-trained NRX on AMD via ONNX Runtime/MIGraphX = first open portable
AI-native receiver. Real gaps there: slot-deadline inference on AMD/Intel unproven; NRX robustness
beyond simulated channels open — research-grade questions, not blockers to Phase 1.

---

## 7. Testing strategy: emulation-first (T1→T3, added 2026-07-17)

Reuses the CXL-PoC tiered method. **T1 (functional, $0):** full pipeline with zero exotic hardware —
ru_emulator ↔ srsRAN DU (test-mode UEs, Open5GS) over veth/virtio bridge (socket-mode OFH or DPDK
virtio/af_packet PMD), GPU-PHY kernels on **PoCL** (CPU OpenCL; AdaptiveCpp OMP for SYCL), bit-exact
vs srsRAN golden vectors, PHY↔L2 seam, plus all deploy automation. **T1 host:** WSL2 directly for
kernel/seam dev; a cheap GCP VM (existing tenancy, n2-standard-16 ≈$0.78/hr) for the *timed*
end-to-end — **not TCG-QEMU on WSL2** (no KVM → OFH slot-timing windows all miss; artifacts, not
findings). **T3 (cloud bare metal):** only two things T1 can't prove — the M1 dmabuf ingest
mechanism (no compute GPU in QEMU; rxe has no raw-QP/dmabuf) and all performance numbers.

**Migration delta T1→T3** (keep behind `ingest_backend`/`compute_backend` interfaces from commit 1):
virtio→mlx5 PMD (config), PoCL→vendor ICD (env, zero code), staged-copy→raw-QP+dmabuf ingest (the
one real code delta = M1 itself), memcpy→pinned-DMA handoff (small glue), kernel/distro pinned
identical. Portability compile model: OpenCL C source-JIT (every vendor driver) and/or SPIR-V
(Intel/AMD/PoCL solid; NVIDIA ingestion newer — verify), or AdaptiveCpp generic SSCP single binary
JITting to PTX/amdgcn/SPIR-V/OpenMP. Bit-exact only for integer stages (LDPC/scrambler/CRC);
float stages (chan-est/eq) validated with tight tolerance across vendors.

---

## 8. Sources
srsRAN ru_emulator: github.com/srsran/srsRAN_Project/discussions/517 · srsRAN O-RAN 7.2 guide & test mode: docs.srsran.com ·
ROCm dmabuf export / XIO: rocm.docs.amd.com (rocm-xio memory-modes) · GPUDirect-RDMA evolution (dmabuf workflow): medium.com/@datenlord ·
bnxt_re dmabuf: github.com/linux-rdma/rdma-core (providers/bnxt_re), utcc.utoronto.ca/~cks (kernel 6.3 auxiliary bus) ·
OCI MI300X GA (mlx5 frontend): blogs.oracle.com/cloud-infrastructure/announcing-ga-oci-compute-amd-mi300x-gpus; github.com/oracle-quickstart/oci-gpu-quickstarts ·
OCI shapes/pricing: docs.oracle.com computeshapes; oracle.com/cloud/price-list ·
Vultr MI300X ($14.80/hr, Broadcom/Juniper): vultr.com/products/cloud-gpu/amd-mi325x-mi300x; blogs.vultr.com/Lisle-data-center; thundercompute.com/blog/amd-mi300x-pricing ·
Hot Aisle pricing/networking: hotaisle.xyz/pricing, hotaisle.xyz/networking ·
Voltage Park: voltagepark.com/pricing · Intel Tiber: ai.cloud.intel.com/pricing; console.cloud.intel.com gpu_instances.
