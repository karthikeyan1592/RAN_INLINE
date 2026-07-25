# v7 — End-to-End CXL RAN Pipeline on GCP (cxl-systems-lab)

## Environment

```
Machine:   GCP n2-standard-4, asia-south2-a (Delhi)
           Intel Cascade Lake, 4 vCPU, 16GB RAM
           --enable-nested-virtualization → /dev/kvm NATIVE
           KVM acceleration real (not TCG) — PoCL LDPC JIT: seconds
           Ubuntu 24.04 LTS

This is the first environment where all five pipeline arrows can
coexist simultaneously:
  radio (srsRAN)  +  eBPF (bpftime)  +  CXL (QEMU KVM)  +
  OCL (PoCL, fast)  +  LDPC (bit-exact kernel)

DO BLR1 had /dev/kvm but nested-virt fell to TCG → PoCL JIT >20min
GCP n2 with --enable-nested-virtualization exposes vmx explicitly.
Confirm first (30 seconds, free):
  grep -c vmx /proc/cpuinfo     # expect non-zero
  ls /dev/kvm                   # expect present
If either fails: STOP, do not proceed, flag the provider issue.
```

---

## What Already Exists (do not redo, do not re-derive)

```
From v4/v5/v6 on WSL2 + prior DO droplet:

PRIMARY_CONFIG:   11,703 µs/slot = 23.4× over 500µs budget. FIXED.
                  Never changes. Not re-measured here.

Bit-exact kernel: gpu_daemon/ldpc_cl/ldpc_decode.cl
                  786-line bg_tables.h (srsRAN BG1/BG2 tables)
                  0 mismatches proven BG1/BG2, Z=384/256. REUSE.

QEMU CXL config:  ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh
                  -enable-kvm -cpu host,-hypervisor -M q35,cxl=on
                  persistent-memdev → /dev/dax0.0 → daxctl system-ram
                  → NUMA node 1 (1920MB, distance=20). REUSE.
                  ONE CHANGE: add pmem=off to the memory-backend-file
                  object (fixes DEV-030 SIGILL from WC page mapping).
                  pmem=off → WB cache type → numactl --membind=1 works.

bpftime:          248.5 ns/call proven (WSL2). REUSE the build.
                  The attach-and-count behavior is proven.
                  v7 adds the DATA MOVEMENT: bpftime handler reads
                  %rdx (LLR span pointer), writes LLR bytes into the
                  shared CXL memfd. This is the one new BPF program.

verify_cxl_checks.sh: 5/5 checks. REUSE — expect same PASS.
```

---

## The One New Thing in v7

Everything else existed. The ONLY NEW CODE in v7 is the bpftime
handler that moves LLR data into the shared CXL region:

```c
// ldpc_llr_mover.bpf.c (new, ~50 lines)
// Attaches to srsRAN ldpc_decoder_impl::decode at offset 0x35280
// (confirmed gate_1 evidence from prior DO session).
// At entry: %rdx = pointer to srsRAN's internal LLR buffer
//           %rcx or stack: LLR length (26,112 bytes for BG1 Z=384)
//
// Handler (runs in bpftime userspace — relaxed limits vs kernel):
//   bpf_probe_read_user(cxl_llr_buf, llr_len, ctx.rdx)
//   atomic_store(&llr_ready_flag, 1)
//
// cxl_llr_buf: pointer into the shared CXL memfd region
//              (passed via bpftime's map mechanism or a global
//               registered with the userspace agent at startup)
//
// This is ~50 lines. If bpf_probe_read_user of 26KB hits a ubpf
// limit (unlikely in userspace but possible): chunk it in 4KB reads.
// Document if needed.
```

---

## Phase 0 — Provision (30 min, one-time)

