# Open AI-RAN PoC v4 — NIC ⇒ CXL ⇒ GPU, No-Compromise Cursor Prompt

## You are picking up a project with real history and a real audit finding

This is NOT a fresh start. Read this whole header before touching code.

### What exists and is REAL (do not redo, do not doubt without new evidence)

```
srsRAN_Project, pinned at commit 3f244c05 (release 25.04)
  - tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark
  - BG1 LS=384 R=0.917 I=20, AVX2: 487.6 us/CB (p50, N=1000)
  - TBS calc (100MHz, MCS28, 273 PRB, mu=1, 0.5ms slot) -> C=24 CB/slot
  - PRIMARY_CONFIG headline: 11,703 us/slot = 23.4x over 500us budget
  - git diff is empty (zero source modifications) — THIS NUMBER STANDS,
    do not change it, do not re-derive it. It is the paper's motivation.

QEMU CXL Type-3, verified on droplet cxl-poc (DO BLR1, s-4vcpu-8gb):
  -enable-kvm -cpu host,-hypervisor -M q35,cxl=on
  + cxl-type3 with persistent-memdev (NOT volatile-memdev — dpa_size=0
    bug in QEMU 8.2)
  Result: /dev/dax0.0 real, daxctl --mode=system-ram -> NUMA node1
  (1920MB, distance 0->1=20), drivers/cxl/ genuinely exercised.
  Reusable launch script: ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh
  Reusable verification: ops/cxl-poc-droplet/scripts/verify_cxl_checks.sh
  (5/5 PASS expected — hypervisor bit clear, no cache-sync warning,
   cxl create-region succeeds, daxctl system-ram, NUMA shows 2 nodes)

PoCL OpenCL CPU runtime: confirmed live ("Portable Computing Language",
  cpu-haswell device), 8 real OpenCL API calls already exist in
  gpu_daemon/gpu_opencl.c (clCreateContext/Buffer/BuildProgram/
  EnqueueNDRangeKernel). CL_MEM_USE_HOST_PTR already used.
```

### What an audit found is TOY/FAKE and is the reason this prompt exists

```
GAP 1 — eBPF not in the data path. bpftool prog show = 0 loaded during
  measurement. measure.c reaches the daemon via a weak-symbol -> Unix
  socket. The reported "12,036us sync / 11,727us async offload" numbers
  are ARITHMETIC (baseline + a separately-measured constant), tagged
  source=measured in latency_ladder.csv — THIS IS THE MISLABEL. The ONE
  time the real pipeline ran end-to-end: ~410,000 us/slot, discarded.

GAP 2 — GPU daemon's LDPC kernel is "simplified 4-variable-node window",
  1 pass vs srsRAN's I=20. l1_sim/ldpc.c encoder = memcpy, decoder uses
  hashed indices ((i*9973+j*4099)%n). functional_correctness.txt = bare
  "PASS" string, no actual bit-correctness check exists anywhere.

GAP 3 — "L1 workload" is a standalone benchmark binary called in a loop,
  not a continuously-running L1 process. For "NIC reaches host" framing
  this is insufficient — there is no NIC in the picture at all.
```

### The missed ingredient — emucxl/Pond, and why Part C's 53us is CORRECT

```
emucxl (Gond & Kulkarni, IIT Bombay, arXiv:2404.08311) and Pond
(ASPLOS'23) emulate CXL via QEMU + NUMA — STRUCTURALLY what we built
in Part C. But their signal (Pond/Cohet: 70-90ns real cross-socket
delta) comes from a REAL MULTI-SOCKET HOST. Our droplet is single-
socket: QEMU's "node1" is a label with no real interconnect distance
behind it. Part C's 53us/±100us-noise result is the CORRECT empirical
confirmation of this — not a QEMU failure, a missing substrate.

RESOLUTION (Phase 4 below): CXLMemSim (SlugLab, arXiv:2303.06153) is
topology-independent software latency injection, validated against
real CXL silicon. This is the path to a real latency-sensitivity
figure regardless of host topology.
```

---

## Architecture — the integrated pipeline (see rendered diagram
"nic_cxl_gpu_integrated_pipeline" from this session)

```
Band 1 — Network namespaces, veth, XDP
  nr-uesoftmodem (netns ue) <--veth--> nr-softmodem --rfsim --phy-test
  (netns gnb). XDP on the gnb-side veth interface observes/timestamps
  RFsim IQ traffic — THIS is the "NIC" in NIC=>CXL=>GPU. Real
  network-stack traffic, not AF_UNIX, regardless of OAI's default
  transport (we force it via netns placement).

Band 2 — L1 PHY, continuous decode calls
  Inside the gNB process: libldpc.so, nrLDPC_coding_decoder() /
  nrLDPC_coding_segment_decoder(), clean C ABI, called every slot.

Band 3 — bpftime, eBPF control plane only
  Userspace uprobe (no kernel trap) on the LDPC decode symbol.
  Reads pointer+length, writes a descriptor, programs the CXL buffer
  mapping on first use, then steps aside. No payload through eBPF.

Band 4 — CXL shared memory, /dev/dax0.0
  LLR input buffer, decoded-output buffer. CXLMemSim wraps this with
  injected latency: 0 / 100 / 142 / 200 / 255 / 300 ns sweep.

Band 5 — GPU daemon, PoCL OpenCL
  Z-tiled (Z=384) kernel, BG1/BG2 LUTs ported from srsRAN, bit-exact
  vs srsRAN's own ldpc_decoder_test_data vectors at I=20.
  Decoded bits returned to Band 2 (green return arrow in diagram).
```

---

