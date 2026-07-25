# Open AI-RAN PoC v5 — TRUE NIC ⇒ CXL ⇒ GPU Data Path (Architecture Rewire)

## What this prompt is

v4 closed GAP 1/2/3: eBPF is genuinely in the path (929,474 CB
intercepts), the LDPC kernel is bit-exact (0 mismatches BG1/BG2), and
labeling is honest (latency_ladder_v2.csv). A re-audit CONFIRMED all
of this. v4 is DONE and its gate files stand.

v5 fixes the THREE things the re-audit flagged as still-wrong about
the ARCHITECTURE — not correctness, the data path itself:

```
v4 actual data path (what runs today):
  OAI ──uprobe──► BPF map (kernel RAM) ──2ms poll──► consumer
       ──copy──► OpenCL (PoCL/CPU, separate buffer)
  CXL is NOT in this path at all (DEV-014).

v5 target data path (NIC ⇒ CXL ⇒ GPU, for real):
  OAI (numactl --membind=CXL) ──uprobe──► 24-byte descriptor
       ──► userspace ring buffer ──► pinned-core busy-poll consumer
       ──► LLR already in /dev/dax0.0 (zero copy)
       ──► OpenCL reads CXL via CL_MEM_USE_HOST_PTR (zero copy)
       ──► decoded bits written back to CXL ──► OAI reads from CXL
```

## The three architecture changes (this is the whole point of v5)

```
CHANGE A — OAI allocations land in CXL, not system RAM
  PROBLEM (re-audit + external systems review): unmodified OAI
  malloc()s its LLR buffers on system RAM. The uprobe captures a
  pointer into system RAM, NOT into /dev/dax0.0. So "LLR is already
  in CXL" was false in v4.
  FIX: numactl --membind=<cxl-node> on the OAI process. This is the
  EXACT Pond/emucxl mechanism — zero source modification, all OAI
  allocations (including LLR buffers) land on the CXL NUMA node.
  DO NOT use LD_PRELOAD-on-malloc (intercepts everything, breaks on
  jemalloc/tcmalloc, and collides with the CET-SHSTK issue below).

CHANGE B — eBPF carries a DESCRIPTOR, not the LLR payload
  PROBLEM (re-audit "eBPF in offload data path" row): v4's
  ldpc_probe.bpf.c copies the LLR bytes INTO a BPF map. That's a
  payload copy through eBPF — exactly what "control-plane only" was
  supposed to avoid, and the reason the path isn't zero-copy.
  FIX: the uprobe writes only {llr_ptr, llr_len, out_ptr, out_len,
  slot_id, cb_index} — 40 bytes — to a userspace ring buffer. The
  LLR data stays where OAI put it (which, after Change A, is CXL).

CHANGE C — consumer is pinned-core busy-poll, not 2ms sleep
  PROBLEM (DEV-014): POLL_NS=2,000,000 in ldpc_measure.c dominates
  the 2,636us/slot interception number. That's an implementation
  artifact, not the bpftime floor (248.5ns).
  FIX: dedicated worker thread, pthread_setaffinity_np to an isolated
  core, strict non-blocking poll on the userspace ring buffer. No
  sleep, no eventfd, no scheduler wakeup. Detects descriptors in
  nanoseconds. (This is the DPDK/BBDEV pattern. bpftime's ring buffer
  is userspace shared memory, so a busy-poll loop sees writes with no
  kernel transition.)
```

## Cost discipline — READ THIS, it changes the workflow