```bash
# SSH in:
gcloud compute ssh cxl-systems-lab --zone=asia-south2-a

# IMMEDIATE FIRST CHECK — if either fails, stop:
grep -c vmx /proc/cpuinfo
ls /dev/kvm

# Install deps (one apt run):
apt-get update && apt-get install -y \
  qemu-system-x86 qemu-kvm qemu-utils \
  build-essential cmake ninja-build git clang llvm \
  libbpf-dev bpftool linux-tools-$(uname -r) \
  pocl-opencl-icd ocl-icd-opencl-dev clinfo \
  numactl ndctl daxctl \
  libelf-dev libssl-dev flex bison libncurses-dev \
  python3-pip python3-numpy python3-pandas \
  libboost-all-dev libconfig++-dev libyaml-cpp-dev \
  libfftw3-dev libgtest-dev libzmq3-dev

# rsync project from local (run on local machine):
rsync -az --exclude '*/build' --exclude '.git' \
  ~/linux_env/cxl/ user@<gcp-ip>:/home/user/cxl/

# Build bpftime on GCP (native x86, no TCG):
cd ~/cxl/third_party/bpftime
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
# Expected: takes 5-10 min on native x86 (not 20+ min TCG)

# Build srsRAN (or confirm benchmark binary exists and runs):
cd ~/cxl/third_party/srsRAN_Project/build
./tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark \
  -L 384 -Tavx2 -R 10
# Expected: runs, shows Mbps output. If SIGILL: build fresh with AVX2.

# Build OpenCL consumer/daemon (reuse v5 sources):
cd ~/cxl/phase5_cxl && make   # or cmake as applicable

# clinfo — confirm PoCL:
clinfo | grep -E "Platform|Device|Type"
# Expected: "Portable Computing Language" + "CPU" device
```

### GATE 0
```
PASS if:
  - grep vmx /proc/cpuinfo: non-zero
  - /dev/kvm: exists
  - srsRAN benchmark runs without SIGILL (AVX2 confirmed)
  - clinfo shows PoCL CPU device
  - bpftime builds (bpftime/build/libruntime.a exists)

FAIL on ANY of these: environment not viable. Do not continue.
```

---

## Phase 1 — CXL VM with pmem=off + verify_cxl_checks (15 min)

```bash
# ONE CHANGE from prior sessions: add pmem=off to the memdev object
# In qemu_cxl_launch.sh, find the memory-backend-file line and change:
#   BEFORE: ...,size=2G,align=256M
#   AFTER:  ...,size=2G,align=256M,pmem=off
# This changes the CXL backing memory from Write-Combining to
# Write-Back cache type, fixing the SIGILL on numactl --membind=1
# (DEV-030). Document as DEV-031 if the change has any side effect.

# Boot the CXL VM:
cd ~/cxl/ops/cxl-poc-droplet/scripts
./qemu_cxl_launch.sh /path/to/disk.qcow2 &
sleep 30

# Run 5/5 verify checks:
./verify_cxl_checks.sh
# Expected: hypervisor bit clear, no cache-sync, cxl create-region,
#           daxctl system-ram, NUMA 2 nodes — ALL 5 PASS.
# If cxl create-region FAILS with pmem=off: revert to pmem=on for CXL
# and document that Option B (pmem=off) broke CXL; fall back to
# Option A (shared memfd approach) for the shared-memory mechanism.
```

```bash
# Inside the VM — critical new test (confirms DEV-030 fix):
numactl --hardware        # expect: 2 nodes (0=DRAM, 1=CXL)
numactl --membind=1 ls    # expect: runs WITHOUT SIGILL
# If still SIGILL with pmem=off: the mbind shim from references/
# cxl_qemu_kvm_gotchas.md §8 is needed — add it to the test command.
```

### GATE 1
```
PASS if: 5/5 verify_cxl_checks.sh AND numactl --membind=1 works
         (no SIGILL) inside the VM.
FAIL -> CXL broken by pmem=off: revert, document, use Option A shm.
     -> SIGILL persists: add the mbind shim and retry once.
```

---

## Phase 2 — Shared CXL region + zero-copy proof (20 min)

Goal: get BOTH OAI's LLR side AND OpenCL's buffer side pointing at
the SAME physical CXL pages in ONE configuration. This is the DEV-023
mode-conflict resolution that blocked every prior session.

