# Implementer Prompt — E2E RAN LDPC-Offload Pipeline

You are implementing a functional end-to-end uplink 5G-NR receive pipeline with GPU LDPC
offload over a CXL-emulated shared-memory fabric. **Write code.** Work in `/root/linux_env/cxl`.

You are a mid-level engineer on this task: **follow the provided design exactly.** The HLD gives
you the module boundaries and interfaces; the LLD gives you the concrete structs, function
signatures, byte layouts, and pseudocode. Do **not** invent alternative module structures, data
layouts, or interfaces. If you believe a design detail is wrong or missing, do not silently
diverge — implement to the spec, and raise the concern in your gate report with a proposed change.

## Source of truth (read these first, in this order; do not contradict them)
1. `E2E_ARCH_SPEC.md` — WHAT/WHY: architecture, latency model, hardware config, milestones, honesty gates.
2. `E2E_HLD.md` — module decomposition, interfaces, process/sequence model, error strategy, module→gate map.
3. `E2E_LLD.md` — CODE LEVEL: exact structs, signatures, eCPRI/CXL byte layouts, per-file pseudocode, reuse-vs-new.
4. `EBPF_OFFLOAD_ARCH.md` — the eBPF intercept mechanism and when to use vs omit it (Gate 6 only).

These documents are authoritative. Ambiguity resolution order: LLD > HLD > SPEC for code details;
SPEC > HLD > LLD for scope/intent. If something in this prompt conflicts with them, state the
conflict in your gate report and follow the documents.

---

## Environment model (critical — obey this)

There are TWO environments. Do not conflate them.

- **WSL (this box) = development + part-by-part + INTEGRATION testing.** WSL has single NUMA, no
  KVM-CXL device, no real GPU (PoCL/CPU only). You **cannot** run the full QEMU-CXL e2e here.
  You **can** build and test every component individually and chain them into an integration test
  over loopback. This is where phases 0–4 happen.