## Hard-gate philosophy — read this twice

```
Each phase below has a HARD GATE. A phase is NOT "complete" without
its gate's evidence (a command's output, a file's contents, a
tracelog count). "I implemented X" is not a gate pass. "bpftool/
bpftime shows N events fired, N == expected" is a gate pass.

If a gate FAILS:
  1. Try the documented fix (given per-phase below) — ONE attempt.
  2. If still failing: switch to the documented FALLBACK, and
     RELABEL accordingly (e.g. "bpftime unusable -> kernel uprobes,
     report 9.94us/CB as MEASURED, not arithmetic"). A correctly-
     labeled fallback number is acceptable. An unlabeled arithmetic
     number presented as measured is what caused the audit failure —
     do not repeat this pattern anywhere in this prompt's output.
  3. Do NOT proceed to the next phase on a failed gate without
     either (1) or (2) resolved and documented in
     paper/results/emulation_mode.txt.

Report back to the user after EACH PHASE's gate, pass or fail, before
continuing — per-phase checkpoints, not one giant silent run.
```

---

# PHASE 0 — Investigation & de-risking (time-boxed, do FIRST)

The riskiest new dependency is bpftime. De-risk it BEFORE investing in
the OAI build. Total Phase 0 budget: aim for half a day, not more.

## 0.1 — bpftime smoke test (CRITICAL FIRST CHECK)

```bash
git clone https://github.com/eunomia-bpf/bpftime.git
cd bpftime
# Follow README build instructions for this Ubuntu version. Note known
# deps: LLVM, clang, recent cmake. If build fails on a dependency,
# spend at most 30 min on the fix before declaring this gate FAILED.

# Smoke test: attach a bpftime uprobe to a trivial loop, same shape as
# the earlier D1 isolation (100k-iteration no-op call), and compare
# against the kernel-uprobe baseline already measured (9,941 ns/call).
#
# Write a tiny test_target.c with a no-op function called in a loop,
# attach via bpftime's uprobe example (example/uprobe or similar —
# check bpftime/example/ for the closest analog), measure mean call
# latency WITH the bpftime uprobe attached vs WITHOUT.
```

Record in `paper/results/bpftime_smoke.csv`:
```
method,overhead_ns
kernel_uprobe_baseline,9941   # from D1, for reference
bpftime_uprobe,<measured>
```

### GATE 0.1
```
PASS if: bpftime builds, attaches, and bpftime_uprobe overhead is
         BOTH lower than 9,941ns AND stable across 3 repeated runs
         (stddev < mean — i.e. NOT the "sigma > mean" noise pattern
         from the WSL2 eBPF measurement).

FAIL  -> DECISION: bpftime is NOT the interception mechanism for this
         project. Phase 2 uses BPF_MAP_TYPE_RINGBUF kernel uprobes
         instead. The 9,941ns number is then the HONEST per-CB
         interception cost — record it as
         `interception_overhead_ns: 9941, method: kernel_uprobe,
          source: measured (D1)` in calibration_check.txt. This is
         FINE. Document the bpftime attempt and failure reason in
         ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md so
         nobody retries it blind.

Either outcome is a PASS for the overall project — write down which
one, then move on. Do not spend more than the time-box on this.
```

## 0.2 — OAI build + RFsim transport + netns/veth setup

```bash
git clone https://github.com/OPENAIRINTERFACE/openairinterface5g.git
cd openairinterface5g
./build_oai --gNB --nrUE -w SIMU 2>&1 | tee build_oai.log

# Confirm the LDPC interception target exists:
nm -D cmake_targets/ran_build/build/liboai_device.so 2>/dev/null \
  | grep -i ldpc
find . -name "libldpc*.so" -exec nm -D {} \; 2>/dev/null | grep -i ldpc
# Expect: nrLDPC_coding_decoder and/or nrLDPC_coding_segment_decoder
```

Regardless of what transport OAI's rfsimulator uses by default, build
the netns/veth setup so the RFsim link is UNAMBIGUOUSLY real
network-stack traffic:

```bash
# Two network namespaces, veth pair between them
ip netns add ue-ns
ip netns add gnb-ns
ip link add veth-ue type veth peer name veth-gnb
ip link set veth-ue netns ue-ns
ip link set veth-gnb netns gnb-ns
ip netns exec ue-ns  ip addr add 10.77.0.1/24 dev veth-ue
ip netns exec gnb-ns ip addr add 10.77.0.2/24 dev veth-gnb
ip netns exec ue-ns  ip link set veth-ue up
ip netns exec gnb-ns ip link set veth-gnb up
ip netns exec ue-ns  ip link set lo up
ip netns exec gnb-ns ip link set lo up

# Quick transport check (informational — proceed with netns regardless):
ip netns exec gnb-ns ss -tlnp &   # watch for rfsimulator listening socket
# launch a brief gNB --rfsim --phy-test in gnb-ns, observe ss output,
# kill after confirming TCP vs unix. Record finding in
# ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md — this is
# useful context but does NOT change the plan: netns+veth is used
# either way.
```

### GATE 0.2
```
PASS if: both netns exist, veth pair up, gNB --rfsim --phy-test
         launched with --rfsimulator.serveraddr 10.77.0.2 (in gnb-ns)
         and UE with --rfsimulator.serveraddr 10.77.0.2 (in ue-ns)
         establishes a connection (check OAI logs for "rfsimulator"
         connect messages on both sides).

FAIL  -> debug netns/veth networking first (this is standard Linux
         networking, not exotic — a ping between 10.77.0.1 and
         10.77.0.2 across the veth pair is the minimal check before
         even involving OAI).
```

