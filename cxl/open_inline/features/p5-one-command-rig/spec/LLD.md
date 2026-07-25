# p5-one-command-rig — LLD

Companion to [`SPEC.md`](SPEC.md) / [`HLD.md`](HLD.md). Design document; paths are the layout the
implementation must produce.

## Module breakdown

```
features/p5-one-command-rig/
  Makefile                       # `make simtest`, `make simtest-keep-up`, `make simtest-clean`
  helpers/
    simtest_runner.py             # main orchestrator: discover -> merge -> up -> invoke -> down
    discover_suites.py            # globs pX-*/gates/suite.yml, schema-validates (IF-P5-SUITE)
    run_gate.sh                   # per-gate: `timeout` wrapper + stdout/stderr capture + classify
    ledger_build.py               # aggregates captured results -> oi-p5-ledger/1 JSON
    ledger_render_md.py           # JSON -> deterministic Markdown
    lint_ledger_no_perf.sh        # rollup no-perf lint (P5-R8), reuses each feature's pattern list
    compare_ledgers.sh            # cross-host diff, ignoring host/timestamp/digest fields (P5-R9)
  schemas/
    suite.schema.json             # oi-p5-suite/1 (validates every feature's gates/suite.yml)
    ledger.schema.json            # oi-p5-ledger/1
  tests/
    mock_suites/                  # synthetic suite.yml + mock gate scripts (pass/fail/timeout/
                                   # blocked cases) — lets P5-G1 run before p1-p4 ship real suites
```

Language split (Q1, Open questions): Makefile as the single documented entrypoint; Python for
manifest/schema validation and ledger JSON handling (ergonomic JSON/YAML + schema validation);
`run_gate.sh` stays POSIX shell so it composes trivially with the `timeout` utility and any gate
script regardless of the script's own language.

## Public APIs (CLI contracts)

```text
make simtest [TIER=sim|physical] [KEEP_UP=1] [ONLY_PHASE=p1,p3] [TIMEOUT_SCALE=1.0]
  -> exit 0 iff ledger.overall == PASS; non-zero otherwise (1 on FAIL, 2 on runner ERROR,
     3 on BLOCKED — mirrors the shared exit-code convention at the whole-run level too)

simtest_runner.py --tier {sim|physical} [--keep-up] [--only-phase ID,...]
                   [--timeout-scale F] [--run-id ID] [--artifacts-dir DIR]
  stdout: progress log (human); final line: path to the written ledger JSON.

discover_suites.py --tier {sim|physical} [--root DIR]
  stdout: JSON array of validated manifest objects (or a per-manifest schema-error list on exit 2)

run_gate.sh --id ID --type unit|integration --timeout-s N --artifacts-dir DIR -- <script> [args...]
  exit 0|1|2|3 (script's own code, passed through) | 124 (external timeout fired, remapped to
  ledger status TIMEOUT by the caller — run_gate.sh itself does not invent a new exit code)
  writes: <artifacts-dir>/{stdout.log,stderr.log,exit_code,duration_s}

ledger_build.py --run-id ID --artifacts-dir DIR --manifests FILE.json --results FILE.json
  -> writes artifacts/p5/<run-id>/ledger.json (oi-p5-ledger/1)

compare_ledgers.sh LEDGER_A.json LEDGER_B.json
  exit 0 iff every gate's status and overall match, modulo {host, started_utc, finished_utc,
  pins_digest is compared for equality — not exempted, since both hosts must run the same pins}
```

## Data structures & formats

### `IF-P5-SUITE` — `oi-p5-suite/1` (each feature's `gates/suite.yml`)

```yaml
schema: oi-p5-suite/1
phase: p1                          # p1..p4 today; p6 defines its own PHYSICAL-tier values
feature: p1-ran-baseline
compose_overlays:                  # paths relative to the feature's own docker/ dir
  - docker/compose.p1.yml
setup: []                          # optional: scripts run once before this phase's gates
teardown: []                       # optional: scripts run once after this phase's gates
gates:
  - id: p1-g1
    type: unit
    script: helpers/kpi_snapshot.sh      # relative to the feature root; p5 never inspects content
    args: []
    timeout_s: 90
  - id: p1-g2
    type: integration
    script: helpers/assert_ecpri.sh
    args: ["--seconds", "30"]
    timeout_s: 660                       # P1-R9's 600 s soak + margin
```

