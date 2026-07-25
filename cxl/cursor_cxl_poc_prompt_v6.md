# v6 — ONE end-to-end pipeline run on the droplet, against REAL /dev/dax0.0

## What this is

v4 proved each COMPONENT real. v5 proved most of the DATA PATH real —
but on WSL2 stand-ins, and the droplet pieces (PROOF 1, PROOF 2, CXL
3-way) ran in INCONSISTENT environments/configs and were NEVER
assembled into one process. v6 has exactly ONE job:

```
Run, as a SINGLE process on the droplet, against the REAL /dev/dax0.0
device (not a host file, not /tmp/cxl_mem.img):

  OAI gNB (numactl --membind=CXL-node)
    → bpftime uprobe on ldpc decode (descriptor only, no payload copy)
    → busy-poll consumer
    → OpenCL bit-exact decode reading LLR directly from /dev/dax0.0
    → decoded bits written back to /dev/dax0.0
    → OAI reads result

and produce ONE end-to-end latency number for it.
```

Nothing else. No new figures, no new framing, no re-deriving the
23.4× anchor. Assemble what exists and run it once, for real.

## The blocker that caused the gap — READ THIS FIRST (DEV-023)

```
DEV-023: /dev/dax0.0 returns ENXIO on mmap "when cxl_mem driver stack
is active". The likely root cause is a MODE CONFLICT we created
ourselves:

  PROOF 1 needed:  daxctl --mode=system-ram  → CXL region becomes
                   NUMA node 1 (for numactl --membind=1). This
                   CONSUMES the devdax char device — dax0.0 is gone,
                   it's now kernel-managed system RAM.

  PROOF 2 needed:  /dev/dax0.0 as a devdax char device to mmap +
                   CL_MEM_USE_HOST_PTR. But if the region is in
                   system-ram mode, /dev/dax0.0 does not exist as a
                   mappable device → ENXIO.

  THEY ARE MUTUALLY EXCLUSIVE on a SINGLE region. You cannot have the
  same CXL region be both a numactl-targetable NUMA node AND a
  mappable /dev/dax0.0 at the same time.

This conflict is WHY no single run assembled the pipeline: the
"OAI allocates in CXL" half (Change A, needs system-ram/NUMA) and the
"OpenCL mmaps /dev/dax0.0" half (PROOF 2, needs devdax) were never
compatible in one configuration.
```

## Phase 0 — RESOLVE THE MODE CONFLICT (this is the crux of v6)

You must get OAI's LLR allocation and OpenCL's buffer to point at the
SAME physical CXL memory in ONE configuration. Investigate, in order:

### Option A — Single region, system-ram mode, BOTH sides use NUMA node
```
- Put the region in system-ram mode → NUMA node 1 (as PROOF 1 did).
- OAI: numactl --membind=1 (allocations land on node 1). ✓ (proven)
- OpenCL consumer: instead of mmap(/dev/dax0.0), allocate its working
  buffer ALSO on node 1 via numa_alloc_onnode(size, 1) or
  mbind(buf, MPOL_BIND, node=1), then pass THAT pointer to
  clCreateBuffer(CL_MEM_USE_HOST_PTR, buf).
- KEY INSIGHT: the descriptor carries an OFFSET. If OAI's LLR is at
  node1-address X and the consumer needs to read it, the consumer
  does NOT need its own buffer — it needs to read X directly. Since
  both OAI and consumer are on the same host, and X is a normal
  virtual address in OAI's space, the consumer must either:
    (a) share the mapping: OAI and consumer share a node-1-backed
        region via shared mmap (MAP_SHARED on a memfd or a file on a
        DAX-backed fs), both numa-bound to node 1, OR
    (b) the consumer reads OAI's buffer via process_vm_readv from the
        node-1 address (works, but is a COPY — acceptable to MEASURE
        but document it's not zero-copy).
- PREFER (a): create ONE shared-memory region (memfd_create or a file
  on a mounted DAX filesystem), mmap MAP_SHARED in BOTH OAI-side
  (via the agent/shim) and consumer, mbind the region to node 1.
  Both see the same physical CXL pages. Descriptor offsets resolve
  in both. OpenCL CL_MEM_USE_HOST_PTR over this region. TRUE zero-copy.
```

