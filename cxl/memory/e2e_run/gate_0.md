# Gate 0 — Scaffolding + portability layer
Date: 2026-07-02
Environment: WSL (this box), CXL_NODE=0

## Scope (from IMPLEMENTER_PROMPT.md)
- Create `e2e/` tree per `E2E_ARCH_SPEC.md` §7.
- Implement `e2e/common/cxl_region.{h,c}` (the CXL-abstraction API) + a unit test
  that allocs, writes, reads back, and verifies node placement via `get_mempolicy`.
- Accept: builds clean; unit test passes; `get_mempolicy` confirms node 0 on WSL.

## Files created

```
e2e/
├── common/
│   ├── cxl_region.h
│   └── cxl_region.c
├── generator/          (empty — scaffolded for Gate 1)
├── guest/
│   ├── rx/              (empty — scaffolded for Gate 1)
│   ├── phy/              (empty — scaffolded for Gate 2)
│   ├── accel/            (empty — scaffolded for Gate 3)
│   └── consumer/         (empty — scaffolded for Gate 3)
├── scripts/             (empty — scaffolded for Gate 5)
└── tests/
    └── test_cxl_region.c
```

## cxl_region API design

`cxl_alloc(size_t bytes)` reads `CXL_NODE` from the environment (default `0`),
validates it against `numa_max_node()`, and calls `numa_alloc_onnode(bytes, node)`.
No other file in this tree may hard-code a NUMA node number — this is the single
seam that flips WSL (`CXL_NODE=0`, plain DRAM node 0) to GCP (`CXL_NODE=1`, CXL
system-RAM node 1) without any code change, per the spec's portability requirement.

`cxl_verify_node()` calls `get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE)` on the region's
base pointer and logs `req_node` vs `actual_node` — this exact log line is meant to
be cited verbatim in every later gate report that touches the region.

Never mmaps `/dev/dax*` (the 23 µs/byte device-memory path that DEV-040 identified
in the prior v8 PoC as forbidden).

## Build (verbatim)

```
$ gcc -Wall -Wextra -O2 -std=c11 \
    common/cxl_region.c tests/test_cxl_region.c \
    -lnuma \
    -o tests/test_cxl_region
```
Exit code: 0. No warnings, no errors.

## Test run 1 — CXL_NODE=0 (explicit)

```
$ CXL_NODE=0 ./tests/test_cxl_region
cxl_region: alloc base=0x78af9d42b000 size=1048576 CXL_NODE=0
cxl_region: region base=0x78af9d42b000 size=1048576 req_node=0 actual_node=0
[test_cxl_region] alloc 1048576 bytes...
[test_cxl_region] write/read-back: 0 mismatches / 1048576 bytes
[test_cxl_region] requested node=0 actual node (get_mempolicy)=0
[test_cxl_region] OVERALL: PASS
```
Exit code: 0.

## Test run 2 — no CXL_NODE set (default path)

```
$ unset CXL_NODE
$ ./tests/test_cxl_region
cxl_region: alloc base=0x7fe7f8300000 size=1048576 CXL_NODE=0
cxl_region: region base=0x7fe7f8300000 size=1048576 req_node=0 actual_node=0
[test_cxl_region] alloc 1048576 bytes...
[test_cxl_region] write/read-back: 0 mismatches / 1048576 bytes
[test_cxl_region] requested node=0 actual node (get_mempolicy)=0
[test_cxl_region] OVERALL: PASS
```
Exit code: 0. Default-env-var path also lands on node 0, as required.

## Test run 3 — CXL_NODE=1 on WSL (guard-rail check, not part of the accept criterion)

WSL has a single NUMA node (`numactl --hardware` → `available: 1 nodes (0)`).
Requesting `CXL_NODE=1` here should fail loudly, not silently fall back or corrupt
placement:

```
$ CXL_NODE=1 ./tests/test_cxl_region
cxl_region: ERROR CXL_NODE=1 requested but max NUMA node is 0 (WSL has 1 node; GCP CXL bring-up must complete before CXL_NODE=1)
[test_cxl_region] alloc 1048576 bytes...
[test_cxl_region] FAIL: cxl_alloc returned NULL
```
Exit code: 1. This is the correct, honest behavior on WSL — not a bug. The GCP
port (Gate 5, deferred) will supply `CXL_NODE=1` on a host with 2 NUMA nodes /
CXL brought up, at which point this same code path succeeds.

## Environment evidence

```
$ numactl --hardware
available: 1 nodes (0)
node 0 cpus: 0 1 2 3
node 0 size: 7943 MB
node 0 free: 676 MB
node distances:
node   0
  0:  10
```

```
$ gcc --version
gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
```

## Verdict: **PASS**

- Builds clean (0 warnings after removing a harmless `-D_GNU_SOURCE` duplicate flag).
- Unit test alloc/write/read-back: 0 mismatches / 1,048,576 bytes.
- `get_mempolicy` confirms `actual_node=0` on WSL, matching `req_node=0` — accept
  criterion met exactly as specified.
- Guard-rail behavior (CXL_NODE=1 on a 1-node box) fails loudly and cleanly, which
  is the correct behavior, not a defect — flagging this explicitly rather than
  silently treating it as in-scope for this gate's accept criterion.

## CXL_NODE value used for the accept run: 0 (WSL, single NUMA node)

## Next gate
Gate 1 — eCPRI loopback / IQ integrity (WSL): traffic generator (known TB → srsRAN
encode → modulate → RE map → simplified eCPRI/UDP), eCPRI RX (parse, reorder,
reassemble RE grid), ≥100 slots, per-slot max abs error ≈ 0.