## 0.3 — CXLMemSim build + smoke test

```bash
git clone https://github.com/SlugLab/CXLMemSim.git
cd CXLMemSim
mkdir build && cd build
cmake .. && make -j$(nproc)

# Smoke test against a trivial memory-bound loop (e.g. a pointer-chase
# over a large array), at --latency=0 and --latency=142:
./cxlmemsim --latency=0   -- ./your_membench
./cxlmemsim --latency=142 -- ./your_membench
# Confirm the 142ns run shows HIGHER latency than the 0ns run by a
# measurable, consistent amount across repeated runs.
```

### GATE 0.3
```
PASS if: CXLMemSim builds, runs, and the latency=142 run is
         measurably and consistently slower than latency=0.

FAIL  -> spend at most 1 hour on build issues (check issue tracker
         for your exact kernel/perf_event_open permissions — CXLMemSim
         typically needs perf counter access). If still failing,
         DECISION: Phase 4's latency-sensitivity figure is deferred
         to Limitations/Future Work (IISc real hardware), and Phase 4
         proceeds WITHOUT the CXLMemSim sweep — but Part C's real
         drivers/cxl/ validation (already done) still stands as the
         CXL-kernel-path result. Document which path in
         emulation_mode.txt.
```

## 0.4 — Bonus: multi-socket droplet check (time-box: 15 minutes, do not exceed)

```bash
# On 2-3 LARGER DigitalOcean droplet sizes (do not provision unless
# quick to check — if doctl/console makes this slow, SKIP entirely,
# this is a bonus not a requirement):
numactl --hardware
# Looking for: "available: 2 nodes" with DISTINCT physical nodes
# (not QEMU-labeled — this would be the HOST's own NUMA, visible
# before any QEMU involvement at all).
```

### GATE 0.4 (informational only, does not block anything)
```
If found: note the droplet size/region in
  ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md as a
  potential future Pond-replication target (real cross-socket delta,
  expect 70-90ns per Cohet). Do NOT pursue further in this session —
  Phase 4's CXLMemSim sweep is the primary path regardless.

If not found (likely): note "checked, single-socket only" and move on.
```

---

# PHASE 1 — Bit-exact OpenCL LDPC kernel (HARD GATE — do not skip)

This phase closes GAP 2. No latency number from Phase 2 onward is
trustworthy until this gate passes. Bit-correctness BEFORE timing —
this directly addresses the audit's central finding.

## 1.1 — Extract real srsRAN BG1/BG2 tables and reference algorithm

```bash
cd third_party/srsRAN_Project
git checkout 3f244c05   # pin — repo may be archived, re-grep paths below
git diff --stat          # must be empty before AND after this phase
                          # (we READ these files, never modify srsRAN)

# Files to read (paths may have shifted slightly at this commit —
# grep if not found exactly here):
ls lib/phy/upper/channel_coding/ldpc/
#   expect: ldpc_decoder_generic.cpp, ldpc_decoder_avx2.cpp,
#           ldpc_luts_impl.cpp, ldpc_graph_impl.cpp
cat include/srsran/phy/upper/channel_coding/ldpc/ldpc_decoder.h
#   confirm: virtual optional<unsigned> decode(bit_buffer& output,
#            span<const log_likelihood_ratio> input,
#            crc_calculator* crc, const configuration& cfg)
#   confirm: configuration.nof_iterations default (research doc says
#            default=6 — VERIFY here, this matters for 1.4 below)
```

Extract into a new header for the OpenCL port:
```bash
mkdir -p gpu_daemon/ldpc_cl
# Write gpu_daemon/ldpc_cl/bg_tables.h containing:
#   - BG1 and BG2 base-graph adjacency (which (row,col) edges exist)
#   - Per-edge, per-Zc shift values (the "iLS" indexed shift tables)
#     for AT LEAST Zc=384 (our calibration value) — extract ALL Zc
#     if not much extra effort, since the test vectors (1.3) cover
#     multiple (bg,ls) pairs and a fuller bit-correctness sweep is
#     stronger evidence
# Extract as DATA (static const arrays), copied/transformed from
# ldpc_luts_impl.cpp / ldpc_graph_impl.cpp — do not regenerate from
# a textbook formula, use srsRAN's own tables verbatim.
```

## 1.2 — Write the Z-tiled OpenCL kernel (Stage 1 + Tweak 4)

```bash
# gpu_daemon/ldpc_cl/ldpc_decode.cl
#
# Design (Z-tiling, per Tweak 4 from the "4 tweaks" evaluation):
#   - One work-group per code block.
#   - Work-items within a group indexed by lifting-size position
#     (Z=384 for our calibration) — i.e. local_size >= 384, or a
#     multiple/divisor with a clean tiling if 384 exceeds device
#     max work-group size (PoCL CPU: check CL_DEVICE_MAX_WORK_GROUP_SIZE,
#     tile across multiple iterations within the work-group if needed).
#   - __local arrays sized for the Z-element vectors used per
#     check-node/variable-node update step — load once from global
#     LLR buffer, iterate ALL 20 min-sum iterations in __local,
#     write back once at the end. This is the "stall avoidance /
#     coalesced access" point Tweak 4 was about.
#   - Min-sum check-node update + variable-node accumulate, mirroring
#     ldpc_decoder_generic.cpp's algorithm structure exactly — same
#     order of operations, same saturation behavior on the int8
#     log_likelihood_ratio representation (srsran clamps to a
#     specific range — match it exactly, check
#     include/srsran/phy/upper/log_likelihood_ratio.h for the
#     saturation bounds).
```

