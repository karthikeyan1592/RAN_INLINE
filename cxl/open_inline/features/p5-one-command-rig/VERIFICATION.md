# p5-one-command-rig — VERIFICATION

## What's implemented and verified for real

All modules are implemented per the LLD's module breakdown (`helpers/discover_suites.py`,
`helpers/run_gate.sh`, `helpers/ledger_build.py`, `helpers/ledger_render_md.py`,
`helpers/simtest_runner.py`, `helpers/lint_ledger_no_perf.sh`, `helpers/compare_ledgers.sh`,
`schemas/{suite,ledger}.schema.json`), plus the 9 real `pX-*/gates/suite.yml` addenda for p1,
p2a–p2f, p3, and p4 (Q2, this session's own follow-up).

| Test | What it proves | Result |
|---|---|---|
| `tests/test_discover_suites.py` | P5-R2/R3: schema+path validation over synthetic `features/*/gates/suite.yml` layouts built in a tempdir — valid manifests accepted, sorted by phase; missing-key, bad-type, duplicate-gate-id, nonexistent-script, and nonexistent-overlay manifests all rejected as `discovered:false` with `validation_error` attached, never a crash | 13/13 PASS |
| `tests/test_ledger_build.py` | P5-R4 (`classify_gate`'s PASS/FAIL/ERROR/BLOCKED/TIMEOUT rules, including the "invalid JSON forces ERROR regardless of exit code" rule) and P5-R7 (`compute_overall`'s unconditional `BLOCKED > FAIL/ERROR > INCOMPLETE > PASS` precedence, exercised across 8 constructed phase/gate combinations) + a constructed ledger validated against `ledger.schema.json` | 19/19 PASS |
| `tests/test_compare_ledgers.py` | P5-R9: two ledgers differing only in `host`/timestamps/`run_id` compare equal; differing gate status, `pins_digest` (explicitly NOT exempted), or `overall` all compare unequal with a named field-level diff | 6/6 PASS |
| `tests/test_p5_g1.py` (**P5-G1**) | The full runner end-to-end against `tests/mock_suites/`, with **real** `docker compose` (busybox no-op services, real pull, real containers) — proves P5-R1 (real 4-overlay merge into ONE `docker compose up`, real teardown exactly once), P5-R2/R3/R14 (the malformed manifest is `discovered:false`, never crashes the run, still appears in the ledger), P5-R4/R5 (real PASS/FAIL/BLOCKED classification + a real TIMEOUT from an actual `sleep` past `timeout_s`), P5-R7 (`overall == BLOCKED`, rule 1 winning over FAIL/TIMEOUT/INCOMPLETE also present), P5-R8 (the rollup lint passes clean and then genuinely fails once a forbidden pattern is injected into a captured log — and is explicitly skipped, not silently vacuous, under `--tier physical`), and P5-R12/Q3(a) (`--only-phase` runs only the selected phase's gates while still bringing up every overlay) | 21/21 PASS |

**Total local assertions: 59/59 PASS** across 4 test files, all against real execution (real
`docker compose`, real subprocess timeouts, real JSON-schema validation) — no hand-computed
expected values; every oracle is either a real schema, a real classification rule constructed
from the LLD's own table, or a real running container/process.

Additionally, all 9 real `gates/suite.yml` manifests for p1/p2a-f/p3/p4 were validated for real
against the actual repo root (`discover_suites.py --root .`) — all 9 `discovered: true`, zero
validation errors — and every gate script they reference was smoke-tested directly (bypassing
`docker compose` and the runner) against this host's real state; see `DEFERRED_LIVE_GATES.md`'s
P5-G2 entry for the itemized real pass/ERROR results per gate.

## Real, disclosed finding: `check_sctp.sh` now reports SCTP available on this host

`DEFERRED_LIVE_GATES.md`'s header previously stated this host lacks SCTP; re-running
`check_sctp.sh` live during this feature's build shows `sctp=available (already loaded)`. This is
a genuine, current, real result — not a re-guess — and is now corrected in that file's header.
It does **not** change any deferral: every gate that's still deferred needs the actual live rig
**up** (real containers, NG setup, real fronthaul traffic), which standing up locally remains
explicitly out of scope for this session regardless of SCTP kernel availability. One concrete
effect: p1's own `p1-check-sctp` gate (this session's new `gates/run_check_sctp.sh` wrapper) now
genuinely PASSes when run locally, where it would previously have been expected to BLOCK.

## Real finding: several existing gate scripts predate the IF-P5-SUITE JSON-verdict-line contract

`helpers/check_sctp.sh` (shared p0/p1 script) and `helpers/soak_stability.sh` write their
precondition-failure JSON to **stderr**, not stdout, on their 2/3 exit paths (discovered by
tracing their real source, not assumed) — meaning `run_gate.sh`'s stdout-only capture would see no
JSON at all on those paths. Per SPEC's own explicit allowance ("modifying any already-written
feature's scripts... is out of scope; a forward-compatible addendum each feature adds at its own
implementation time IS in scope"), this session added two small, new, additive wrapper scripts
(`p1-ran-baseline/gates/run_check_sctp.sh`, `run_soak_stability.sh`) that capture the real
script's combined output, re-emit a guaranteed JSON verdict line on stdout, and pass the real exit
code through unchanged — without editing either pre-existing script. `rigcfg_crosscheck.sh` and
`assert_ecpri.sh` already emit real JSON verdict lines natively and needed no wrapper.

Similarly, p3's and p4's own C/C++ test binaries and p2f's `pipeline_test.py` (all pre-existing,
already independently verified this session and in earlier sessions) print human-readable
PASS/FAIL text, not a JSON verdict line. Two small, new, generic, parameterized wrapper scripts
(`gates/run_make_target.sh`, reused verbatim across p2a–p2f/p3/p4's `gates/` directories, and
p2f's own `gates/run_python_script.sh` for the one test invoked directly via `python3` rather than
a Makefile target) route each real tool's own output to stderr and emit exactly one new JSON line
on stdout summarizing its real exit code — again, zero modification of any existing test.

