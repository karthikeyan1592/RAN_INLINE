# p5-one-command-rig — HLD

Companion to [`SPEC.md`](SPEC.md). Requirement IDs P5-R1…R15 referenced throughout.

## Context diagram

```mermaid
flowchart TB
  mk["make simtest\n(entrypoint)"] --> runner["simtest runner\n(discover -> merge overlays -> up ->\ninvoke gates -> capture -> aggregate -> down)"]

  subgraph discover["discovery (IF-P5-SUITE)"]
    s1["p1-ran-baseline/\ngates/suite.yml"]
    s2["p2-phy-kernels/\ngates/suite.yml"]
    s3["p3-live-tap-ul-inject/\ngates/suite.yml"]
    s4["p4-phy-l2-seam/\ngates/suite.yml"]
  end
  runner --> discover

  runner -->|docker compose up\n(union of overlays)| rig["running SIM rig\n(5gc+gnb+ru-emu+gpu-phy+l2-stub)"]
  runner -->|invoke, per gate:\ntimeout-wrap + capture| gates["each feature's own\ngate scripts (opaque)"]
  gates --> ledger["oi-p5-ledger/1\nJSON + Markdown"]
  ledger --> lint["rollup no-perf lint\n(P5-R8)"]
  runner -->|docker compose down| rig

  subgraph stub["PHYSICAL replay (stub only, p6 owns detail)"]
    direction TB
    p6["p6 PHYSICAL-tier\ngates/suite.yml + overlays"]
  end
  runner -.->|"--tier physical\n(same runner, different discovery set)"| stub
```

## Components

| Component | Role | Reqs |
|---|---|---|
| **`make simtest` entrypoint** | Thin Makefile target invoking the runner with default args. | P5-R11 |
| **Suite discovery** | Finds and schema-validates every `pX-*/gates/suite.yml` (or PHYSICAL equivalent under `--tier physical`). | P5-R2/R3/R12 |
| **Overlay merge + compose lifecycle** | Merges each discovered manifest's `compose_overlays` onto the p0 base; brings the union up once, tears it down once. | P5-R1 |
| **Gate invoker** | Runs each declared gate's `script` with `args`, wrapped in an external timeout; classifies the outcome. | P5-R4/R5 |
| **Artifact capture** | Redirects stdout/stderr, records exit code + duration + parsed verdict JSON per gate, with no cooperation required from the script. | P5-R6 |
| **Ledger aggregator** | Builds `oi-p5-ledger/1` (JSON) + a deterministic Markdown render. | P5-R7/R14 |
| **Rollup no-perf lint** | Scans the aggregated ledger + all captured output for forbidden performance-threshold patterns; a defense-in-depth gate distinct from each feature's own lint. | P5-R8/R13 |
| **Cross-host comparator** | Diffs two ledgers (WSL2 vs. GCP run), ignoring host-identity/timestamp/digest fields, to evidence P5-R9. | P5-G2 |
| **`--tier` selector** | Switches which manifest set (`sim` vs. `physical`) discovery scans; identical runner code either way. | P5-R12 |

## Interfaces (every boundary named)

1. **`IF-P5-SUITE`** — the phase-suite manifest schema (`oi-p5-suite/1`, LLD §Configuration): the
   **central new contract** this feature defines. Every feature from p1 onward implements it by
   shipping `gates/suite.yml`; p5 never reaches past this file into a feature's internals.
2. **`IF-P5-LEDGER`** — the aggregate report schema (`oi-p5-ledger/1`, LLD §Data structures):
   consumed by a human (Markdown render), by CI (JSON `overall` field), and by the cross-host
   comparator.
3. **`IF-P5-CLI`** — `make simtest [--tier sim|physical] [--keep-up] [--only-phase P1,P2,...]
   [--timeout-scale F]` and the underlying runner's own CLI.
4. **Reused, not redefined:** the exit-code convention (`0/1/2/3`) and one-JSON-verdict-line
   convention already established by `IF-P0-*`/`IF-P1-ASSERT`/p3's assertion tooling. p5 depends on
   this convention; it does not restate or modify it.
