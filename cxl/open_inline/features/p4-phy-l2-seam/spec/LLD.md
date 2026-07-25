# p4-phy-l2-seam — LLD

Companion to [`SPEC.md`](SPEC.md) / [`HLD.md`](HLD.md). Design document; paths are the layout the
implementation must produce.

## Module breakdown

```
features/p4-phy-l2-seam/
  docker/
    compose.p4.yml         # additive gpu-phy volume mount + new l2-stub service (P4-R10/R11)
    Dockerfile.l2stub       # l2-stub image (project code, minimal; no OCUDU MAC)
  src/
    oi_seam_ring.h          # header + slot layout structs, status enum (P4-R1/R2)
    oi_seam.c / oi_seam.h   # reserve/publish/wait_status/release API (IF-P4-API)
    l2_stub_main.c          # consumer entrypoint: attach, validate, counters, verdict JSON
  helpers/
    gate_p4_ordering.sh     # drives the out-of-order synthetic injector (P4-G1)
    gate_p4_wrap.sh         # drives the slow-consumer wrap driver (P4-G2)
    gate_p4_restart.sh      # kill/respawn producer and consumer scenarios (P4-G3)
    gate_p4_integration.sh # full P3->P2->ring->l2-stub run (P4-G4)
  tests/                    # unit tests for oi_seam.c against mock producer/consumer pairs
```

## Public APIs (IF-P4-API)

```c
/* Create-or-attach the named ring segment. `create` mirrors the CXL PoC's cxl_region_open()
 * signature deliberately (D2): the Phase-2 port is expected to add an mbind() call here and
 * change nothing else about this function's contract. */
oi_seam_ring_t *oi_seam_open(const oi_seam_config_t *cfg, int create);
void            oi_seam_close(oi_seam_ring_t *r);

/* Producer: block (bounded wait, no deadline) until a slot is free; returns it in RESERVED state
 * with `seq` assigned. Backpressure point for P4-R7. */
oi_seam_slot_t *oi_seam_reserve(oi_seam_ring_t *r, uint64_t *out_seq);

/* Producer: publish a status transition with release semantics — payload MUST already be
 * fully written before this call (P4-R3). */
void oi_seam_publish(oi_seam_slot_t *s, oi_seam_status_t st);

/* Consumer: spin/poll (bounded backoff: spin -> sched_yield -> nanosleep) until slots[idx].status
 * == want; acquire semantics on return. Works identically whether `want` was set synchronously
 * (SIM memcpy), from a DMA-completion callback (PHYSICAL), or observed by raw polling with no
 * event at all (Phase-2 CXL) — this call never depends on an OS wakeup, only on eventually
 * re-reading the atomic (P4-R5). */
oi_seam_slot_t *oi_seam_wait_status(oi_seam_ring_t *r, uint64_t idx, oi_seam_status_t want);

/* Consumer: mark DONE, then advance tail (frees the slot for the producer). Persists the new
 * tail value so a consumer restart can resume correctly (P4-R9). */
void oi_seam_release(oi_seam_ring_t *r, uint64_t idx);

/* Consumer/producer: read the current epoch; a change vs. the last-observed value means the
 * ring was reinitialized — caller MUST discard per-key ordering state (P4-R8). */
uint32_t oi_seam_epoch(const oi_seam_ring_t *r);
```

Naming intentionally parallels the CXL PoC's `ring_reserve`/`slot_publish`/`ring_wait_status`/
`ring_release` (HLD D1/D2) — a Phase-2 port is a rename-and-add-`mbind`, not a redesign.

## Data structures & formats (IF-P4-RING — byte-precise)