## 1.3 — Verification harness: srsRAN's own bit-exactness oracle

```bash
cd third_party/srsRAN_Project
# Fetch test vectors (off by default):
cmake -B build -DBUILD_TESTING=On -DCMAKE_BUILD_TYPE=Release
# This should trigger download of ldpc_decoder_test_data.tar.gz —
# if it doesn't, find the fetch mechanism (likely a CMake
# ExternalProject or a script under cmake/) and run it explicitly.

find . -iname "ldpc_decoder_test_data*"
tar xzf <path>/ldpc_decoder_test_data.tar.gz -C /tmp/ldpc_vectors/

# Inspect the header to understand the file_vector<T> binary format:
cat tests/unittests/phy/upper/channel_coding/ldpc/ldpc_decoder_test_data.h \
  | head -100
# Confirm: per-(bg, ls[, rate?]) test case structure, LLR input file,
# expected-bits output file, filler value = 254.
```

Write `gpu_daemon/ldpc_cl/bit_diff_test.c` (or .cpp, your choice):
```
For each (bg, ls) test case in the vector set:
  - Load file_vector<log_likelihood_ratio> input
  - Run gpu_daemon's OpenCL kernel (1.2) at I=20 on this input
  - Load file_vector<uint8_t> expected output
  - Compare bit-for-bit, MASKING positions where expected==254 (filler)
  - Record: case_id, n_bits, n_mismatches, bit_diff_rate
Write results to paper/results/bit_correctness.csv
```

## 1.4 — Iteration-count reconciliation (read research doc caveat)

```
srsRAN's decoder configuration default is nof_iterations=6 (per
research doc; CONFIRM exact value in 1.1's header read). Our
calibration (487.6us/CB, 11,703us/slot headline) is at I=20.

For bit-correctness (1.3): run the OpenCL kernel at WHATEVER
iteration count the test VECTORS were generated at (check the test
data header / generation script — srsLDPCDecoderUnittest.m via
srsRAN_matlab — for the iteration count used when generating expected
outputs). If vectors were generated at I=6 and your kernel runs I=20,
either:
  (a) also run the kernel at I=6 for THIS bit-diff check (separate
      from the timing config), and additionally report I=20 timing
      with a note "bit-correctness verified at I=6 (test vector
      generation parameter); I=20 used for timing per project
      calibration — early-stop CRC-pass behavior means I=20 with
      CRC-based early termination should still match I=6 outputs
      when CRC passes early", OR
  (b) if early-stop/CRC behavior makes (a) untrue for some vectors,
      report bit-diff at the VECTOR's native iteration count as the
      correctness claim, and keep I=20 as a SEPARATE, clearly-labeled
      timing-only configuration. Do not conflate the two — state
      both numbers and what each one is for.
```

### GATE 1 (HARD — blocks Phase 2)
```
PASS if: paper/results/bit_correctness.csv shows bit_diff_rate == 0
         for ALL (bg, ls) cases tested (at minimum, BG1 LS=384 — our
         calibration's exact configuration — MUST be zero; broader
         coverage strengthens the result but BG1/LS=384 is the
         non-negotiable minimum).

FAIL  -> Do NOT proceed to Phase 2. Debug the OpenCL kernel against
         ldpc_decoder_generic.cpp line-by-line for the failing
         (bg,ls) cases. Common culprits: LLR saturation bounds
         mismatch, base-graph table transcription error, check-node
         vs variable-node update ORDER (some min-sum variants update
         in a specific sequence that affects convergence within a
         fixed iteration count).

         If, after genuine debugging effort, BG1/LS=384 cannot be
         made bit-exact: fall back to AFF3CT as an INDEPENDENT
         reference — generate a BER-vs-EbN0 curve with AFF3CT's
         min-sum decoder at matched iteration count, and generate the
         SAME curve with your OpenCL kernel. GATE becomes "BER curves
         overlap within statistical noise" instead of bit-exact.
         Document explicitly which gate variant was used and why.

Replace the fake functional_correctness.txt ("PASS" string, no check)
with the real bit_correctness.csv output — this file's existence with
REAL non-trivial content is itself part of the gate evidence.
```

---

# PHASE 2 — Interception: bpftime (or kernel-uprobe fallback per 0.1)

This phase closes GAP 1's interception half. Use whichever mechanism
Gate 0.1 selected.

## 2.1 — Identify the exact symbol to attach to

```bash
# From Phase 0.2's libldpc.so:
nm -D <path>/libldpc.so | grep -i ldpc
objdump -d --demangle <path>/libldpc.so | grep -A2 "nrLDPC_coding"

# Decide: nrLDPC_coding_decoder (slot-level, called once per slot,
# internally loops C=24 codeblocks) vs
# nrLDPC_coding_segment_decoder (per-codeblock, called 24x per slot).
#
# RECOMMENDATION: attach at nrLDPC_coding_segment_decoder (per-CB) —
# this gives a per-CB descriptor count of 24/slot, directly
# comparable to our C=24 derivation from PRIMARY_CONFIG, and matches
# the granularity the original eBPF design (ring buffer event per CB)
# was built around.
```

## 2.2 — Descriptor format (control-plane only, per Tweak 1 — already
##       the right design, now actually wire it)

```c
// shared between probe and daemon — keep this SMALL, pointer+length
// only, per the diagram's Band 3 ("no payload through eBPF")
struct ldpc_offload_desc {
    uint64_t timestamp_ns;
    uint32_t slot_id;
    uint32_t cb_index;     // 0..23
    uint64_t llr_ptr;       // address of input LLR buffer
    uint32_t llr_len;
    uint64_t out_ptr;       // address of output bit buffer
    uint32_t out_len;
};
```

