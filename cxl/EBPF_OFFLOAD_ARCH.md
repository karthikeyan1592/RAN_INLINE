# eBPF-Driven LDPC Offload — Mechanism & Architecture

**Type:** Architecture / mechanism document (companion to `E2E_ARCH_SPEC.md`)
**Purpose:** Define precisely what eBPF does (and does not do) in the LDPC-offload pipeline,
the exact intercept→offload mechanism, the observe-vs-replace distinction, and when eBPF should
be used vs omitted. Includes reference links.

---

## 1. The core clarification (read first)

**eBPF is not the compute engine, and it does not bypass CPU processing.**

- eBPF is a small, sandboxed VM (bounded instructions, no unbounded loops in-kernel, no floating
  point to speak of, no arbitrary library calls). **You cannot run an LDPC decoder, FFT,
  equalizer, or soft-demapper in eBPF.**
- The **GPU / OpenCL kernel does the offloaded compute.** eBPF, at most, is the *switch* that
  reroutes one function call to that engine.
- The CPU is **not bypassed.** It runs the entire uplink PHY (channel estimation → equalization →
  soft-demapping) and everything after decode (CRC → MAC → higher layers). **Only the single
  expensive LDPC-decode stage** is diverted to the accelerator; control returns to the CPU
  immediately after.

```
 eCPRI in
    │
    ▼
 [CPU] chan-est → equalize → soft-demap      ← real DSP; eBPF CANNOT do this
    │  LLRs
    ▼
 ┌──────── the ONE offloaded stage ────────┐
 │  LDPC decode → runs on GPU/accelerator   │ ← only stage that leaves the CPU
 └──────────────────────────────────────────┘
    │  decoded bits
    ▼
 [CPU] CRC → MAC → higher layers             ← CPU resumes
```

---

## 2. Where eBPF sits: the intercept point

A uprobe is **one breakpoint at one address** — the **entry of srsRAN's
`ldpc_decoder_impl::decode()`**. It does *not* intercept "instructions" broadly; it traps when the
CPU reaches that one function-entry address, runs the eBPF handler, then resumes.

An **entry uprobe alone gives you _observe_, not _replace_** — after the handler returns, srsRAN's
own CPU decode still executes. This was the limitation of the prior v8 path (it captured LLRs but
srsRAN still decoded on the CPU → a tap, not an offload).

---

## 3. The exact intercept → offload mechanism (three parts)

To make the GPU decode run **instead of** srsRAN's decode, three pieces must work together — and
**only the first is eBPF**:

```
 srsRAN calls  decode(llr_ptr, out_ptr, len)
        │
        ▼  ← uprobe breakpoint at decode() entry
 ┌───────────────────────────────────────────────────────────┐
 │ 1. eBPF handler (bpftime): read llr_ptr / len from registers │  ← eBPF = the SWITCH
 │ 2. call a registered NATIVE helper  gpu_decode(llr, out, len) │  ← native C (NOT eBPF) = ENGINE
 │       • runs the real OpenCL/GPU LDPC decode                  │
 │       • writes the decoded bits into srsRAN's out_ptr buffer  │
 │ 3. OVERRIDE the return  → skip srsRAN's own CPU decode         │  ← bpftime function-override
 └───────────────────────────────────────────────────────────┘
        │
        ▼  srsRAN continues as if it had decoded  →  CRC → MAC
```

Accurate one-line description:

> The uprobe **intercepts at srsRAN's `decode()` entry**; a **native helper** invoked from the
> eBPF program runs the **GPU LDPC decode** and writes the answer into srsRAN's output buffer; and
> **bpftime's return-override skips srsRAN's CPU decode**. eBPF is the *switch that reroutes*; the
> native GPU helper is the *engine that computes*.

### Why a native helper, not eBPF, does the work
eBPF cannot call `clEnqueueNDRangeKernel`, cannot touch the GPU, cannot do floating point. bpftime
lets you **register custom native helper functions** that an eBPF program may call. The eBPF
program's job is only: read args → call `gpu_decode` helper → override return. The helper is
ordinary compiled C that does the real OpenCL work.

---

## 4. Two non-negotiable correctness requirements

1. **Override, not observe.** You must use bpftime's **function-override** so srsRAN's decode body
   is skipped and the GPU result is what the caller sees. If you only hook the entry and copy LLRs,
   srsRAN *still* decodes on the CPU — that is a tap, **not offload** (the v8 mistake). Two decodes,
   no acceleration.
2. **Synchronous from srsRAN's view.** srsRAN expects `decode()` to return *with the answer*. The
   GPU decode takes ~ms and is naturally async. To be correct, the helper must **block the srsRAN
   thread until the GPU result is written to `out_ptr`.** This is functionally correct but means GPU
   latency is **not** hidden behind other work — report latency honestly (no pipelining claim
   unless you actually pipeline; cf. Pattern E in the audit gates).

---

## 5. When to use eBPF vs when to omit it

This whole uprobe + override + helper design is justified by **exactly one** claim: *transparent
acceleration of an **unmodified** RAN binary (no recompile).* If you control the srsRAN build, the
**same replacement is cleaner via srsRAN's decoder-factory plugin** — register the GPU decoder as
the `ldpc_decoder` implementation and srsRAN calls it directly; no breakpoint, no override, no
sync hack, fully testable.