```
DEVELOP ON WSL2. DEPLOY TO DO ONLY AT THE END.

The DigitalOcean droplet costs ~₹6/hr and is the ONLY place
/dev/dax0.0 exists (WSL2 has no /dev/kvm → no QEMU CXL VM). To save
cost, ALL code-writing, compiling, and logic-debugging happens on
WSL2 against a SYSTEM-RAM STAND-IN for CXL, and the droplet is spun
up ONCE at the very end to (a) confirm the path works against real
/dev/dax0.0 and (b) collect the CXLMemSim latency numbers.

WSL2 phase (Phases 1-4): everything except real CXL + CXLMemSim
  - Use a tmpfs or plain mmap'd file as a CXL STAND-IN so the
    consumer/OpenCL zero-copy logic can be written and tested.
  - numactl --membind on WSL2 single-node is a NO-OP but the LAUNCH
    COMMAND is identical — write it now, it just binds to node0.
  - Everything is structured so the ONLY thing that changes for DO
    is: stand-in path → /dev/dax0.0, and numactl node0 → CXL node.

DO phase (Phase 5 only): spin up, deploy, measure, tear down
  - Provision via ops/cxl-poc-droplet/scripts/provision.sh
  - rsync the WSL2-built tree, rebuild in the QEMU CXL VM
  - Run the real-CXL path check + CXLMemSim sweep
  - Collect CSVs, then checkpoint.sh + teardown.sh
  - Target: ONE droplet session, a few hours, under ₹100 total.
```

## Hard-gate philosophy (unchanged from v4)

```
A phase is complete only with RAW EVIDENCE (command output, file
contents, counts) — not "I implemented X". If a gate fails: try the
documented fix once, else the documented fallback WITH HONEST
RELABELING. Never present arithmetic as measured. Report to the user
after each phase's gate.

Memory protocol (v4_memory_protocol.md) STILL APPLIES: every v5 gate
gets a gate file with Spec/Commands/Raw-evidence/Self-verdict/
Deviations/Files sections under memory/v5_run/implementer/. Telemetry
(v5 telemetry prompt) audits them. Same discipline that caught the
original failure.
```

## What stays REAL from v4 (do not redo, do not doubt)

```
- PRIMARY_CONFIG: 11,703 us/slot = 23.4x. FIXED ANCHOR. Never moves.
- bit-exact OpenCL kernel: gpu_daemon/ldpc_cl/ldpc_decode.cl +
  bg_tables.h (786-line srsRAN BG1/BG2 tables). 0 mismatches.
  REUSE AS-IS. v5 changes how it's FED (CXL pointer), not the kernel.
- bpftime: 248.5 ns/call floor, builds, attaches to OAI. REUSE.
- OAI gNB + netns/veth: Gate 0.2/3 setup. REUSE.
- QEMU CXL VM: ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh,
  -cpu host,-hypervisor, persistent-memdev → /dev/dax0.0. REUSE.
- CET-SHSTK gotcha (references/cxl_qemu_kvm_gotchas.md §8): numactl
  --membind to CXL node SIGILLs ld.so's shadow stack; workaround is
  the mbind(>=16KB) LD_PRELOAD shim ALREADY WRITTEN. REUSE for
  Change A.
```

---

# PHASE 1 (v5) — CXL stand-in + descriptor ring buffer (WSL2, no cost)

Goal: build the zero-copy descriptor path on WSL2 against a CXL
STAND-IN, so the only thing that changes for DO is the backing path.

## 1.1 — CXL stand-in abstraction

Create `phase5_cxl/cxl_region.c` + `.h` with a single seam:

```c
// cxl_region.h
// ONE function decides where the "CXL" region comes from.
// WSL2: a plain mmap'd file (stand-in). DO: /dev/dax0.0.
// Selected by env var CXL_BACKING (default: stand-in file).
void *cxl_region_map(size_t size, uintptr_t *out_phys_hint);
// Returns a 4096-aligned base pointer (assert it — PoCL needs this).
```

```c
// cxl_region.c — backing selection
//   if getenv("CXL_BACKING") == "/dev/dax0.0":
//        fd = open("/dev/dax0.0", O_RDWR); mmap MAP_SHARED
//   else (WSL2 stand-in):
//        path = getenv("CXL_BACKING") ?: "/tmp/cxl_standin.bin"
//        ftruncate to size; mmap MAP_SHARED
//   ASSERT (base % 4096 == 0) either way.
//   Print which backing was used + the base address.
```

This is the seam that keeps DO to a one-line change later. Everything
downstream uses cxl_region_map(); it does not know or care whether
it's a stand-in or real DAX.

## 1.2 — Descriptor ring buffer (userspace, lock-free SPSC)

Create `phase5_cxl/desc_ring.h`:

```c
// Single-producer (uprobe handler) / single-consumer (pinned poller).
// Lives in shared memory so bpftime's userspace handler and the
// consumer both see it. Power-of-2 capacity, head/tail indices,
// no locks, no syscalls.
struct ldpc_desc {
    uint64_t timestamp_ns;
    uint32_t slot_id;
    uint32_t cb_index;
    uint64_t llr_off;    // OFFSET into the CXL region, not raw ptr
    uint32_t llr_len;
    uint64_t out_off;    // OFFSET into the CXL region
    uint32_t out_len;
    uint32_t seq;        // producer increments; consumer checks
};
// NOTE: offsets, not pointers — because after Change A the LLR is in
// the CXL region, and offset-into-region is mapping-independent
// (works whether base is stand-in or /dev/dax0.0).
```

```
Design notes for the agent:
- Capacity >= 2 * max_CBs_per_slot * pipeline_depth, power of 2.
- Producer: write payload fields, then a release-store to seq/head.
- Consumer: acquire-load head, read entry, advance tail.
- Use C11 atomics (atomic_store_explicit memory_order_release /
  acquire) — this is SPSC so no CAS needed.
```

## 1.3 — Pinned-core busy-poll consumer (Change C)

Create `phase5_cxl/consumer.c`:

```c
// - pthread_setaffinity_np to an isolated core (CPU N-1; document
//   the choice; on the DO droplet, consider isolcpus= but don't
//   require it — pinning alone is the main win).
// - STRICT busy-poll loop: no sleep, no eventfd, no condvar.
//     while (running) {
//        if (desc_ring_try_pop(&ring, &d)) { handle(&d); }
//        else { cpu_relax(); }   // PAUSE instruction, no syscall
//     }
// - handle(&d):
//     llr  = cxl_base + d.llr_off;   // already in CXL, zero copy
//     out  = cxl_base + d.out_off;
//     enqueue OpenCL kernel on llr -> out   (Phase 2 wires the
//       actual CL_MEM_USE_HOST_PTR; for Phase 1, stub handle() to
//       just memcpy llr->out and count, so the RING is testable
//       before OpenCL is wired in)
// - Maintain counters: descriptors_seen, handle_ns histogram.
```

### GATE 1 (v5)
```
PASS if (all on WSL2, stand-in backing):
  - A test producer pushing K descriptors into desc_ring is fully
    drained by the busy-poll consumer with descriptors_seen == K and
    ZERO drops, across K = 1e3, 1e5, 1e6.
  - Consumer's poll-to-handle latency (producer release-store ->
    consumer reads entry) p50 is sub-microsecond (this is the
    busy-poll floor; record the actual histogram). Contrast with
    v4's 2ms — record both in the gate file.
  - cxl_region_map() asserts 4096-alignment and prints the backing
    path. Evidence: the printed base address + alignment assertion
    passing.

FAIL -> if drops occur: ring capacity / memory-ordering bug (check
        release/acquire pairing). If latency isn't sub-us: confirm
        the poll loop has no accidental syscall (strace it — a clean
        busy-poll shows NO syscalls in steady state; if you see
        futex/nanosleep, the sleep wasn't actually removed).
```

---

# PHASE 2 (v5) — Wire OpenCL to read CXL directly (WSL2, no cost)

Goal: the bit-exact kernel from v4 now reads/writes the CXL region
in-place (zero copy), still on WSL2 stand-in.

## 2.1 — clCreateBuffer over the CXL region

```c
// In consumer's OpenCL setup (reuse gpu_daemon/ldpc_cl kernel):
//   cxl_base = cxl_region_map(REGION_SZ, NULL);   // Phase 1 seam
//   cl_mem llr_buf = clCreateBuffer(ctx,
//       CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
//       REGION_SZ, cxl_base, &err);
//   // out buffer can be a sub-region or a second mapping; keep LLR
//   // and OUT in the SAME cxl_base region at different offsets so
//   // one CL_MEM_USE_HOST_PTR buffer covers both (simpler) OR two
//   // buffers over two sub-ranges. Document the choice.
//   assert(err == CL_SUCCESS);
```

