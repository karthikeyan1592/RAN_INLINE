# p5-one-command-rig — SPEC

**Feature:** SIM phase P5 — `make simtest`: one-command compose-up → P1–P4 assertions → teardown.
**Tier:** SIM (T1). **Authority:** [`ARCHITECTURE_v3_SIM.md`](../../../ARCHITECTURE_v3_SIM.md)
§1/§4 (P5 row + footer DoD is the gate source); master:
[`ARCHITECTURE_v3.md`](../../../ARCHITECTURE_v3.md) (referenced as the master data path this
whole rig proves incrementally). Pins: OCUDU `release_26_04`
([`research/ocudu_repin.md`](../../../research/ocudu_repin.md)).

**Backend statement (SIM §3):** P5 provides **no backend of its own** — like P1, it is
orchestration over backends other features implement. It discovers and runs whichever backend
implementations the currently-selected tier's phase suites declare: at `--tier sim` (default),
`ingest_backend` = p3's af_packet tap, `compute_backend` = p2's PoCL pipeline, `handoff_backend`
= p4's memcpy ring. This artifact is later the **PHYSICAL deploy unit**: the same runner, pointed
at a different `--tier`, discovers PHYSICAL-tier suites (`p6`) instead — a backend swap only
(SIM §1), not a runner change.

## Purpose

Deliver a single entrypoint, `make simtest`, that: (1) brings up the union of compose overlays
needed for phases P1–P4, (2) discovers and runs every phase's gate scripts through one abstract,
versioned contract (`IF-P5-SUITE`), (3) aggregates a structured pass/fail report — an
honesty ledger, never a performance report — and (4) tears the stack down. The **same artifact**
(compose files + this runner + the ledger schema) is what later replays on PHYSICAL hardware with
only backend-swapped overlays and PHYSICAL-tier suites (`p6`'s concern; this spec only stubs the
delta). Gate: identical pass/fail behavior on WSL2 and the GCP `n2-standard-16` VM.

## In scope

- The `make simtest` entrypoint and the underlying runner (discovery, invocation, timeout,
  capture, aggregation).
- **`IF-P5-SUITE`**: the abstract contract every feature's gate scripts must satisfy to be
  discovered and run by p5 — defined here, in the abstract, as the interface p1–p4 (and later p6)
  implement; p5 does not redefine or depend on any feature's internal script names or logic.
- **`IF-P5-LEDGER`**: the structured pass/fail/honesty report schema (JSON + deterministic
  Markdown render).
- A rollup no-perf lint, defense-in-depth on top of each feature's own no-perf rule.
- A `--tier {sim|physical}` selector whose `physical` value is a **stub**: full behavior belongs
  to `p6`.
- Cross-host (WSL2 vs. GCP) ledger-comparison tooling to evidence the DoD gate.

## Out of scope

- Any feature's gate-script internals (p1's `assert_ecpri.sh`, p2's per-kernel gates, p3's
  bit-exact harness, p4's ring tests) — opaque, consumed only through `IF-P5-SUITE`.
- The PHYSICAL-tier suite manifests, device-mount overlays, and backend-swap implementation
  themselves — `p6`'s concern; this spec only stubs the run-book delta (§PHYSICAL replay run-book
  stub in the HLD).