```c
typedef enum {
    OI_SEAM_EMPTY    = 0,  /* free, producer may reserve */
    OI_SEAM_RESERVED = 1,  /* producer owns it, mid-write; consumer MUST NOT read */
    OI_SEAM_READY    = 2,  /* payload complete, release-published; consumer may read */
    OI_SEAM_DONE     = 3   /* consumer finished; producer may reuse once tail passes it */
} oi_seam_status_t;

#define OI_SEAM_TB_MAX_BYTES   /* implementation-computed: max TB size across the p2 MVP config's
                                   pinned MCS set {4,13,21} at 51 PRB, 11 data symbols, 1 layer.
                                   NOT fabricated here — computed and pinned at implementation;
                                   config validates producer/consumer agree (P4-R14). */

typedef struct __attribute__((aligned(64))) {
    _Atomic uint32_t status;        /* oi_seam_status_t; release/acquire discipline (P4-R3) */
    uint32_t         epoch;         /* ring generation this slot belongs to (P4-R8) */
    uint64_t         seq;           /* global monotonic insertion sequence (wrap detection) */
    uint32_t         sfn;
    uint16_t         slot;
    uint16_t         rnti;
    uint8_t          harq_id;
    uint8_t          crc_ok;        /* 1 = pass, 0 = fail */
    uint8_t          _pad[2];
    uint32_t         tb_len;        /* bytes actually valid in tb[] */
    uint64_t         t_enqueue_ns;  /* CLOCK_MONOTONIC_RAW, producer-side; OBSERVATIONAL ONLY —
                                       never a gate operand (P4-R13) */
    uint8_t          tb[OI_SEAM_TB_MAX_BYTES];  /* zero-padded tail beyond tb_len */
} oi_seam_slot_t;

typedef struct __attribute__((aligned(64))) {
    uint32_t         magic;          /* fixed 'OISM' constant */
    uint32_t         format_version; /* this document = version 1 */
    uint32_t         epoch;          /* bumped by producer on every (re)init (P4-R8) */
    uint32_t         ring_capacity;  /* N slots, power of two (wrap arithmetic) */
    uint32_t         slot_bytes;     /* sizeof(oi_seam_slot_t); cross-build safety check */
    uint8_t          _pad0[44];      /* pad header to a cacheline boundary before the counters */
    _Atomic uint64_t head;           /* producer reserve counter, monotonic, reset only on epoch bump */
    _Atomic uint64_t tail;           /* consumer free counter; persisted for reattachment (P4-R9) */
    uint8_t          _pad1[48];      /* isolate head/tail from slots[] on separate cachelines */
    /* oi_seam_slot_t slots[ring_capacity] follow immediately */
} oi_seam_ring_hdr_t;
```

**Wrap arithmetic (P4-R7):** physical index for sequence `seq` is `seq % ring_capacity`. Producer
computes `next = head`; if `next - tail >= ring_capacity` the ring is full — `oi_seam_reserve`
blocks (bounded spin + `sched_yield`/`nanosleep` backoff; no deadline, SIM tier) until the consumer
advances `tail`. `seq` is never reused across a wrap — only `seq % ring_capacity` recurs — so a
slot's `seq` field lets any observer detect exactly which logical slot currently occupies a given
physical index.

**Restart arithmetic (P4-R8/R9):** `epoch` is monotonically incremented by the producer only, only
at ring (re)initialization, never mid-run. A consumer caches `(epoch, tail)` externally (a small
sidecar file in the same volume, e.g. `oi/seam/consumer_state.json`) after every `release()`; on
restart it reads that file, compares `epoch` to the ring header's current `epoch`: equal → resume
from the persisted `tail`; different → the stream restarted, discard all per-key state, resume
from the ring's current `tail` (which will be 0 for a freshly-reinitialized ring).

## Configuration (YAML/env schema)

### compose.p4.yml (normative shape; additive to p0's `gpu-phy`, new `l2-stub`)