```
CRITICAL PoCL caveat (from research doc): CL_MEM_USE_HOST_PTR needs
4096-byte alignment (Phase 1.1 guarantees) AND buffer size a multiple
of 64 bytes (pad REGION_SZ). If PoCL SILENTLY COPIES despite this,
the zero-copy claim is false — DETECT THIS:
  - After kernel run, write a sentinel into the CXL region from the
    CPU side at a known offset BEFORE enqueue, have the kernel read
    it; and have the kernel write a sentinel the CPU reads after.
  - If CPU-side writes are visible to the kernel and vice versa
    WITHOUT explicit clEnqueueWrite/ReadBuffer, zero-copy is real.
  - If you MUST call clEnqueueWriteBuffer for the kernel to see the
    data, PoCL copied — record this honestly as a PoCL limitation in
    the gate file and emulation_mode.txt; the architecture is still
    valid (real GPU OpenCL or SVM would zero-copy), it's a PoCL-CPU
    artifact.
```

## 2.2 — Per-descriptor decode through the kernel

```c
// handle(&d) now:
//   llr_off / out_off select sub-ranges of the SAME cl_mem (use
//     clSetKernelArg with offset, or a sub-buffer via
//     clCreateSubBuffer at d.llr_off — note sub-buffer origin must
//     be CL_DEVICE_MEM_BASE_ADDR_ALIGN aligned; pad offsets).
//   set kernel args (BG, Z, n_iter=I per v4's bit-exact config),
//   clEnqueueNDRangeKernel (Z-tiled, work-group per CB),
//   clFinish, read decoded bits from cxl_base + out_off.
```

## 2.3 — Re-verify bit-exactness THROUGH the new path

The v4 bit_diff_test proved the KERNEL is bit-exact. v5 must prove the
kernel is STILL bit-exact when fed via the CXL-region path (offsets,
sub-buffers, USE_HOST_PTR) — a wiring change can corrupt data without
touching the kernel.

```
Re-run the v4 bit-correctness oracle, but route the LLR input and
decoded output THROUGH cxl_region (write LLR to cxl_base+llr_off,
decode via handle(), read from cxl_base+out_off, compare to expected).
Write paper/results/bit_correctness_cxlpath.csv.
```

### GATE 2 (v5)
```
PASS if:
  - bit_correctness_cxlpath.csv shows bit_diff_rate == 0 for BG1/
    LS=384 (minimum) routed THROUGH the CXL-region path. This proves
    the zero-copy wiring didn't corrupt the bit-exact result.
  - Zero-copy detection (2.1) reported: either CONFIRMED zero-copy,
    or HONESTLY recorded as "PoCL copied — CPU artifact" with the
    sentinel evidence shown.
  - Evidence: the sentinel test output + the bit_diff CSV.

FAIL -> bit_diff != 0 through the path means an offset/alignment/
        sub-buffer bug (the kernel itself is proven good by v4).
        Diff the LLR bytes at cxl_base+llr_off against the oracle
        input to find where the wiring corrupts them.
```

---

# PHASE 3 (v5) — Rewire the uprobe: descriptor not payload (WSL2)

Goal: bpftime uprobe writes a 40-byte descriptor (Change B) instead
of copying LLR into a BPF map. Still WSL2; OAI's LLR lives in the
stand-in region via the launch wrapper.

## 3.1 — OAI allocation diversion (Change A), stand-in form

```bash
# The LAUNCH COMMAND that will be identical on DO except node number.
# On WSL2 (single node) this binds to node0 = no-op, but the command
# is what ships to DO:
numactl --cpunodebind=0 --membind=0 \
  env LD_PRELOAD="$BPFTIME_AGENT:$MBIND_SHIM" \
      BPFTIME_VM_NAME=ubpf \
  nr-softmodem -O <conf> --phy-test --rfsim ...

# $MBIND_SHIM = the CET-SHSTK workaround shim from
# references/cxl_qemu_kvm_gotchas.md §8 (mbind >=16KB allocations to
# the target node, keep small allocs incl. shadow stack on node0).
# On WSL2 this shim is harmless; on DO it's REQUIRED or ld.so SIGILLs.
```

