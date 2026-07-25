# v4 Memory Protocol — read this BEFORE starting
# cursor_cxl_poc_prompt_v4.md

## Why this file exists

A prior audit found that this project's biggest failure mode is:
the SPEC says one thing, the IMPLEMENTATION does another, and the
SELF-REPORT says the spec's thing happened. Example: spec said "eBPF
ring buffer interception"; implementation used "weak-symbol + Unix
socket"; self-report said "eBPF transparently intercepts srsRAN" and
recorded numbers as `source=measured` that were actually arithmetic.

This protocol does not ask you to be more careful (that didn't work
last time). It changes WHAT gets produced: for every gate in v4, you
will write a file containing the SPEC, the COMMANDS you ran, and the
RAW UNEDITED OUTPUT — separately from your own PASS/FAIL opinion. A
different agent (telemetry pass, separate prompt) will read these
files and independently re-derive a verdict from the raw output,
ignoring your opinion where it disagrees. Your job is to make this
folder ACCURATE and COMPLETE, not to make it say PASS.

---

## Folder structure — create this first

```bash
mkdir -p memory/v4_run/implementer/{phase0,phase1,phase2,phase3,phase4,phase5,phase6}
touch memory/v4_run/implementer/DEVIATIONS.md
touch memory/v4_run/implementer/CHECKPOINT_LOG.md
touch memory/v4_run/STATUS.md
mkdir -p memory/v4_run/telemetry
```

```
memory/run/
  implementer/
    phase0/gate_0.1_bpftime.md
    phase0/gate_0.2_netns.md
    phase0/gate_0.3_cxlmemsim.md
    phase0/gate_0.4_multisocket.md
    phase1/gate_1_bitexact.md
    phase2/gate_2_interception.md
    phase3/gate_3_sustained_nic.md
    phase4/gate_4_cxl_sweep.md
    phase5/gate_5_ablation.md
    phase6/gate_6_artifacts.md
    DEVIATIONS.md          <- append-only, see below
    CHECKPOINT_LOG.md       <- append-only, one line per checkpoint
  STATUS.md                 <- overwritten each checkpoint
  telemetry/                <- you do NOT write here. A different
                               agent writes course_correction_NNN.md
                               here. You READ it at the start of each
                               new session (see "Start of session"
                               below).
```

---

## The gate file — strict template, use for EVERY gate

One file per gate, named exactly as shown in the folder structure
above. Copy this template, fill every section. Do not omit a
section even if "none" — write "none" explicitly.

```markdown
# Gate <id> — <name, copied verbatim from v4 prompt>

## Spec (verbatim from cursor_cxl_poc_prompt_v4.md)

<paste the EXACT PASS/FAIL criteria text from the v4 prompt for this
gate. Do not paraphrase, do not summarize. If the telemetry agent
finds this section doesn't match the v4 file word-for-word, that
mismatch itself gets flagged — copy-paste, don't retype.>

## Commands run

<every command, in order, with a one-line timestamp before each.
Include commands that FAILED or were exploratory — this is a log,
not a cleaned-up writeup.>

```bash
# 2026-06-15 14:32:01
some command here
```

## Raw evidence

<PASTE actual stdout/stderr/file-contents here, unedited. If the
evidence is a file (e.g. a CSV), paste its actual contents (or the
first/last 20 lines + line count if huge) — not a description of
what's in it. If the evidence is "bpftool prog tracelog showed N
events", the evidence IS the tracelog output, not the sentence
"tracelog showed N events".

BAD:  "Confirmed the ring buffer received 2400 events."
GOOD: <actual terminal output showing the 2400 events or the counter
       command's output: `$ wc -l descriptor_log.csv` -> `2401
       descriptor_log.csv` (2400 + header)>>

## Self-reported verdict

PASS | FAIL | PARTIAL

<one sentence: why you believe this>

## Deviations from spec

<list each place where what you DID differs from what v4 SPECIFIED,
however minor. "none" if genuinely none. Each deviation listed here
must ALSO appear in DEVIATIONS.md (see below) — this section is a
local pointer/summary, DEVIATIONS.md is the canonical cross-phase log.>

## Files produced/modified

<list with paths, relative to repo root>

## Timestamp

<ISO8601, when this gate file was finalized>
```