### Option B — Two CXL regions (if A's shared-region proves hard)
```
- Configure QEMU with TWO cxl-type3 devices (two pxb-cxl/rp/memdev
  sets), OR one region split: region0 → system-ram (node1, for OAI),
  region1 → devdax /dev/dax0.0 (for OpenCL).
- Then the consumer COPIES LLR from node1 (OAI's) to /dev/dax0.0
  (OpenCL's). This is a copy — NOT zero-copy — but it lets BOTH
  mechanisms exist. MEASURE it, label it "two-region, one copy at
  the handoff (not zero-copy; single-region shared-mapping is future
  work)".
```

### Option C — DAX filesystem (fs-dax instead of devdax)
```
- Mount a filesystem on the DAX device: mkfs.ext4 /dev/pmem0 (if the
  region presents as pmem), mount -o dax. Files on it are page-cache-
  bypassed direct CXL access. OAI's LLR (via a file-backed mmap the
  shim sets up) and OpenCL both mmap files on this fs. Both numa-
  local to the CXL node.
- This sidesteps the devdax-vs-system-ram char-device conflict
  entirely by using fs-dax.
```

### GATE 0 (v6) — the conflict is resolved
```
PASS if: you can demonstrate, in ONE configuration, BOTH:
  (1) an allocation that OAI would make (test with a malloc under
      numactl --membind, or an mmap of the shared region) physically
      resides on CXL memory — confirm via get_mempolicy →
      numa_node=<cxl> OR /proc/<pid>/numa_maps showing the pages on
      the CXL node, AND
  (2) an OpenCL CL_MEM_USE_HOST_PTR buffer over the SAME region
      succeeds (err==CL_SUCCESS) and a sentinel written CPU-side is
      read by the kernel without clEnqueueWriteBuffer (zero-copy) —
      OR, if Option B, the two-region copy path works and is labeled
      not-zero-copy.
  Evidence: numa_node/numa_maps output + sentinel test output +
  which Option (A/B/C) was used and why.

FAIL -> if no option works: the white paper's "OpenCL decodes from
        CXL" claim cannot be made on this droplet. Document the exact
        ENXIO/mode-conflict reason. This becomes an explicit
        Limitation and the e2e run below uses the closest achievable
        (e.g. node1 system-ram for both via process_vm_readv copy),
        CLEARLY labeled. Do not fake it.
```

## Phase 1 — Build the missing droplet piece: OAI

```
The uprobe needs a live OAI gNB ON THE DROPLET to attach to (all v5
uprobe evidence was WSL2). Build OAI on the droplet:
  cd openairinterface5g && ./build_oai --gNB --nrUE -w SIMU
This is a long compile but one-time. If the build is genuinely
intractable in-session, FALLBACK: use the srsRAN ldpc_decoder_
benchmark as the L1 workload (it has a real, uprobe-able decode
symbol too) running in a slot-paced loop — but LABEL this as
"srsRAN-benchmark workload, not full OAI gNB" and note OAI-on-droplet
as remaining work. The point of v6 is the CXL data path being real
end-to-end; the workload being OAI-vs-srsRAN-loop is secondary to
that.
```

### GATE 1 (v6)
```
PASS if: a real L1 process (OAI gNB preferred, srsRAN-benchmark loop
         acceptable-with-label) runs on the droplet and its LDPC
         decode symbol is uprobe-able (nm/objdump shows the symbol;
         a test bpftime attach fires ≥1 event). Evidence: symbol
         resolution + attach-fires count.
```

## Phase 2 — Assemble and run END-TO-END, once, on the droplet

```
Wire the EXISTING, already-proven pieces together in ONE process tree
on the droplet, using Gate 0's resolved CXL configuration:

  [L1 workload, numactl/region-bound to CXL per Gate 0]
    → bpftime uprobe (descriptor only — REUSE v5's, confirm no
      payload copy in the handler via grep)
    → busy-poll consumer (REUSE v5's SPSC ring, pinned core)
    → OpenCL bit-exact kernel (REUSE v4's ldpc_decode.cl + the REAL
      bg_tables.h — and use a Z the tables are CORRECT for: Z=384,
      NOT Z=224 which DEV-011 flagged as wrong-tables. If the workload
      forces Z=224, EXTEND bg_tables.h first or accept the run proves
      path-not-correctness and say so) reading LLR from the CXL region
    → decoded bits to CXL output region
    → workload reads result back

Run N≥1000 CB decodes. Record per-CB and per-slot timing of the FULL
assembled path.
```