- **GCP = full e2e + real CXL NUMA node + 2-NUMA latency test.** Deferred to the end ("later/
  finally"). You do NOT run GCP yet. You make the code **port cleanly** so that on GCP it is a
  config flip, not a rewrite.

### Portability requirement (this is what makes WSL→GCP a config flip)
Put ALL "CXL memory" behind one abstraction — e.g. `e2e/common/cxl_region.{h,c}`:
- `cxl_region_t *cxl_alloc(size_t bytes);` binds the region to a NUMA node chosen by config.
- Backend selected by env var `CXL_NODE` (default `0`):
  - **WSL:** `CXL_NODE=0` → `numa_alloc_onnode(bytes, 0)` or plain shared memory (single node).
  - **GCP:** `CXL_NODE=1` → CXL system-RAM node 1 (`numa_alloc_onnode`/`mbind` to node 1).
- **Never** mmap `/dev/dax*` directly (the 23 µs/byte device path — DEV-040). System-RAM NUMA only.
- Every stage allocates its shared buffers through this one API. No hard-coded node numbers anywhere else.
- Verify placement at runtime with `get_mempolicy` and log the node actually used.

Result: phases 0–4 run on WSL with `CXL_NODE=0`; phase 5 on GCP runs the *same binaries* with
`CXL_NODE=1`. If you find yourself special-casing WSL vs GCP outside `cxl_region.c` and the launch
scripts, stop and refactor.

---

## Resolved design decisions (do not re-litigate)
- Fronthaul: **O-RAN split 7.2x, frequency-domain IQ** (resource elements). The guest DU starts at
  channel estimation — **no FFT in the guest**. This matches `srsran::pusch_processor` input.
- Offload trigger (core build): **srsRAN decoder-factory plugin (direct call), NOT eBPF.** eBPF is
  a later overlay (phase 6) only if the "unmodified binary" claim is pursued — and then it MUST
  override, not observe (see `EBPF_OFFLOAD_ARCH.md` §4).
- eCPRI framing: **simplified, documented** header + IQ payload. Strict 7.2x is a later overlay.
- PHY: **real srsRAN blocks.** The `cxl_ran_poc/l1_sim/` stub (memcpy-equalize, random LLR) is
  BANNED as a data source.

---

## Gate execution protocol (MANDATORY — no bypassing)

Work strictly gate by gate. For EACH gate:

1. Implement the gate's scope.
2. Run the gate's acceptance test.
3. Write a report file `memory/e2e_run/gate_<N>.md` containing:
   - **VERBATIM** command output / test output (paste it, do not summarize numbers into prose).
   - Real measured values (latencies, error counts, node placement) — never computed/placeholder.
   - Explicit **PASS / FAIL / BLOCKED** verdict against the acceptance criteria.
   - Files created/changed, exact paths.
   - Which environment it ran in (WSL / GCP) and `CXL_NODE` value.
4. **Do not proceed to the next gate until the current gate PASSES with evidence in the report.**
5. If a gate **FAILS or is BLOCKED**, write that honestly in the report and **STOP for human review.**
   Do not fabricate a pass, do not skip ahead, do not "assume it works."

### Hard honesty rules (these are pass/fail gates, from the prior audit)
- **No synthetic numbers.** Every latency/throughput is measured from code that ran. The 23.4×
  slot-budget anchor and the ~6× GPU projection are *citations* — label them as such, never as this
  pipeline's own measurement. Do not edit `calibration_check.txt`.
- **Code must provably run** (outputs depend on inputs; log evidence).
- **No stub kernels / no random LLR** as a data source. Real PHY only.
- **CXL region in the path for EVERY codeblock** (LLR in, TB out) — assert a counter, log it.
- **Real oracle:** the generator persists ground-truth TB bits; the consumer CRC-checks and
  bit-compares. `bit_errors` is a real count (0 at high SNR, rising at low SNR).
- **Sequential vs pipelined reported honestly.**
- If you did not do something, say so. A blocked gate reported honestly is success; a faked pass is not.

Report files must actually exist at `memory/e2e_run/` (the prior run repeatedly claimed files that
were never written — do not repeat that).

---

## Phases & gates

For each gate, consult `E2E_HLD.md` §8 (module→gate map) for which modules it exercises, and
`E2E_LLD.md` for the exact structs/signatures/pseudocode of those modules. The gate scope below is
the *acceptance contract*; the LLD is *how* to build it.

### Gate 0 — Scaffolding + portability layer (WSL, CXL_NODE=0)
- Create `e2e/` tree per `E2E_ARCH_SPEC.md` §7 (generator, guest rx/phy/accel/consumer, common, scripts).
- Implement `e2e/common/cxl_region.{h,c}` (the abstraction above) + a unit test that allocs,
  writes, reads back, and verifies node placement via `get_mempolicy`.
- **Accept:** builds clean; unit test passes; `get_mempolicy` confirms node 0 on WSL.

### Gate 1 — eCPRI loopback / IQ integrity (WSL) [spec M0]
- Traffic generator: known TB bits → srsRAN encode → modulate → RE map → simplified eCPRI packets
  over UDP; persist ground-truth TB bits per slot.
- eCPRI RX: parse, reorder by SEQ_ID, reassemble the frequency-domain RE grid per slot.
- **Accept:** received RE grid matches transmitted within float tolerance for ≥100 slots;
  loss/reorder handled. Report the per-slot max abs error (≈0).

### Gate 2 — Real PHY → real LLR (WSL) [spec M1]
- Feed the received RE grid into the **real srsRAN UL PHY** (`pusch_processor` path:
  chan-est → equalize → soft-demap). Output LLRs in the exact layout `ldpc_decode.cl` expects.
- **Accept:** LLRs come from the real soft-demapper; hard-decision agreement > 99% at 20 dB SNR.
  **FAIL if** any path uses `l1_sim` or random LLRs.

### Gate 3 — LDPC offload through CXL region + oracle (WSL, PoCL, CXL_NODE=0) [spec M2]
- LLRs → `cxl_region` → LDPC accelerator (separate process, `ldpc_decode.cl` on PoCL, BG1/BG2
  auto-detect) → decoded TB → `cxl_region` → consumer: CRC24 + bit-compare vs ground truth.
- Every CB's LLR and TB transit the region (assert + log counter).
- **Accept:** CRC pass and `bit_errors==0` at high SNR for BG1 and BG2 (Z=384, ≥500 slots each);
  `bit_errors>0` appears as SNR drops; region placement logged; per-CB CXL transit counter == CB count.

### Gate 4 — Full integration run (WSL, single-NUMA/PoCL) [spec M3, integration variant]
- Chain gates 1–3 for ≥2000 slots across an SNR sweep. This is the **integration test**, not the
  QEMU-CXL e2e (that's GCP).
- Write `paper/results/e2e_wsl.csv` (real columns: slot, bg, Z, llr_len, snr_db, crc_pass,
  bit_errors, phy_us, xfer_us, decode_us, e2e_us, source=wsl_pocl_node0).
- Produce a BER-vs-SNR figure and a `SELF_AUDIT.md` checking the run against honesty rules A–E.
- **Accept:** CSV complete with real measurements; BER curve monotonic/waterfall; unique slots
  decoded == slots generated (no ring overflow/replay — DEV-045); self-audit passes.

### Gate 5 — GCP port: real CXL node + 2-NUMA latency (GCP — DEFERRED, do NOT run yet)
- Document exactly what changes: `CXL_NODE=1`, QEMU CXL system-ram bring-up (or run on a GCP
  2-NUMA host directly), launch scripts. Same binaries, no logic changes.
- When run (later): repeat gate 3/4 tests with `CXL_NODE=1`; then the **emucxl/Pond 2-NUMA
  latency overlay** — place the region on a CPU-less remote node and report the real node0-vs-node1
  decode-latency delta.
- For now: implement the launch/setup scripts and a `README_GCP.md` port guide; mark the gate
  **BLOCKED (awaiting GCP)** in its report. Do not fake results.

### Gate 6 — eBPF transparent-intercept overlay (optional) [spec M4b, per EBPF_OFFLOAD_ARCH.md]
- Only if the "accelerate an *unmodified* srsRAN binary" claim is pursued.
- bpftime uprobe at `decode()` + native GPU helper + **return-override** (skip the CPU decode).
- **Accept:** a counter proving CPU decode ran 0 times and GPU decode ran N times; results still
  bit-exact vs ground truth. **FAIL if** it only observes/copies while srsRAN still decodes on CPU.

---

## Safety / scope guardrails
- Operate only within `/root/linux_env/cxl` and the cxl-poc GCP project. **Never touch production
  ("jyotishyogi") or any unrelated instance.**
- Do not delete or overwrite `calibration_check.txt`, existing `memory/` audit files, or committed
  results. Add new files under `e2e/` and `memory/e2e_run/`.
- Commit only if asked.

## Deliverables
- `e2e/` source tree (generator, rx, phy, accel, consumer, common/cxl_region, scripts).
- `memory/e2e_run/gate_0.md … gate_N.md` — one honest report per gate, verbatim evidence.
- `paper/results/e2e_wsl.csv`, BER figure, `SELF_AUDIT.md`.
- `e2e/scripts/README_GCP.md` — the WSL→GCP port guide (for gate 5).

## Start
Read `E2E_ARCH_SPEC.md`, `E2E_HLD.md`, and `E2E_LLD.md` fully first. Then begin at **Gate 0**:
create the `e2e/` tree per LLD §1, implement `libcxlregion` + `libe2ecommon` per LLD §2–§3, build,
run the unit test, write `memory/e2e_run/gate_0.md` with verbatim evidence, and report the
PASS/FAIL/BLOCKED verdict before moving on. Do not skip gates. Do not fabricate. Follow the LLD
structures exactly.