```c
// setup_shared_cxl_region() — call once at startup (in consumer/parent):
//   fd = memfd_create("llr_cxl", MFD_CLOEXEC)
//   ftruncate(fd, CXL_REGION_SZ)
//   base = mmap(NULL, CXL_REGION_SZ, PROT_RW, MAP_SHARED, fd, 0)
//   mbind(base, CXL_REGION_SZ, MPOL_BIND, {cxl_node=1}, ...)
//   → base is now a CXL-backed shared region, mappable by both
//     the consumer (for OpenCL) and the bpftime handler (for LLR write)
//
//   confirm: get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE, base) → cxl_node
//
//   cl_mem = clCreateBuffer(ctx, CL_MEM_USE_HOST_PTR, SZ, base, &err)
//   assert(err == CL_SUCCESS)
//
//   sentinel test (zero-copy confirmation):
//     write 0xDEADBEEF at base[0] (CPU side, no clEnqueue)
//     run a trivial OpenCL kernel that reads base[0]
//     if kernel sees 0xDEADBEEF without clEnqueueWriteBuffer: ZERO-COPY
//
//   share fd with bpftime handler (pass path via env or socket):
//     bpftime agent mmap(MAP_SHARED, fd, ...) in worker thread
//     → cxl_llr_buf = same base pointer in bpftime's address space
//     → bpf_probe_read_user writes TO cxl_llr_buf
//     → OpenCL reads FROM the same pages via cl_mem
```

### GATE 2
```
PASS if:
  (1) get_mempolicy on the shared region base → cxl_node=1
  (2) clCreateBuffer(CL_MEM_USE_HOST_PTR) err == CL_SUCCESS
  (3) Sentinel test: kernel reads CPU-written value WITHOUT
      clEnqueueWriteBuffer → zero_copy=YES
  Evidence: get_mempolicy output + sentinel test output.

FAIL -> (1) fails: mbind to node 1 not working (check numactl --hardware
         shows node 1 available, daxctl system-ram completed).
     -> (3) fails: PoCL is copying despite alignment. Document as
         PoCL limitation, NOT zero-copy — still continue, but label
         the copy in emulation_mode.txt and in the final results.
```

---

## Phase 3 — Write the LLR mover (the one new program, ~1 hr)

```c
// ldpc_llr_mover.bpf.c
// Attach to srsRAN ldpc_decoder_impl::decode at offset 0x35280
// (confirmed from prior Gate 1 evidence: 12 events at this offset).

// Step 1: confirm offset still valid on THIS build:
nm third_party/srsRAN_Project/build/.../ldpc_decoder_benchmark \
  | grep -i "ldpc_decoder_impl.*decode"
# Re-derive offset if it shifted. Document if different from 0x35280.

// Step 2: confirm %rdx holds LLR ptr at decode() entry:
//   (From prior Gate 1 tracefs evidence: fetchargs %rdx = LLR span)
//   Quick re-confirm with 5 events:
echo 'p:ldpc_probe .../ldpc_decoder_benchmark:0x35280 llr=%rdx' \
  > /sys/kernel/debug/tracing/uprobe_events
echo 1 > /sys/kernel/debug/tracing/events/uprobes/ldpc_probe/enable
./ldpc_decoder_benchmark -L 384 -Tavx2 -R 5
cat /sys/kernel/debug/tracing/trace | grep ldpc_probe
# Expect: %rdx is a non-null, plausible heap address.
# Document the actual register/stack layout for the build on THIS
// machine — CPU ABI is the same but compiler may differ.

// Step 3: write the bpftime handler:
//   On each probe fire:
//     bpf_probe_read_user(cxl_llr_buf, 26112, ctx.rdx)
//     atomic_store(&llr_seq, llr_seq + 1)  // consumer polls seq
//   cxl_llr_buf and llr_seq: live in the shared memfd from Phase 2,
//   mapped by the bpftime agent worker thread at startup.
//   Pass the mapped address via bpftime's BPF map mechanism or as
//   a registered global pointer (check bpftime examples/uprobe for
//   the pattern — bpftime userspace agents can share memory directly
//   with the host process without kernel map overhead).
```

