# p0-rig-scaffold — Implementation Status

Spec: [`spec/SPEC.md`](spec/SPEC.md) · [`spec/HLD.md`](spec/HLD.md) · [`spec/LLD.md`](spec/LLD.md).
This file records what was actually built and verified (2026-07-22), against the spec's
requirements/gates, in one place — the spec docs stay the source of truth for *what*, this is the
record of *what happened when we built it*.

## What's implemented

Full module breakdown per LLD: `docker/{upstream,compose.sim.yml,gpu-phy/,oracle/}`,
`helpers/{build_images,assert_clinfo,run_ldpc_suite,smoke_up,check_sctp}.sh`,
`src/oracle_verdict/oracle_verdict.py`, `tests/{ci_p0.sh,check_pins_schema.py,test_oracle_verdict.sh}`.

`docker/upstream` is a **symlink to the pinned `third_party/ocudu` checkout** (tag `release_26_04`,
SHA `050a2bb7...`), not a copy — avoids duplicating the ~96MB OCUDU source tree just to reach it as
a build dependency, while `docker/upstream/docker/docker-compose.yml` and `context: ..` inside it
resolve correctly since the symlink points at the whole repo root, matching OCUDU's own internal
layout. `helpers/build_images.sh` verifies the pin (tag == `release_26_04`) before every build.

## What's verified for real (not just written)

| Requirement/Gate | Method | Result |
|---|---|---|
| **P0-R1** (upstream vendored, pin recorded) | `git describe --tags --exact-match` in `build_images.sh` | ✅ pinned at `release_26_04` |
| **P0-R2** (override doesn't alter upstream) | `docker compose config`, both with/without override, diffed programmatically (Python, service-by-service dict compare) | ✅ `5gc`/`gnb` service defs byte-identical; only `gpu-phy`/`oracle` added |
| **P0-R3** (image contents/no privilege) | Built `oi/gpu-phy:dev` for real (multi-stage: OCUDU lib build → AdaptiveCpp source build → LDPC suite link → runtime image) | ✅ built, 253MB compressed; `compose.sim.yml`'s gpu-phy service has no `devices`/`privileged`/`cap_add` |
| **P0-R4** (clinfo/platform assertion) | `assert_clinfo.sh` run **inside the built container** | ✅ `platform=Portable Computing Language devices=1`, exit 0; negative test (`OI_CL_PLATFORM=bogus`) correctly exit 1 |
| **P0-R5** (oracle-verdict CLI) | `tests/test_oracle_verdict.sh` against the built `oi/oracle:dev` image's vector store | ✅ PASS/FAIL/exit-2 all correct |
| **P0-R6** (LDPC suite, unmodified, 0 mismatches) | `run_ldpc_suite.sh` **inside the built container**, real OCUDU-linked `bit_diff_test` binary | ✅ **0/4,096,000 bit mismatches** (BG1/BG2 × LS=384/256, 200 msgs each), exit 0 |
| **P0-R7** (pins manifest + label parity) | `build_images.sh` step 4 + `check_pins_schema.py` | ✅ schema valid, `org.openinline.pins` label identical on both images |
| **P0-R8** (CI job, no perf threshold) | `tests/ci_p0.sh` — wraps the above end to end | ✅ written, exercises the same verified path (see caveat below) |
| **P0-R9** / **P0-G2** (compose up, 5gc+gnb stable) | `smoke_up.sh` | ⚠️ **partially verified** — see below |

## Honest gaps / caveats

1. **P0-R9 SCTP precondition path: verified for real, not just written.** This host has no
   `CONFIG_IP_SCTP` (`check_sctp.sh` → exit 3, message: *"NGAP needs CONFIG_IP_SCTP; stock WSL2
   kernel may lack it — rebuild WSL2 kernel with CONFIG_IP_SCTP=m or run on GCP VM"*) — exactly the
   documented WSL2-like case SIM §1 anticipates. `smoke_up.sh` correctly short-circuits here before
   ever attempting `docker compose up`.
2. **The full "5gc+gnb stable 60s + NG-setup-attempt log" success path is NOT observed on this
   host**, for two compounding reasons: (a) SCTP is absent here regardless, so a full bring-up
   could never reach "stable" on this box even if built; (b) the upstream `gnb` image build
   compiles the *complete* OCUDU gNB from source with its full RF stack (UHD, DPDK, ROHC) — a
   substantially heavier build than the LDPC-suite-only `ocudu_channel_coding` library target this
   feature actually needs, and it did not finish within this session (still running in the
   background at `/tmp/.../scratchpad/gnb_5gc_build.log` — restart if it was killed with your
   shell). This does not block P0-R9's own gate (which is specifically about the precondition
   check + stability/log assertions, verified above where reachable); it only means the upstream
   image itself hasn't finished a from-scratch build here. **Next verification step, on a host with
   both Docker and SCTP (e.g. the GCP `n2-standard-16` VM per SIM §4 DoD): let the `gnb`/`5gc` build
   finish, then run `helpers/smoke_up.sh` for the full P0-G2 pass.**
3. **P0-R8 (CI job)**: the script (`tests/ci_p0.sh`) is written and exercises exactly the commands
   already verified individually above; it has not been run in an actual CI runner (no CI provider
   is chosen yet — LLD Open Question Q3).
4. Both real bugs found *during* this verification pass (not present in the original spec, introduced only by
   attempting a real build) were fixed in place: a missing-package chain in the gpu-phy Dockerfile
   (`libsctp-dev`, `libmbedtls-dev`, `opencl-headers`, `lld-17`, `libnuma-dev` — all present on the
   dev host already, silently masking their absence until a clean container proved otherwise), and
   a `docker image inspect`-on-missing-image stdout leak that corrupted `pins.json`'s
   `base_image.digest` field (fixed with an explicit exit-code check instead of a bare `||`
   fallback).

## Port note (licensing-relevant, see `docker/gpu-phy/ldpc_suite/MODIFICATIONS.md` for full detail)

`bit_diff_test.cpp` (the LDPC oracle-generation harness) was mechanically re-pinned from the old
srsRAN Project (AGPLv3) to OCUDU (BSD-3-Clause-Open-MPI) — include paths and namespace only, plus
one real upstream API-surface change (`ldpc_encoder::encode()`'s config-argument type) tracked
verbatim, zero test-logic change. The bit-exact kernel under test (`ldpc_decode.cl`, `bg_tables.h`)
is untouched. This means `oi/gpu-phy` carries no AGPL-derived code.
