# Course correction 001 — 2026-06-15

## Gates covered, verdicts

| Gate | Spec-match | Evidence | Verdict (mine) | Verdict (self-reported) | Status |
|------|-----------|----------|----------------|--------------------------|--------|
| 0.1  | match (verbatim) | sufficient | PASS | PASS | CONFIRMED |
| 0.2  | match (verbatim) | sufficient | PASS | PASS | CONFIRMED |
| 0.3  | match (verbatim) | sufficient | FAIL (spec FAIL path) | FAIL | CONFIRMED |
| 0.4  | match (verbatim) | sufficient | not found (informational) | not found | CONFIRMED |
| 1    | match (verbatim) | sufficient | PASS | PASS | CONFIRMED |
| 2    | — | — | — | — | NOT_YET_REACHED |
| 3    | — | — | — | — | NOT_YET_REACHED |
| 4    | — | — | — | — | NOT_YET_REACHED |
| 5    | — | — | — | — | NOT_YET_REACHED |
| 6    | — | — | — | — | NOT_YET_REACHED |

---

## Spot-check results

### Gate 1 (bit-correctness) — re-run 2026-06-15

Binary: `/root/linux_env/cxl/cxl_ran_poc/gpu_daemon/ldpc_cl/bit_diff_test`
Command: `cd /root/linux_env/cxl/cxl_ran_poc/gpu_daemon/ldpc_cl && ./bit_diff_test`

```
OpenCL device: cpu-haswell-12th Gen Intel(R) Core(TM) i5-12450HX
Testing BG1 LS=384 ls_idx=1  msg=8448 bits  codeword=25344 bits  n_iter=6
  msg[0]: 0/8448 mismatches
  msg[1]: 0/8448 mismatches
  msg[2]: 0/8448 mismatches
BG1 LS=384: 0 mismatches / 84480 bits  bit_diff_rate=0.000000  [PASS]

Testing BG2 LS=384 ls_idx=1  msg=3840 bits  codeword=19200 bits  n_iter=6
  msg[0]: 0/3840 mismatches
  msg[1]: 0/3840 mismatches
  msg[2]: 0/3840 mismatches
BG2 LS=384: 0 mismatches / 38400 bits  bit_diff_rate=0.000000  [PASS]

Testing BG1 LS=256 ls_idx=0  msg=5632 bits  codeword=16896 bits  n_iter=6
  msg[0]: 0/5632 mismatches
  msg[1]: 0/5632 mismatches
  msg[2]: 0/5632 mismatches
BG1 LS=256: 0 mismatches / 56320 bits  bit_diff_rate=0.000000  [PASS]

Testing BG2 LS=256 ls_idx=0  msg=2560 bits  codeword=12800 bits  n_iter=6
  msg[0]: 0/2560 mismatches
  msg[1]: 0/2560 mismatches
  msg[2]: 0/2560 mismatches
BG2 LS=256: 0 mismatches / 25600 bits  bit_diff_rate=0.000000  [PASS]

Gate 1 overall: PASS
```

**Agreement with recorded evidence:** EXACT MATCH. My re-run bit_diff_rate=0 for all 4
cases (BG1/BG2 × LS=384/256) matches bit_correctness.csv on disk verbatim. The
non-negotiable minimum (BG1/LS=384) is satisfied. Gate 1 bit-correctness is real.

### Gate 2 (bpftime in the data path) — NOT YET REACHED

Phase 2 has not been executed. Gate 2 cannot be spot-checked. This gate is the direct
heir to the prior audit's central failure (GAP 1: eBPF never in the data path). The
Gate 2 spot-check MUST be performed in course_correction_002, running the full Gate 2
protocol from the telemetry prompt (live bpftime attach on nrLDPC_coding_decoder in OAI
during an rfsimulator run, counter count vs expected n_slots×24).

---

## Deviation audit

All DEV-001 through DEV-008 are new (first invocation — no prior course correction).

**DEV-001** (Gate 0.4: no larger droplet provisioned)
- Downstream impact claim: "None on any blocking gate." → ACCURATE. Gate 0.4 is
  explicitly informational; no blocking gate or CSV column depends on it.