### GATE 3
```
PASS if: the bpftime handler compiles, attaches to the srsRAN binary,
         and after ONE srsRAN decode call:
         (a) llr_seq increments (probe fired)
         (b) cxl_llr_buf[0..2] contains non-trivial LLR values
             (e.g. ±10 class values from real LDPC input, NOT 0x00
             or 0xFF which would mean probe_read failed or zeroed)
         (c) the bytes match what tracefs's %rdx pointed to (spot-
             check 4 bytes from the tracefs capture vs cxl_llr_buf)

FAIL -> llr_seq didn't increment: bpftime attach failed (check PID,
         offset, library path). Re-run with bpftime verbose logging.
     -> bytes are all zero: probe_read failed (check that ctx.rdx is
         actually the LLR pointer and not a span struct that needs
         one more dereference — check srsRAN's ldpc_decoder_impl.cpp
         to confirm the calling convention for this build).
```

---

## Phase 4 — THE END-TO-END RUN (the run this entire project exists for)

Wire the phases together and run N=1000 CB decodes:

```bash
# LAUNCH ORDER (all inside the QEMU VM, all in one shell session):

# Terminal 1: start the consumer (parent, owns the shared CXL region,
#   runs the bit-exact OCL LDPC kernel):
./consumer \
  --cxl-node 1 \
  --n-cbs 1000 \
  --output paper/results/e2e_gcp.csv \
  &
CONSUMER_PID=$!

# Terminal 2: attach bpftime LLR mover to srsRAN BEFORE launching it:
LD_PRELOAD=.../libbpftime-syscall-server.so \
  BPFTIME_VM_NAME=ubpf \
  ./bpftime_agent --attach ldpc_decoder_benchmark:0x35280 \
                  --handler ldpc_llr_mover.bpf.o \
                  --cxl-shmem-path /proc/$CONSUMER_PID/fd/<memfd-num> \
  &
AGENT_PID=$!
sleep 2   # let agent register with the bpftime server

# Terminal 3: launch srsRAN workload, bound to CXL node 1:
numactl --cpunodebind=0 --membind=1 \
  ./ldpc_decoder_benchmark -L 384 -Tavx2 -R 1000

# After srsRAN exits: wait for consumer to finish writing the CSV.
wait $CONSUMER_PID
```

### THE ONE GATE THAT MATTERS — GATE 4

```
PASS if ALL FIVE are true and have raw evidence in gate_4.md:

(a) ONE process tree on the GCP VM:
    pstree showing srsRAN + consumer + bpftime agent running together,
    with bpftime agent's PID confirmed attached to srsRAN's PID.
    `cat /proc/<srsran_pid>/maps | grep bpftime` shows the agent loaded.

(b) LLR from srsRAN lands in CXL memory:
    During the run, read one descriptor's llr address and confirm its
    numa node:
    `cat /proc/$SRSRAN_PID/numa_maps | grep <llr_addr_prefix>`
    Expect: `N1=<non-zero>` (pages on CXL node 1).
    NOT a separate malloc test. The LIVE srsRAN LLR address.

(c) OpenCL reads from THAT CXL memory (zero-copy):
    Gate 2 already proved CL_MEM_USE_HOST_PTR over node-1 is
    zero-copy. Here confirm the consumer's cl_mem base address
    matches the shared memfd base (same VA range as (b)'s numa_maps).

(d) Bit-exact LDPC through the full path:
    bit_diff_rate = 0 in e2e_gcp.csv.
    The Z used must be one the bg_tables.h supports correctly
    (Z=384 preferred; Z=256 acceptable; Z=224 ONLY if tables were
    extended — document if not extended).
    A PASS here with Z=384 is cleanest.

(e) ONE measured latency number:
    e2e_gcp.csv exists, source=measured, N=1000 rows, not N=0 or N=1.
    The per-CB and per-slot p50 numbers come from THIS run.
    They must NOT equal (interception_p50 + ocl_p50) from any earlier
    separate measurement — the telemetry will check this arithmetic.
    They will be dominated by PoCL-CPU compute time (~170ms/CB class
    based on prior measurements WITH KVM — expect much less than
    389ms from TCG, possibly 10-50ms with real KVM).

PARTIAL is acceptable and should be labeled:
    If (a)+(b)+(c)+(e) pass but (d) fails because Z=224 tables not
    extended: "pipeline assembled and measured, bit-exactness at
    Z=384 pending table extension" is an HONEST PARTIAL. DO NOT
    relabel it PASS. DO NOT claim bit_diff=0 unless it's actually 0.

FAIL on (a): process tree not assembled — bpftime not attached.
FAIL on (b): LLR not in CXL — numactl --membind=1 not effective.
FAIL on (e): e2e_gcp.csv is arithmetic sum of earlier numbers —
             not a real measurement. This is the original failure
             mode reincarnated.
```

