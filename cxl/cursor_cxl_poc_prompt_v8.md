# v8 — End-to-End CXL RAN Pipeline

## You are a senior systems engineer building a proof-of-concept

You will assemble a working end-to-end pipeline from mostly-existing
components. You write code, debug failures, and report evidence.
You do NOT redesign architecture, re-derive anchors, write papers,
or generate synthetic data. When something fails, debug and fix it.
Report after each phase.

---

## The Architecture (what you are building)

This PoC demonstrates transparent offload of 5G NR LDPC decode from
a CPU-based RAN stack to an OpenCL accelerator through CXL shared
memory, intercepted by eBPF — without modifying the RAN application.

The data flow, end to end:

```
srsRAN ldpc_decoder_benchmark (runs LDPC decode on CPU)
  │
  │  At the decode() function entry point:
  ▼
bpftime userspace uprobe handler
  │  Reads the LLR buffer pointer from register %rdx
  │  Copies ~26 KB of LLR bytes into a shared CXL memory region
  │  Writes a 40-byte descriptor (offset, length, sequence number)
  │  into a lock-free SPSC ring buffer
  │  Handler returns — srsRAN continues unmodified
  │
  ▼
Busy-poll consumer thread (pinned to a dedicated CPU core)
  │  Spins on the SPSC ring, no sleep, no syscall
  │  Pops descriptor, resolves LLR location in CXL region
  │
  ▼
CXL shared memory (QEMU Type-3, NUMA node 1)
  │  The LLR data is already here (bpftime wrote it)
  │  OpenCL maps this region via CL_MEM_USE_HOST_PTR (zero-copy)
  │
  ▼
OpenCL LDPC decode kernel (PoCL CPU runtime)
  │  Bit-exact min-sum decoder, BG1/BG2, Z-tiled
  │  Reads LLR from CXL, writes decoded bits back to CXL
  │
  ▼
e2e_gcp.csv — per-CB latency, bit-correctness, measured
```

Key architectural properties:

- **Transparent interception**: srsRAN binary is unmodified (git diff
  is empty). The uprobe attaches externally. Zero lines of RAN code
  changed.

- **CXL as the shared fabric**: both the interceptor (bpftime handler)
  and the accelerator (OpenCL daemon) access the same physical CXL
  memory. The LLR data is written once by the handler and read in-place
  by OpenCL — no intermediate copy through kernel BPF maps, no
  /proc/pid/mem reads, no socket transfer.

- **Vendor-neutral accelerator**: PoCL runs the decode kernel on CPU
  today. Replacing PoCL with AMD ROCm or any OpenCL-capable GPU
  requires zero code changes — same kernel, same CL_MEM_USE_HOST_PTR
  path. This is the open alternative to NVIDIA's CUDA-locked Aerial
  stack.

---

## What already exists (DO NOT rebuild)

These components are proven across v4-v7 sessions. Reuse them as-is:

| Component | Status | Evidence |
|-----------|--------|----------|
| srsRAN `ldpc_decoder_benchmark` | Real upstream binary, AVX2, BG1/BG2 | git diff empty, runs on host |
| bpftime userspace uprobe | 248.5 ns/call, 929K CB intercepts | WSL2 measurement |
| OpenCL LDPC kernel (`ldpc_decode.cl` + `bg_tables.h`) | Bit-exact, 0 mismatches BG1/BG2 Z=384/256 | `bit_correctness.csv` |
| SPSC descriptor ring (`desc_ring.h`) | 17 ns push-pop, 0 drops at N=1M | WSL2 measurement |
| Busy-poll consumer | Pinned-core, no sleep | v5 Gate 1 |
| CXL stand-in seam (`cxl_region.c`) | Switches via env var CXL_BACKING | v5 |
| QEMU CXL config | persistent-memdev, `-cpu host,-hypervisor`, 5/5 verify checks | droplet sessions |

**Fixed anchor** (never changes, never re-measured):
`PRIMARY_CONFIG: 11,703 µs/slot = 23.4× over 500 µs slot budget`

## What does NOT exist (your job)

Only two things:

1. **`lddc_llr_mover.bpf.c`** — a bpftime handler (~50 lines) that
   reads the LLR pointer from %rdx at `ldpc_decoder_impl::decode()`
   entry, copies the LLR bytes into the shared CXL memfd region, and
   pushes a descriptor to the SPSC ring. This is the ONLY new code.

2. **The assembled pipeline** — all existing components wired together
   in one process tree on the GCP CXL VM, producing one end-to-end
   measurement CSV.

---

## Environment

GCP n2-standard-4, asia-south2-a, Ubuntu 22.04,
`--enable-nested-virtualization` for real nested KVM.
Cost ~₹16/hr. Budget 4-6 hours. Delete when done.

Inside this host: a QEMU VM with CXL Type-3 device, running the
entire pipeline (srsRAN + bpftime + consumer + OpenCL).

---

## Phase 0 — Provision and verify the GCP host

**Goal**: a GCP instance with nested KVM, all dependencies installed,
srsRAN and bpftime built, PoCL available.

**Steps**:
1. Create the GCP n2-standard-4 instance with nested virtualization.
2. Confirm `/dev/kvm` exists and `vmx` is in cpuinfo.
3. Install build dependencies: QEMU with KVM, clang/llvm, libbpf,
   PoCL OpenCL, numactl/ndctl/daxctl, srsRAN build deps, bpftime
   build deps.
4. rsync the project source from local.
5. Build srsRAN `ldpc_decoder_benchmark` with AVX2. Run it briefly
   to confirm no SIGILL.
6. Build bpftime. Confirm the shared library exists.
7. Confirm `clinfo` shows a PoCL CPU device.
8. Find the uprobe offset for THIS build of srsRAN: use `nm` to find
   `ldpc_decoder_impl::decode` (C++ mangled symbol). Record the hex
   offset. It will differ from prior builds (v6 was 0x35280, v7 was
   0x30cf0).

**Gate 0 — pass if**:
- vmx present, /dev/kvm exists
- srsRAN benchmark runs and produces Mbps output
- bpftime shared library built
- PoCL shows CPU device in clinfo
- Uprobe offset recorded from nm on THIS build

---

## Phase 1 — Boot QEMU CXL VM and verify

**Goal**: a QEMU VM running with real KVM acceleration, CXL Type-3
device online as NUMA node 1.

**Steps**:
1. Create a QEMU disk image with Ubuntu 22.04 and kernel 6.6+
   (6.8 HWE preferred). Install the same dependencies inside the VM.
2. Launch QEMU with the verified CXL configuration:
   - `-enable-kvm -cpu host,-hypervisor` (KVM accel + clear hypervisor
     CPUID bit so drivers/cxl/ region creation succeeds)
   - `-M q35,cxl=on` with a `pxb-cxl` host bridge, `cxl-rp` root port,
     `cxl-type3` device backed by a `memory-backend-file` with
     `share=on,pmem=off`
   - `pmem=off` gives Write-Back cache type (avoids the SIGILL from
     Write-Combining pages that DEV-030 discovered)
3. Inside the VM: load CXL kernel modules, run `cxl create-region`,
   `daxctl reconfigure-device --mode=system-ram`. Verify NUMA node 1
   appears with ~1920 MB.
4. Smoke test: `numactl --membind=1 ls` — must succeed without SIGILL.
5. Confirm KVM inside the VM: `dmesg | grep -i kvm` must show KVM,
   not TCG. This is critical — TCG makes PoCL JIT take >20 minutes
   and historically caused the agent to swap in stub kernels.

**Known issues and fallbacks**:
- If `cxl create-region` fails with `pmem=off`: revert to `pmem=on`
  and use the mbind shim from the gotchas document (mbind allocations
  >=16KB to CXL node, keep small allocs on node 0 to avoid shadow
  stack SIGILL). Document which path was taken.
- If `numactl --membind=1` SIGILLs despite `pmem=off`: same shim.
- ndctl/daxctl version: Ubuntu apt ships v72.1 which may lack
  `cxl create-region`. Build ndctl v80 from source if needed (this
  was done in v7).

