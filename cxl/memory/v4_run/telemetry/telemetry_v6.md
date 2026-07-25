# v6 Telemetry — audit the ONE end-to-end droplet run

You are a SEPARATE agent from the implementer of `v6_e2e_prompt.md`.
You implement nothing. You read `memory/v6_run/implementer/`, compare
against the spec and the repo, and write a course-correction. Re-run
the evidence-producing commands yourself for the gate that matters.

## Self-contained context

This project's recurring failure mode: pieces proven in isolation get
presented as a working whole. v4 proved each COMPONENT real; v5 proved
the data path but on WSL2 stand-ins and in INCONSISTENT droplet
configs; the pieces were NEVER assembled into one process on the
droplet against real /dev/dax0.0. v6 exists to do EXACTLY that one
thing and nothing else.

The specific technical crux is DEV-023: /dev/dax0.0 returns ENXIO
because of a MODE CONFLICT — daxctl --mode=system-ram (needed for
OAI's numactl --membind=CXL-node) CONSUMES the devdax char device
that OpenCL's CL_MEM_USE_HOST_PTR needs to mmap. The two halves of the
pipeline were mutually exclusive. v6 Gate 0 resolves this; v6 Gate 2
runs the assembled pipeline once.

Your job is to make sure "we ran it end-to-end" is TRUE, not claimed.
The single highest-value thing you do is independently verify Gate 2.

## STEP 1 — what's new
```bash
ls memory/v6_run/telemetry/ | sort
cat memory/v6_run/implementer/DEVIATIONS.md   # continues numbering
```
Skip CONFIRMED gates unless a new DEV affects them or the gate file
changed in git since the last course-correction.

## STEP 2 — per-gate (spec-match, evidence-sufficiency, independent
verdict) — same as prior telemetry. Paraphrase that softens criteria
is a finding. Descriptive prose instead of raw output →
INSUFFICIENT_EVIDENCE. Derive your verdict before reading theirs.

## STEP 3 — MANDATORY independent re-runs

### Gate 0 spot-check — the mode conflict is ACTUALLY resolved
```bash
# Read which Option (A/B/C) they used:
grep -A30 "Self-reported verdict\|Option" \
  memory/v6_run/implementer/gate_0.md | head -40
# Independently confirm BOTH halves coexist in ONE config:
# (1) an allocation on the CXL node:
#     run their PROOF-1-style check; confirm get_mempolicy/numa_maps
#     shows numa_node = the CXL node.
# (2) an OpenCL CL_MEM_USE_HOST_PTR buffer over the SAME region:
#     re-run their sentinel test; confirm CL_SUCCESS and that a
#     CPU-written sentinel is seen by the kernel WITHOUT
#     clEnqueueWriteBuffer (true zero-copy) — OR that the labeled
#     two-region copy path is honestly marked not-zero-copy.
```
```
If only ONE half works in the config they call "resolved" → DISPUTED,
HIGH. The whole point was getting BOTH in one config. "system-ram for
PROOF 1" in one run and "devdax for PROOF 2" in a DIFFERENT run is the
SAME unassembled state v6 exists to fix — if that's what gate_0.md
actually shows, it is NOT resolved.
```

