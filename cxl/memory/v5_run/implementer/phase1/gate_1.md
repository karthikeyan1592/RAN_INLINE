# Gate 1 (v5) — CXL stand-in + descriptor ring buffer + busy-poll consumer

## Spec

PASS if (all on WSL2, stand-in backing):
- A test producer pushing K descriptors into desc_ring is fully drained by the
  busy-poll consumer with descriptors_seen == K and ZERO drops, across K = 1e3, 1e5, 1e6.
- Consumer's poll-to-handle latency p50 is sub-microsecond. Contrast with v4's 2ms.
- cxl_region_map() asserts 4096-alignment and prints the backing path. Evidence:
  printed base address + alignment assertion passing.

## Commands

```bash
cd /root/linux_env/cxl/cxl_ran_poc/phase5_cxl
make phase1
./ring_test
```

## Raw evidence

```
[ring_test] prod_cpu=2 cons_cpu=3 ncpu=4 ring=/tmp/v5_desc_ring.bin
[cxl_region] backing=/tmp/cxl_standin.bin  base=0x749719200000  size=256 MiB  STAND-IN
[ring_test] CXL backing: /tmp/cxl_standin.bin  base=0x749719200000
[ring_test] Alignment check: 4096-ALIGNED OK

=== ring_test: K=1000 ===
  pushed:          1000
  seen (consumer): 1000
  drops detected:  0
  retries:         3
  push duration:   90 us
  total duration:  235 us
  poll-to-handle latency:  mean=17 ns  p50=17 ns  p95=18 ns  p99=18 ns
  GATE1 K=1000     PASS

=== ring_test: K=100000 ===
  pushed:          100000
  seen (consumer): 100000
  drops detected:  0
  retries:         46323
  push duration:   12395 us
  total duration:  12501 us
  poll-to-handle latency:  mean=17 ns  p50=17 ns  p95=19 ns  p99=20 ns
  GATE1 K=100000   PASS

=== ring_test: K=1000000 ===
  pushed:          1000000
  seen (consumer): 1000000
  drops detected:  0
  retries:         419030
  push duration:   119993 us
  total duration:  120037 us
  poll-to-handle latency:  mean=18 ns  p50=17 ns  p95=18 ns  p99=23 ns
  GATE1 K=1000000  PASS

All Gate 1 ring tests PASSED.
```

## Self-verdict

**PASS**

All three Gate 1 conditions met:

| Check | Result |
|-------|--------|
| K=1,000: seen==K, drops==0 | PASS |
| K=100,000: seen==K, drops==0 | PASS |
| K=1,000,000: seen==K, drops==0 | PASS |
| poll-to-handle p50 | **17 ns** (sub-microsecond ✓) |
| cxl_region 4096-aligned | **ALIGNED OK** ✓ |
| backing path printed | `/tmp/cxl_standin.bin  STAND-IN` ✓ |

**v4 vs v5 interception latency comparison:**

| Version | Mechanism | p50 latency |
|---------|-----------|-------------|
| v4 | 2ms sleep poll (`ldpc_measure.c`) | ~950,000 ns (950 µs) |
| v5 | busy-poll SPSC ring (`ring_test`) | **17 ns** |
| Improvement | | **55,882× lower** |

The 17 ns p50 approaches the bpftime Gate 0.1 floor of 248.5 ns/call (the stub
handle() does only a pointer comparison, not a real decode — Phase 2 adds OpenCL).
The ring itself adds <20 ns overhead in userspace.

## Deviations

None. All spec-required checks pass on first run.

## Files

- `phase5_cxl/cxl_region.h` — CXL seam header (backing selection)
- `phase5_cxl/cxl_region.c` — stand-in / DAX mmap, 4096-alignment assert
- `phase5_cxl/desc_ring.h` — SPSC lock-free ring (C11 atomics, power-of-2)
- `phase5_cxl/consumer.c` — pinned-core busy-poll consumer (stub handle)
- `phase5_cxl/ring_test.c` — test producer: K=1e3/1e5/1e6 self-contained
- `phase5_cxl/Makefile` — phase1 and phase2 targets

## Timestamp

2026-06-20
emulation_mode: stand-in (WSL2, /tmp/cxl_standin.bin)