- Any latency/throughput requirement or claim — **forbidden at SIM tier** (SIM preamble rule);
  the PHYSICAL-tier ledger is explicitly exempted from this rule (README's "measurement
  deliverables" carve-out for `p6`), but building that exemption's mechanics is `p6`'s job.
- Modifying any already-written feature's scripts to conform to `IF-P5-SUITE` — that is a
  forward-compatible **addendum** each feature adds at its own implementation time (Honesty notes).

## Requirements

Every requirement is testable; test mapping in LLD §Test plan.

| ID | Requirement |
|---|---|
| **P5-R1** | `make simtest` SHALL perform exactly: (1) merge and bring up the union of compose overlays declared by all discovered phase-suite manifests atop the p0 base, (2) execute every discovered phase's gates in phase order P1→P2→P3→P4, (3) aggregate one ledger, (4) tear the stack down (`docker compose down -v` or equivalent) regardless of pass/fail, unless a `--keep-up` debug flag is given. |
| **P5-R2** | p5 SHALL discover phase suites via a fixed convention: each feature ships `pX-name/gates/suite.yml` conforming to schema `oi-p5-suite/1` (LLD §Configuration). p5 SHALL NOT hardcode any feature's internal script names — only the manifest schema. |
| **P5-R3** | The suite manifest schema SHALL declare: phase id, feature name, required compose overlay file(s), an ordered list of gates each with `{id, type (unit\|integration), script, args, timeout_s}`, and optional setup/teardown hook lists. |
| **P5-R4** | Every gate script referenced by a suite manifest SHALL follow the pre-existing exit-code convention (0 pass / 1 fail / 2 setup-or-IO error / 3 precondition-blocked, shared with `IF-P0/P1-ASSERT`) and SHALL emit exactly one JSON object as the last line of stdout as its verdict. p5 imposes **no additional required fields** on that JSON beyond what each feature's own spec already defines. |
| **P5-R5** | p5 SHALL wrap every gate invocation with an external timeout (the manifest's `timeout_s`, or a phase-type default: 120 s unit / 900 s integration, overridable via `OI_P5_TIMEOUT_SCALE`); a timeout SHALL be classified distinctly (`TIMEOUT`) in the ledger, never conflated with `FAIL` or `BLOCKED`. |
| **P5-R6** | p5 SHALL capture, per gate, stdout/stderr into `artifacts/p5/<run-id>/<phase>/<gate-id>/{stdout.log,stderr.log}`, the exit code, wall-clock duration, and the parsed verdict JSON — without requiring any gate script to know its own artifact path. |
| **P5-R7** | p5 SHALL emit one aggregate ledger (`oi-p5-ledger/1`, LLD §Data structures) in JSON plus a deterministic Markdown render, recording per-gate status ∈ `{PASS, FAIL, ERROR, BLOCKED, TIMEOUT}` and per-phase `discovered` ∈ `{true, false}` (a phase with `discovered: false` carries `gates: []` and is annotated `NOT_DISCOVERED` at the phase level — **not** a per-gate status value), one `overall` verdict ∈ `{PASS, FAIL, BLOCKED, INCOMPLETE}` per the precedence `BLOCKED > FAIL > INCOMPLETE > PASS` (LLD §Data structures), host identity (`wsl2\|gcp`), and pins/rig-config digests. |
| **P5-R8** | The ledger SHALL carry an explicit `performance_claims` field that SHALL always be empty at SIM tier. A rollup lint step SHALL scan every captured verdict JSON and stdout/stderr for forbidden latency/throughput-threshold patterns (reusing each feature's own `lint_no_perf.sh`-style rule set) and fail a dedicated ledger-lint gate if any are found used as an operand rather than an observational note. |
| **P5-R9** | p5 SHALL run identically (same compose base, same discovery mechanism, same invocation/timeout/capture logic) on WSL2 and on the GCP `n2-standard-16` VM; a run on each SHALL produce the same per-gate `status` values and the same `overall` verdict, host-identity/timestamp/digest fields excepted (SIM §4 P5 row DoD). |
| **P5-R10** | On a precondition failure common to a whole session (e.g. SCTP absent, per p1-R6/p0-R9), p5 SHALL surface the run's `overall` verdict as `BLOCKED` (not `FAIL`), preserving exit-code-3 semantics end-to-end into the ledger. |
| **P5-R11** | `make simtest` SHALL be a single entrypoint requiring no manual step between compose-up and teardown other than supplying host-specific `.env` values already defined by p0/p1. |
| **P5-R12** | p5 SHALL provide a `--tier {sim|physical}` (or `OI_TIER` env) selector; `sim` (default) discovers only SIM-tier suite manifests; `physical` (stub only — full behavior owned by `p6`) discovers PHYSICAL-tier manifests instead, through the **identical** runner/ledger code path — the "backend swap only" property (SIM §1) applies to which manifests/overlays are discovered, not to the runner. |
| **P5-R13** | No requirement, default, or lint rule in this feature's `--tier sim` path SHALL contain a latency/throughput pass/fail threshold; PHYSICAL-tier ledgers (out of this feature's scope beyond the stub) are explicitly exempted per the README's "measurement deliverables" carve-out for `p6`. |
| **P5-R14** | The ledger schema SHALL be versioned (`oi-p5-ledger/1`) and self-describing enough that a ledger produced by a partial run (e.g. only P1's manifest exists yet) is still valid and clearly marks the missing phases `NOT_DISCOVERED` rather than silently omitting them. |
| **P5-R15** | p5 SHALL NOT modify, wrap, or duplicate any feature's gate-script logic; it only discovers, invokes, times out, captures, and aggregates — enforced by review (no gate-script-equivalent shell/assertion logic embedded in the runner beyond generic invocation plumbing). |

## Acceptance gates

Traceability: SIM §4, row **P5**. Definition of done (SIM §4 footer, and P5 row's own phrasing
"this artifact = PHYSICAL deploy unit (backend swap only)"): gates green on WSL2 **and** the GCP
`n2-standard-16` VM.

| Gate | Type | Statement | SIM §4 source |
|---|---|---|---|
| **P5-G1** | unit / CI | The runner's discovery, invocation, timeout, capture, and ledger-aggregation logic is correct against mock `suite.yml` manifests + mock gate scripts (covers P5-R2..R8, R14, R15) without requiring p1–p4's real gates to exist yet. | P5 "Test gate (unit): CI green" |
| **P5-G2** | integration | A full `make simtest` run (real p1–p4 suites, once each ships `gates/suite.yml`) produces `overall: PASS` on WSL2 **and** on the GCP `n2-standard-16` VM, with identical per-gate status sets (P5-R1, R9, R11, R12). | P5 "Integration gate: this artifact = PHYSICAL deploy unit (backend swap only)" |

## Dependencies on other features

- **`p0-rig-scaffold`** — compose base, pins manifest (`IF-P0-PINS`) the ledger's digest fields
  reference.
- **`p1-ran-baseline`**, **`p2-phy-kernels`**, **`p3-live-tap-ul-inject`**, **`p4-phy-l2-seam`** —
  each is required, at its own implementation time, to ship a `gates/suite.yml` conforming to
  `IF-P5-SUITE` (a small, additive, non-breaking addendum to specs already written — anticipated
  explicitly by p1 HLD's `IF-P1-ASSERT`: "Wrapped, not redefined, by p5's suite contract"). This
  spec defines the contract abstractly; it does not amend those features' specs.
- Consumed later by **`p6-physical-m1-ingest`**, which supplies PHYSICAL-tier suite manifests and
  backend-swap compose overlays under the same `IF-P5-SUITE`/`IF-P5-LEDGER` contracts (§PHYSICAL
  replay run-book stub, HLD).

## Honesty-ledger notes (what P5 does NOT prove)

- **P5 proves orchestration and rollup, not correctness.** It inherits whatever SIM-vs-real gaps
  each child phase's own SPEC already declares (P1's test-mode-UEs-don't-NAS-attach, P3's
  protocol-real/data-synthetic framing, P2's tolerance-vs-bit-exact distinctions, P4's synthetic
  ordering-gate caveat) — the ledger surfaces these by reference; it does not resolve or improve
  on them.
- **"Green on WSL2 and GCP" is a no-host-accident proof, not a correctness upgrade.** It shows the
  rig behaves the same across two Linux hosts, not that any phase's underlying claim is stronger
  than that phase's own SPEC states.
- **The PHYSICAL replay run-book is a stub, unverified.** Nothing in this feature has run against
  real hardware, real device mounts, or a real vendor ICD; all of that is `p6`'s deliverable.
- **`IF-P5-SUITE` is not yet satisfied by any already-written feature.** p1/p2/p3/p4's specs
  predate this contract; until each ships a `gates/suite.yml`, P5-G2 (the full integration run)
  cannot execute for real — only P5-G1 (mock-manifest CI) can, today. This is flagged as an
  explicit, anticipated follow-up, not a contradiction (see Dependencies).
- No performance evidence of any kind leaves this feature's SIM-tier path (SIM preamble rule;
  P5-R13).