### Gate 2 spot-check — the end-to-end run is REAL (the core check)
```bash
# This is the gate the entire prompt exists for. Re-verify each of
# (a)-(e) from the spec independently, do not trust the narrative:

# (a) ONE process tree — was OAI/workload + consumer actually
#     co-running with the uprobe attached to the live PID?
grep -A10 "pstree\|ps aux\|process tree" memory/v6_run/implementer/gate_2.md
#     If you can re-launch briefly: start the assembled pipeline,
#     `pstree -p <root>`, confirm workload + consumer share the tree
#     and `cat /proc/<workload_pid>/maps` shows the bpftime agent.

# (b) the LLR address from a LIVE descriptor resolves to CXL memory —
#     NOT a separate malloc test. Find the descriptor's llr address
#     in their evidence, and confirm they showed its numa node via
#     numa_maps/get_mempolicy on THAT address. A generic "PROOF 1
#     showed mbind works" is NOT this — it must be the actual address
#     a real descriptor carried during the run.
grep -A8 "llr.*0x\|descriptor.*addr\|numa_node" memory/v6_run/implementer/gate_2.md

# (c)+(d) bit-exactness THROUGH the assembled path for the Z used:
#     re-run / confirm bit_diff=0 for that Z. If the run used Z=224
#     with un-extended tables (DEV-011), bit-exactness CANNOT hold —
#     so a claimed PASS with Z=224 and bit_diff=0 is internally
#     contradictory → DISPUTED. Check the Z and the tables together.
cat paper/results/e2e_droplet.csv
grep -i "z=\|Zc\|bit_diff\|lifting" memory/v6_run/implementer/gate_2.md

# (e) the e2e number is source=measured and genuinely from this run:
grep -i "source" paper/results/e2e_droplet.csv
```
```
DISPUTED / HIGH conditions for Gate 2:
  - The "end-to-end" number is actually two separately-measured
    pieces added together (the original sin — check whether
    e2e_droplet.csv's value equals interception + ocl from earlier
    CSVs rather than a fresh single-run measurement).
  - (b) uses a generic mbind/malloc test, not the live descriptor's
    address.
  - Z=224 + claimed bit_diff=0 (contradiction — wrong tables can't
    produce zero diff).
  - "one process tree" is asserted but no pstree/maps evidence.
  - PoCL-copy presented as zero-copy without the sentinel proof.

A run that genuinely assembles (a)+(b)+(c)+(e) but FAILS (d) because
of Z=224 tables is an HONEST PARTIAL — confirm it's LABELED partial,
not relabeled pass. That labeling is itself the thing to verify.
```

## STEP 4 — deviations + the persistent ghosts
```bash
cat memory/v6_run/implementer/DEVIATIONS.md
```
Check downstream-impact accuracy (go read the named file now). And
the carried ghosts:
```
DEV-011 (Z=224 wrong shift tables): if the e2e run used Z=224, was
  bit-exactness (Gate 2d) honestly reported as failing / tables
  extended / Z switched to 384? A silent bit_diff=0 at Z=224 is wrong.
DEV-022 (no PMU → no CXLMemSim sweep): still legitimately deferred;
  confirm RESULTS_SUMMARY states it, doesn't fake a sweep.
DEV-023 (the ENXIO mode conflict): Gate 0 is its resolution — is the
  resolution REAL (both halves, one config) per STEP 3?
```

## STEP 5 — cross-cutting
```bash
# Anchor unchanged:
grep -A2 "per_slot_latency_us\|overshoot_factor" calibration_check.txt
# Old arithmetic numbers not resurrected:
grep -rn "12036\|11727" paper/ 2>/dev/null
# Is the NEW e2e number a real single measurement, or a sum? Compare
# e2e_droplet.csv's value against (interception_only + gpu_compute_full)
# from latency_ladder_v2_v5.csv — if it EQUALS the sum, it's arithmetic
# wearing an "e2e" label. Flag HIGH.
# Droplet torn down at end:
# (look for teardown.sh evidence + status.sh showing it gone)
```

## OUTPUT — memory/v6_run/telemetry/course_correction_<NNN>.md
```
Same structure as prior telemetry: verdict table; STEP 3 re-run
results (Gate 0 both-halves + Gate 2 (a)-(e)); deviation/ghost audit;
cross-cutting (anchor, sum-vs-measured, teardown); required actions;
STOP/GO; machine-readable summary.
```

## Stance
```
- The ONE question for v6: did a SINGLE run on the droplet take an
  LLR that an unmodified workload placed in REAL CXL memory, decode
  it bit-exactly via OpenCL reading that CXL memory, and return it —
  producing ONE measured latency number? Every check serves
  answering that honestly.
- "Assembled" means one process tree, one run, one number. Pieces in
  separate runs is the state we're LEAVING, not evidence.
- An e2e number that equals the sum of previously-separate numbers is
  the original mislabel reincarnated. Check for it explicitly.
- Z=224 and bit_diff=0 cannot both be true with un-extended tables.
- An honest PARTIAL (e.g. path assembled, bit-exactness deferred to
  table extension) is a fine outcome to CONFIRM — relabeling it as
  full PASS is not.
- 23.4× anchor never moves.
```