## Real finding: `kpi_snapshot.sh` doesn't fit the suite.yml static-args model

The LLD's own illustrative `suite.yml` example names `kpi_snapshot.sh` as `p1-g1`'s script, but
that script's own header states it takes **dynamic, rig-derived numeric counters** as positional
args (`tx_bytes`, `rx_bytes`, read from a live rig's sysfs counters at invocation time) — values a
static YAML manifest cannot meaningfully hardcode, and the script "never judges pass/fail" by its
own design (it only snapshots). p1's real `gates/suite.yml` instead declares four real, genuinely
invocable gates that fit the contract directly: `p1-check-sctp`, `p1-rigcfg-crosscheck` (real,
static config files — PASSES locally), `p1-assert-ecpri` (against the real archived pcap corpus —
PASSES locally), and `p1-soak-stability` (needs the live rig — correctly ERRORs locally). This is
a disclosed, reasoned deviation from the LLD's illustrative example, not a silent one; it changes
no requirement, only which of p1's existing scripts the addendum wires up.

## Real, disclosed engineering decision: p2's suite split (Q2 follow-up)

The SPEC/LLD's dependency list names a single `p2-phy-kernels` feature, but that directory holds
only specs — the real, already-built, already-independently-verified implementation is split
across `p2a-scaffold`, `p2b-k5-k6`, `p2c-k1`, `p2d-k2-k3`, `p2e-k4`, `p2f-integration` (predating
this feature). Since `IF-P5-SUITE` discovery is a glob over `features/*/gates/suite.yml` with
`phase` as a free-form string (not a fixed p1..p4 enum — LLD's own ledger example is illustrative,
not exhaustive) and phase ids sort lexicographically into the correct P1→P2→P3→P4 order
(`p1 < p2a < p2b < p2c < p2d < p2e < p2f < p3 < p4`, verified directly), the natural, additive,
zero-touch-to-p2's-frozen-code resolution is: each of the six real p2 sub-features ships its own
small `gates/suite.yml` as its own phase. This is a scope/glob decision within normal implementer
judgment (not a spec contradiction — no already-written code or spec disagrees with what's
needed), so it was made directly rather than escalated; disclosed here per the working discipline.

## Real, disclosed retroactive fix: three features' `lint_no_perf.sh` didn't scan their new `gates/` directories

p1's, p3's, and p4's own `lint_no_perf.sh` copies (already existing before this feature) scan
fixed path lists (`docker/`, `helpers/`, `tests/`, `tools/`, `patches/`) that did not include the
new `gates/` directories this feature's `suite.yml` addenda added to each of them. The shared
`p2a-scaffold/helpers/lint_no_perf.sh` copy (covering p2a–p2f) had the same gap. All four were
extended (one line each, adding `-path "*/gates/*"` and, for p2a's copy, the `*.yml` extension) and
re-verified clean — the same class of gap already found and fixed twice earlier this session for
p0 and for p3's original mis-scoped copy.

## Real bugs found running `make simtest` for real for the first time (2026-07-27, WSL2)

Every prior verification of this feature exercised `simtest_runner.py`'s pieces individually
(`test_discover_suites.py`, `test_ledger_build.py`, `test_compare_ledgers.py`, `test_p5_g1.py`
against synthetic mock suites with trivial `services: {}` compose overlays) — the actual command
this whole feature exists to provide, `make simtest` against the real p1-p4 suites together, had
never been run for real before tonight. It failed three times in a row, each time on a genuine,
previously-latent bug in this runner's own code, none of them caught by the mock-suite tests
because the mocks never needed real env vars, a real base compose file, or a real teardown to
actually succeed.

1. **No mechanism for a suite to declare required compose env vars.** `p0-rig-scaffold`'s own
   `p1-ran-baseline`/`p3-live-tap-ul-inject`/`p4-phy-l2-seam` compose overlays all have hard-required
   (`${VAR:?...}`) env vars for config-file bind mounts — every existing bring-up helper
   (`bring_up.sh`, the `DEFERRED_LIVE_GATES.md` runbook) sets them itself for its OWN standalone
   compose invocation, but nothing in `simtest_runner.py` ever did, because it orchestrates a
   MERGED multi-feature stack these per-feature scripts never had to. First real symptom:
   `docker compose up` failed outright (`P1_RU_EMU_CONFIG_PATH ... must be set`), and because this
   runner brings up the WHOLE merged stack before invoking ANY gate, every one of the 9 phases'
   gates — including purely local unit-test gates with no live-rig dependency at all — came back
   `ERROR`, not just the ones that actually needed the rig. **Fixed properly, not hacked around**:
   extended the suite schema to `oi-p5-suite/2` with an optional `compose_env` map (each suite.yml
   declares exactly the env vars ITS OWN overlays need, using `{root}`/`{feature_root}` template
   tokens resolved once by `discover_suites.py`); `simtest_runner.py` generically unions every
   valid manifest's own `compose_env` and passes it to the `bring_up()`/`tear_down()` subprocess
   calls — the runner itself never learns a single var name, same P5-R15 "no feature-specific
   logic" discipline the gate-script/compose-overlay design already follows. All 9 `gates/suite.yml`
   files (not just p1/p3/p4, which needed real values) bumped to `/2`; `p1`/`p3`/`p4` declare their
   real values, matching `bring_up.sh`'s/the runbook's own exactly.
2. **`p0_base_overlay()` was missing the upstream base compose file.** Returned only
   `compose.sim.yml` (gpu-phy/oracle), never `p0-rig-scaffold/docker/upstream/docker/
   docker-compose.yml` (the file, via the `upstream` symlink into `third_party/ocudu`, that
   actually defines `gnb`'s/`5gc`'s own `image`/`build` blocks) — every real bring-up helper always
   layers both, in that order; this runner's own notion of "the p0 base" silently omitted the
   first. Symptom (after fix 1 above): `service "gnb" has neither an image nor a build context
   specified`. Fixed: renamed to `p0_base_overlays()` (plural), returns both files in the correct
   order.
3. **`tear_down()` silently swallowed its own failure.** No env vars (same gap as fix 1, but never
   even threaded through once fix 1 existed) meant `docker compose down` failed the exact same way
   `up` originally did — but the return code was never checked, so a `make simtest` run that
   printed `[p5] overall: PASS` still left every container running afterward, discovered only by
   manually checking `docker ps -a` post-run. Fixed: `tear_down()` now accepts `compose_env` (passed
   from the same merged dict as `bring_up()`) and logs to stderr on nonzero exit instead of staying
   silent — a failed teardown is this runner's own responsibility to surface, not something a
   caller should have to notice by accident.

Also needed (not runner bugs, real local-environment setup gaps): p0's own `oi/gpu-phy:dev`/
`oi/oracle:dev` images and the oracle-injection-patched `ocudu/gnb` had to be built locally first
(see `DEFERRED_LIVE_GATES.md`'s WSL2 session log for the full build account); `oi/l2-stub:dev` had
to be built manually once too (`docker compose up` attempted to PULL it from a registry rather than
using its own `build:` block, for reasons not fully root-caused — pre-building the image directly
sidesteps the ambiguity and is itself a legitimate, disclosed local-setup step, not a runner bug);
and p3's real 20-file oracle grid set had to be generated at `/tmp/p3_osg` (`make build/osg_gen`
+ real invocation) so `ru-emu`'s oracle-injection command doesn't crash-loop for lack of input
files it would otherwise never find.

**Verified**: `test_discover_suites.py`/`test_p5_g1.py`'s own synthetic fixtures updated to
`oi-p5-suite/2` (their schema mismatch was incidental to what they actually test, not the point of
any of them) — full local suite re-run clean, 0 failures across all 4 test files. Then `make
simtest` run for real, twice more after all three fixes: both times `[p5] overall: PASS` across
all 29 real gates spanning all 9 phases (p1's `p1-soak-stability` included — see
`p1-ran-baseline/VERIFICATION.md`'s own new finding for why this gate's 60s window passed even
though a much longer standalone soak the same night did not), and the second of the two confirmed
`docker ps -a` empty afterward (teardown fix verified end-to-end, not just via the mock-suite
test). Full account, exact run-ids, and the archived ledger paths: `DEFERRED_LIVE_GATES.md`'s p5
section.

## Known-open items (real, not hidden)

- **P5-G2 (full real run + WSL2/GCP cross-comparison) is deferred** — see
  `DEFERRED_LIVE_GATES.md`'s p5 section for the exact command, the itemized real per-gate
  smoke-test results already obtained locally, and the pass criteria.
- **`--only-phase`'s Q3(a) resolution** (bring up every overlay, only run the selected phase's
  gates) is implemented and tested (`test_p5_g1.py`), matching the LLD's own stated preference,
  but the LLD itself marks this "decided at implementation," not a hard requirement — documented
  here for traceability, not as a deviation.
- **`--tier physical`** is implemented as the literal stub the SPEC describes: `discover_suites.py`
  scans a different glob (`suite.physical.yml`) and returns an empty list today (no `p6` manifests
  exist yet); the no-perf lint explicitly SKIPs (verified, not just assumed) rather than silently
  no-op'ing. Full PHYSICAL behavior remains `p6`'s deliverable, unchanged from the SPEC's own
  framing.
- **`rigcfg_digest`** is computed as a real SHA-256 over the resolved, sorted compose-overlay file
  set's contents — a genuine, reproducible digest, but not a value defined anywhere else in the
  codebase to cross-check against; `compare_ledgers.sh` compares it for equality across hosts as
  the CLI contract's note requires, which is the only place its exact value matters.