## 2.3a — IF bpftime (Gate 0.1 PASS)

```bash
# Use bpftime's uprobe mechanism (per its examples/ directory structure
# from 0.1) to attach to the symbol from 2.1, running against the
# OAI gNB process (PID from Phase 0.2's launch).
#
# The handler:
#   - reads the function's argument registers (pointer to LLR buffer,
#     length, pointer to output buffer) per the calling convention —
#     confirm nrLDPC_coding_segment_decoder's signature from OAI source
#     (openair1/PHY/CODING/ — LDPCImplementation.md referenced in
#     research doc) to get argument-register mapping right
#   - writes a ldpc_offload_desc (2.2) to bpftime's shared-memory
#     IPC primitive (check bpftime docs for the ring-buffer-equivalent
#     API — likely a shared mmap region with a SPSC/MPSC queue)
#   - on FIRST call only: programs the CXL buffer mapping (Phase 4) —
#     i.e. this is where "programs CXL mapping then steps aside"
#     (Band 3 of the diagram) happens
#   - the probe handler itself does NOT call into the daemon or block —
#     it writes the descriptor and returns
```

## 2.3b — IF kernel-uprobe fallback (Gate 0.1 FAIL)

```c
// BPF_MAP_TYPE_RINGBUF, libbpf CO-RE — this is the mechanism that
// was ALREADY COMPILED (prog IDs existed) but never in the data path.
// Now actually attach it to 2.1's symbol and wire reserve/submit:
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 4096);
} ldpc_events SEC(".maps");

SEC("uprobe/nrLDPC_coding_segment_decoder")
int BPF_UPROBE(on_ldpc_decode, /* args per 2.1's signature */) {
    struct ldpc_offload_desc *e =
        bpf_ringbuf_reserve(&ldpc_events, sizeof(*e), 0);
    if (!e) return 0;
    // populate e from probe context
    bpf_ringbuf_submit(e, 0);
    return 0;
}
```
Userspace side: libbpf ring_buffer__poll loop, same descriptor
consumption logic as 2.3a from here on.

## 2.4 — Userspace consumer

```
Regardless of 2.3a/2.3b: a userspace process polls the descriptor
queue, and for each descriptor:
  - on first descriptor: mmap /dev/dax0.0 (or the daxctl system-ram
    node1 region) — this is "programs CXL buffer mapping" — record
    the mapping, do this ONCE
  - copies (or, if the LLR buffer is ALREADY in a region the daemon
    can reach via shared mapping — see Phase 4 — does NOT copy)
    llr_ptr[0..llr_len) to the CXL-backed input buffer
  - signals the GPU daemon (Phase 1's kernel) that CB cb_index is
    ready (a flag/sequence-number in the SAME CXL region — per the
    earlier "shared memory + lightweight handshake = CXL.mem, not
    CXL.cache" resolution — no message-passing layer needed beyond
    this)
  - daemon decodes, writes result to CXL output buffer, increments
    a result-ready counter
  - userspace consumer copies result back to out_ptr, OAI's
    nrLDPC_coding_segment_decoder returns the real decoded bits
```

### GATE 2 (HARD — blocks Phase 3)
```bash
# With the OAI gNB (Phase 0.2) running --phy-test (synthetic traffic,
# steady slot cadence even without a real UE attach), and 2.3a/2.3b +
# 2.4 attached:

# bpftime: use its tracing/introspection (check 0.1's chosen tooling)
# kernel-uprobe: bpftool prog tracelog, or bpftool map dump on
#                ldpc_events

# Run for N slots (start with N=100 for the gate check, scale to 1000+
# in Phase 5):
#   expected descriptor count ~= N_slots * 24 (C=24 CBs/slot)

PASS if: observed descriptor count is within a few percent of
         N_slots * 24, AND the daemon's decoded output, written back
         via 2.4, is observed to differ from a "do nothing, pass
         input through" stub (i.e. real compute is happening, not
         just descriptor plumbing).

FAIL  -> if descriptor count is 0: probe is not firing — check symbol
         resolution (2.1), PID targeting, argument-register mapping.
         If descriptor count is wrong/inconsistent: check for missed
         events (ring buffer full?) or double-counting.

This is THE gate that GAP 1 was about. Do not write any latency
number to ANY results file until this passes.
```

---

# PHASE 3 — Continuous L1 + NIC-level evidence (closes GAP 3)

By this point: Phase 1 gives bit-exact compute, Phase 2 gives real
interception. Phase 3 makes the WORKLOAD continuous and adds the
"NIC" evidence layer — separate from, and in addition to, Phase 2's
interception.

## 3.1 — Sustained run

```bash
# In gnb-ns (Phase 0.2):
nr-softmodem -O <conf> --gNBs.[0].min_rxtxtime 6 --phy-test --rfsim \
  --rfsimulator.serveraddr server &
# In ue-ns:
nr-uesoftmodem --rrc_config_path . --phy-test --rfsim \
  --rfsimulator.serveraddr 10.77.0.2 &

# Phase 2's interception (2.3a or 2.3b + 2.4) attached to the gNB PID.

# Run for >=10,000 slots. At 0.5ms/slot (mu=1) this is ~5 seconds of
# wall-clock PHY time — but allow for startup/sync overhead, run
# longer wall-clock if needed to accumulate 10,000 STEADY-STATE slots
# (discard initial sync period).
```

## 3.2 — XDP NIC-level observation (separate eBPF program, NOT the
##       offload trigger — this is evidence, Band 1 of the diagram)