### GATE 2 (v6) — THE gate this whole prompt exists for
```
PASS if ALL of:
  (a) It is ONE process tree on the droplet — show `ps`/`pstree`
      evidence that OAI/workload + consumer are running together,
      and the uprobe is attached to the live workload PID.
  (b) The CXL region is REAL: the LLR address the uprobe captures
      resolves to CXL memory (numa_maps / get_mempolicy on the
      actual address from a live descriptor — not a separate malloc
      test). Show the descriptor's llr address AND its numa node.
  (c) OpenCL read the LLR from that CXL region (Gate 0's zero-copy
      confirmed, or the labeled copy path).
  (d) Bit-exactness holds THROUGH this assembled path for the Z used:
      decoded bits match the srsRAN oracle for at least one CB
      (bit_diff=0). If Z=224 with un-extended tables, this will FAIL
      bit-exactness — in that case either extend tables to Z=224
      first, or run the workload at Z=384, so (d) genuinely passes.
  (e) ONE end-to-end latency number is produced and written to
      paper/results/e2e_droplet.csv with an honest emulation_mode
      string and source=measured (it genuinely is, this time).

  Evidence: pstree + live-descriptor-address numa lookup + sentinel/
  zero-copy result + bit_diff=0 for the run's Z + the e2e CSV.

FAIL -> name WHICH of (a)-(e) failed and why. A partial assembly
        (e.g. everything but bit-exactness because of Z=224 tables)
        is a DOCUMENTED PARTIAL, not a PASS. Do not relabel a partial
        as complete — that is the exact failure this project keeps
        catching.
```

## Phase 3 — Honest write-up of what the e2e run proved

```
Update paper/results/RESULTS_SUMMARY.md with a new subsection:
"End-to-end assembled pipeline (droplet, real CXL)":
  - the e2e latency number (per-CB, per-slot), source=measured
  - which Gate 0 option (A/B/C) gave the CXL config, and whether the
    path was true zero-copy or a labeled copy
  - the Z used and whether bit-exactness held for it
  - explicit statement of what is now proven that wasn't before:
    "the full chain runs as one process against CXL memory" — and
    what still isn't: GPU is PoCL-CPU (the ~390ms/CB compute, 6x-
    projection-to-real-GPU caveat UNCHANGED), CXLMemSim sweep still
    blocked (DEV-022, no PMU), real-CXL-silicon latency still
    future/IISc work.
  - the 23.4× anchor is UNCHANGED and still the headline motivation.
```

### GATE 3 (v6)
```
PASS if: RESULTS_SUMMARY.md has the e2e subsection, every number
         traces to Gate 2's CSV, caveats are stated, anchor unchanged.
```

## Cost discipline
```
This IS the paid droplet session. Minimise it:
  - provision.sh → install_deps.sh → build OAI (the long part) →
    Gate 0 mode-conflict resolution → assemble → Gate 2 run →
    collect CSV → checkpoint.sh → teardown.sh.
  - If OAI build is the time sink, consider building it ONCE, then
    checkpoint.sh (snapshot) so a re-run restores from snapshot
    without rebuilding.
  - Tear down at the end. status.sh must show it gone.
```

## Memory + report-back
```
- memory/v6_run/implementer/gate_{0,1,2,3}.md, same template
  (Spec/Commands/Raw-evidence/Self-verdict/Deviations/Files).
  DEV numbering continues (next after v5's last, ~DEV-025+).
- Report after each gate. Final report:
    Gate 0 (mode conflict): PASS/FAIL → Option A/B/C, zero-copy y/n
    Gate 1 (OAI on droplet): PASS/FAIL → OAI or srsRAN-loop
    Gate 2 (E2E assembled):  PASS/FAIL/PARTIAL → which of (a)-(e)
    Gate 3 (write-up):       PASS/FAIL
    E2E number: <per-slot> µs, source=measured, on real CXL
    23.4× anchor: UNCHANGED
    Droplet: TORN DOWN (cost: <hrs>×₹6)
```