```
IMPORTANT for the stand-in: on WSL2 there is no separate CXL node, so
OAI's LLR buffer is in normal RAM. To still exercise the descriptor
path, the uprobe handler computes llr_off as (llr_ptr - region_base)
ONLY when llr_ptr falls within the registered CXL region; on WSL2
stand-in, instead COPY the LLR into cxl_base+llr_off inside the
handler (one copy, WSL2-only) and set llr_off accordingly. Gate the
copy behind CXL_BACKING==standin so that on DO (where OAI already
allocates in /dev/dax0.0) NO copy happens. Document this WSL2-only
copy explicitly — it is the one place WSL2 and DO behavior differ,
and the DO run must show the copy is skipped.
```

## 3.2 — bpftime uprobe writes the descriptor

```c
// Replace v4's "copy LLR into BPF map" with:
//   - read args of nrLDPC_coding_segment_decoder (or _decoder; use
//     the per-CB symbol per v4 Gate 2 — confirm offset, v4 had
//     LDPCdecoder at 0x63110)
//   - compute llr_off/out_off relative to the registered CXL region
//     base (passed to the agent via env or a fixed mmap address)
//   - desc_ring_push({ts, slot_id, cb_index, llr_off, llr_len,
//     out_off, out_len, seq})
//   - return — NO payload copy through eBPF, NO blocking
```

```
Address-the-DEV-003-ghost (atomic counter): the descriptor ring is
SPSC per probe site. If OAI calls the decode symbol from MULTIPLE
threads concurrently, you have MULTIPLE producers -> SPSC is unsafe.
RESOLVE EXPLICITLY (this was flagged twice in v4 telemetry and never
closed):
  (a) confirm via the call pattern whether decode is serialized per
      slot (log thread IDs at the uprobe for 100 calls), OR
  (b) if multi-threaded, use one SPSC ring PER producer thread
      (keyed by tid) and have the consumer poll all of them, OR
  (c) a single MPSC ring with a proper multi-producer enqueue.
  State which, with the thread-ID evidence, in the gate file.
```

### GATE 3 (v5)
```
PASS if (WSL2, stand-in):
  - Live OAI gNB run (netns/veth per v4 Gate 0.2/3) produces
    descriptors in the ring at the expected per-CB rate; consumer
    drains them; descriptors_seen consistent with slot count *
    C_actual (state C_actual — v4 found phy-test gives C=2; see
    Phase 5 note on C=24).
  - The uprobe handler does NOT copy payload (except the documented
    WSL2-only stand-in copy in 3.1) — show the handler source and
    confirm no LLR-sized memcpy in the DO path.
  - Thread-safety of the ring RESOLVED with thread-ID evidence
    (closes the DEV-003 ghost).
  - Evidence: descriptor count vs expected, thread-ID log, handler
    source.

FAIL -> 0 descriptors: symbol/offset/arg-register issue (v4 Gate 2
        debugging applies). Inconsistent count under load: ring
        capacity or the multi-producer issue above.
```

---

# PHASE 4 (v5) — End-to-end on WSL2 stand-in + ablation harness

Goal: full path runs end-to-end on WSL2 (OAI → descriptor → busy-poll
consumer → OpenCL-over-CXL-standin → back to OAI), and the Phase-5
measurement harness is built and dry-run — so the DO session only
swaps the backing and collects numbers.

## 4.1 — Full pipeline dry-run on WSL2

```
Wire it together:
  OAI (numactl node0, agent+shim) → uprobe descriptor → ring →
  pinned busy-poll consumer → OpenCL kernel reads cxl_standin →
  decoded bits to cxl_standin+out_off → OAI reads result.
Confirm OAI's decode actually RECEIVES correct bits back (not just
that the pipeline runs): since the kernel is bit-exact (Gate 2 v5),
OAI's CRC on the returned bits should PASS for the decoded CBs.
Log CRC pass/fail as seen by OAI.
```

## 4.2 — Ablation harness (replaces v4 ldpc_measure.c)

```c
// phase5_cxl/ablation.c — the C=24-aware, busy-poll, CXL-path harness.
// Rows (each N>=1000 slots, report mean/p50/p95/p99):
//   baseline            : PRIMARY_CONFIG anchor 11,703 us/slot (FIXED,
//                         do not re-measure)
//   interception_only   : descriptor path + busy-poll, consumer
//                         returns input unchanged (no OpenCL). THIS
//                         is now the REAL interception cost at the
//                         busy-poll floor — should be ORDERS below
//                         v4's 2,636us (which was poll-dominated).
//   gpu_compute_full    : + OpenCL bit-exact decode over CXL.
//   (DO adds: x CXLMemSim latency points)
// MUST record C_actual and, if C_actual != 24, ALSO report the
// C=24-normalized projection with the formula shown (per v4 DEV-009;
// telemetry will check this).
```