```c
// xdp_rfsim_observe.bpf.c — attach to veth-gnb (gnb-ns side)
// Purpose: timestamp packets on the RFsim link. This is SEPARATE
// from Phase 2's interception (which is a uprobe on a function call
// inside the gNB process, not a network-layer program). Two
// independent eBPF programs, two independent evidence streams.

SEC("xdp")
int observe_rfsim(struct xdp_md *ctx) {
    // record (timestamp_ns, packet_len) to a ring buffer
    // do NOT drop, do NOT modify — XDP_PASS always
    return XDP_PASS;
}
```

```bash
ip netns exec gnb-ns ip link set dev veth-gnb xdp obj xdp_rfsim_observe.o
# Userspace consumer polls the ring buffer, writes:
#   paper/results/nic_packet_timeline.csv
#   columns: timestamp_ns, packet_len, direction
```

### GATE 3
```
PASS if:
  (a) Phase 2's interception sustains >=10,000 descriptors with
      counts consistent with C=24/slot (i.e. Gate 2's check, but at
      N=10,000+ scale — confirms no degradation/drops over a longer
      run)
  (b) nic_packet_timeline.csv shows a periodic arrival pattern with
      inter-arrival times clustering around the configured slot
      duration (0.5ms for mu=1) — this is the "L1 workload reaching
      host via NIC" evidence. Compute and report the inter-arrival
      time distribution (mean, stddev) in this gate's output.

FAIL (a) -> revisit Phase 2's gate at scale — ring buffer sizing,
            polling frequency, possible drops under sustained load.

FAIL (b) -> if XDP shows NO periodic pattern (e.g. all traffic at
            startup only, then silence): --phy-test may not be
            generating sustained per-slot traffic as expected, or
            the veth interface isn't the one carrying RFsim traffic
            (check Phase 0.2's transport investigation again — is
            there a DIFFERENT interface, e.g. if OAI's rfsimulator
            uses a port that's NOT routed via 10.77.0.2?). Debug the
            routing before declaring this gate failed outright.
```

---

# PHASE 4 — CXL wiring + CXLMemSim latency sensitivity (resolves
#            emucxl/Pond gap)

## 4.1 — Boot the verified CXL VM

```bash
cd ops/cxl-poc-droplet/scripts
./qemu_cxl_launch.sh <disk-image-with-phase0-3-binaries>.qcow2
./verify_cxl_checks.sh
# Expect 5/5 PASS (hypervisor bit clear, no cache-sync warning,
# cxl create-region succeeds, daxctl system-ram, NUMA 2 nodes) —
# this was already verified in Part C, re-confirm it still holds
# with whatever disk image now contains Phases 0-3's binaries.
```

If 4.1 fails on a previously-passing check: something in the new disk
image's kernel/config differs from Part C's verification image —
debug the image build, do not proceed with an unverified CXL VM.

## 4.2 — Map CXL memory in the GPU daemon (PoCL alignment rules)

```c
// gpu_daemon: mmap /dev/dax0.0 (or the daxctl system-ram node1 region)
void *cxl_base = mmap(NULL, region_size, PROT_READ|PROT_WRITE,
                       MAP_SHARED, dax_fd, 0);

// PoCL CL_MEM_USE_HOST_PTR requirements (from research doc):
//   - host pointer 4096-byte aligned (DAX mappings are page-aligned —
//     verify with: assert((uintptr_t)cxl_base % 4096 == 0))
//   - buffer SIZE a multiple of 64 bytes — size your LLR/output
//     buffers accordingly (pad if needed; document padding in a
//     comment, do not silently change buffer semantics)

cl_mem llr_buf = clCreateBuffer(ctx,
    CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
    llr_buf_size, cxl_base + llr_offset, &err);
// check err == CL_SUCCESS — if PoCL falls back to a copy despite
// alignment being correct, this is a PoCL-specific limitation;
// document it (research doc flagged this as a "known PoCL
// limitation" possibility) and report whether zero-copy was achieved
// or PoCL silently copied (compare cxl_base+offset pointer identity
// vs what PoCL's internal buffer reports, if introspectable; at
// minimum, note in emulation_mode.txt which path was taken)
```

## 4.3 — CXLMemSim latency sweep (Gate 0.3 PASS path)

```bash
# Wrap the GPU daemon process (from 4.2, which is now the consumer
# from Phase 2.4) with CXLMemSim, sweeping injected latency:
for lat in 0 100 142 200 255 300; do
  ./cxlmemsim --latency=${lat} -- ./gpu_daemon <args>
  # Phase 0-3's full pipeline runs against this daemon instance.
  # N>=1000 slots per latency point (Phase 5 formalizes N and
  # percentile reporting — Phase 4 establishes the SWEEP exists).
  # Write: paper/results/cxl_latency_sensitivity.csv
  #   columns: injected_latency_ns, slot_us_mean, slot_us_p50,
  #            slot_us_p95, slot_us_p99
done
```

## 4.4 — Documentation: the emucxl/Pond resolution

```bash
# Add to paper/results/RESULTS_SUMMARY.md (Phase 6 will integrate
# this fully, but write the core paragraph here while context is
# fresh):
#
# "Our QEMU CXL Type-3 + daxctl NUMA-node setup (Part C) is
#  structurally equivalent to the methodology of emucxl [Gond &
#  Kulkarni, IIT Bombay] and Pond [Li et al., ASPLOS'23], both of
#  which emulate CXL via QEMU+NUMA on REAL MULTI-SOCKET hosts, where
#  cross-socket NUMA distance provides a real latency signal
#  (70-90ns per Cohet/SimCXL). Our single-socket cloud host lacks
#  this substrate — Part C's measured node0/node1 delta (53us,
#  within +-100us noise) correctly reflects the ABSENCE of real
#  interconnect distance, not a QEMU emulation failure. We therefore
#  use CXLMemSim [SlugLab, arXiv:2303.06153] — software latency
#  injection validated against real CXL silicon, topology-
#  independent — for latency-sensitivity characterization (4.3)."
```