**Gate 1 — pass if**:
- `cxl list` shows a CXL device
- `numactl --hardware` shows 2 NUMA nodes, node 1 with ~1920 MB
- `numactl --membind=1 ls` completes without SIGILL (or shim used, documented)
- `dmesg` confirms KVM, not TCG, inside the VM

---

## Phase 2 — Shared CXL memory region with zero-copy proof

**Goal**: a single shared memory region, physically on CXL NUMA node 1,
accessible by both the bpftime handler (writer) and the OpenCL consumer
(reader) — resolving the DEV-023 mode conflict.

**Background**: in prior versions, `daxctl --mode=system-ram` consumed
`/dev/dax0.0` as a char device, making it unmappable by OpenCL. The
resolution is to use a `memfd_create` + `mbind` approach: both sides
map the same memfd, mbind'd to the CXL NUMA node. Neither side needs
`/dev/dax0.0` directly.

**Steps**:
1. Create a shared memory region using `memfd_create`. Size: 64 MB
   (enough for multiple LLR buffers + output buffers).
2. `mmap` it as `MAP_SHARED`.
3. `mbind` the entire region to CXL NUMA node 1 (`MPOL_BIND`).
4. Verify placement: `get_mempolicy` with `MPOL_F_ADDR | MPOL_F_NODE`
   on the base address must return node 1.
5. Pre-fault all pages with `memset` (avoids the DEV-031 EFAULT from
   demand-paging).
6. Create an OpenCL buffer over this region using `CL_MEM_USE_HOST_PTR`.
   Verify `clCreateBuffer` returns `CL_SUCCESS`.
7. Zero-copy sentinel test: write a known value (e.g. 0xDEADBEEF) at
   `base[0]` from the CPU side. Run a trivial OpenCL kernel that reads
   `base[0]`. If the kernel sees the value WITHOUT any explicit
   `clEnqueueWriteBuffer` call, zero-copy is confirmed. If PoCL
   silently copies despite correct alignment, document this honestly
   as a PoCL limitation (the architecture is still valid — a real GPU
   OpenCL ICD would zero-copy).

**Gate 2 — pass if**:
- `get_mempolicy` on region base shows node = CXL node (1)
- `clCreateBuffer(CL_MEM_USE_HOST_PTR)` returns `CL_SUCCESS`
- Sentinel test result documented: zero_copy = YES or NO (with evidence)

---

## Phase 3 — Write the LLR mover (the one new program)

**Goal**: a bpftime uprobe handler that, on every `ldpc_decode()` call,
copies the LLR buffer from srsRAN's memory into the shared CXL region
and writes a descriptor to the SPSC ring.

**What the handler must do**:
1. Fire at the entry of `ldpc_decoder_impl::decode()` (the offset from
   Phase 0, step 8).
2. Read the LLR span pointer from register `%rdx` (PARM3 in x86-64
   System V ABI — this is the `span<const log_likelihood_ratio>` data
   pointer). Read the span size from `%rcx` (PARM4).
3. Copy `llr_len` bytes from the LLR pointer to the shared CXL region
   at the current write offset. Use `bpf_probe_read_user` — bpftime's
   userspace BPF has relaxed size limits compared to kernel BPF.
4. Write a descriptor to the SPSC ring: `{timestamp, llr_offset,
   llr_len, output_offset, output_len, sequence_number}`.
5. Advance the write offset for the next codeblock (wrap around within
   the region).

**What the handler must NOT do**:
- Write to a `BPF_MAP_TYPE_ARRAY` in kernel memory (this was the v4
  failure — LLR went to kernel RAM, not CXL).
- Copy the entire LLR through the eBPF ring buffer (this was the v4
  "payload through eBPF" problem — only the 40-byte descriptor goes
  through the ring).
- Block or sleep.

**How to pass the CXL region pointer to the handler**:
bpftime runs the handler in userspace. Check bpftime's documentation
and examples (especially `example/uprobe/`) for how to share host
memory pointers with the handler — typically via a BPF map initialized
by the loader before attach, or via a global variable in the handler
that the agent populates. The handler needs two pointers: the CXL
region base and the SPSC ring base.