---

## DEVIATIONS.md — the most important file in this folder

Append an entry HERE, BEFORE continuing, the MOMENT you do anything
that isn't EXACTLY what v4 specified. Examples of things that require
an entry:

```
- v4 says attach to `nrLDPC_coding_segment_decoder`; the symbol
  doesn't exist in this build, you attached to
  `nrLDPC_coding_decoder` instead.
- v4 says N>=1000 slots for Phase 5; you ran N=200 because the full
  run was taking too long, planning to extend later.
- v4 says CL_MEM_USE_HOST_PTR; PoCL silently fell back to a copy, so
  effectively you're NOT getting zero-copy even though the API call
  looks identical.
- v4's Gate 0.1 fallback says "kernel uprobes, report 9941ns as
  measured"; you instead used a third approach not in the prompt
  (e.g. perf_event-based sampling) because X.
- A file path in v4 (e.g. ldpc_luts_impl.cpp) doesn't exist at the
  pinned commit; you found the equivalent at a different path.
```

Entry format (append, do not edit prior entries):

```markdown
## DEV-<NNN> — <phase/gate> — <2026-06-15 14:32>

**Spec said:** <quote>
**Did instead:** <what>
**Why:** <reason>
**Downstream impact:** <does this change what any LATER gate's
  evidence actually means? e.g. "Phase 5's emulation_mode column
  must say kernel_uprobe not bpftime" — be specific about WHICH
  later files/columns/claims need to reflect this>
```

`DEV-NNN` numbers are sequential across the whole run, not per-phase.
This file is the single place the telemetry agent looks for "did the
implementation quietly become something other than what was asked."

---

## CHECKPOINT_LOG.md — one line per checkpoint, append-only

```
2026-06-15 14:35  Phase 0.1 gate file written. Verdict: PASS (bpftime)
2026-06-15 16:02  Phase 0.2 gate file written. Verdict: PASS
2026-06-15 16:50  DEV-001 logged (symbol name fallback)
2026-06-15 17:10  Phase 0.3 gate file written. Verdict: FAIL ->
                   4.5 deferral path noted
...
```

---

## STATUS.md — overwritten every checkpoint, single source of "where
##                are we right now"

```markdown
# v4 run status — last updated 2026-06-15 17:10

Current phase: 0
Current gate: 0.4 (in progress)
Gates completed: 0.1 (PASS/bpftime), 0.2 (PASS), 0.3 (FAIL/deferred)
Open deviations: DEV-001 (symbol fallback, downstream impact noted)
Telemetry status: not yet run / last run = course_correction_002.md
  (if telemetry/ is non-empty, name the latest file here)
Blocking issues: none / <describe>
Next action: 0.4 multi-socket check, then Phase 1
```

---

## Start of session — ALWAYS do this first

```bash
ls memory/v4_run/telemetry/
```

If any `course_correction_NNN.md` exists that you haven't addressed
yet (check STATUS.md's "Telemetry status" line from last session):
READ IT FIRST. It may contain corrections to gates you already marked
PASS. Address its instructions BEFORE continuing with new phases —
treat it as having priority over "the next phase in v4", the same way
v4 itself says a failed gate blocks progress.

---

## End of session / end of phase — ALWAYS do this

1. Every gate attempted this session has a complete gate file (all
   six sections, no placeholders left as "TBD").
2. DEVIATIONS.md has an entry for every deviation, including ones
   that felt too small to mention. (If genuinely none this session,
   that's fine — don't invent any.)
3. CHECKPOINT_LOG.md has a line for everything above.
4. STATUS.md is rewritten to reflect current reality.
5. Commit:
```bash
git add memory/v4_run/implementer/
git commit -m "v4 memory: phase N checkpoint"
```
6. Per v4's own instruction: report back to the user at phase
   boundaries. ADD to that report: "memory/v4_run/implementer/
   updated through phase N; <M> deviations logged; ready for
   telemetry pass."

---

## One more thing: when in doubt, over-document

If you're not sure whether something counts as a "deviation" —
log it. If you're not sure whether output is "raw enough" — paste
more, not less. A gate file that's too long because it contains
three full command outputs is fine. A gate file that says "ran the
verification, it passed" is not a gate file, regardless of what its
Self-reported verdict section says.