Validation rules: `phase` and `feature` non-empty; `compose_overlays` paths must resolve and exist
relative to the feature root; each `gates[]` entry's `id` unique within the manifest; `type` ∈
{unit, integration}; `timeout_s` a positive number; `script` path must exist and be executable.
A manifest failing validation is marked `discovered: false` with a `validation_error` field
attached (same phase-level `NOT_DISCOVERED` annotation as a missing manifest, §Data structures —
not a distinct mechanism) — it never silently drops out of the ledger (P5-R14).

### `IF-P5-LEDGER` — `oi-p5-ledger/1`

```json
{
  "schema": "oi-p5-ledger/1",
  "run_id": "20260721T140500Z-a1b2c3d",
  "started_utc": "...", "finished_utc": "...",
  "tier": "sim",
  "host": {"kind": "wsl2|gcp", "kernel": "...", "n2_standard_16": true},
  "pins_digest": "sha256:...",
  "rigcfg_digest": "sha256:...",
  "phases": [
    {
      "phase": "p1", "feature": "p1-ran-baseline",
      "discovered": true,
      "gates": [
        {"id": "p1-g1", "type": "unit", "status": "PASS", "exit_code": 0,
         "duration_s": 12.3, "verdict": {"...": "..."},
         "artifacts": "artifacts/p5/<run-id>/p1/p1-g1/"},
        {"id": "p1-g2", "type": "integration", "status": "PASS", "exit_code": 0,
         "duration_s": 601.4, "verdict": {"...": "..."},
         "artifacts": "artifacts/p5/<run-id>/p1/p1-g2/"}
      ]
    },
    {"phase": "p2", "feature": "p2-phy-kernels", "discovered": false, "gates": []},
    {"phase": "p3", "feature": "p3-live-tap-ul-inject", "discovered": false, "gates": []},
    {"phase": "p4", "feature": "p4-phy-l2-seam", "discovered": false, "gates": []}
  ],
  "overall": "INCOMPLETE",
  "performance_claims": [],
  "honesty_notes": [
    "SIM proves function and integration only — no number above is a performance claim.",
    "Per-phase honesty-ledger caveats live in each feature's own SPEC.md; this ledger references, not duplicates, them."
  ]
}
```

Per-gate `status` ∈ `{PASS, FAIL, ERROR, BLOCKED, TIMEOUT}` — `NOT_DISCOVERED` is **not** a gate
status (no gate object is ever synthesized for an undiscovered phase); per-phase `discovered` ∈
`{true, false}` is the sole mechanism for reporting "no suite present." If `discovered=false`,
`gates: []`, and the phase is annotated `NOT_DISCOVERED` at the **phase** level in the ledger
(one flag, not a gate array entry) — distinct from both `PASS` and `FAIL`, so a partial run is
never misreported as green (P5-R14).

**`overall` derivation (unconditional precedence, no case-dependent exceptions):**
1. `BLOCKED` if any gate's status is `BLOCKED` — always wins, regardless of any other gate's
   status (this is the rule the SCTP-precondition scenario in §Error handling depends on: once a
   precondition gate is `BLOCKED`, `overall` is `BLOCKED` even if downstream phases that couldn't
   run are separately marked `ERROR`);
2. else `FAIL` if any gate's status is `FAIL` or `ERROR`;
3. else `INCOMPLETE` if any phase is `NOT_DISCOVERED` (and no gate was `BLOCKED`/`FAIL`/`ERROR`);
4. else `PASS` (every discovered gate is `PASS`, no phase is `NOT_DISCOVERED`).

This is a strict, mechanically checkable precedence — `BLOCKED > FAIL/ERROR > INCOMPLETE > PASS`
— with no "only when the blocked gate is upstream of the failure" qualifier: that qualifier is
undecidable by the runner (it cannot generally prove causality between a blocked precondition and
a downstream `ERROR`), so rule 1 applies whenever *any* gate is `BLOCKED`, full stop.

## Configuration (YAML/env schema)