**ABI verification**:
Before writing the handler, confirm the register mapping. srsRAN's
`ldpc_decoder_impl::decode()` is a C++ method:
`decode(bit_buffer& output, span<const log_likelihood_ratio> input, ...)`
Under x86-64 System V: `rdi` = this, `rsi` = output, `rdx` = span.data,
`rcx` = span.size. Verify with `objdump -d` on the function prologue
for THIS build — the prior builds confirmed this mapping but compiler
version differences could change it.

**Gate 3 — pass if**:
- Handler compiles and attaches via bpftime to the srsRAN binary
- After running `ldpc_decoder_benchmark -R 1` (one repetition):
  - At least one descriptor appears in the SPSC ring (sequence > 0)
  - The LLR bytes at `cxl_base + llr_offset` are non-trivial (values
    in the ±5..±20 range, not all zeros or 0xFF)
  - Spot-check: 4 bytes from the CXL region match the same 4 bytes
    from a tracefs `%rdx` capture of the same decode call

**If bpftime won't build or attach on GCP (time-box: 30 min)**:
Fall back to kernel uprobe + libbpf CO-RE. The handler logic is
identical but runs in kernel context. Overhead increases from 248 ns
to ~10 µs per call. Document the fallback. This is acceptable — the
architecture is the same, only the interception mechanism changes.

---

## Phase 4 — Assemble and run end-to-end

**Goal**: all components running simultaneously in one process tree
inside the QEMU CXL VM, producing a measured end-to-end CSV.

**Assembly order**:
1. Start the consumer process. It owns the shared CXL region (Phase 2),
   initializes OpenCL with `CL_MEM_USE_HOST_PTR` over the region, and
   enters its busy-poll loop on the SPSC ring, waiting for descriptors.
2. Start the bpftime agent (or kernel uprobe loader). It attaches the
   LLR mover handler to the srsRAN binary at the decode offset. It
   must have access to the same shared CXL region (via the memfd file
   descriptor) and the SPSC ring.
3. Launch srsRAN `ldpc_decoder_benchmark` with `numactl --membind=1`
   (so its allocations land on CXL node 1) and parameters: `-L 384`
   (lifting size matching bg_tables.h), `-Tavx2`, `-R 1000` (1000
   repetitions).
4. As srsRAN runs: each `decode()` call fires the uprobe → LLR copied
   to CXL → descriptor in ring → consumer pops → OpenCL decodes from
   CXL → decoded bits written to CXL output region.
5. After srsRAN exits: consumer writes per-CB results to `e2e_gcp.csv`.

**What the consumer does per descriptor**:
- Pop descriptor from SPSC ring.
- Set OpenCL kernel arguments pointing to `cxl_base + llr_offset`
  (input) and `cxl_base + output_offset` (output).
- Enqueue the bit-exact LDPC decode kernel (from `ldpc_decode.cl` +
  `bg_tables.h`). Z=384, BG1, I=6 minimum (I=20 if JIT fast enough
  under KVM — bit-exactness was proven at I=6 in v4).
- `clFinish` — wait for decode.
- Compare decoded bits against expected output (srsRAN's own decoder
  result, or the test vector oracle from v4). Record bit_diff.
- Record timing: total end-to-end from uprobe timestamp to decode
  completion.

**What e2e_gcp.csv must contain**:
Columns: `cb_index, llr_len, decode_us, bit_diff, e2e_us, emulation_mode, source`
- `decode_us`: OpenCL kernel execution time
- `bit_diff`: number of bit mismatches vs oracle (must be 0)
- `e2e_us`: total from uprobe fire to decode completion
- `emulation_mode`: string describing the exact configuration
- `source`: must say `measured` (and must actually be measured)

**Gate 4 — the only gate that matters. Pass if ALL FIVE are true**:

**(a) One process tree**: evidence showing srsRAN, consumer, and
bpftime agent running simultaneously. `ps` or `pstree` output with
PIDs. They must overlap in time — not sequential.

**(b) LLR in CXL memory**: a live descriptor's LLR address resolves
to the shared CXL region, confirmed by `get_mempolicy` or
`/proc/PID/numa_maps` showing the address on CXL node 1. Not a
separate test allocation — the ACTUAL address from a running decode.

