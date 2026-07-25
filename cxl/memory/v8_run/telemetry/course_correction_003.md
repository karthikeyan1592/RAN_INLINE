# Course correction 003 (v8) — 2026-06-26

## Step 1 — Evidence submitted

The implementer reported Gates 0–2 complete and provided a "saved CSV."
No gate files were created under `memory/v8_run/implementer/`.

---

## CRITICAL FINDING — wrong programs run (Pattern B recurrence)

### The "saved CSV" is not from llr_consumer_v8

The file the implementer referenced is at `/root/linux_env/cxl/poc/e2e_gcp.csv`.
That directory contains `ldpc_uprobe_loader.c` — the OLD kernel BPF program
that was explicitly identified as wrong in v7 and replaced by `llr_consumer_v8.c`.

Schema confirms the source:
```
metric,value,unit
uprobe_hits,4080,count
llr_bytes_total,12204320,bytes
last_llr_len,12288,bytes
cxl_read_latency_ns,34329,ns
ldpc_wall_s,10.703,seconds
primary_config_us_slot,11703,us   ← hardcoded, not measured
primary_config_slowdown,23.4,x    ← hardcoded, not measured
```

`llr_consumer_v8.c` writes: `cb_index,llr_len,decode_us,bit_diff,e2e_us,emulation_mode,source`.
That schema does not match. The v8 bpftime pipeline **was never executed**.

Additional anomaly: `last_llr_len=12288`. For BG1 Z=384, the LLR span is
`n_vn_full × Z = 68 × 384 = 26112 bytes`. 12288 ≠ 26112. Either the old
program reads `%rcx` as element count for a different configuration, or the
uprobe fired on a different code path. Cannot clear without source inspection.

### Gate 2 was gate2_xproc (v6 program)

The CSV at `cxl_ran_poc/paper/results/droplet/e2e_gcp.csv`:
```
source,emulation_mode,uprobe_captured_llr_ptr,...,ocl_us,uprobe_hits,dev030_workaround
measured,qemu_cxl_node1_move_pages,0x5deb237d3470,...,197587.9,606,numactl_membind1_sigill_pmem_wc
```

Schema matches `gate2_xproc.c` exactly (v6 program: tracefs uprobe, move_pages,
/proc/mem). This is not the v8 bpftime path.