**DEV-002** (Gate 0.1: ubpf JIT instead of LLVM JIT)
- Downstream impact claim: "Phase 2 must run gNB-side agent with BPFTIME_VM_NAME=ubpf."
- Verification: STATUS.md lists DEV-002 as open and correctly identifies the need.
  Phase 2 gate file does not yet exist. Impact claim is ACCURATE but NOT YET PROPAGATED
  to a gate file — acceptable since Phase 2 is the next step; must be documented there.

**DEV-003** (Gate 0.1: non-atomic counter increment)
- Downstream impact claim: "Phase 2's gNB is multi-threaded. Non-atomic op could
  miscount. Must be addressed/justified in Phase 2 gate file."
- Verification: No Phase 2 gate file yet. Claim is ACCURATE; the risk is real (OAI
  nrLDPC_coding_decoder is called from multiple PHY threads). Must be resolved in Phase 2.

**DEV-004** (Gate 0.3: CXLMemSim CLI mismatch)
- Downstream impact claim: "Moot — Gate 0.3 FAILed for different reason." → ACCURATE.
  No downstream file or column needs updating.

**DEV-005** (Gate 0.3: WSL2 no PMU → CXLMemSim cannot run)
- Downstream impact claim: "Phase 4 latency sweep deferred. Deferral must be stated
  explicitly in emulation_mode.txt."
- Verification: `paper/results/emulation_mode.txt` currently exists but contains
  stale v3 content (see Cross-gate consistency below). It does NOT yet state the
  CXLMemSim deferral explicitly. Gate 0.3 file correctly defers this to Phase 4.
- Status: DEFERRED PROPAGATION — acceptable since Phase 4 not yet reached, but
  Phase 4 must overwrite emulation_mode.txt in full and include the deferral statement.

**DEV-006** (Gate 0.2: gNB --rfsimulator.serveraddr flag unsupported, omitted)
- Downstream impact claim: "None — effect is equivalent. All subsequent phases launch
  gNB without this flag."
- Verification: ss output shows ESTAB on 10.77.0.2:4043 — the gNB is bound to 10.77.0.2
  inside gnb-ns. Downstream impact claim ACCURATE. No later file needs updating.

**DEV-007** (Gate 0.2: UE PHY sync failure during test)
- Downstream impact claim: "Phase 2 will resolve via fresh reconfig.raw."
- Verification: STATUS.md next-action block explicitly notes "Need: fresh reconfig.raw
  (DEV-007)." ACCURATE; will be addressed in Phase 2.

**DEV-008** (Gate 1: runtime-oracle test vectors instead of cmake-downloaded vectors)
- Downstream impact claim: "None. bit_correctness.csv format unchanged, gate criterion
  identical."
- Verification: My spot-check re-ran the same binary and got bit_diff_rate=0 → the
  runtime-oracle approach works and matches the gate criterion. ACCURATE. The argument
  that runtime-oracle is strictly stronger than pre-generated vectors is sound.

---

## Cross-gate consistency

### emulation_mode column in CSVs

```
grep -rh emulation_mode paper/results/*.csv
```

Headers found:
- `latency_ladder.csv`: `component,per_slot_us,source,emulation_mode`
  - Rows: `baseline_cpu,11703,measured,bare-metal-kvm-host`; 
          `sync_offload_cpu_daemon,12036,measured,bare-metal-kvm-host`;
          `async_offload_cpu_daemon,11727,measured,bare-metal-kvm-host` — these are v3
          artifacts with `source=measured` where 12036 and 11727 are arithmetic
          compositions. See old numbers note below.
- Various baseline/offload CSV files have `emulation_mode` header — these are all v3
  artifacts, not v4-authored.
- `latency_ladder_v2.csv`: **ABSENT** — expected, Phase 5 not yet reached.

### PRIMARY_CONFIG headline (must not change)

```
grep -A3 "PRIMARY_CONFIG|per_slot_latency_us|overshoot_factor" calibration_check.txt
```

```
PRIMARY_CONFIG: 100MHz, mu=1 (0.5ms slot), MCS28, 273 PRB, 12 PDSCH sym, 12 DMRS RE/PRB
per_slot_latency_us: 11703   # 487.6 * 24
slot_budget_us: 500
overshoot_factor: 23.4       # 11703 / 500
```

**CONFIRMED UNCHANGED.** 11,703 µs/slot, 23.4×. This is the one number that must never
move. It has not moved.

### Old discredited numbers (12,036; 11,727)

```
grep -rn "12036|12,036|11727|11,727" paper/ 2>/dev/null
```

