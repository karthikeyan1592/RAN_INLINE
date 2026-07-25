# v5 Telemetry Prompt — audit memory/v5_run/ against the v5 spec

You are a SEPARATE agent from whoever is implementing
`cursor_cxl_poc_prompt_v5.md`. You do not implement anything. You read
what the implementer CLAIMS (`memory/v5_run/implementer/`), compare it
against the SPEC (`cursor_cxl_poc_prompt_v5.md`) and what the REPO
actually contains, and produce a course-correction document.

Re-invoked after each phase. Be efficient: don't re-audit gates a
prior course_correction CONFIRMED, unless new DEVIATIONS retroactively
affect them.

## Self-contained context (you have no prior conversation history)

This project's ORIGINAL audit found an implementing agent reported
"all done" while (a) the central eBPF interception was never in the
data path (0 programs loaded during measurement) and (b) two headline
latency numbers were arithmetic compositions mislabeled
`source=measured`. v4 fixed those: eBPF genuinely fires (929,474 CB
intercepts), the LDPC kernel is bit-exact (0 mismatches), labels are
honest. v4 is CONFIRMED done.

v5 is an ARCHITECTURE REWIRE, not a redo. It changes ONLY the data
path, to make the "NIC ⇒ CXL ⇒ GPU" claim literally true:

```
CHANGE A — OAI allocations diverted to the CXL NUMA node via
           `numactl --membind=<cxl-node>` (zero source mod). In v4
           the LLR was in system RAM, so "LLR is in CXL" was false.
CHANGE B — the eBPF uprobe writes a 40-byte DESCRIPTOR (offsets, not
           pointers) to a userspace ring buffer, NOT the LLR payload.
           In v4 the uprobe copied the LLR into a BPF map.
CHANGE C — the consumer is a pinned-core BUSY-POLL loop, not a 2ms
           sleep. v4's 2,636us interception number was POLL_NS=2ms,
           not the 248.5ns bpftime floor.
```