## 4.3 — C=24 config attempt (close DEV-009)

```
v4's phy-test gave C_actual=2, making ablation numbers 1/12th of
PRIMARY_CONFIG's workload and NOT directly comparable to 23.4x.
ATTEMPT to make OAI produce C=24:
  - Raise MCS / PRB allocation in the gNB phytest conf so TBS pushes
    the codeblock count to 24 (the same MCS28/273PRB regime that
    PRIMARY_CONFIG assumes). Check the OAI conf for the MCS and
    n_rb_dl / PRB fields.
  - Confirm via the descriptor cb_index range (0..23) or a per-slot
    CB count log.
If C=24 is achievable: all Phase 5 ablation runs at C=24, directly
comparable to 23.4x. If NOT achievable in phy-test: keep C_actual,
and the harness MUST emit the C=24 projection explicitly (state the
scaling and that it's a projection). Either way, DEV-009 is CLOSED
by being resolved-or-explicitly-quantified, not left ambiguous.
```

### GATE 4 (v5)
```
PASS if (WSL2 stand-in):
  - Full pipeline runs end-to-end AND OAI's CRC passes on returned
    decoded bits (proves correct bits flow back, not just plumbing).
  - ablation.c produces a dry-run latency_ladder_v2.csv on stand-in
    with honest source labels and C_actual recorded; interception_
    only row is sub-10us-class (busy-poll floor), NOT 2,636us —
    record the actual number and contrast with v4 explicitly.
  - C=24 either achieved (cb_index hits 0..23, evidence shown) or the
    projection formula is in the harness output (DEV-009 closed).
  - Evidence: CRC-pass log, the stand-in CSV, C_actual evidence.

FAIL -> CRC fails: bits are corrupted somewhere in the path (Gate 2
        v5 proved the kernel; suspect descriptor offsets or the
        return copy). Interception_only still ~ms: the sleep wasn't
        actually removed (strace for nanosleep/futex).
```

---

# PHASE 5 (v5) — DEPLOY TO DO: real CXL + CXLMemSim (THE ONLY PAID PHASE)

Everything before this ran on WSL2 for free. NOW, and only now, spin
up the droplet. Goal: confirm the path works against real /dev/dax0.0
and collect the latency numbers. Target ONE session, then tear down.

## 5.1 — Provision + deploy

```bash
# From WSL2:
cd ops/cxl-poc-droplet/scripts
./provision.sh                 # creates cxl-poc, prints IP, checks
                                # /dev/kvm + avx2
./install_deps.sh              # qemu/bpftime-deps/pocl/numactl/etc.

# rsync the WSL2-built tree (source, not build artifacts):
rsync -az --exclude '*/build' --exclude '.git' \
  ~/linux_env/cxl/ root@<droplet-ip>:/root/cxl/

# On the droplet: rebuild bpftime, OpenCL daemon, consumer, ablation,
# OAI (or rsync OAI build if ABI-compatible — safer to rebuild).
```

## 5.2 — Boot the QEMU CXL VM and switch backing to real DAX

```bash
# On the droplet:
cd /root/cxl/ops/cxl-poc-droplet/scripts
./qemu_cxl_launch.sh <disk-image>.qcow2     # -cpu host,-hypervisor,
                                             # persistent-memdev
./verify_cxl_checks.sh                       # expect 5/5 PASS

# Inside the VM, daxctl --mode=system-ram → NUMA node1, /dev/dax0.0.
# THE ONLY CODE-PATH CHANGE FROM WSL2:
export CXL_BACKING=/dev/dax0.0
# and the OAI launch numactl now targets the REAL cxl node:
numactl --cpunodebind=0 --membind=1 \
  env LD_PRELOAD="$BPFTIME_AGENT:$MBIND_SHIM" BPFTIME_VM_NAME=ubpf \
  nr-softmodem ...
# (--membind=1 = the CXL NUMA node; $MBIND_SHIM now MANDATORY for the
#  CET-SHSTK shadow-stack issue, per gotchas §8.)
```