## 4.5 — IF Gate 0.3 FAILED (no CXLMemSim)

```
Phase 4 still delivers: real drivers/cxl/ path (4.1, already verified
in Part C), real CXL-backed shared memory used by the ACTUAL pipeline
(4.2, now genuinely in the data path per Phase 2's gate — this alone
is a major upgrade from the audit's findings, regardless of latency
sensitivity).

What's deferred: the latency-SENSITIVITY sweep (4.3/cxl_latency_
sensitivity.csv). State this explicitly in Limitations:
"CXL.mem latency-sensitivity characterization requires either
 software injection (CXLMemSim, build issues encountered — see
 ops/cxl-poc-droplet/references/) or real multi-socket/CXL hardware
 (IISc HACC, future work)."

This is a SMALLER gap than GAP 1/2/3 — those were about whether the
ARCHITECTURE'S CENTRAL CLAIM is backed by running code at all. This
is about ONE additional figure (latency sensitivity) being deferred.
Do not let a Gate 0.3 failure block Phases 1-3's much more important
gates.
```

### GATE 4
```
PASS if: 4.1's 5/5 still holds with the new image, 4.2's CXL mapping
         is confirmed in the data path (Phase 2's descriptors
         reference addresses within the /dev/dax0.0 mapping — verify
         with a pointer-range check: llr_ptr/out_ptr from descriptors
         fall within [cxl_base, cxl_base+region_size)), and EITHER
         4.3's sweep produced cxl_latency_sensitivity.csv with 6
         latency points x N>=100 samples each (scaled to N>=1000 in
         Phase 5), OR 4.5's documented deferral is written.
```

---

# PHASE 5 — Measurement rigor: ablation + comparison (Basu-quality)

By now: bit-exact compute (Phase 1), real interception at scale
(Phases 2-3), real CXL in the data path with or without latency
sweep (Phase 4). Phase 5 produces the numbers that REPLACE the
discredited 12,036/11,727 arithmetic entries.

## 5.1 — Ablation table

For each row, N>=1000 slots, report mean/p50/p95/p99 (not mean alone):

```
row                    | what's enabled
-----------------------|------------------------------------------
baseline               | direct ldpc_decoder_benchmark, no
                       | interception at all (UNCHANGED from
                       | PRIMARY_CONFIG: 11,703us/slot — do not
                       | re-measure, this is the anchor)
+interception_only     | Phase 2/3 running, descriptors fire
                       | (Gate 2/3 evidence), daemon receives
                       | descriptor but returns INPUT UNCHANGED
                       | (no compute) — isolates pure
                       | interception+CXL-roundtrip overhead
+gpu_compute_full      | as above, but daemon runs Phase 1's
                       | bit-exact kernel — THE real number
+gpu_compute_full      | x6 rows, one per Phase 4.3 latency point
  x cxlmemsim_sweep    | (or single row + note if 4.5 deferral)
```

Write `paper/results/latency_ladder_v2.csv` with an HONEST `source`
column for every row — `measured` only where it genuinely is.
Do NOT reuse the old latency_ladder.csv's mislabeled rows; this is a
new file, and `source=measured` here must mean what it says.

## 5.2 — Comparison vs cited priors

```
paper/results/comparison_table.csv:

source                          | value           | what it measures
---------------------------------|-----------------|------------------
This work — baseline (PRIMARY)   | 11,703 us/slot  | CPU, no offload
This work — +gpu_compute_full    | <5.1's number>  | full pipeline,
                                  |                 | bit-exact, 0ns
This work — interception overhead| <Gate2/0.1 num> | bpftime or
                                  |                 | kernel-uprobe,
                                  |                 | per-CB
Six Times to Spare (DGX Spark)   | 710 us          | GPU LDPC compute
Cloudflare                       | 1,670 ns        | bare-metal uprobe
Pond (real cross-socket NUMA)    | 70-90 ns        | CXL-proxy delta
Real CXL Type-3 (Intel MLC/      | 214-394 ns      | load-to-use
  HEIMDALL)                      |                 | latency
This work — CXLMemSim sweep      | 0/100/142/200/  | injected, see
                                  | 255/300 ns      | 5.1's last rows
```

## 5.3 — The headline pair, honestly derived this time

```
23.4x  (11,703us baseline / 500us) — UNCHANGED, real, TS38212-derived.

NEW second number: <5.1's gpu_compute_full @ realistic CXLMemSim
point (142 or 255ns)> / 500us — THIS is the "with this architecture"
number, and unlike the old 12,036/11,727, it comes from an actual
end-to-end run through Phases 1-4's real pipeline.

Report whatever this number IS. If it's still >1x, that's fine and
expected (PoCL-CPU is not a real GPU) — frame as:
  "the architecture overhead (interception + CXL + correctness-
   verified compute on a CPU-class OpenCL device) is Xus/slot;
   replacing the CPU OpenCL device with a real GPU (PoCL -> ROCm/
   CUDA-OpenCL, zero code change per the architecture's design)
   would reduce the compute portion by ~6x per [Six Times to Spare],
   projecting Yus/slot — see Limitations for why this projection
   is not itself re-measured in this work."
This PROJECTED step is now clearly separated from the MEASURED
end-to-end number — exactly the distinction the audit's CHECK 5
was about, but now the measured side of it is real.
```