A COST RULE governs v5: Phases 1-4 run on WSL2 for free against a
CXL STAND-IN (tmpfs/mmap'd file behind a `cxl_region_map()` seam);
the DigitalOcean droplet (the only place real /dev/dax0.0 exists) is
spun up ONLY in Phase 5 to validate against real DAX and collect the
CXLMemSim sweep. Spending money before Gate 5 is itself a finding.

Approach with this history in mind: a confident self-reported PASS is
not evidence by itself. The failure mode this whole apparatus exists
to catch is "claimed architecture != deployed architecture."

---

## STEP 1 — What's new since the last course-correction

```bash
ls memory/v5_run/telemetry/ | sort
cat memory/v5_run/implementer/DEVIATIONS.md   # continues v4's
                                               # numbering; v5 starts
                                               # at DEV-015+
```

Read the most recent `course_correction_*.md`'s machine-readable
summary. Skip re-auditing CONFIRMED gates UNLESS:
  - a new DEV-NNN (higher than last recorded) names that gate in its
    "Downstream impact", OR
  - the gate file's git history shows modification after the last
    course-correction (`git log --oneline -- memory/v5_run/
    implementer/gate_N.md`).

Everything else (new gate files, prior DISPUTED / INSUFFICIENT, gates
hit by new deviations) gets the full procedure.

---

## STEP 2 — Per-gate audit procedure

For each in-scope v5 gate file (gate_1 .. gate_6):

### 2a. Spec-match
```bash
grep -A 18 "^### GATE <N> (v5)" cursor_cxl_poc_prompt_v5.md
```
Compare VERBATIM against the gate file's "## Spec" section. A
paraphrase that SOFTENS criteria (e.g. spec says "sub-microsecond
p50", gate file's copy says "low latency") is a finding — record the
exact diff. Copy-paste was required, not retype.

### 2b. Evidence-sufficiency
Read "## Raw evidence". Is it ACTUAL output/file-content, or a
DESCRIPTION? "busy-poll p50 was 800ns" is a description; the actual
histogram output is evidence. Could a skeptic reach the same verdict
using ONLY this section? If it requires trusting a sentence →
INSUFFICIENT_EVIDENCE, full stop, course-correction is "re-produce
with raw output".

### 2c. Independent verdict
Derive YOUR OWN PASS/FAIL/PARTIAL from spec + evidence BEFORE reading
the self-reported verdict. Then:
  yours == self-reported → CONFIRMED
  yours != self-reported → DISPUTED (state both + why, prominently).

---

## STEP 3 — Mandatory spot-checks (v5's two original-audit-DNA gates)

In v4 these were Gate 1 (bit-exact) and Gate 2 (eBPF-in-path). In v5
the equivalents are:

### v5 Gate 2 spot-check — bit-exactness THROUGH the CXL path
The kernel was proven bit-exact in v4. v5 Gate 2 proves it's STILL
bit-exact when fed via the CXL-region path (offsets, sub-buffers,
CL_MEM_USE_HOST_PTR) — a wiring change can corrupt data without
touching the kernel. Re-run it yourself:
```bash
cat memory/v5_run/implementer/gate_2.md | grep -A6 "Commands run"
# identify the bit_correctness_cxlpath test; run it for BG1/LS=384:
<re-run>
# compare bit_diff_rate to paper/results/bit_correctness_cxlpath.csv
```
Disagreement → DISPUTED, severity HIGH. ALSO check the zero-copy
sentinel claim: did the gate file CONFIRM zero-copy, or honestly
record "PoCL copied"? If it claims zero-copy, look for the sentinel
evidence (CPU-side write visible to kernel without clEnqueueWrite);
if that evidence is absent, the zero-copy claim is INSUFFICIENT.

### v5 Gate 3 spot-check — descriptor uprobe fires, NO payload copy
This is the heir to the original GAP 1. Independently confirm the
uprobe fires on live OAI AND carries only a descriptor:
```bash
# per the bpftime mechanism (check gate_3.md), run a short live OAI
# gNB (netns/veth) and confirm descriptors flow:
<launch ~30s OAI run + consumer per gate_3.md>
# count descriptors vs expected (slots * C_actual)
# THEN inspect the uprobe handler SOURCE:
grep -n "memcpy\|copy" phase5_cxl/*.c ebpf/*.bpf.c | grep -i llr
```
Findings that are severity HIGH:
  - 0 descriptors during a live run (GAP 1 recurrence).
  - The handler contains an LLR-sized payload copy in the DO path
    (Change B not actually done — only the documented WSL2-only
    stand-in copy, gated behind CXL_BACKING==standin, is allowed).
  - If descriptors are 0, IMMEDIATELY check whether latency_ladder_
    v2.csv rows still carry `source=measured` numbers that look like
    `baseline + constant` — the exact original failure recurring.

---

## STEP 4 — DEVIATIONS.md audit (DEV-015+ ; DEV-003/009/014 ghosts)

```bash
cat memory/v5_run/implementer/DEVIATIONS.md
```

For each new DEV-NNN: is "Downstream impact" ACCURATE? Go check the
named file RIGHT NOW. A logged-but-not-propagated deviation is a
CONSISTENCY FAILURE (this is how the original mislabel survived).

THREE SPECIFIC GHOSTS v5 was supposed to close — verify each is
actually closed, not just claimed:
```
DEV-003 (atomic counter, flagged TWICE in v4, never closed):
  v5 Gate 3 must resolve the multi-producer ring question with
  THREAD-ID EVIDENCE (SPSC-per-thread / MPSC / proof-of-
  serialization). Check gate_3.md has the thread-ID log, not just an
  assertion. If absent AGAIN → escalate: this is the third miss.

DEV-009 (C=2 vs C=24):
  v5 Gate 4 must EITHER show cb_index hitting 0..23 (C=24 achieved)
  OR have the C=24 projection formula in the harness output. Check
  ablation output / latency_ladder_v2.csv for C_actual and, if !=24,
  the explicit projection. "Measured at C=2, compared to 23.4x
  (C=24)" without normalization is the misleading-comparison finding.

DEV-014 (no CXL in path / 2ms poll):
  This is the WHOLE v5 thesis. Gate 4's interception_only row must be
  the busy-poll floor (sub-10us class), NOT ~2,636us. If it's still
  ms-class, Change C didn't happen — strace evidence for nanosleep/
  futex in the "busy-poll" loop is the proof either way.
```

Also: does any new deviation move a PREVIOUSLY-CONFIRMED v5 gate back
to DISPUTED? (e.g. a Phase 3 descriptor-format change that
invalidates Phase 2's through-path bit-exactness.)

---

## STEP 5 — Cross-gate consistency + the COST RULE

```bash
# PRIMARY_CONFIG anchor must NOT have moved:
grep -A2 "PRIMARY_CONFIG\|per_slot_latency_us\|overshoot_factor" \
  calibration_check.txt
# expect 11703 / 23.4 unchanged.

# Honest labels; no resurrected old numbers:
grep -rn "12036\|12,036\|11727\|11,727" paper/ 2>/dev/null
grep -rh "source" paper/results/latency_ladder_v2.csv

# emulation_mode vocabulary (v5 introduces the CXL_BACKING /
# busy-poll / descriptor path — check the strings are coherent):
grep -rh emulation_mode paper/results/*.csv

# THE COST RULE — the v5-specific check:
# Phases 1-4 are WSL2-only. If ANY gate file BEFORE gate_5 contains
# evidence a DO droplet was provisioned (doctl create output, a
# droplet IP, /dev/dax0.0 access, ssh root@<ip>), that is a FINDING:
# money was spent before the spec allows.
grep -rln "doctl compute droplet create\|/dev/dax0.0\|root@.*droplet\|provision.sh" \
  memory/v5_run/implementer/gate_1.md \
  memory/v5_run/implementer/gate_2.md \
  memory/v5_run/implementer/gate_3.md \
  memory/v5_run/implementer/gate_4.md 2>/dev/null
# Any hit here → flag "DO resources used before Phase 5 (cost rule
# violation)" in the course-correction. (Gate 5 is the ONLY gate
# where /dev/dax0.0 and droplet provisioning are expected.)
```

If Gate 5 IS in scope: verify the droplet was TORN DOWN
(`status.sh shows it gone`) — a still-running droplet is leaked cost.

---

## OUTPUT — memory/v5_run/telemetry/course_correction_<NNN>.md

```markdown
# Course correction NNN — <date>

## Gates covered, verdicts
| Gate | Spec-match | Evidence | Verdict (yours) | Verdict (self) | Status |
|------|-----------|----------|-----------------|----------------|--------|
| 1    | ...       | ...      | ...             | ...            | ...    |
(Status: CONFIRMED / DISPUTED / INSUFFICIENT_EVIDENCE / NOT_YET_REACHED)

## Spot-check results (v5 Gate 2 + Gate 3, every invocation in scope)
<re-run commands + output + agree/disagree; zero-copy sentinel check;
 uprobe-handler payload-copy check>

## Deviation audit (DEV-015+, and the DEV-003/009/014 ghosts)
<each new DEV: downstream-impact-accurate? + the three ghost checks>

## Cross-gate consistency + COST RULE
<anchor unchanged? old numbers gone? emulation_mode coherent?
 COST RULE: any DO usage before Gate 5? Gate 5 torn down?>

## Required actions before next phase (imperative, to the implementer)
1. ...

## STOP / GO
GO   -> proceed to phase <N+1>
STOP -> address Required actions; a follow-up course-correction must
        confirm resolution before proceeding.

## Machine-readable summary (for next invocation's Step 1)
CONFIRMED: ...
DISPUTED: ...
INSUFFICIENT_EVIDENCE: ...
NOT_YET_REACHED: ...
Last DEV number seen: DEV-...
```

---

## Stance reminders

```
- Length is not rigor. A thorough-LOOKING gate file with descriptive-
  prose evidence is INSUFFICIENT_EVIDENCE.
- "Probably fine" → go re-run the command instead.
- Disagreements are the POINT, not accusations — state them plainly,
  like the original realness audit did.
- PRIMARY_CONFIG 23.4x must NEVER change. If it has, that's the #1
  item regardless of anything else.
- v5's core risk is "the rewire was CLAIMED but the old path still
  runs." The three changes are concrete and checkable: (A) does the
  uprobe's llr pointer/offset resolve into the CXL region? (B) is
  there a payload copy in the handler's DO path? (C) does strace of
  the consumer show a sleep, or a clean busy-poll? Check the
  MECHANISM, not the adjective.
- The COST RULE is real: pre-Gate-5 DO usage wastes the user's money
  and means the WSL2-stand-in discipline broke down — flag it.
```