# p0-rig-scaffold — LLD

> **PIN UPDATE (2026-07-19, propagated to this file 2026-07-21):** upstream is **OCUDU**
> (`gitlab.com/ocudu/ocudu`, `release_26_04`, BSD-3) — the successor to srsRAN Project. See
> `../../../research/ocudu_repin.md`. The `pins.json` example below previously named the old
> `srsran/srsRAN_Project` GitHub URL paired with the OCUDU-only tag `release_26_04` — a repo/tag
> combination that never existed together (that tag only exists in OCUDU's repo). Fixed below.

Companion to [`SPEC.md`](SPEC.md) / [`HLD.md`](HLD.md). This is a design document; paths below are
the layout the implementation must produce, not existing files.

## Module breakdown

```
features/p0-rig-scaffold/
  docker/
    upstream/                 # vendored OCUDU release_26_04 docker assets (P0-R1, unmodified)
      docker-compose.yml
      ...                     # upstream Dockerfiles/configs as shipped
    compose.sim.yml           # override: adds gpu-phy + oracle services (P0-R2)
    gpu-phy/Dockerfile        # Ubuntu 24.04 + PoCL + AdaptiveCpp-OMP + clinfo + LDPC suite (P0-R3)
    oracle/Dockerfile         # vector store + verdict CLI (P0-R5)
  helpers/
    build_images.sh           # builds all images, emits pins.json per image (P0-R7)
    assert_clinfo.sh          # in-container platform assertion (P0-R4)
    run_ldpc_suite.sh         # wraps IF-P0-SUITE inside gpu-phy (P0-R6)
    smoke_up.sh               # compose up + stability/log checks + SCTP precheck (P0-R9)
    check_sctp.sh             # shared with p1 (single source; p1 spec defines semantics)
  src/
    oracle_verdict/           # verdict CLI (small; language per Open questions Q5)
  tests/                      # CI-invoked wrappers around helpers (P0-R8)
```

## Public APIs (signatures)

All P0 "APIs" are CLI contracts (no library code beyond the verdict CLI).

```text
assert_clinfo.sh
  usage: assert_clinfo.sh [--platform-regex REGEX]   # default: 'Portable Computing Language'
  runs clinfo inside gpu-phy; exit 0 iff exactly one matching platform and
  $OI_CL_PLATFORM resolves to it; prints "platform=<name> devices=<n>".

run_ldpc_suite.sh
  usage: run_ldpc_suite.sh [--vectors DIR]           # default: $OI_VECTOR_DIR/ldpc
  exit 0 iff suite reports 0 bit mismatches; stdout ends with one JSON line:
  {"suite":"ldpc","cases":N,"mismatches":0,"platform":"pocl"}

oracle-verdict (IF-P0-VERDICT)
  usage: oracle-verdict --kernel K --case ID --result FILE [--tolerance-profile P]
  exit 0 = PASS, 1 = FAIL, 2 = usage/IO error
  stdout: one JSON line:
  {"kernel":"ldpc","case":"bg1_z384_c42","verdict":"pass",
   "compare":"bit_exact","mismatches":0}
  (--tolerance-profile reserved for p2 float stages; P0 ships bit_exact only.)

smoke_up.sh
  usage: smoke_up.sh [--hold-seconds 60]
  exit 0 iff: SCTP precheck passes; compose up succeeds; gnb+5gc services running with
  0 restarts for hold window; gnb log matched NG-setup-attempt pattern.
  exit 3 = SCTP precondition failed (distinct, actionable — see Error handling).
```

## Data structures & formats

### Vector store layout (IF-P0-VECTORSTORE)

Filesystem contract inside the `oracle` image (and exported as a named volume to `gpu-phy`):

```
/oi/vectors/
  <kernel>/                   # p0: "ldpc" only; p2 adds chest, eq, demap, descr, ratedm, depkt
    manifest.json             # {"kernel":"ldpc","cases":[{"id":...,"files":[...],"compare":"bit_exact"}]}
    <case-id>/
      input.bin  expected.bin  params.json
```

No byte-precise wire format is defined here: nothing crosses a process/network boundary in P0
except files; `input.bin`/`expected.bin` keep the pre-existing LDPC suite's own formats
(inherited, not redefined). p2's LLD owns per-kernel formats for the new kernels.

### pins.json (IF-P0-PINS)

```json
{
  "schema": "oi-pins/1",
  "built_utc": "2026-07-17T00:00:00Z",
  "ocudu": {"repo": "https://gitlab.com/ocudu/ocudu", "tag": "release_26_04", "sha": "<40-hex>"},
  "base_image": {"name": "ubuntu:24.04", "digest": "sha256:..."},
  "open5gs_images": [{"name": "...", "tag": "...", "digest": "sha256:..."}],
  "pocl": {"source": "apt|git", "version": "..."},
  "adaptivecpp": {"repo": "https://github.com/AdaptiveCpp/AdaptiveCpp", "tag": "<pinned>", "sha": "..."},
  "ldpc_suite": {"origin": "<prior-work repo/path>", "sha": "..."},
  "images": [{"name": "oi/gpu-phy", "digest": "sha256:..."}, {"name": "oi/oracle", "digest": "sha256:..."}]
}
```

Also stamped on each image as OCI label `org.openinline.pins` (JSON string).

## Configuration (YAML/env schema)

### Environment variables (gpu-phy service)

| Var | Default (SIM) | Meaning |
|---|---|---|
| `OI_CL_PLATFORM` | `pocl` | OpenCL platform selector (IF-P0-CLPLATFORM). PHYSICAL values per SIM §3.1: `rocm`, `nvidia`, `intel-neo`. |
| `OI_VECTOR_DIR` | `/oi/vectors` | vector store mount point |
| `OI_LOG_DIR` | `/oi/logs` | writable log dir (bind-mounted by helpers/p5) |

### compose.sim.yml (schema excerpt — normative shape)

```yaml
services:
  gpu-phy:
    image: oi/gpu-phy:${OI_TAG:-dev}
    build: {context: ./gpu-phy}
    environment: {OI_CL_PLATFORM: pocl, OI_VECTOR_DIR: /oi/vectors}
    volumes: [oi-vectors:/oi/vectors:ro]
    # MUST NOT contain: devices, privileged, cap_add (P0-R3; p1/p3 add NET_RAW by later override)
    command: ["sleep", "infinity"]        # skeleton: idle service, exec'd into by helpers
  oracle:
    image: oi/oracle:${OI_TAG:-dev}
    build: {context: ./oracle}
    volumes: [oi-vectors:/oi/vectors]
volumes: {oi-vectors: {}}
```

Version pins (README convention): OCUDU (`gitlab.com/ocudu/ocudu`) `release_26_04`; Ubuntu 24.04
base; PoCL from Ubuntu
24.04 archive (`pocl-opencl-icd`, PoCL 5.x) unless Q1 forces a pinned source build; AdaptiveCpp
pinned tag (Q2); all recorded in `pins.json` — the manifest is authoritative, this doc is not.

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| PoCL platform absent/duplicated | `assert_clinfo.sh` | exit ≠0, print all platforms found; CI fails P0-G1 |
| `OI_CL_PLATFORM` unresolvable | suite platform helper | exit ≠0 with `unknown platform '<v>'`; never silent fallback to another platform (would fake a pass on the wrong backend) |
| LDPC mismatch > 0 | suite summary JSON | exit ≠0; verdict JSON carries mismatch count; CI red |
| Vector store missing/corrupt | manifest.json validation before suite run | exit 2 (setup error, distinct from test FAIL) |
| Host lacks SCTP | `check_sctp.sh` (grep `/proc/net/protocols`, try `modprobe sctp`) | `smoke_up.sh` exits 3 with message: "NGAP needs CONFIG_IP_SCTP; stock WSL2 kernel may lack it — rebuild WSL2 kernel with CONFIG_IP_SCTP=m or run on GCP VM" (SIM §1) |
| gnb/5gc restart during hold window | `docker inspect` RestartCount | smoke exit ≠0, dump last 200 log lines of failed service |
| Image build failure | `build_images.sh` | fail fast; no pins.json emitted for failed image (partial manifests forbidden) |

## Test plan (per requirement)

| Req | Test |
|---|---|
| P0-R1 | CI: `git diff --no-index` vendored tree vs a fresh checkout of upstream at the pinned SHA → empty; pins.json contains matching tag+SHA. |
| P0-R2 | `docker compose -f upstream/docker-compose.yml config` vs same with `-f compose.sim.yml` added: rendered definitions of upstream services are identical (yq-normalized diff empty); new services present. |
| P0-R3 | Inspect built image: base = Ubuntu 24.04; `clinfo`, PoCL ICD file, `acpp` present. Inspect rendered service: no `devices`/`privileged`/`cap_add` keys. |
| P0-R4 | Run `assert_clinfo.sh` in the running service → exit 0; negative test: `OI_CL_PLATFORM=bogus` → exit ≠0. |
| P0-R5 | `oracle-verdict` PASS on a known-good result blob; FAIL (exit 1) on a 1-bit-flipped copy; exit 2 on missing case ID. |
| P0-R6 | `run_ldpc_suite.sh` → exit 0, JSON `mismatches:0`, full case count matches manifest; suite source SHA in pins equals prior-work SHA (unmodified). |
| P0-R7 | After build: pins.json validates against `oi-pins/1` schema; label `org.openinline.pins` on both images parses to same content. |
| P0-R8 | CI pipeline run from clean cache is green; grep CI config for timing thresholds → none. |
| P0-R9 | `smoke_up.sh` on WSL2 (SCTP-enabled kernel) and GCP VM → exit 0; on a host without SCTP → exit 3 with the documented message. |

Gate mapping: P0-G1 = {R3,R4,R6} · P0-G2 = {R9} · P0-G3 = {R8} (which transitively runs R1–R7 checks).

## Open questions

1. **Q1 — PoCL from apt vs pinned source build:** Ubuntu 24.04's `pocl-opencl-icd` version must be
   verified adequate for the LDPC suite (cl_khr features, SPIR-V ingestion if used). If not, build
   PoCL from a pinned tag in the Dockerfile. Decide at implementation; pins.json records either way.
2. **Q2 — AdaptiveCpp pin:** exact tag (candidate: latest release ≤ 2026-07 supporting the
   generic SSCP compiler + OMP backend) to be fixed at first image build and recorded in pins.
3. **Q3 — CI provider:** the architecture docs do not name one. This spec defines the job contract
   only (docker-capable runner, required-green). Provider choice (GitHub Actions vs self-hosted on
   the GCP VM) is an implementation decision; note P0-G2 in CI needs an SCTP-capable runner kernel.
4. **Q4 — LDPC suite vendoring path:** import the prior-work suite as a git submodule vs vendored
   copy; either satisfies P0-R6 as long as the SHA is pinned and sources are byte-identical.
5. **Q5 — verdict CLI language:** Python 3 (stdlib-only) vs C++. Leaning Python-in-oracle-image
   (no host dependency); must not leak into `gpu-phy` runtime requirements.
6. **Q6 — P0-G2 vs SCTP:** SIM §4's P0 integration gate ("compose up brings up 5gc+gnb") already
   requires SCTP for the gnb→AMF NG setup attempt, though the SIM doc files SCTP under day-1/P1.
   This spec resolves it via the P0-R9 precondition + distinct exit 3; flagged upward as a doc gap.