## 5.3 — The TWO things WSL2 couldn't prove

```
PROOF 1 — LLR genuinely lands in CXL, no copy (validates Change A):
  With CXL_BACKING=/dev/dax0.0 and numactl --membind=1, capture an
  uprobe descriptor's llr_ptr and confirm it falls within the
  /dev/dax0.0 mmap range [cxl_base, cxl_base+REGION_SZ). If yes:
  OAI's unmodified allocation landed in CXL, the WSL2-only stand-in
  copy (3.1) is SKIPPED, and the path is genuinely zero-copy.
  Evidence: the pointer-range check output, and confirmation the
  stand-in copy branch did NOT execute (a counter or log line).

PROOF 2 — descriptors reference CXL, OpenCL reads CXL (validates the
  whole NIC⇒CXL⇒GPU claim):
  Confirm llr_off/out_off resolve to /dev/dax0.0 addresses and the
  OpenCL CL_MEM_USE_HOST_PTR buffer is over the DAX mapping. Re-run
  the zero-copy sentinel test (Phase 2.1) against real DAX.
```

## 5.4 — CXLMemSim latency sweep (closes Gate 0.3 / Phase 4 v4 deferral)

```bash
# Build CXLMemSim on the droplet (real PMU exists here, unlike WSL2):
# perf_event_open(PERF_TYPE_HARDWARE) should now succeed.
for lat in 0 100 142 200 255 300; do
  ./cxlmemsim --latency=${lat} -- ./ablation --row gpu_compute_full \
     --slots 1000 --cxl-backing /dev/dax0.0
done
# Writes paper/results/cxl_latency_sensitivity.csv:
#   injected_latency_ns, slot_us_mean, p50, p95, p99
```

## 5.5 — Collect, then STOP BILLING

```bash
# rsync results back to WSL2:
rsync -az root@<droplet-ip>:/root/cxl/paper/results/ \
  ~/linux_env/cxl/paper/results/
# Then, immediately:
cd ops/cxl-poc-droplet/scripts
./checkpoint.sh      # snapshot (resume later if needed)
./teardown.sh        # DESTROY droplet — billing stops NOW
```

### GATE 5 (v5)
```
PASS if (on DO, real /dev/dax0.0):
  - verify_cxl_checks.sh 5/5 PASS.
  - PROOF 1: descriptor llr_ptr within /dev/dax0.0 range; stand-in
    copy branch SKIPPED. Evidence shown.
  - PROOF 2: OpenCL buffer over DAX, sentinel zero-copy test passes
    (or PoCL-copy honestly recorded).
  - cxl_latency_sensitivity.csv has 6 points x N>=1000, and the
    slot time increases monotonically (or near-) with injected
    latency (if it DOESN'T, CXLMemSim isn't affecting the path —
    investigate before claiming the figure).
  - ablation latency_ladder_v2.csv collected at real DAX, honest
    source labels, C_actual recorded.
  - Droplet TORN DOWN (status.sh shows it gone) — cost contained.

FAIL -> verify_cxl fails: image/kernel issue (v4 Part C config is
        known-good; diff against it). llr_ptr NOT in DAX range:
        numactl --membind didn't divert OAI (check the shim, check
        node number, check daxctl actually onlined node1). CXLMemSim
        no effect: it may not be intercepting the OpenCL process —
        confirm it wraps the right PID.
```

---

# PHASE 6 (v5) — Artifacts + honest architecture writeup

## 6.1 — Figures

```
latency_ladder_v5.pdf       baseline / interception_only (busy-poll
                            floor) / gpu_compute_full — the REWIRED
                            numbers. Show v4's 2,636us interception
                            vs v5's busy-poll number as the headline
                            improvement of the rewire.
cxl_latency_sensitivity.pdf 6-point CXLMemSim sweep (real DAX) —
                            the figure v4 never had.
data_path_v5.pdf            the NIC⇒CXL⇒GPU diagram, annotated with
                            "zero-copy" on the CXL edges and the
                            actual measured per-edge costs.
```