---

## Phase 5 — Write-up and emulation_mode (30 min)

```
e2e_gcp.csv: columns:
  cb_index, llr_source_node, ocl_read_ns, decode_ns,
  bit_diff, total_ns, emulation_mode, source

emulation_mode string:
  "gcp-n2-kvm-qemu-cxl-pmem-off-bpftime-spsc-pocl-cpu"

paper/results/RESULTS_SUMMARY.md — add section:
  "End-to-end pipeline (GCP, real KVM, real CXL emulation)":
    - Gate 4(a)-(e) status
    - e2e per-CB p50, per-slot (×24 CBs if C=24, else state C_actual)
    - Honest caveats:
        "GPU = PoCL-CPU. Real GPU target: IISc MI210 (future)."
        "QEMU CXL does not model latency. CXLMemSim pending PMU."
        "23.4× anchor unchanged: 11,703 µs/slot."
    - What is now proven that wasn't before:
        "Full chain assembled in one process tree on real KVM CXL VM.
         LLR from srsRAN LDPC decode intercepted by bpftime, written
         to CXL-backed shared region, decoded by bit-exact OpenCL
         kernel, result returned — one measured end-to-end latency."
```

---

## Memory + report-back

```
memory/v7_run/implementer/gate_{0,1,2,3,4,5}.md
  Same template: Spec/Commands/Raw-evidence/Self-verdict/Deviations.
  DEV numbering continues from v6 (last was DEV-029; v7 starts DEV-030+).

Report after EACH gate:
  Gate 0: GCP KVM confirmed / deps installed
  Gate 1: CXL VM 5/5 + pmem=off + numactl works
  Gate 2: shared CXL region + zero-copy proof
  Gate 3: LLR mover fires, real LLR bytes in CXL
  Gate 4: e2e run → e2e_gcp.csv → PASS/PARTIAL/FAIL with (a)-(e) status
  Gate 5: write-up committed

Final report format:
  Gate 0 (GCP KVM): PASS/FAIL
  Gate 1 (CXL VM):  PASS/FAIL, pmem=off worked/fallback-to-shim
  Gate 2 (shared CXL + zero-copy): PASS/FAIL, zero_copy=YES/NO(PoCL-copy)
  Gate 3 (LLR mover): PASS/FAIL, cxl_llr_buf[0..2]=<values>
  Gate 4 (E2E):     PASS/PARTIAL/FAIL
                    (a)pstree: YES/NO
                    (b)LLR-in-CXL: YES/NO (numa_maps evidence)
                    (c)OCL-reads-CXL: YES/NO
                    (d)bit_diff=0: YES/NO (Z=<N>)
                    (e)measured number: <per-CB p50> µs
  Gate 5 (write-up): PASS/FAIL
  PRIMARY_CONFIG 23.4×: UNCHANGED
  GCP cost: <hrs used, delete instance when done>
```

---

## Cost note

GCP n2-standard-4 in asia-south2-a: ~$0.19/hr (~₹16/hr).
Phases 0-5 estimated 4-6 hours total = ~₹64-96.

Delete when done:
  gcloud compute instances delete cxl-systems-lab \
    --zone=asia-south2-a --quiet