```yaml
services:
  gpu-phy:                          # additive extension only (mirrors P1-R1's pattern)
    volumes:
      - oi_seam_ring:/oi/seam
    environment:
      OI_SEAM_RING_PATH: /oi/seam/ring.bin
      OI_SEAM_RING_CAPACITY: "64"          # power of two; MVP default, revisit at implementation
      OI_SEAM_TB_MAX_BYTES: "${OI_SEAM_TB_MAX_BYTES}"   # computed constant, see Data structures
      OI_SEAM_FORMAT_VERSION: "1"
  l2-stub:
    image: oi/l2-stub:${OI_TAG:-dev}
    build:
      context: ../../..
      dockerfile: features/p4-phy-l2-seam/docker/Dockerfile.l2stub
    volumes:
      - oi_seam_ring:/oi/seam
    environment:
      OI_SEAM_RING_PATH: /oi/seam/ring.bin
      OI_SEAM_RING_CAPACITY: "64"
      OI_SEAM_TB_MAX_BYTES: "${OI_SEAM_TB_MAX_BYTES}"
      OI_SEAM_FORMAT_VERSION: "1"
    depends_on:
      gpu-phy: {condition: service_started}
volumes:
  oi_seam_ring: {}                  # named, persists across either service's restart (P4-R10)
```

### `oi_seam_config_t` (env-overridable; both sides must agree, checked at attach)

```yaml
schema: oi-p4-seam-config/1
ring_path: /oi/seam/ring.bin
ring_capacity: 64
tb_max_bytes: <computed>
format_version: 1
consumer_state_path: /oi/seam/consumer_state.json   # l2-stub only
```

Cross-check rule: on `oi_seam_open()`, if the segment already exists, its header's
`ring_capacity`/`slot_bytes`/`format_version` MUST equal the caller's config; mismatch is a
structured error (exit 2), never a silent truncate/reinterpret (P4-R14).

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| Ring segment absent, consumer starts first | `oi_seam_open(create=0)` finds no valid `magic` | exit 2, actionable message: "l2-stub started before gpu-phy initialized the ring" |
| Header mismatch (capacity/slot size/version) | `oi_seam_open()` field compare | exit 2, structured diff of expected vs. found |
| Ring full, producer blocked indefinitely (consumer dead, not restarting) | `oi_seam_reserve` bounded-wait exceeds a generous test-harness timeout | test harness (not the library) times out and fails the gate — SIM tier has no deadline of its own, but gate scripts need a bound to terminate |
| Producer crashes mid-write (`status` stuck at `RESERVED`) | consumer's `wait_status` observes no transition past a stall-detection threshold (test-harness only, not a runtime SLA) | flagged as a producer-fault in the restart gate; not a "hitless recovery" feature — this is a correctness-testing tool, fail-fast is correct |
| `epoch` changes mid-run without a corresponding consumer resync | consumer compares `epoch` on every `wait_status` return, not just at startup | consumer discards per-key state immediately, logs the transition, continues from new `head=tail=0` |
| Two producers or two consumers attach at once (misconfiguration) | not structurally prevented by the ring (single-writer/single-reader design) | out of scope to detect at runtime; flagged as an operational precondition, not a protocol failure (LLD Open questions) |
| `tb_len` exceeds `OI_SEAM_TB_MAX_BYTES` | producer-side bounds check before `memcpy` | producer refuses to publish, exits 2 — a config/sizing bug, never a truncated write |

## Test plan (per requirement)