| Your claim / scenario | Mechanism | eBPF needed? |
|---|---|---|
| Accelerate an **unmodified / vendor** DU binary (transparent, no source change) | uprobe + **return-override** + native GPU helper | **Yes** — and it must override, not observe |
| You build / own the pipeline (control srsRAN's build) | srsRAN **decoder-factory plugin** → GPU | **No** — direct call, cleaner, deterministic |
| Fast eCPRI packet steering at the NIC (separate concern, ingress only) | eBPF **XDP / AF_XDP** redirect | Optional perf optimization; unrelated to LDPC offload |

Both offload paths end at the same outcome — "the GPU's `ldpc_decode` runs instead of srsRAN's
CPU decode." The only reason to choose eBPF is if **"no source modification" is itself the
contribution.**

### Recommendation for this project
- **Core pipeline (M0–M3 in `E2E_ARCH_SPEC.md`):** use the **decoder-factory plugin** (no eBPF).
  It is deterministic and easy to verify, and you control the code.
- **Transparent-intercept overlay (M4b):** implement the **bpftime uprobe + return-override**
  variant on the *unmodified* binary as a separate demonstration, if "drop-in acceleration without
  recompiling the RAN stack" is a claim you want to make. Do the override, or do not call it offload.

---

## 6. Honesty gates (carried from the audit)

- **Pattern B (code runs):** if eBPF is present, prove the override path actually executed and the
  CPU decode was skipped (e.g., a counter showing 0 CPU-decode invocations, N GPU decodes).
- **Pattern E (sequential vs pipelined):** the synchronous-block design is sequential; do not
  present it as latency-hidden pipelining.
- **Do not present an observe-only tap as offload.** If srsRAN still decodes on the CPU, say so.

---

## 7. References

### 7.1 CXL as a coherent CPU↔accelerator / "NVLink alternative" — verified via web search (2026-07)
- NVLink, UALink, and CXL — understanding the interconnects and their complementary roles:
  https://rcrtech.com/semiconductor-news/interconnects-nvlink-ualink-and-cxl/
- Intel framing CXL as "its answer to NVLink" (TechPowerUp):
  https://www.techpowerup.com/254462/intel-reveals-the-what-and-why-of-cxl-interconnect-its-answer-to-nvlink
- UALink and CXL 4.0 — GPU interconnect & memory-pooling guide (Introl):
  https://introl.com/blog/ualink-cxl-4-gpu-interconnect-memory-pooling-guide-2025
- **Cohet: A CXL-Driven Coherent Heterogeneous Computing Framework** (arXiv:2511.23011) — the most
  on-point academic framework for CXL-coherent CPU+accelerator compute:
  https://arxiv.org/abs/2511.23011
- CXL thriving as a memory link (SemiEngineering):
  https://semiengineering.com/cxl-thriving-as-memory-link/
- Octopus: Enhancing CXL Memory Pods via Sparse Topology (arXiv:2501.09020):
  https://arxiv.org/abs/2501.09020

### 7.2 Nearest prior art & methodology — as cited in `cxl_ran_poc/claude_research.md`
*(arXiv IDs reproduced from the repo's research doc; not independently re-verified in this session.)*
- **eGPU** (Yiwei Yang et al., bpftime-based) — eBPF + GPU + CXL.mem with a dynamic delay model;
  closest to this project's eBPF + GPU + CXL triad. (See `claude_research.md`.)
- **AtlasRAN** (arXiv:2603.14661) — coherent memory + accelerator offload for RAN (OAI vs Sionna,
  LDPC to CUDA), on DGX-class hardware. https://arxiv.org/abs/2603.14661
- **UDON** (Hermes et al., Arm; arXiv:2404.02868) — dual-socket NUMA to emulate CXL Type-2 compute
  offload. https://arxiv.org/abs/2404.02868
- **DecodeX** (arXiv:2511.02952) — LDPC decode split across CPU/GPU/ASIC. https://arxiv.org/abs/2511.02952
- "Offloading to CXL-based Computational Memory" (arXiv:2512.04449). https://arxiv.org/abs/2512.04449
- **emucxl** (Raja Gond & Purushottam Kulkarni, IIT Bombay; arXiv:2404.08311) — NUMA-based CXL
  emulation (the method for the latency-sensitivity overlay). https://arxiv.org/abs/2404.08311
- **Pond** (Li et al., ASPLOS'23; arXiv:2203.00241) — CPU-less remote-NUMA CXL emulation precedent.
  https://arxiv.org/abs/2203.00241
- **CXLMemSim** (Yang, Safayenikoo, Ma, Khan, Quinn; arXiv:2303.06153) — software CXL.mem latency
  model for timing sensitivity. https://arxiv.org/abs/2303.06153
- **DirectCXL** (KAIST, Myoungsoo Jung group; USENIX ATC 2022) — early real CXL prototype for
  direct host↔disaggregated-resource access.
- **"Six Times to Spare"** (arXiv:2602.04652) — real GPU LDPC on Blackwell GB10, ~6× over CPU,
  within the 5G slot budget (the projected-GPU-speedup citation). https://arxiv.org/abs/2602.04652

### 7.3 Tooling
- **bpftime** — userspace eBPF runtime (uprobe attach, custom native helpers, function override):
  https://github.com/eunomia-bpf/bpftime
- **srsRAN Project** — 5G NR software PHY; LDPC decoder factory is the plugin seam:
  https://github.com/srsran/srsRAN_Project

---

## 8. Summary

- eBPF **routes**, the GPU **computes**; the CPU is **not bypassed** — only the LDPC stage is diverted.
- A true eBPF offload = **uprobe intercept + native GPU helper + return-override** (skip the CPU
  decode). Observe-only is not offload.
- The GPU call must be **synchronous** from srsRAN's perspective; report latency without a
  pipelining claim unless you pipeline.
- Use eBPF **only** to accelerate an **unmodified** binary; if you own the build, the srsRAN
  **decoder-factory plugin** is the cleaner path and needs no eBPF.
```
