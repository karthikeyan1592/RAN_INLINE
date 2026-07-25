# v7 Telemetry — audit the GCP end-to-end run

You are a SEPARATE agent. You implement nothing. You audit
`memory/v7_run/implementer/` against `v7_e2e_prompt.md` and the
actual repo/results, then write a course-correction.

## Self-contained context

Seven versions of prompts. One recurring failure: pieces proven in
separate environments never assembled into one process. v7 is the
FINAL attempt — GCP n2 with --enable-nested-virtualization, real KVM,
QEMU CXL + bpftime + PoCL all in the SAME VM in ONE run.

The three root causes of every prior failure:
  1. TCG instead of KVM (DO didn't support nested-virt well) →
     PoCL JIT took >20 min → LDPC kernel swapped for hard-decision stub
  2. bpftime handler never wrote LLR to CXL — only counted calls
  3. srsRAN LLR in private anonymous RAM, no shared CXL mapping

v7 fixes: GCP KVM (real speed), shared memfd on CXL node, LLR mover.

The prior pattern to watch for:
  "e2e number" = earlier separate numbers added together (not measured)
  "bit_diff=0" with wrong Z and no real LDPC kernel
  "assembled" with bpftime not actually attached to srsRAN PID

---

## STEP 1 — what's new

```bash
ls memory/v7_run/telemetry/ | sort
cat memory/v7_run/implementer/DEVIATIONS.md  # v7 starts DEV-030+
```

Skip CONFIRMED prior gates. New scope: gate_{0..5}.md files.

---

## STEP 2 — per-gate: spec-match, evidence-sufficiency, independent
verdict. Same discipline as always. Evidence must be raw output, not
description. Paraphrase that softens = finding.

---

## STEP 3 — MANDATORY re-runs

### Gate 0 (KVM confirmed — the foundation of everything)

This is the gate that determines whether the TCG problem is solved.
```bash
# Re-confirm independently:
ssh <gcp-ip> "grep -c vmx /proc/cpuinfo"
ssh <gcp-ip> "ls -la /dev/kvm"
ssh <gcp-ip> "cat /proc/cpuinfo | grep 'model name' | head -1"
# Inside the VM (SSH through to QEMU):
ssh <gcp-ip> "cat /sys/devices/system/cpu/vulnerabilities/spec_store_bypass 2>/dev/null; lscpu | grep -i virtualization"
```

Specifically re-check: does the QEMU VM itself have KVM acceleration?

```bash
# On GCP host, check if QEMU was launched with -enable-kvm:
ssh <gcp-ip> "ps aux | grep qemu | grep -v grep"
# Expect: -enable-kvm in the command line.
# ALSO check the VM guest kernel's dmesg:
# ssh into the VM, then: dmesg | grep -i "kvm\|hypervisor\|paravirt"
# On KVM guest: "Hypervisor detected: KVM"
# On TCG:       "Hypervisor detected: None" or no KVM mention
```

```
If dmesg shows TCG (no KVM) despite -enable-kvm in the command:
  GCP may not be passing vmx to nested guests on this instance type.
  Check: ssh <gcp-ip> "cat /proc/cpuinfo | grep vmx" on the HOST.
  If vmx absent on the host: --enable-nested-virtualization didn't
  propagate. This is a STOP — the environment doesn't support what
  we need. Flag immediately.

If dmesg shows KVM: Gate 0 is real. Continue.

PoCL timing is indirect evidence:
  If the Gate 4 e2e run shows OCL decode in seconds (not hours),
  KVM was working. If it shows >10 min, it's TCG again.
```

### Gate 3 (LLR mover — the one new program, most likely to fail)

This is the new code. Independently re-run it.

```bash
# Confirm the handler attached to srsRAN's PID:
cat memory/v7_run/implementer/gate_3.md | grep -A10 "Commands run"
# Re-run a short version (5 decode calls):
<bpftime attach + srsRAN -R 5>
# Check cxl_llr_buf[0..2] for plausible LLR values (expect ±10 class)
```

```
CRITICAL CHECK — confirm the LLR pointer is correct:
  LLR for BG1 Z=384 is 9216 int8 values. The first few should be
  ±5 to ±20 range for a typical AWGN channel simulation.
  If cxl_llr_buf is all zeros: probe_read found the right address
  but it was zero-initialized (unlikely) OR it's reading the wrong
  location (likely %rdx was a span struct, needs one dereference).
  If bytes are garbage/random: reading from a wrong address entirely.
  If bytes look like ±10 class int8s: CORRECT LLR data.
```

### Gate 4 (the end-to-end run — the only gate that matters)

For EACH of (a)-(e), independently verify:

```bash
# (a) ONE process tree:
cat memory/v7_run/implementer/gate_4.md | grep -A20 "pstree\|process tree"
# Does it show srsRAN PID + consumer PID + bpftime agent PID running
# SIMULTANEOUSLY? "I ran them in sequence" is NOT assembled.
# The consumer must be WAITING for descriptors while srsRAN decodes.

# (b) LLR actually in CXL node 1:
# Look for numa_maps evidence with a REAL srsRAN LLR address:
grep -A5 "numa_maps\|llr.*node\|N1=" memory/v7_run/implementer/gate_4.md
# N1=<non-zero> on the LLR address line = CXL. N0-only = DRAM (fail).

# (c) OCL reads from same CXL region:
# The consumer's cl_mem base should be in the same VA range as the
# shared memfd, which is the same range as the LLR from (b).

# (d) bit_diff=0:
cat paper/results/e2e_gcp.csv | head -5
awk -F, '{sum+=$5} END {print "total_bit_diff="sum}' \
  paper/results/e2e_gcp.csv
# If total bit_diff > 0: OCL kernel not decoding correctly for this Z.
# Check which Z was used and whether bg_tables.h covers it.

# (e) NOT arithmetic — the critical anti-pattern check:
# Get the e2e per-CB p50 from e2e_gcp.csv
E2E=$(awk -F, 'NR>1{a[NR]=$6} END{n=NR-1; asort(a); print a[int(n/2)]}' \
  paper/results/e2e_gcp.csv)
# Get interception and ocl from earlier CSVs:
INT=$(grep interception_only paper/results/latency_ladder_v2_v5.csv | cut -d, -f2)
OCL=$(grep gpu_compute_full paper/results/latency_ladder_v2_v5.csv | cut -d, -f2)
echo "E2E=$E2E, INT=$INT, OCL=$OCL, SUM=$(echo "$INT+$OCL" | bc)"
# If E2E ≈ INT+OCL: ARITHMETIC, flag HIGH — the original failure.
# E2E should reflect GCP KVM timing (faster PoCL JIT, real latency),
# NOT the WSL2/DO numbers added together.
```

---

## STEP 4 — deviations (DEV-030+) + persistent ghosts

Ghosts that must finally die in v7:
```
DEV-011 (Z=224 wrong tables): gate_4 must show which Z was used.
  If Z=224: did they extend bg_tables.h? bit_diff=0 without extension
  is IMPOSSIBLE with real LDPC — flag HIGH if claimed.

DEV-030 (numactl SIGILL, pmem=off fix): gate_1 must show this resolved.
  If pmem=off broke CXL (5/5 failed) and they used the mbind shim
  as fallback: that's acceptable IF documented. If SIGILL persists
  AND no fallback: the LLR can't be on CXL → (b) fails.

DEV-003 (non-atomic counter, flagged THREE times, never closed):
  v7 uses an SPSC ring (Phase 2's design). Is there actually an
  atomic seq in the LLR mover? Check the handler source.
  If not: document it, assess risk (single producer=bpftime handler,
  single consumer=OCL daemon → SPSC is safe; state this explicitly
  as the resolution).
```

---

## STEP 5 — cross-cutting

```bash
# Anchor:
grep "per_slot_latency_us\|overshoot_factor" calibration_check.txt
# Must be 11703 / 23.4. Non-negotiable.

# Old arithmetic numbers gone:
grep -rn "12036\|11727" paper/ 2>/dev/null

# Arithmetic e2e check (done in STEP 3 Gate 4(e) above — re-state
# the finding here explicitly as pass/fail)

# GCP instance deleted (stops billing):
# Look for `gcloud compute instances delete` in the gate_5 file.
# If NOT deleted: flag — ongoing cost.
```

---

## OUTPUT — memory/v7_run/telemetry/course_correction_NNN.md

Same structure as prior. Include:
- Per-gate table (0-5)
- KVM confirmation from Gate 0 (dmesg evidence)
- LLR value check from Gate 3 (plausible ±10-class values)
- Gate 4 (a)-(e) each assessed independently
- The arithmetic e2e check (e2e vs INT+OCL sum)
- Persistent ghosts: DEV-011/030/003 resolution status
- Anchor unchanged
- GCP instance deleted
- STOP/GO + machine-readable summary

## Stance

```
This is v7. The environment is finally correct. The outcome is
therefore determined by whether the CODE was written correctly —
specifically the LLR mover (Gate 3) and the assembled run (Gate 4).

If Gate 4 (a)-(e) all pass with raw evidence: this project has what
it set out to prove. Confirm it clearly and completely.

If (d) fails (bit_diff>0): the LDPC kernel has a bug through the
assembled path — a wiring issue, not a kernel issue (kernel proven
in isolation). The chain assembled (worth stating) but correctness
needs one more debug iteration.

If (e) shows arithmetic: the EXACT original failure, attempt 7.
State it as such, clearly, without softening.

The PRIMARY_CONFIG 23.4× never moves. It is still the motivation.
Everything else is what closes the gap toward it.
```