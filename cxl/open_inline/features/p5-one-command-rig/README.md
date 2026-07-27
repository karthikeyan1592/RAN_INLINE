# p5-one-command-rig

`make simtest`: one-command orchestration over phases p1–p4 — discover each phase's
`gates/suite.yml` (`IF-P5-SUITE`), merge and bring up the union of their compose overlays atop the
p0 base, run every discovered gate in phase order with an external timeout and full stdout/stderr
capture, aggregate one structured pass/fail/honesty ledger (`IF-P5-LEDGER`), and tear the stack
down. See `spec/{SPEC,HLD,LLD}.md` for the full design and `VERIFICATION.md` for what was actually
built and tested, including several real, disclosed findings — pre-existing gate scripts that
predate the JSON-verdict-line contract, the LLD's own `kpi_snapshot.sh` example not fitting the
static-args model, and the p2-phy-kernels-vs-p2a–p2f directory split.

## Scope

- **`helpers/discover_suites.py`** — globs and schema-validates every `pX-*/gates/suite.yml`
  (or `suite.physical.yml` under `--tier physical`); an invalid or missing manifest is reported as
  `discovered: false`, never a crash.
- **`helpers/run_gate.sh`** — external `timeout` wrapper + stdout/stderr/exit-code/duration capture
  for one gate invocation, with zero cooperation required from the invoked script.
- **`helpers/ledger_build.py`** — gate classification (`PASS/FAIL/ERROR/BLOCKED/TIMEOUT`) and the
  unconditional overall-derivation precedence (`BLOCKED > FAIL/ERROR > INCOMPLETE > PASS`).
- **`helpers/simtest_runner.py`** — the orchestrator: discover → merge overlays → `docker compose
  up` → invoke every discovered gate in phase order → aggregate → render → `docker compose down`.
  Contains no feature-specific assertion logic (P5-R15) — every gate script stays opaque.
- **`helpers/ledger_render_md.py`** — deterministic Markdown render of the JSON ledger (JSON stays
  authoritative; Markdown is never a second source of truth).
- **`helpers/lint_ledger_no_perf.sh`** — rollup no-perf lint over a run's captured artifacts,
  defense-in-depth on top of each feature's own `lint_no_perf.sh`; explicitly skipped (not
  silently) under `--tier physical`.
- **`helpers/compare_ledgers.sh`** — cross-host ledger diff, ignoring `host`/timestamps/`run_id`;
  `pins_digest`/`rigcfg_digest` and every gate's status ARE compared for equality.
- **`schemas/{suite,ledger}.schema.json`** — `oi-p5-suite/1` and `oi-p5-ledger/1`.
- **`gates/suite.yml`** addenda shipped this session for **p1-ran-baseline**, **p2a-scaffold**,
  **p2b-k5-k6**, **p2c-k1**, **p2d-k2-k3**, **p2e-k4**, **p2f-integration**, **p3-live-tap-ul-inject**,
  **p4-phy-l2-seam** — each a small, additive contract-conformance layer (thin wrapper scripts
  where a pre-existing gate script predates the JSON-verdict-line convention; zero modification of
  any already-written test or assertion logic).

## Gates this slice owns

Traceable to `spec/SPEC.md`'s Acceptance gates (P5-G1/G2). **P5-G1 runs fully locally** against
`tests/mock_suites/` with real `docker compose` (busybox services) — see `make test-g1`. **P5-G2**
(real full run + WSL2/GCP cross-comparison) needs the live rig; see `../../DEFERRED_LIVE_GATES.md`'s
p5 section for the exact command, the itemized real per-gate smoke-test results already obtained
locally for every one of the 9 real manifests, and the pass criteria.

## Build / run

```bash
make test          # unit tests (discovery, ledger classification/rollup, compare_ledgers) + P5-G1
make simtest        # the real thing: discover p1-p4's real suites, merge overlays, run, aggregate
make simtest-keep-up
make simtest-clean
```