### GATE 5
```
PASS if: latency_ladder_v2.csv and comparison_table.csv exist, every
         row's `source` column is accurate (measured rows trace to
         an actual run's output; projected rows say `projected` and
         show their formula), and the new headline pair (23.4x +
         the Phase-1-through-4 derived second number) is stated in
         RESULTS_SUMMARY.md with full provenance for both halves.
```

---

# PHASE 6 — Paper artifacts

## 6.1 — Figures (regenerate; some REPLACE prior figures entirely)

```
latency_cdf_v2.pdf
  CDF of 5.1's rows (baseline, +interception_only, +gpu_compute_full),
  REAL data this time. 500us deadline line retained.

latency_breakdown_v2.pdf
  5.1's ablation table as grouped/stacked bars.

cxl_latency_sensitivity.pdf  (NEW — only if Gate 0.3 PASS)
  x = injected latency (0/100/142/200/255/300 ns), y = us/slot
  (gpu_compute_full row). THIS REPLACES the old flat 3-bar
  "cxl_kernel_path" figure from the prior session — that figure's
  finding (53us/noise) is now EXPLAINED (4.4) rather than re-plotted
  as a result.

nic_packet_timeline.pdf  (NEW)
  Phase 3.2's inter-arrival-time histogram or timeline — the "NIC"
  evidence.

bit_correctness_table.pdf or .md  (NEW)
  Phase 1.3's per-(bg,ls) results — replaces the fake "PASS" string.
```

## 6.2 — RESULTS_SUMMARY v2 — full rewrite, sections:

```
1. PRIMARY_CONFIG (unchanged) — 11,703us/slot, 23.4x, TS38212 chain
2. Bit-correctness (Phase 1) — table/figure reference, methodology
3. Interception (Phase 2/3) — bpftime or kernel-uprobe (per 0.1),
   descriptor-count evidence (Gate 2/3), per-CB overhead number
4. NIC-level evidence (Phase 3.2) — inter-arrival timing figure
5. CXL path + latency sensitivity (Phase 4) — Part C's real
   drivers/cxl/ result + emucxl/Pond resolution (4.4) +
   cxl_latency_sensitivity.csv or documented deferral
6. End-to-end results (Phase 5) — ablation table, comparison table,
   new headline pair with full provenance
7. White-paper framing (research doc Q4):
   - inline (Nokia/NVIDIA Aerial/cuPHY) vs lookaside (Ericsson/
     Intel/O-RAN WG6 AAL) — this work = vendor-neutral lookaside
   - differentiate from US20250085998 (centralized AAL broker):
     "transparent interception vs explicit-API broker"
   - cite O-RAN.WG6.AAL-GAnP-R004-v11.00 as the explicit-API
     precedent this work complements
8. Limitations:
   - Gate 0.1/0.3 outcomes (whichever fallback paths were taken)
   - PoCL-CPU vs real GPU (the projected 6x, Six Times to Spare)
   - CXLMemSim vs real CXL hardware (IISc HACC, future work)
   - Tweak 2 (devdax hygiene) / Tweak 3 (CXL.cache, Type-2, not in
     mainline QEMU) as future architectural directions
   - bit-correctness iteration-count caveat (1.4) if (b) was used
```

## 6.3 — emulation_mode strings (new vocabulary for this version)

```
oai-rfsim-netns-veth-{bpftime|kuprobe}-cxl-persistent-memdev-
  cxlmemsim-{N}ns
```
Use this consistently across all Phase 1-5 output CSVs' emulation_mode
columns.

## 6.4 — Update ops/cxl-poc-droplet skill

```bash
cd ops/cxl-poc-droplet
# Add: bpftime build notes + Gate 0.1 outcome (references/)
# Add: CXLMemSim build notes + Gate 0.3 outcome (references/)
# Add: OAI build notes + netns/veth setup script (scripts/setup_netns.sh)
# Add: XDP observation program (ebpf/xdp_rfsim_observe.bpf.c or
#      similar location matching project conventions)
git add -A
git commit -m "v4: bpftime/CXLMemSim/OAI integration notes from
  no-compromise pipeline session"
```

### GATE 6
```
PASS if: all of 6.1's figures render without error, RESULTS_SUMMARY.md
         (v2) reads as a coherent paper Evaluation section with every
         number traceable to a Phase 1-5 gate's output, and the skill
         repo is updated/committed.
```

---

# Final report-back format

After ALL phases (or after a documented stop at a deferred gate per
4.5/0.1/0.3's fallback paths), report:

```
Gate 0.1 (bpftime):       PASS / FAIL -> <chosen mechanism>
Gate 0.2 (netns/veth):    PASS / FAIL
Gate 0.3 (CXLMemSim):     PASS / FAIL -> <4.3 or 4.5 path>
Gate 0.4 (multi-socket):  found / not found (informational)
Gate 1 (bit-exact):       PASS / FAIL -> <(a) or (b) variant if FAIL>
Gate 2 (interception):    PASS / FAIL -> descriptor count vs expected
Gate 3 (sustained+NIC):   PASS / FAIL -> inter-arrival stats
Gate 4 (CXL+sweep):       PASS / FAIL
Gate 5 (ablation):        PASS / FAIL -> new headline pair
Gate 6 (artifacts):       PASS / FAIL

New headline pair: 23.4x (unchanged) / <new second number>
Old discredited numbers (12,036/11,727us) status: REMOVED, replaced
  by latency_ladder_v2.csv
```