```yaml
# .env / environment consumed by simtest_runner.py
OI_TIER: sim                      # sim | physical
OI_P5_TIMEOUT_SCALE: "1.0"        # multiplies every gate's timeout_s
OI_P5_RUN_ID: ""                  # empty = auto-generate <utc>-<short-sha>
OI_P5_ARTIFACTS_DIR: artifacts/p5
OI_P5_ONLY_PHASE: ""              # comma list; empty = all discovered phases
OI_P5_KEEP_UP: "0"                # 1 = skip teardown (debugging)
```

Makefile targets:
```
simtest:        runs simtest_runner.py --tier $(OI_TIER) [args from env above]
simtest-keep-up: same, --keep-up
simtest-clean:   docker compose -f <last-used union> down -v; rm -rf artifacts/p5/<run-id>
```

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| No `gates/suite.yml` found for a phase | `discover_suites.py` glob returns nothing for that phase id | ledger marks that phase `discovered: false`; `overall` becomes `INCOMPLETE`, not silently `PASS` |
| Manifest fails schema validation | `discover_suites.py` JSON-schema check | phase marked `discovered: false` with the validation error attached; run continues for other phases |
| `compose_overlays` path missing | pre-flight existence check before `docker compose up` | runner ERROR (exit 2), no partial compose-up attempted |
| `docker compose up` fails (any service) | non-zero from the compose CLI | runner ERROR (exit 2); teardown is still attempted (best-effort) before exiting |
| Gate script missing/non-executable | `run_gate.sh` pre-check | that gate's status = `ERROR`; other gates in the phase still run (no cross-gate abort by default) |
| Gate exceeds `timeout_s` | external `timeout` utility returns 124 | that gate's status = `TIMEOUT`; run continues to the next gate |
| Gate's last stdout line is not valid JSON | `run_gate.sh`/`ledger_build.py` parse check | gate's status forced to `ERROR` regardless of exit code, with the raw stdout tail attached for debugging |
| Ledger-lint (P5-R8) finds a forbidden pattern | `lint_ledger_no_perf.sh` over the built ledger + captured logs | separate `ledger-lint` gate fails; `overall` becomes `FAIL` even if every individual gate was `PASS` |
| WSL2 vs. GCP ledgers disagree | `compare_ledgers.sh` (run manually or as a scheduled CI matrix job) | reported as a P5-R9 gate failure, not folded into either individual run's own `overall` |
| SCTP/precondition blocked session-wide | a `p1`/`p0` gate itself exits 3 | that gate's status = `BLOCKED`; if every downstream phase consequently can't run (rig never came up), the runner marks them `ERROR` (rig unavailable); `overall` = `BLOCKED` regardless, per the unconditional rollup precedence (§Data structures): rule 1 (`BLOCKED` wins) fires whenever any gate is `BLOCKED`, independent of the `ERROR` statuses also present |

## Test plan (per requirement)