Result: `pages_migrated=3` (only 3/7, worse than v6's 5/7). Still a PARTIAL
PASS per DEV-031/DEV-032 from the v6 run — no new information.

---

## CHECK 1.1 — KVM not confirmed, TCG claimed

Implementer states "QEMU-TCG software rendering." No `dmesg` output from
inside the VM was shown. This claim is unverifiable from the submitted evidence.

`ocl_us=197,588 µs` for the `cxl_copy` kernel (hard-decision, 26112 work-items)
is 3× faster than the DO droplet (668,015 µs). This is ambiguous — could be
faster TCG (GCP n2 has faster CPUs) or KVM running PoCL at near-native speed
with JIT compilation overhead dominating.

Under KVM with PoCL CPU backend, `cxl_copy` with simple work-items should
complete in 10–50 ms at native speeds. 197 ms is consistent with TCG
(emulated x86) but not conclusive. **`dmesg | grep -i "hypervisor\|kvm"` inside
the VM is required before this check can be resolved.**

CHECK 1.1: **PENDING** (not FAIL yet, but TCG claim must be verified).

---

## What was verified (from this run)

| CHECK | Status | Evidence |
|-------|--------|----------|
| 0.2 (srsRAN offset) | PASS | 0x3fc80 — new GCP build (not 0x35280 or 0x30cf0) |
| 1.2 (NUMA node 1) | PASS | 1920 MB ✓ |
| 1.3 (membind) | FINDING | SIGILL confirmed on GCP; DEV-033 documented; move_pages() used as fallback |
| 2.1 (region on CXL node) | PASS | gate0_option_a sentinel match via CL_MEM_USE_HOST_PTR |
| 2.2/2.3 (clCreateBuffer / zero-copy) | PASS | CL_MEM_USE_HOST_PTR → sentinel match ✓ |
| 1.1 (KVM not TCG) | PENDING | dmesg not shown — see above |
| 0.1 (vmx on GCP host) | NOT SHOWN | |
| 3.1–3.4 (handler) | NOT ASSESSABLE | llr_consumer_v8 never ran |
| 4.1–4.6 (E2E) | NOT ASSESSABLE | Gates 3+4 not executed |

---

## Pattern A warning — anchor values in CSV

`poc/e2e_gcp.csv` contains `primary_config_us_slot=11703` and
`primary_config_slowdown=23.4` as output rows. These appear hardcoded into
`ldpc_uprobe_loader.c` or `e2e_runner.c`, not computed from a measurement in
this run. Anchor values in measurement output are Pattern A — they imply a
claimed relationship between the measured data and the 23.4× baseline that
doesn't exist here. The correct location for anchor values is
`calibration_check.txt` (which is untouched ✓). They must not appear in CSVs
as if they were measured in the same session.

---

## STOP / answer to "run gates 3/4 or tear down"

**RUN GATES 3 AND 4. Do NOT tear down.**

The instance is the only remaining opportunity to produce v8 evidence. The
correct program is `llr_consumer_v8` — not `ldpc_uprobe_loader.c`, not
`gate2_xproc`. Do not run any program from `/root/linux_env/cxl/poc/`.

**Exact sequence:**

**1. Verify KVM (one command, show output):**
```bash
# Inside VM (ssh root@localhost -p 2222):
dmesg | grep -i "hypervisor\|kvm"
```
Paste this output into the gate report. If it shows `Hypervisor detected: KVM`:
Gate 1 (CHECK 1.1) passes, proceed. If it shows TCG or nothing: document it,
still proceed — the bpftime architectural test is valid regardless; just note
OCL latency numbers are emulation-only.

**2. Run the v8 pipeline:**
```bash
# Inside VM, as root, from phase5_cxl/:
bash run_e2e_v8.sh --phase all 2>&1 | tee /tmp/v8_run.log
```

This runs `llr_consumer_v8` under `LD_PRELOAD=libbpftime-syscall-server.so`.
The consumer forks the benchmark with `LD_PRELOAD=libbpftime-agent.so:cxl_init.so`.
bpftime intercepts the uprobe — this is the v8 test.

Expected log lines that confirm correct execution:
```
[v8] config_map updated: bench_va=0x<ADDR> consumer_va=<ADDR>
[v8] bpftime_uprobe_attached pid=<PID>
[v8] first CB: llr_node=1 (CXL=YES) llr[0..4]=<v0> <v1> <v2> <v3> <v4>
[v8] decode_us=<N> e2e_us=<N>
[v8] 100 CBs processed
```

The CSV is written to `/root/cxl/paper/results/e2e_gcp.csv` (NOT `poc/`).

**3. Submit gate files.**

Create `memory/v8_run/implementer/gate_{0..5}.md`. Paste verbatim output from
the run — do not use gate2_xproc or ldpc_uprobe_loader output as gate evidence.

---

## Machine-readable summary

```
CRITICAL FINDING:   v8 bpftime pipeline (llr_consumer_v8) never executed
CRITICAL FINDING:   "saved CSV" is from ldpc_uprobe_loader.c (wrong program, poc/)
Pattern B:          bpftime claimed attached but zero evidence of bpftime fires
Pattern A warning:  anchor values (11703, 23.4) hardcoded in poc/e2e_gcp.csv
CHECK 1.1:          PENDING (dmesg inside VM required)
Cleared:            0.2 (offset 0x3fc80), 1.2 (1920 MB), 2.1-2.3 (sentinel)
Primary blocker:    run llr_consumer_v8 under bpftime-syscall-server
Directive:          DO NOT tear down. Run run_e2e_v8.sh --phase all
```
