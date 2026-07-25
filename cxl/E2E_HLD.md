# E2E RAN LDPC-Offload — High-Level Design (HLD)

**Type:** High-Level Design. Sits below `E2E_ARCH_SPEC.md` (what/why) and above `E2E_LLD.md`
(exact code). Defines module boundaries, interfaces, process model, sequence, and error strategy.
**Audience:** implementer (mid-level). Follow these boundaries exactly; do not invent alternative
module structures. Code-level detail (structs, signatures, byte layouts) is in the LLD.

---

## 1. System decomposition

Six modules + two shared libraries. Generator runs on the **host**; the rest run **inside the
guest** as **separate processes** sharing the CXL region.

```
                         common libs (linked by all):
                         ┌─ libcxlregion  (CXL/NUMA shared region + ring + sync)
                         └─ libe2ecommon  (ecpri, ldpc_params, config, timing)

 HOST                                  GUEST (separate processes, shared CXL region)
 ┌───────────────┐  eCPRI/UDP   ┌────────────┐   ┌────────────┐   ┌────────────┐   ┌───────────┐
 │  M1 Generator  │────────────►│ M2 RX       │──►│ M3 PHY      │──►│ M4 Accel    │──►│ M5 Consumer│
 │  (encode/mod/  │  virtio-net  │ eCPRI→grid  │gr │ srsRAN →LLR │LLR│ OCL decode  │TB │ CRC+compare│
 │   map/ecpri)   │             │             │id │             │   │  →TB        │   │  →CSV      │
 │  +ground truth │             └────────────┘   └─────┬──────┘   └─────┬──────┘   └─────┬─────┘
 └───────────────┘                                     │ writes         │ r/w           │ reads
                                                        ▼                ▼               ▼
                                              ╔═══════════════════ CXL region (NUMA node CXL_NODE) ═══════════════════╗
                                              ║  ring of slots: {status, seq, bg, Z, llr[], tb[], timestamps}          ║
                                              ╚════════════════════════════════════════════════════════════════════════╝
```

| Module | Process | Language | Responsibility |
|---|---|---|---|
| **M1 Generator** | host | C/C++ (srsRAN) | Known TB → encode → modulate → RE-map → AWGN → eCPRI packets. Persist ground-truth bits. |
| **M2 RX** | guest | C | Receive eCPRI, reorder, reassemble frequency-domain RE grid per slot. Hand grid to M3. |
| **M3 PHY** | guest | C++ (srsRAN) | Real UL PHY (equalize → soft-demap) grid → LLRs. Write LLRs into CXL slot; mark READY_LLR. |
| **M4 Accel** | guest | C (OpenCL) | On READY_LLR: BG-detect, decode via `ldpc_decode.cl`, write TB into CXL slot; mark READY_TB. |
| **M5 Consumer** | guest | C | On READY_TB: CRC24 + bit-compare vs ground truth; record latencies; free slot; write CSV. |
| **libcxlregion** | all | C | Allocate CXL/NUMA-backed region, ring management, slot state machine, sync. |
| **libe2ecommon** | all | C | eCPRI pack/parse, LDPC graph params + BG detection, config, timing. |

---

## 2. Interfaces between modules (contracts)

All inter-stage hand-off is through **the CXL region only** (M2→M3 grid hand-off may be a private
shm/queue, but LLR and TB **must** transit the CXL region — honesty gate D). Contracts:

- **M1 → M2:** eCPRI packets over UDP. Contract = the eCPRI/U-plane wire format (LLD §4).
- **M2 → M3:** reassembled RE grid for a slot. Contract = `re_grid_t` (LLD §5) via a private
  per-slot buffer or a lightweight local queue (not the CXL region).
- **M3 → M4:** CXL slot with `status=READY_LLR`, `llr[]` filled, `bg/Z/llr_len` set. Contract =
  `e2e_slot_t` (LLD §3).
- **M4 → M5:** same slot with `status=READY_TB`, `tb[]` filled. Contract = `e2e_slot_t`.
- **M5 → ring:** sets `status=DONE` to free the slot (backpressure to M3).

Ground-truth channel (out of band): M1 writes `${GROUND_TRUTH_DIR}/tb_<seq>.bin`; M5 reads it.
Keyed by `seq`, so it survives reordering.

---

## 3. Process & deployment model

- **5 processes** (M1 host; M2–M5 guest). M2–M5 attach the same named CXL region (`shm_open` +
  NUMA bind). This makes the CXL hand-off a real inter-process boundary (the whole point).
- Startup order: create region (first process to attach initializes header) → M5 → M4 → M3 → M2 →
  M1. A `run_wsl_integration.sh` launches them in order and tears down cleanly.