**(c) OpenCL reads from CXL**: the consumer's `cl_mem` buffer base
address matches the shared memfd base from Phase 2. OpenCL is reading
from the same region the handler wrote to — not a separate copy.

**(d) bit_diff = 0**: decoded bits through the full path match the
oracle for at least BG1 Z=384. The decode kernel must be the bit-exact
`ldpc_decode.cl`, NOT a memcpy stub, hard-decision threshold, or
1-iteration approximation.

**(e) CSV is measured, not arithmetic**: the per-CB e2e_us values must
NOT equal any sum of separately-measured components from prior sessions.
Specifically, they must not equal 11,703 + anything, or 2,636 + 163,528,
or any other combination from prior `latency_ladder` files.

**PARTIAL is acceptable**: if (a)(b)(c)(e) pass but (d) fails due to a
Z mismatch or table issue, report honestly as "pipeline assembled,
bit-exactness pending." Do not label a partial as PASS.

---

## Phase 5 — Collect results and destroy instance

**Goal**: results on local machine, GCP instance deleted, billing stopped.

**Steps**:
1. Copy `e2e_gcp.csv` from the VM to the GCP host to local machine.
2. Delete the GCP instance. Verify with `gcloud compute instances list`
   showing zero items.

**Gate 5 — pass if**: CSV is on local machine, instance is deleted.

---

## Rules

1. **No architecture changes.** The uprobe fires at `ldpc_decode()`.
   LLR goes to CXL via bpftime handler. OpenCL decodes from CXL.
   This is the architecture. Do not propose alternatives.

2. **No re-derivation.** PRIMARY_CONFIG = 11,703 µs/slot = 23.4×.
   Fixed. Never re-measured. Never changed.

3. **No synthetic data.** If a measurement fails, report the failure.
   Do not fill CSVs with calculated numbers labeled as measured.

4. **Minimal new code.** The LLR mover is ~50 lines. Everything else
   is assembly of existing components. If you find yourself writing
   hundreds of lines of new code, you are off track.

5. **Time-box build failures.** 30 minutes max on any single build
   issue. If bpftime won't build: fall back to kernel uprobe. If
   PoCL JIT is slow: confirm KVM first, then reduce iteration count.
   Log every fallback.

6. **Report after each phase.** Do not run all phases silently.

7. **Delete the instance when done.** No exceptions.

---

## Fallback ladder

| Problem | Fallback | Log as |
|---------|----------|--------|
| bpftime won't build | Kernel uprobe + libbpf CO-RE | `interception: kernel_uprobe, 9941 ns/call` |
| PoCL LDPC JIT > 5 min | Confirm KVM first; then use I=6 not I=20 | `iterations: 6, bit-exact proven at I=6` |
| numactl --membind=1 SIGILL | mbind shim (>=16KB to CXL, small allocs to node 0) | `membind: shim_used` |
| cxl create-region fails with pmem=off | Revert to pmem=on + shim | `pmem: on, shim_used` |
| bit_diff != 0 | Debug wiring (kernel proven correct in v4). Do NOT change the kernel | `bit_diff: debugging wiring` |
| ndctl too old for cxl create-region | Build ndctl v80 from source | `ndctl: built_v80` |

---

## Final report format

```
Gate 0 (provision):      PASS / FAIL
Gate 1 (CXL VM):         PASS / FAIL — pmem=off worked / shim used
Gate 2 (shared CXL):     PASS / FAIL — zero_copy = YES / NO
Gate 3 (LLR mover):      PASS / FAIL — LLR bytes[0..2] = <values>
Gate 4 (E2E):            PASS / PARTIAL / FAIL
  (a) process tree:      YES / NO
  (b) LLR in CXL:        YES / NO (node = <N>)
  (c) OCL reads CXL:     YES / NO
  (d) bit_diff = 0:      YES / NO (Z = <N>)
  (e) CSV measured:       YES / NO (N = <rows>)
Gate 5 (cleanup):        PASS / FAIL — instance deleted

E2E per-CB p50: <X> µs (source = measured)
PRIMARY_CONFIG 23.4×: UNCHANGED
Fallbacks used: <list or "none">
GCP cost: <hours> × ₹16/hr = ₹<total>
```