| Req | Test |
|---|---|
| P4-R1/R2 | Static struct-layout test: `sizeof`/`offsetof` assertions match this document's byte layout on the target toolchain; round-trip a slot through `memcpy` and verify field-for-field equality. |
| P4-R3 | Thread-sanitizer / helgrind run over a producer+consumer pair hammering one ring; assert no data race is reported and payload is never observed torn (read-before-release never sees partial writes). |
| P4-R4 | Code review + static check: no GPU-API symbol (`cl*`, `hip*`, event types) referenced anywhere in `oi_seam.*` or `oi_seam_ring.h`. |
| P4-R5 | Documentation/design-review gate (not a runtime test): confirm the ring format's only cross-transport dependency is the atomic `status` field; explicitly re-derive that a hypothetical polling-only consumer (no event) still satisfies every other requirement. |
| P4-R6 | **P4-G1**: synthetic harness feeds the producer two `(rnti,harq_id)` streams, deliberately completing stream B's slot N before stream A's slot N-1; consumer asserts monotonic `(sfn,slot)` per key, 0 violations. |
| P4-R7 | **P4-G2**: harness pauses the consumer, drives the producer to `ring_capacity+k` reservation attempts; assert producer blocks (not error, not drop) until consumer resumes; resulting slot count and per-key order still correct afterward. |
| P4-R8 | **P4-G3(a)**: kill the producer process mid-run, restart it; assert the next slot's `epoch` is incremented and the consumer's ordering/reorder state was reset (verified via consumer log/counters), not silently continuing stale per-key expectations. |
| P4-R9 | **P4-G3(b)**: kill the consumer mid-run (ring not full, producer keeps blocking-or-buffering per P4-R7), restart the consumer; assert it resumes at the persisted `tail`, delivers every slot published while it was down exactly once. |
| P4-R10 | Compose-level test: `docker restart l2-stub` and separately `docker restart gpu-phy` (in isolation, ring pre-populated); assert the volume-backed segment's contents survive both (`magic`/`epoch`/slot contents unchanged by the restart itself). |
| P4-R11 | L2 stub unit tests: feed known-good and known-bad (CRC-fail, out-of-order) synthetic slots; assert counters and exit code match; grep the l2-stub source tree for any FAPI/SCF-222 symbol — must find none. |
| P4-R12 | Integration smoke: p2's actual drain call shape is fed through the producer unchanged; assert no field renaming/reinterpretation happens between p2's I8 record and the ring slot (a 1:1 field mapping table, reviewed against p2's LLD once written). |
| P4-R13 | `lint_no_perf.sh`-style grep (reused pattern from p1/p2/p3) over this feature's tree: zero threshold-bearing keys used as gate operands; `t_enqueue_ns` only ever appears in observational/log output. |
| P4-R14 | Negative test: start `l2-stub` and `gpu-phy` with deliberately mismatched `OI_SEAM_RING_CAPACITY`; assert exit 2 with a structured diff, no partial ring use. |
| **P4-G4** | Full integration: p3's live injected UL → p2 pipeline → producer → ring → l2-stub, over a configured run (mirrors P3-R11's ≥1000-slot default); assert 0 CRC/order mismatches, matching SIM §4 P4 integration-gate wording exactly. |

## Open questions

1. **Q1 — Drain call-site ownership.** Whether `gpu-phy`'s own event loop calls p2's drain API and
   publishes to the ring directly, or a thin separate adapter thread/process does it, is left to
   p2/p4 implementation once p2's LLD (not yet written) fixes the exact drain API signature. Either
   shape satisfies this spec; the ring format does not care.
2. **Q2 — `OI_SEAM_RING_CAPACITY` sizing.** 64 slots is a placeholder MVP default; the real number
   should be derived from expected in-flight slot depth once p2's buffering strategy (HLD §5,
   double-buffering) is implementation-fixed. Not a correctness parameter (P4-R7 holds at any
   capacity ≥ 1), only a throughput-shaping one — and SIM makes no throughput claim, so any
   reasonable placeholder is acceptable pending real numbers from PHYSICAL.
3. **Q3 — `OI_SEAM_TB_MAX_BYTES` exact value.** Deliberately left as "implementation-computed" here
   rather than guessed, to avoid recording a wrong byte-precise constant in a spec document;
   compute it from p2's MVP config (51 PRB × 11 data symbols × Qm=6 max, minus overhead) at
   implementation time and pin it in both `compose.p4.yml` and `oi_seam_ring.h`.
4. **Q4 — Multiple concurrent producers/consumers.** Current design is strictly single-writer/
   single-reader (matching the CXL PoC precedent and the MVP's single-gpu-phy/single-l2-stub
   topology). Scaling to multiple L2 stubs or multiple gpu-phy workers is unscoped; flagged for a
   future spec revision if p5 or p6 need it.
5. **Q5 — Stall-detection timeout value for the "producer crashed mid-write" test-harness check.**
   A concrete number belongs in the gate script, not this format spec (SIM tier has no deadline of
   its own); left to `gate_p4_restart.sh` implementation, generous enough to avoid flaking under
   WSL2/GCP scheduling variance.