Found in:
- `calibration_check.txt` lines 94, 102, 105 (as comments/computed values)
- `RESULTS_SUMMARY.md` lines 70-71 (in the offload table)
- `latency_ladder.csv` rows 3-4 (as `source=measured` — this is the mislabeling)
- `breakdown.csv` line 8

All of these are **v3 session artifacts**. Phase 5 has not been reached; v4 Section 5.1
explicitly says to write `latency_ladder_v2.csv` to REPLACE these — that hasn't happened
yet. Their presence is expected at this stage. However, flagging now so Phase 5 cleans
them up rather than supplements them.

### Stale emulation_mode.txt — LIVE RISK

**Finding:** `paper/results/emulation_mode.txt` currently reads:
```
ebpf_status: WORKING
ebpf_progs_loaded: intercept_ldpc_decode (BPF prog 2822), intercept_fft_process (BPF prog 2823)
```

This is from the v3 session. Phase 2 (bpftime on nrLDPC_coding_decoder in OAI) has NOT
been done. The file currently ASSERTS that eBPF is working in the pipeline — this is
precisely the prior audit's central false claim (GAP 1: "eBPF never in the data path").
This file must be OVERWRITTEN in Phase 4, not appended to. Any reader who opens
emulation_mode.txt before Phase 4 will see a false claim. Risk is bounded because no
further grading or submission happens before Phase 4, but noting it explicitly.

Gate 0.3 file correctly says "(emulation_mode.txt to be written in Phase 4)" — this
instruction should be taken as overwrite, not append.

---

## Required actions before Phase 2

1. **Address DEV-003 (non-atomic counter) explicitly before wiring the Phase 2 probe.**
   OAI nrLDPC_coding_decoder is called from multiple PHY threads simultaneously. A plain
   `*v += 1` counter WILL miscount under real load. Choose one of: (a) per-CPU BPF map,
   (b) rebuild bpftime with the LLVM JIT and use `__sync_fetch_and_add`, or (c) SPSC
   ring with a single producer per probe site. Document the choice and its justification
   in the Phase 2 gate file.

2. **Generate fresh reconfig.raw/rbconfig.raw before the Phase 2 OAI test run** (per
   DEV-007 and STATUS.md). Start gNB first (without UE), let it write fresh files to
   `/tmp/oai_gnb/`, then launch UE with `--reconfig-file /tmp/oai_gnb/reconfig.raw`.
   This ensures nrLDPC_coding_decoder is actually called with matching config so UL
   decodes succeed and the counter has real events to count.

3. **Gate 2 raw evidence must include all three of the following** — none is sufficient
   alone, all three are required:
   - (a) **Attach confirmation**: actual bpftime loader output showing symbol resolved
     for `nrLDPC_coding_decoder` in `libldpc.so` (at 0x000e9b30 or current value), not
     a description of it.
   - (b) **Counter from a LIVE OAI rfsimulator run**: not the synthetic single-threaded
     victim from Gate 0.1. The gNB must be running inside gnb-ns, rfsimulator TCP
     established, and the counter must increment during that live run. Include the exact
     counter value and compute the expected count (n_ULSCH_rounds × expected_decoder_calls).
   - (c) **Counter != 0 and plausible**: the prior audit's failure was 0 events. If the
     counter is 0, Gate 2 is FAIL regardless of what the attach log says.

4. **Phase 2 gate file must document BPFTIME_VM_NAME=ubpf** (DEV-002) as the runtime
   environment for the gNB-side bpftime agent.

---

## STOP / GO

**GO** — implementer may proceed to Phase 2.

Gate 1 bit-correctness is independently confirmed by re-run. All Phase 0 gates have
correct verdicts and sufficient evidence. The deviations are accurately logged and their
downstream impacts are either addressed or correctly deferred. Phase 2 is unblocked.

The required actions above must be addressed IN Phase 2's gate file; they do not
retroactively block the GO, but Phase 2's gate will be judged FAIL if items 1, 2, or 3
are not satisfied.

---

## Gates covered, verdicts (machine-readable summary for next invocation's Step 1)

```
CONFIRMED: 0.1, 0.2, 0.3, 0.4, 1
DISPUTED: none
INSUFFICIENT_EVIDENCE: none
NOT_YET_REACHED: 2, 3, 4, 5, 6
```

Last DEV number seen: DEV-008