| Req | Test |
|---|---|
| P5-R1 | Mock-suite integration test: two mock phases with distinct `compose_overlays` (trivial no-op services); assert the runner merges both overlays into one `docker compose up`, runs both phases' gates in declared order, and tears down exactly once. |
| P5-R2/R3 | Schema tests: valid manifests accepted; manifests missing required keys / bad `type` values / nonexistent `script` paths rejected with `NOT_DISCOVERED` + attached error, never a runner crash. |
| P5-R4 | Reuse-of-convention test: feed `run_gate.sh` a mock script that exits each of 0/1/2/3 with a valid JSON verdict line; assert the runner's classification matches (PASS/FAIL/ERROR/BLOCKED) without requiring any extra JSON fields. |
| P5-R5 | Timeout test: mock script that sleeps past its declared `timeout_s`; assert status `TIMEOUT`, not `FAIL`; assert `OI_P5_TIMEOUT_SCALE=2.0` doubles the effective bound. |
| P5-R6 | Artifact-layout test: after a mock run, assert `stdout.log`/`stderr.log`/exit code/duration exist at the documented path for every gate, including ones that crashed before producing any JSON. |
| P5-R7 | Ledger schema test: validate a built ledger against `ledger.schema.json`; assert `overall` derivation matches the documented rule across constructed PASS/FAIL/BLOCKED/TIMEOUT/NOT_DISCOVERED combinations. |
| P5-R8 | Lint test: inject a forbidden pattern (e.g. a mock gate's verdict JSON containing `"max_latency_us": 500` as an asserted field) into a mock run; assert `lint_ledger_no_perf.sh` fails and `overall` flips to `FAIL` even though the mock gate itself exited 0. |
| P5-R9 | `compare_ledgers.sh` unit test: two ledgers differing only in `host`/timestamps/`run_id` compare equal; two ledgers differing in any gate's `status` compare unequal, with a field-level diff printed. |
| P5-R10 | Mock scenario: a phase's gate exits 3 (BLOCKED); assert `overall` = `BLOCKED`, not `FAIL`, and downstream phases that could not run are marked `ERROR` with the blocked gate cited as cause. |
| P5-R11 | Manual/CI check: `make simtest` with only `.env` populated brings up, runs, and tears down without any other manual intervention (grep the Makefile/runner for any interactive prompt — must find none). |
| P5-R12 | `--tier physical` test (stub-level only): assert discovery scans a different manifest root/glob and that no `--tier sim` code path is invoked; full PHYSICAL manifest content is `p6`'s test responsibility, not this feature's. |
| P5-R13 | `lint_ledger_no_perf.sh` self-test: confirm the lint is applied when `--tier sim` (default) and confirm the code path that would apply it is explicitly skipped (not silently vacuous) when `--tier physical`, per the README carve-out. |
| P5-R14 | Partial-discovery test: only a mock `p1` manifest present, `p2`-`p4` absent; assert ledger marks p2-p4 `discovered: false`, `overall` = `INCOMPLETE`, and the Markdown render visibly lists the missing phases rather than omitting them. |
| P5-R15 | Code-review check: grep `helpers/` for any feature-specific assertion logic (regexes/thresholds naming a specific feature's counters) — must find none; all such logic must live inside the invoked scripts, not the runner. |
| **P5-G1** | Full run of the `tests/mock_suites/` fixture set (pass/fail/timeout/blocked/malformed-manifest cases) on both a local dev machine and CI; ledger `overall` matches the fixture's expected value in every case. |
| **P5-G2** | Once p1–p4 ship real `gates/suite.yml` files: `make simtest` on WSL2 and on the GCP `n2-standard-16` VM; `compare_ledgers.sh` reports equal. |

## Open questions

1. **Q1 — Runner implementation language.** Python recommended for manifest/schema/ledger handling
   (ergonomic JSON/YAML + jsonschema libraries widely available); shell (`run_gate.sh`) for the
   actual per-gate invocation to compose trivially with `timeout` and stay language-agnostic toward
   whatever a gate script is written in. Final choice deferred to implementation; not a spec
   correctness parameter.
2. **Q2 — `gates/suite.yml` addenda for already-written features.** p1/p2/p3/p4's specs predate
   `IF-P5-SUITE`; each needs a small, additive, non-breaking manifest file added at its own
   implementation time (already anticipated by p1 HLD's `IF-P1-ASSERT` note). This spec does not
   amend those features' specs; tracked here as a known, expected follow-up, not a contradiction.
3. **Q3 — `--only-phase` interaction with the single merged compose-up.** Running only `P3`/`P4`
   without `P1`'s fronthaul overlay up is meaningless (P3/P4 need the live rig P1 establishes).
   Resolution options: (a) `--only-phase` still brings up every *overlay* but only *runs the gates*
   of the selected phases, or (b) the runner rejects an `--only-phase` selection whose declared
   overlays have unmet prerequisites. Decided at implementation; likely (a), since it's simpler and
   never produces a misleading "phase passed" on an incomplete rig.
4. **Q4 — Cross-host comparison automation.** `compare_ledgers.sh` exists as a tool; whether it
   runs as a scheduled CI matrix job (WSL2 runner + GCP runner, same commit) or is invoked manually
   per release is left open — either satisfies P5-R9 as written (the requirement is about the
   runner's behavior being identical, not about how often the comparison is exercised).
5. **Q5 — `run_id` scheme.** `<UTC-timestamp>-<short-git-sha>` proposed (human-sortable,
   collision-resistant enough for dev use); a UUID alternative is equally valid — decided at
   implementation, not a correctness parameter.
6. **Q6 — PHYSICAL-tier ledger schema deltas.** Whether `oi-p5-ledger/1` needs a new version once
   `p6` adds real measurement fields (explicitly permitted at PHYSICAL tier) is `p6`'s question to
   answer when it's written; this spec only reserves the `tier` field and the carve-out (P5-R13).
