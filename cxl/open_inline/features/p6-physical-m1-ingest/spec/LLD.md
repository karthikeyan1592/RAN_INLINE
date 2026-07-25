# p6-physical-m1-ingest — LLD

**Scope:** module breakdown, public APIs, byte-precise structures, config schema, error handling,
and test plan for the day-1 probe suite, the M1 spike harness, and the production `ingest_backend`
([`SPEC.md`](SPEC.md), [`HLD.md`](HLD.md)).

## 1. Module breakdown

| Module | Files (conceptual) | Depends on |
|---|---|---|
| `probe_nic_ident` | `probe01_nic_ident.c` | libpci/`lspci`, `libibverbs` |
| `probe_dmabuf_mr` | `probe02_dmabuf_mr.c` | CUDA driver API / ROCm HSA API, `libibverbs`/`mlx5dv` |
| `probe_rawqp_hostmem` | `probe03_rawqp_hostmem.c` | `libibverbs`, `mlx5dv` |
| `probe_sriov_vf` | `probe04_sriov_vf.sh` + `probe04_vf_loopback.c` | `sysfs`, `libibverbs` |
| `probe_hugepages_dpdk` | `probe05_dpdk_testpmd.sh` | DPDK, hugepages |
| `probe_gpu_ldpc` | `probe06_gpu_ldpc.sh` | vendor ICD, SIM-2 LDPC suite binary (reused, unmodified) |
| `probe_bar1` | `probe07_bar1.sh` | `nvidia-smi` |
| `probe_openkm_dmabuf` | `probe08_openkm.sh` | `modinfo`, CUDA driver API |
| `probe_sriov_numvfs` | `probe09_numvfs.sh` | `sysfs` |
| `probe_selfloopback` | `probe10_selfloopback.c` | `libibverbs`, `mlx5dv` (shares code with `m1_rawqp_selfloopback`) |
| `m1_dmabuf_probe` | `m1_step1_dmabuf.c` | same as `probe_dmabuf_mr` (R11 reuses R2's binary) |
| `m1_rawqp_selfloopback` | `m1_step2_hostmem_loopback.c` | same as `probe_selfloopback` (R12 reuses R10's binary) |
| `m1_dmabuf_swap` | `m1_step3_dmabuf_loopback.c` | step 2's binary + step 1's dmabuf MR, recombined |
| `m1_byte_verify` | `m1_step4_crc_kernel.{c,cl}` | device-side CRC kernel (OpenCL C, portable per SIM §3 kernel rules) |
| `m1_l2fwdnv_crosscheck` | vendored build script only, no project code | upstream `NVIDIA/l2fwd-nv` (unmodified, run alongside) |
| `oi_ingest_vram` | `oi_ingest_vram.c`, `oi_ingest_vram.h` | `libibverbs`, `mlx5dv`, vendor dmabuf-export wrapper |
| `oi_ingest_cpustaged` | `oi_ingest_cpustaged.c` | DPDK PMD or af_packet, pinned-memory alloc, `cudaMemcpy`/`hipMemcpy` |
| `oi_ingest_common` | `oi_ingest.h`, `oi_ingest_stats.c`, `oi_ingest_flowcfg.c` | shared struct/API definitions used by both impls (and, by convention, by p3's SIM impl) |
| `wiring_selector` | `wiring_select.c` + `wiring.yaml` schema | `oi_ingest_common` config loader |
| `latency_recorder` | `latency_record.c`, record format below | none (measurement-only, no gate reads it) |

## 2. Public APIs

### 2.1 `oi_ingest` — the SIM/PHYSICAL swap-point API

Signature set is defined here (first concrete version in the project; p3's SIM af_packet
implementation is the API's other party — see SPEC Dependencies). Both `oi_ingest_vram` and
`oi_ingest_cpustaged` implement this identically; `compute_backend` code links against exactly one
vtable, chosen at init.

```c
// oi_ingest.h — shared by SIM (p3) and PHYSICAL (p6) implementations

typedef struct oi_ingest_s *oi_ingest_handle_t;   // opaque

typedef enum {
    OI_INGEST_OK              = 0,
    OI_INGEST_ERR_INIT        = 1,  // backend-specific init failure (see §5 error handling)
    OI_INGEST_ERR_NO_DEVICE   = 2,  // required NIC/GPU device not found
    OI_INGEST_ERR_DMABUF      = 3,  // dmabuf export/registration failed (VRAM impl only)
    OI_INGEST_ERR_FLOW_RULE   = 4,  // flow-steering rule install/conflict failed
    OI_INGEST_ERR_RING_FULL   = 5,  // no free descriptor slots (backpressure)
    OI_INGEST_ERR_TIMEOUT     = 6,  // poll timed out, not an error condition per se
} oi_ingest_status_t;

typedef enum { OI_INGEST_IMPL_VRAM = 0, OI_INGEST_IMPL_CPUSTAGED = 1 } oi_ingest_impl_t;

typedef struct {
    oi_ingest_impl_t impl;             // selected at init, immutable thereafter
    const char       *nic_dev;         // e.g. "mlx5_0"
    const char       *gpu_vendor;      // "nvidia" | "amd" ; ignored for CPU-staged w/o GPU landing
    uint16_t          ethertype;       // flow-steering match, e.g. 0xAEFE
    uint32_t          ring_depth;      // number of ring slots (power of 2)
    uint32_t          slot_size_bytes; // per-slot arena buffer size (>= max frame size)
    uint8_t           self_loopback;   // 1 = set MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC|_MC
    uint8_t           wiring_mode;     // 0..4, see wiring.yaml (§4); informational for VRAM impl,
                                       // selects PMD/af_packet iface for CPU-staged impl
} oi_ingest_config_t;

typedef struct {
    uint64_t arena_offset;   // byte offset into the packet arena (device or host, per impl)
    uint32_t len;            // frame length in bytes
    uint64_t rx_ts_ns;       // CLOCK_MONOTONIC_RAW at reap (matches p3-R10 convention)
    uint32_t flags;          // bit0: ethertype-matched: bit1: reserved
} oi_frame_desc_t;

typedef struct {
    uint64_t frames_seen;        // total frames the NIC/PMD observed on the bound port/QP
    uint64_t frames_matched;     // passed the ethertype/flow-steering filter
    uint64_t frames_delivered;   // reaped by compute_backend via oi_ingest_poll
    uint64_t frames_dropped;     // ring-full or CQE-error (never silently discarded uncounted)
} oi_ingest_stats_t;

oi_ingest_handle_t oi_ingest_init(const oi_ingest_config_t *cfg, oi_ingest_status_t *status_out);
oi_ingest_status_t oi_ingest_start(oi_ingest_handle_t h);                 // installs flow rule, arms ring
void               *oi_ingest_arena_base(oi_ingest_handle_t h);           // VRAM device ptr (VRAM impl) or host ptr (CPU-staged impl); compute_backend treats both as an opaque base for pointer arithmetic against arena_offset
oi_ingest_status_t oi_ingest_poll(oi_ingest_handle_t h, oi_frame_desc_t *out, uint32_t max_out,
                                   uint32_t *n_out, int timeout_ms);      // non-blocking with timeout_ms=0
oi_ingest_status_t oi_ingest_release(oi_ingest_handle_t h, uint32_t desc_idx); // returns slot to ring
oi_ingest_status_t oi_ingest_get_stats(oi_ingest_handle_t h, oi_ingest_stats_t *out);
void               oi_ingest_teardown(oi_ingest_handle_t h);
```

Contract notes (binding on both implementations, per SPEC P6-R20/R21):
- `oi_ingest_arena_base` + `arena_offset` is the only way `compute_backend` addresses a frame —
  never a separate host-copy pointer for CPU-staged. The CPU-staged implementation's arena *is* the
  pinned host buffer; its "swap" is invisible above this line because the pointer arithmetic is
  identical, only the pointed-to memory's location differs.
- `oi_ingest_poll` delivers descriptors in kernel/PMD-delivery order (matches p3-R10's ordering
  guarantee, extended to the PHYSICAL tier).
- No implementation may drop a frame without incrementing `frames_dropped` — R19's negative test
  depends on this.

### 2.2 Day-1 / M1 probe programs

Each probe is a standalone executable, no shared library dependency beyond what's listed in §1,
exit code 0 = PASS, nonzero = FAIL, stdout = human-readable + a final `RESULT: <KEY>=<VALUE>` line
per recorded field (machine-parseable by the report aggregator).

```
probe02_dmabuf_mr   --vendor {nvidia|amd} --size-bytes N
  -> RESULT: DMABUF_EXPORT=ok|fail
  -> RESULT: MR_REGISTER=ok|fail

probe03_rawqp_hostmem --nic mlx5_0 --ethertype 0xAEFE
  -> RESULT: QP_CREATE=ok|fail
  -> RESULT: FLOW_RULE=ok|fail
  -> RESULT: LOOPBACK_RX=ok|fail        (single-frame smoke test via same-function self-loopback,
                                          MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC — NOT an
                                          external cable or VF; this is what makes R22's "no VFs,
                                          no fabric, no cable" claim true for its R1/R3 dependency)

probe10_selfloopback --nic mlx5_0 --ethertype 0xAEFE --count 1000
  -> RESULT: SELF_LOOPBACK_FLAG=supported|unsupported
  -> (deeper/higher-volume validation of the same mechanism R3 smoke-tests at n=1 — sustained
      1000-frame self-loopback, distinct from R3 only in thoroughness, not mechanism)
  -> RESULT: FRAMES_SENT=1000
  -> RESULT: FRAMES_RECEIVED=<N>
  -> RESULT: LOSS=<FRAMES_SENT-FRAMES_RECEIVED>

m1_step3_dmabuf_loopback --nic mlx5_0 --vendor nvidia --ethertype 0xAEFE --count 1000
  -> RESULT: DMABUF_MR_ON_RAWQP=ok|fail   (this line IS the M1 moment's verdict)

m1_step4_crc_kernel --expect-crc32 <hex> --count 1000
  -> RESULT: CRC_MISMATCHES=<N>            (PASS iff 0)
```

## 3. Data structures & formats (byte-precise)

### 3.1 VRAM RX-buffer ring layout (`oi_ingest_vram`)

One contiguous dmabuf-registered VRAM allocation, size = `ring_depth * slot_size_bytes`, plus a
small host-side (not device) descriptor-ring control block (mirrors standard verbs RQ/CQ pairing —
the *data* lives in VRAM, the *ring metadata* lives in host memory the CPU polls).

```
VRAM region (dmabuf fd, one ibv_reg_dmabuf_mr call, lkey = LK):
  +-------------------+-------------------+-----+-------------------+
  | slot 0            | slot 1            | ... | slot (depth-1)    |
  | slot_size_bytes    | slot_size_bytes   |     | slot_size_bytes   |
  +-------------------+-------------------+-----+-------------------+
  slot i base = arena_base + i * slot_size_bytes   (arena_offset in oi_frame_desc_t == i * slot_size_bytes)

Host-side ring control block (cacheline-aligned, one instance per QP):
  struct oi_vram_ring_ctl {
      uint32_t head;            // next slot to post an RX WQE against (producer, host-managed)
      uint32_t tail;            // oldest slot not yet released by compute_backend (consumer)
      uint32_t depth;           // == ring_depth, immutable after init
      uint32_t _pad;
      uint64_t lkey;            // MR lkey for the dmabuf region, cached for WQE construction
  };  // 24 bytes, one per QP, host memory (not shared with device)
```

RX WQEs are posted against `slot = head`, tagged with `wr_id = head` so the completion (CQE)
identifies which slot landed data; `oi_ingest_poll` drains completed CQEs, fills `oi_frame_desc_t`
with `arena_offset = wr_id * slot_size_bytes`, `len` from the CQE byte count, advances `tail` on
`oi_ingest_release`.

### 3.2 CPU-staged arena layout (`oi_ingest_cpustaged`)

Structurally identical slot layout, but the region is a `cudaHostAlloc`/`hipHostMalloc` pinned host
buffer (not dmabuf-registered — no MR needed on the CPU-staged path, since the NIC never touches
GPU memory directly). Descriptor delivery: DPDK PMD RX burst (or af_packet `recvmmsg`) fills a slot,
then an explicit `cudaMemcpyAsync`/`hipMemcpyAsync` (host-pinned → device VRAM working buffer used
by `compute_backend`, a *separate* allocation from this arena) completes before the descriptor is
handed to `oi_ingest_poll`'s caller — i.e., by the time `compute_backend` sees a descriptor, the
frame is already resident in VRAM either way; the two impls differ only in how many copies and
which engine (NIC DMA vs. CPU-initiated GPU-DMA) performed the landing.

### 3.3 Flow-steering rule config

```c
typedef struct {
    uint16_t ethertype;        // network byte order match value, e.g. htons(0xAEFE)
    uint8_t  match_ethertype;  // 1 = install ethertype-only rule (wiring priority 0/1 default)
    uint8_t  match_5tuple;     // 1 = also match src/dst IP:port (wiring priority 3/4, UDP-encap)
    uint32_t src_ip;           // used iff match_5tuple
    uint32_t dst_ip;           // used iff match_5tuple
    uint16_t udp_dst_port;     // used iff match_5tuple (UDP-encapsulated eCPRI, priority 4)
} oi_flow_rule_t;
```
Maps 1:1 to an `ibv_create_flow` spec (`ibv_flow_attr` + one `ibv_flow_spec_eth` and, when
`match_5tuple`, one additional `ibv_flow_spec_ipv4`/`ibv_flow_spec_tcp_udp`).

### 3.4 Per-stage latency record format (measurement deliverable, R27)

One append-only line per run, never consumed by a gate:

```json
{"ts_utc": "2026-MM-DDThh:mm:ssZ", "box": "oci-bm-gpu-a10-4", "impl": "vram|cpustaged",
 "wiring_mode": 0, "stage": "dmabuf_reg_ns|qp_setup_ns|byte_landing_ns|throughput_pktps",
 "packet_size_bytes": 1500, "value": 0, "unit": "ns|pkts_per_sec", "run_id": "uuid",
 "label": "MEASUREMENT-ONLY-NOT-A-GATE"}
```
The `label` field is mandatory and literal — any tooling that renders this record SHALL surface it
unmodified (README spec-conventions rule: performance numbers never gate anything outside p6).

## 4. Configuration (YAML/env)

```yaml
# oi_ingest.yaml — read once at oi_ingest_init
ingest:
  impl: vram              # vram | cpustaged   (env override: OI_INGEST_IMPL)
  nic_dev: mlx5_0
  gpu_vendor: nvidia       # nvidia | amd       (env override: OI_GPU_VENDOR)
  ethertype: 0xAEFE
  ring_depth: 1024
  slot_size_bytes: 9000    # matches SIM fronthaul MTU (ARCHITECTURE_v3_SIM.md §2 fronthaul net)
  self_loopback: true
  wiring_mode: 0           # 0..4, see wiring_modes below

wiring_modes:
  0: {name: self_loopback,        needs: [pf_only]}
  1: {name: bifurcated_pf_share,  needs: [mlx5_loopback_devarg]}
  2: {name: sriov_vf_pair,        needs: [sriov_numvfs_writable]}
  3: {name: oci_l2_vlan,          needs: [second_host_or_vnic]}
  4: {name: udp_encap_l3,         needs: []}

fallback:
  freeze_breaker_days: 14         # SPEC R16 — 2 weeks
  trigger_on: m1_step3_fail       # auto-flip impl: vram -> cpustaged if step 3 doesn't pass in window
  manual_override: false          # true forces cpustaged regardless of spike outcome (documented escape)
```

Env-var overrides exist for CI/harness convenience (`OI_INGEST_IMPL`, `OI_GPU_VENDOR`,
`OI_WIRING_MODE`) — same precedence rule as SIM's `OI_CL_PLATFORM` (SIM §3.1).

## 5. Error handling

| Failure | Detected at | Handling |
|---|---|---|
| dmabuf export fails (CUDA/ROCm call error) | `oi_ingest_init` (VRAM impl) | return `OI_INGEST_ERR_DMABUF`; init aborts, no partial ring left allocated; caller (spike harness or production selector) logs and, in production, may trigger the config-level fallback (`fallback.trigger_on`) |
| `ibv_reg_dmabuf_mr` rejects the fd | `oi_ingest_init` (VRAM impl) | same as above — this is exactly SPEC R13's FAIL branch; the spike harness treats it as the freeze-breaker trigger, not a crash |
| Flow-rule install conflict (rule already exists / firmware rejects) | `oi_ingest_start` | return `OI_INGEST_ERR_FLOW_RULE`; caller SHALL NOT retry with a silently different rule — surfaced as a config error requiring operator action |
| Self-loopback flag unsupported by firmware | `oi_ingest_start` (VRAM impl, `wiring_mode: 0`) | return `OI_INGEST_ERR_INIT`; documented open question (§7) — no automatic fallback to a different wiring mode (D7: explicit config only), operator must re-select `wiring_mode` |
| Ring full (consumer too slow) | `oi_ingest_poll` / WQE post time | increment `frames_dropped`, return `OI_INGEST_ERR_RING_FULL` on the next post attempt if sustained; never silently overwrite an unreaped slot |
| CPU-staged memcpy failure (`cudaMemcpyAsync` error) | reap path, CPU-staged impl | treated as a dropped frame (`frames_dropped++`), descriptor not delivered; logged with the CUDA/HIP error code |
| GPU device lost / reset mid-run | either impl, detected on next API call | `oi_ingest_poll`/`_get_stats` return `OI_INGEST_ERR_NO_DEVICE`; caller tears down and re-inits — no in-place recovery attempted (out of scope: HA/hot-restart is not a PHY-0/1 requirement) |
| Freeze-breaker window elapsed with step 3 still failing | spike harness scheduling, not the library | harness sets `fallback.trigger_on` condition true; next `oi_ingest_init` in the production path reads `impl: cpustaged` from an updated config — **a config change, not code change** (SPEC R16/R21) |

## 6. Test plan (per requirement)

| Requirement | Test | PASS/FAIL criterion |
|---|---|---|
| P6-R1 | run `probe_nic_ident`, inspect `RESULT:` lines | PASS iff ≥1 mlx5 device listed |
| P6-R2 | run `probe02_dmabuf_mr --vendor {nvidia,amd}` | PASS iff both `DMABUF_EXPORT=ok` and `MR_REGISTER=ok` |
| P6-R3 | run `probe03_rawqp_hostmem` | PASS iff `QP_CREATE=ok`, `FLOW_RULE=ok`, `LOOPBACK_RX=ok` |
| P6-R4 | run `probe04_sriov_vf.sh` | recorded PASS/FAIL, non-blocking; feeds R24 eligibility |
| P6-R5 | run `probe05_dpdk_testpmd.sh`, observe forwarded-packet counter | PASS iff counter > 0 over a fixed test window |
| P6-R6 | run `probe06_gpu_ldpc.sh` (invokes SIM-2 LDPC suite binary against the box's vendor ICD) | PASS iff suite reports 0 mismatches vs golden vectors |
| P6-R7 | run `probe07_bar1.sh` (NVIDIA only) | always "recorded"; FAIL only if BAR1 field absent/0 |
| P6-R8 | run `probe08_openkm.sh` (NVIDIA only) | PASS iff license string `Dual MIT/GPL` AND `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED == 1` |
| P6-R9 | run `probe09_numvfs.sh` | recorded PASS/FAIL, non-blocking; feeds R24 eligibility (same underlying check as R4) |
| P6-R10 | run `probe10_selfloopback --count 1000` | PASS iff `FRAMES_RECEIVED == 1000` |
| P6-R11 | run `m1_step1_dmabuf` | PASS iff dmabuf MR probe (same criterion as R2) succeeds, logged as the formal spike entry |
| P6-R12 | run `m1_step2_hostmem_loopback --count 1000` | PASS iff frames received == sent, host-RAM MR, self-loopback flags active |
| P6-R13 | run `m1_step3_dmabuf_loopback --count 1000` | PASS iff `DMABUF_MR_ON_RAWQP=ok` and received count == sent count |
| P6-R14 | run `m1_step4_crc_kernel --count 1000` after R13 | PASS iff `CRC_MISMATCHES=0` |
| P6-R15 | build+run upstream `l2fwd-nv`, compare its received-frame count to sender count | PASS iff l2fwd-nv's own counters show 0 loss (independent of our stats) — NVIDIA boxes only, no-op/skipped on AMD (recorded as such, not a FAIL) |
| P6-R16 | spike-harness scheduling check: elapsed wall-clock since spike start vs 14-day budget, gated on R13's latest result | PASS iff (R13 passed within budget) OR (config flips to `impl: cpustaged` and R20/R21 pass) before day 14 |
| P6-R17 | soak test: run `oi_ingest_vram` for ≥10 min under steady synthetic traffic (from R12/R13's sender), monitor for any device allocation call after `oi_ingest_start` (instrumented build) | PASS iff zero post-setup allocations observed and ring remains operative throughout |
| P6-R18 | run with `ethertype: 0xAEFE` then rerun with `ethertype: 0xBEEF` against the same 0xAEFE sender | PASS iff first run delivers frames, second run delivers 0 (negative test) |
| P6-R19 | induce ring-full (slow consumer, small `ring_depth`) and read `oi_ingest_get_stats` | PASS iff `frames_dropped > 0` and `frames_seen == frames_matched + <frames not ethertype-matched>` reconciles |
| P6-R20 | black-box test: run the R18/R19 test suite unmodified against `oi_ingest_cpustaged` | PASS iff identical observable behavior (same stats semantics, same arena/descriptor contract) |
| P6-R21 | build `compute_backend`-equivalent test harness once; run it against both impls via config-only switch, no recompile | PASS iff both runs succeed with no source/binary change to the harness |
| P6-R22 | covered by R10/R12 (self-loopback flag success is priority 0's precondition) | same PASS criterion as R10/R12 |
| P6-R23 | attempt bifurcated PF share with mlx5 loopback devarg, external sender = `ru_emulator`(DPDK) | PASS iff devarg honored and frames flow ru_emulator → our QP within the ½-day probe budget |
| P6-R24 | attempt SR-IOV VF pair creation + eswitch loopback frame VF0→VF1, gated on R4/R9 PASS | PASS iff frame observed VF0→VF1 within the ½-day probe budget |
| P6-R25 | two VNICs/hosts on an OCI L2 VLAN, send one 0xAEFE frame, tcpdump the other end | PASS iff frame observed intact (ethertype preserved) |
| P6-R26 | UDP-encapsulate a sample eCPRI frame, send over ordinary IP, decapsulate at receiver | PASS iff decapsulated payload byte-identical to original |
| P6-R27 | run `latency_recorder` across a packet-size sweep once R13/R14 (or R20 fallback) pass | not PASS/FAIL — record emitted and validated only for schema conformance (§3.4), never for value thresholds |

## 7. Open questions

1. **Own-VF creation on OCI bare metal (R4/R9/R24):** unverified whether `sriov_numvfs` is
   writable on an Oracle-managed NIC at all, or locked down by firmware/hypervisor policy. Answer
   is a day-1 probe result, not assumable in advance.
2. **A10 BAR1 aperture size (R7):** unresolved in public sources; workstation Ampere siblings switch
   256 MiB↔32 GiB by mode, datacenter A10's OCI-shipped BAR1 is unconfirmed. Affects ring-capacity
   headroom (`ring_depth * slot_size_bytes` ceiling), not the mechanism's feasibility.
3. **Firmware support for `MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC` on OCI's specific
   ConnectX firmware revision:** documented in rdma-core as an API; not confirmed present/enabled
   on the exact firmware OCI ships. First real test is R10.
4. **Whether `ibv_reg_dmabuf_mr`'s resulting lkey is accepted by RX WQEs on a raw-packet QP at
   all** (chain link 4, deep-feasibility §1): the entire reason this is a spike and not a known
   quantity. No public counter-example exists either — genuinely open until R13 runs.
5. **Whether OCI's mlx5 firmware honors the bifurcated-PF-share loopback devarg (wiring priority
   1, R23)** — deep-feasibility §3 flags this as a ½-day probe with no prior confirmation.
6. **CPU-staged fallback's extra-copy cost** is unknown until measured (R27) — architecturally
   accounted for (one additional host-to-device copy vs. the VRAM path's zero-copy landing) but not
   quantified; this is intentionally a measurement deliverable, not a design input at spec time.
7. **Whether OCI L2 VLANs (wiring priority 3) actually carry a non-IP ethertype (0xAEFE) end to
   end** — deep-feasibility §3 cites a plausible VMware-style use case but no direct confirmation
   for eCPRI specifically; R25 is the first real test.
8. **p3's eventual LLD may name its `oi_ingest` structures differently** since it does not exist
   yet at time of writing (SPEC Dependencies) — if/when it lands, a reconciliation pass may be
   needed to keep both tiers' concrete struct layouts (not just the conceptual API) identical; not
   resolvable from p6 alone.