- On WSL all five run on one box over UDP loopback, `CXL_NODE=0`. On GCP the same binaries run
  with `CXL_NODE=1` (and optionally M1 outside the QEMU guest). **No code changes between
  environments** — only config + launch scripts (see LLD §9, portability).

### Threading
- M2: 1 RX thread (socket) + reassembly. Single-threaded is fine for non-RT.
- M3, M4, M5: single-threaded poll loops over the ring. Keep it simple; correctness over speed.
- No shared mutable state outside the CXL region + its atomics.

---

## 4. Main sequence (per slot)

```
M1  pick seed → info_bits b → write tb_<seq>.bin
    encode(b)→codeword → ratematch → QAM mod → RE map → +AWGN(snr)
    ecpri_pack(grid) → send UDP → WAIT ack/gap (backpressure, non-RT)
M2  recv packets → reorder by seq_id → reassemble re_grid_t → release to M3
M3  claim free ring slot (status EMPTY→INUSE) → run srsRAN equalize+soft-demap → LLRs
    write llr[] + bg/Z/llr_len + t_llr_ready_ns → status=READY_LLR (release-store)
M4  see READY_LLR (acquire-load) → BG-detect → zero-pad punctured VNs
    OCL decode (ldpc_decode.cl) → write tb[] + t_tb_ready_ns → status=READY_TB
    increment cxl_transit_counter (assert == processed CB count at end)
M5  see READY_TB → CRC24(tb) → bit-compare tb vs tb_<seq>.bin
    compute phy_us/xfer_us/decode_us/e2e_us → append CSV row → status=DONE (frees slot)
```

Backpressure: M3 blocks when no slot is EMPTY/DONE (ring full) → generator naturally throttled via
the ack. This prevents the v8 ring-overflow/replay bug (DEV-045).

---

## 5. Timing model (measurement points)

`CLOCK_MONOTONIC` is system-wide on one host → cross-process timestamps in the slot are valid.
**Never** use a bpftime/`bpf_ktime` clock for these (the v8 cross-clock bug).

| Metric | Interval | Measured by |
|---|---|---|
| `phy_us` | M3 slot-claim → LLR written | M3 |
| `xfer_us` | `t_llr_ready_ns` → M4 pickup | M4 (reads slot ts, same clock) |
| `decode_us` | OCL enqueue → clFinish | M4 |
| `e2e_us` | M2 grid-release → M5 verify done | spans processes; use slot ts |

The 500 µs slot budget appears only as a reference line. Numbers are non-real-time processing
latency (see spec §4).

---

## 6. Error handling & logging strategy

- **Fail loud, never silently skip.** Unrecognized BG, malformed packet, CRC fail, alloc failure →
  log with context and mark the slot/record as error (e.g., `bit_errors=-1`, `crc_pass=0`), do not
  drop it from the CSV. (A silently-skipped CB was a prior audit finding.)
- Each process logs a one-line startup banner: pid, role, `CXL_NODE`, region name, node verified
  via `get_mempolicy`.
- End-of-run: each process prints its counters (slots processed, CXL transits, errors). M4 must
  print `cxl_transit_counter` and it must equal the processed-CB count (gate D assertion).
- Return codes: 0 = clean; non-zero = which stage failed. No `exit(0)` on error paths.

---

## 7. Build & dependencies

- Build system: a top-level `Makefile` (or CMake) under `e2e/`. Targets per module + `all`.
- Deps: srsRAN (in `third_party/srsRAN_Project`), PoCL/OpenCL, `libnuma`, POSIX shm/rt.
- Reuse existing assets: `ldpc_decode.cl`, BG shift tables, and the existing
  `cxl_ran_poc/phase5_cxl/cxl_region.c` as the *starting point* for `libcxlregion` (refactor to the
  LLD API; do not fork logic).
- No new heavyweight deps without noting it in the gate report.

---

## 8. Module → gate mapping

| Gate (IMPLEMENTER_PROMPT) | Modules exercised |
|---|---|
| Gate 0 | libcxlregion + libe2ecommon skeleton + unit test |
| Gate 1 | M1 + M2 (eCPRI loopback) |
| Gate 2 | M3 (real PHY → LLR) |
| Gate 3 | M4 + M5 + full CXL slot state machine (oracle) |
| Gate 4 | M1–M5 integration run + CSV + BER + self-audit |
| Gate 5 | same, `CXL_NODE=1` on GCP + 2-NUMA overlay (deferred) |
| Gate 6 | eBPF uprobe+override variant of M4 trigger (optional) |

Exact data structures, signatures, byte layouts, and pseudocode: see `E2E_LLD.md`.