## 6.2 — RESULTS_SUMMARY v5 — the honest architecture story

```
The narrative is now:
  1. PRIMARY_CONFIG 23.4x (fixed anchor, unchanged).
  2. v4 proved each COMPONENT real (eBPF in path, bit-exact LDPC).
  3. v5 proved the DATA PATH real: OAI allocations in CXL (numactl,
     zero source mod), descriptor-only eBPF (no payload copy),
     busy-poll consumer (no 2ms artifact), OpenCL reads CXL in-place.
  4. Measured: interception_only at busy-poll floor (Xus, vs v4's
     2,636us), gpu_compute_full over real DAX (Yus), CXLMemSim
     sensitivity (6 points).
  5. Honest caveats (UNCHANGED, state them):
     - "GPU" = PoCL/CPU; real GPU is the 6x projection (Six Times to
       Spare). v5 did NOT add a real GPU.
     - QEMU CXL doesn't model latency natively (Part C / emucxl/Pond
       single-socket finding); CXLMemSim provides the injected-
       latency figure; real-silicon validation is IISc/future work.
     - C_actual handling (24 achieved, or projection stated).
     - If PoCL copied instead of zero-copy: stated as CPU artifact.
```

## 6.3 — Update the skill + commit

```bash
cd ops/cxl-poc-droplet
# Add phase5_cxl build notes, the CXL_BACKING seam, the numactl+shim
# launch line, CXLMemSim build notes (real-PMU-required), to
# references/. Commit memory/v5_run/ too.
git add -A && git commit -m "v5: true NIC=>CXL=>GPU data path —
  numactl CXL diversion, descriptor-only eBPF, busy-poll consumer,
  real-DAX validation + CXLMemSim sweep"
```

### GATE 6 (v5)
```
PASS if: all 6.1 figures render, RESULTS_SUMMARY v5 tells the
         component-real (v4) + path-real (v5) story with every number
         traceable to a gate, caveats stated, skill updated/committed.
```

---

# Final report-back

```
Gate 1 (ring + busy-poll):     PASS/FAIL — busy-poll p50 vs v4 2ms
Gate 2 (OpenCL over CXL):      PASS/FAIL — bit-exact through path;
                               zero-copy CONFIRMED / PoCL-copied
Gate 3 (descriptor uprobe):    PASS/FAIL — no payload copy; DEV-003
                               thread-safety resolved
Gate 4 (WSL2 end-to-end):      PASS/FAIL — OAI CRC passes; interception
                               floor; C=24 achieved/projected
Gate 5 (DO real DAX + sweep):  PASS/FAIL — llr in DAX; sentinel;
                               CXLMemSim 6pts; TORN DOWN
Gate 6 (artifacts):            PASS/FAIL

Architecture status: NIC⇒CXL⇒GPU data path — component-real (v4) +
  path-real (v5: numactl CXL diversion / descriptor-only eBPF /
  busy-poll / OpenCL-reads-CXL). Caveat: GPU=PoCL-CPU (6x projection).
PRIMARY_CONFIG 23.4x: UNCHANGED.
DO cost this run: <hours x ₹6/hr>, droplet torn down.
```

---

# Memory + telemetry (REUSED from v4, point them at v5_run)

```
- Apply v4_memory_protocol.md, but folder = memory/v5_run/implementer/
  with gate files gate_1 .. gate_6 (v5's six gates). Same template:
  Spec / Commands / Raw-evidence / Self-verdict / Deviations / Files.
  DEVIATIONS.md continues the SAME DEV-NNN sequence (next is DEV-015+,
  since v4 ended at DEV-014) so the history is continuous.
- Run the v4 telemetry prompt after each v5 phase, folder = v5_run.
  Its Gate 1/2 mandatory spot-checks map to v5 Gate 2 (bit-exact
  through CXL path) and v5 Gate 3 (descriptor uprobe firing) — the
  two that carry the original-audit DNA. The PRIMARY_CONFIG 23.4x
  immutability check still applies.
- The cost rule is itself a telemetry check: if any gate file BEFORE
  Gate 5 shows a DO droplet was provisioned, that's a FINDING (money
  spent early) — Phases 1-4 are WSL2-only by design.
```