5. **PHYSICAL delta surface (stub)** — the set of things `--tier physical` changes: which
   `gates/suite.yml` files are discovered, which compose overlays are merged, and that the
   rollup no-perf lint (P5-R8/R13) does not apply to that tier's ledger. Full ownership: `p6`.

## Data flow

```
make simtest
  ── runner: discover_suites(tier) ──► [suite.yml, suite.yml, ...] (schema-validated)
  ── runner: merge compose_overlays from each manifest ──► docker compose -f <union> up -d
  ── runner: for phase in [P1,P2,P3,P4] (declared order):
       for gate in phase.gates (declared order):
         run_gate(gate)  # external timeout wrapper + stdout/stderr capture + exit-code classify
         ──► artifacts/p5/<run-id>/<phase>/<gate.id>/{stdout.log,stderr.log}
         ──► {status, exit_code, duration_s, verdict_json} recorded in-memory
  ── runner: aggregate ──► oi-p5-ledger/1 (JSON) ──► render Markdown
  ── runner: rollup no-perf lint over the ledger + all captured output
  ── runner: docker compose down -v   (always, unless --keep-up)
  ── exit 0 iff ledger.overall == PASS
```

Cross-host evidence flow (P5-R9, manual/CI-scheduled, not part of a single `make simtest` run):
```
run on WSL2 ──► ledger_wsl2.json
run on GCP  ──► ledger_gcp.json
compare_ledgers.sh ledger_wsl2.json ledger_gcp.json ──► diff ignoring {host, timestamps, digests}
  ──► PASS iff every gate's status and the overall verdict match
```

## Deployment view

| Where | What runs | Tier |
|---|---|---|
| WSL2 host | full `make simtest` (compose union up, gates, ledger, teardown) | SIM |
| GCP `n2-standard-16` | identical invocation (SIM §4 P5 row DoD) | SIM |
| CI runner | P5-G1 (mock-manifest unit tests of the runner itself) always; P5-G2 only on an
  SCTP-capable, Docker-capable runner | SIM |
| PHYSICAL boxes (later) | same runner, `--tier physical`, discovering `p6`'s suites/overlays —
  device mounts (`/dev/infiniband`,`/dev/kfd`,`/dev/dri`), hugepages, vendor ICD env vars, and the
  `ingest_backend`/`handoff_backend` swaps all live in what gets *discovered*, not in the runner | — (p6 stub) |

### PHYSICAL replay run-book (stub — full detail belongs to `p6`)

What changes when this same artifact runs on PHYSICAL, at a glance:

| Delta | SIM (`--tier sim`, this feature) | PHYSICAL (`--tier physical`, `p6`) |
|---|---|---|
| Discovered manifests | `p1..p4/gates/suite.yml` | `p6`'s PHYSICAL-tier manifests (own `suite.yml` set) |
| Compose overlays merged | `compose.p1.yml`, p3's tap overlay, `compose.p4.yml` | PHYSICAL overlays adding `devices: [/dev/infiniband, /dev/kfd, /dev/dri]`, hugepage mounts, `cap_add` as needed |
| `ingest_backend` | af_packet + memcpy (p3) | mlx5 raw-packet QP + `ibv_reg_dmabuf_mr` (p6, new code) |
| `compute_backend` | PoCL (`OI_CL_PLATFORM=pocl`) | vendor ICD (`OI_CL_PLATFORM=rocm\|cuda\|level_zero`) — env only, same image (`IF-P0-CLPLATFORM`) |
| `handoff_backend` | plain memcpy ring (p4) | pinned-buffer async DMA + completion event, **same ring format** (p4's `IF-P4-RING`, per p4 HLD D1) |
| No-perf rule | enforced (P5-R13) | explicitly exempted — PHYSICAL ledgers may carry real measurement fields, as "measurement deliverables," never pass/fail thresholds (README carve-out) |
| Runner code | — | **unchanged** — only which manifests/overlays are discovered differs |

This table is the entire PHYSICAL delta this spec commits to; the mechanism (M1 ingest, day-1
hardware checklist, actual vendor ICD wiring) is `p6`'s deliverable, not elaborated here.

## Design decisions (with rationale)

1. **D1 — Manifest-based discovery, not hardcoded feature knowledge.** The assignment requires the
   contract to be defined "abstractly," since p2/p4's gate internals may not be finalized. A
   `gates/suite.yml` per feature, discovered by a fixed glob, means p5 never needs to know a
   feature's script names, arguments, or count — matching P5-R2/R15 and keeping p5 stable while
   p1–p4 iterate independently.
2. **D2 — One merged `docker compose up` for the whole P1–P4 session, not per-phase up/down.**
   P3/P4 need the live rig; bringing it up and down once per phase would multiply SCTP-negotiation
   and NG-setup wait time for zero additional proof value (the same reasoning p1 HLD D1 applied to
   compose-base choice) and would make phase-to-phase state (e.g. P1's pcap corpus feeding P2, the
   live tap feeding P3/P4) harder to reason about. Single up, ordered gate execution, single down.
3. **D3 — Timeout applied externally (wrapping the invocation), not required inside each script.**
   Requiring every gate script to implement its own timeout logic would force already-written
   scripts (p0/p1/p3) to change. Wrapping invocation externally (e.g. the `timeout` utility) adds
   the behavior with zero change to existing scripts — directly satisfying "don't invent p2/p4
   internals" and keeping p1/p3 untouched.
4. **D4 — JSON ledger is authoritative; Markdown is a deterministic render of it, never a second
   source of truth.** Keeps CI parsing (JSON) and human review (Markdown) from ever disagreeing.
5. **D5 — `BLOCKED` (exit 3) is a distinct ledger status, never folded into `FAIL`.** Preserves the
   precondition-vs-defect distinction p0/p1 already built into their exit-code convention
   end-to-end into the rollup — a WSL2 host missing SCTP support should read as "environment not
   ready," not "the rig is broken" (P5-R10).
6. **D6 — `IF-P5-SUITE` lives in each feature's own directory (`pX-name/gates/suite.yml`), not
   centrally inside p5.** Each feature owns its own test contract; p5 only aggregates. This mirrors
   the codebase's existing pattern (every feature owns its `helpers/` scripts) and is what makes
   the contract "abstract" rather than an intrusion into p2/p4's design space.
7. **D7 — Ledger schema is versioned and tolerates partial discovery (`NOT_DISCOVERED`).** Since
   this spec is written before p1–p4 ship their `suite.yml` addenda, a `make simtest` run today
   would discover nothing; the ledger must say so explicitly rather than reporting a vacuous
   `overall: PASS` — directly enabling honest incremental adoption (P5-R14).

## Rejected alternatives

- **Centralizing all gate logic inside p5.** Rejected: violates the assignment's "don't invent
  p2/p4 internals" instruction, couples p5 to every feature's internal test code, and breaks the
  moment any feature restructures its own tests.
- **Hardcoding per-feature script paths/names in the runner.** Rejected: identical failure mode to
  the above at a smaller scale — any feature renaming a helper script would silently break p5.
- **Adopting an existing test framework (pytest/bats/etc.) as the aggregator.** Rejected: the
  honesty-ledger format and the no-perf rollup lint are bespoke project requirements, not generic
  testing concerns; and gate scripts are (and will remain) a mix of shell/Python/C — plain
  subprocess invocation + one JSON verdict line is simpler and more portable than adopting any one
  framework's plugin model.
- **Requiring every gate script to implement its own `--timeout-seconds` flag.** Rejected (D3):
  forces retroactive changes on already-written, already-reviewed p0/p1/p3 scripts.
- **Folding `TIMEOUT` and `BLOCKED` into `FAIL`.** Rejected (D5): loses exactly the diagnostic
  signal (environment-not-ready vs. flaky-under-load vs. genuine defect) that matters most across
  two different hosts (WSL2 vs. GCP) with different scheduling/latency variance.
- **Requiring each gate script's JSON verdict to carry p5-specific metadata (phase, gate id,
  type).** Rejected: that metadata already lives in the `suite.yml` manifest; requiring scripts to
  duplicate it would be redundant and would again force changes on already-written scripts.
