# Course correction 002 (v8) — 2026-06-26

## Step 1 — What changed since CC-001

Two new artifacts added:

| File | What |
|------|------|
| `phase5_cxl/cxl_init.c` | LD_PRELOAD constructor — maps CXL shm in benchmark's post-exec address space, writes benchmark VA to `CXL_VA_FILE` |
| `llr_consumer_v8.c` edits | memfd → shm_open; sentinel cxl_base=0; env vars before fork; poll VA file then update config_map; attach uprobe only after VA confirmed |
| `Makefile` | `gate-v8` now builds `cxl_init.so` + `llr_consumer_v8` |

---

## Finding closed — execl() mmap unmapping (Pattern D, latent)

CC-001 CHECK 3.1 issued SOURCE PASS based on `bpf_probe_write_user` being
implemented in bpftime. That check was incomplete: it did not assess whether
`cxl_base` (the target VA) was valid in the benchmark's address space.

The original code used `memfd_create` + `mmap` in the consumer, then `fork()`,
then `execl()`. From `execve(2)`: "All existing memory mappings are unmapped."
The mmap at `cxl_base` would have been destroyed by `execl()`. The BPF handler
would then call `bpf_probe_write_user(cxl_base + offset, ...)` targeting a
VA not mapped in the benchmark — `bpf_probe_write_user` returns -EFAULT, no
data reaches CXL. At runtime this would have shown `llr[0..4]=0 0 0 0 0` in
CXL (Pattern D recurrence via silent -EFAULT).

**The fix is architecturally correct.** The mechanism:

```
Consumer                                Benchmark (after fork+exec)
────────────                            ────────────────────────────
shm_open("/cxl_region_v8", O_CREAT)    (LD_PRELOAD=bpftime-agent:cxl_init.so)
mmap → consumer_va                      cxl_init constructor:
mbind(node 1)                             shm_open("/cxl_region_v8")
config_map.cxl_base = 0 (sentinel)       mmap → bench_va  [NEW VA, valid here]
set env vars                              mbind(node 1)
fork + execl                              write bench_va → CXL_VA_FILE
poll CXL_VA_FILE (≤5 s)              ←── VA written
read bench_va
config_map.cxl_base = bench_va
bpf_program__attach_uprobe(bench_pid) ← uprobe attached ONLY after VA is valid
```

The handler now writes to `bench_va + slot*CB_STRIDE` — a VA that is mapped
and CXL-backed in the benchmark's own address space. `bpf_probe_write_user`
will succeed.

Sentinel guard is effective: `cxl_base == 0` → handler returns 0 immediately,
no writes before VA is populated. Uprobe is attached after `config_map` is
updated — no window where the handler fires with stale cxl_base.

CHECK 3.1 revised: **SOURCE PASS (full)** — both the helper and the target VA
are now verified sound.

---

## Remaining FINDING — ring write ordering (non-blocking)

In `lddc_llr_mover.bpf.c` lines 135–156:

```c
__u32 seq = __sync_fetch_and_add(head_p, 1);   // head incremented HERE
...
struct desc_t *d = bpf_map_lookup_elem(&ring_map, &ring_slot);
if (d) {
    d->timestamp_ns = bpf_ktime_get_ns();       // descriptor written AFTER
    d->llr_offset   = llr_off;
    ...
}
```

`ring_head` is incremented BEFORE the descriptor is written. A consumer polling
the head counter could see `head = seq+1` before descriptor[seq] is fully
populated.

Mitigating factors:
1. x86 TSO (total store order) — stores from one CPU are seen in program order
   by other CPUs, so after the atomic-add is visible, subsequent descriptor
   stores will also appear in order
2. bpftime maps are in POSIX shmem (same NUMA domain) — no cross-socket
   coherency delay
3. The consumer's `bpf_map_lookup_elem` call adds latency that serves as an
   implicit delay

Risk: worst case is reading a descriptor with zero `llr_offset` and `llr_len`
on the very first CB if the consumer wins the race — immediately visible as
`decode_us=0` or garbage in the CSV. Not a silent error.

**Correct fix** (post-gate): write descriptor FIRST, then `__sync_fetch_and_add`.
Not blocking for gate pass — the failure mode is self-evident from CSV data.

---

## BG1 hardcoding FINDING — unchanged from CC-001

`llr_consumer_v8.c` still hardcodes `n_vn_full=68, n_cn=46, n_vn_info=22` for
the OCL kernel regardless of `desc.llr_len`. Non-blocking if `-L 384 -I 5
-T avx2` produces BG1-only traffic. Gate_4.md must explicitly state which BG
configurations appeared in the run and whether `llr_len = 26112` for all
captured CBs.

---

## Source status after CC-002

| CHECK | Source status |
|-------|--------------|
| 3.1 (writes CXL not kernel RAM) | SOURCE PASS (full — execl VA fixed) |
| 3.4 (descriptor 40 bytes) | SOURCE PASS |
| 4.4 (bit-exact kernel) | SOURCE PASS |
| Ring write ordering | FINDING (non-blocking) |
| BG1 hardcoding | FINDING (non-blocking if BG1-only run confirmed) |
| Pattern A (arithmetic) | NOT CLEARABLE from source |
| All runtime checks (0.1–X.4) | NOT ASSESSABLE — no run |

---

## STOP — same blocker as CC-001

`memory/v8_run/implementer/` does not exist. GCP instance unreachable.
No gate programs have been executed. No e2e_gcp.csv exists.

Source is now ready to run. The only remaining action before an audit can
proceed is: provision GCP → run `run_e2e_v8.sh --phase all` inside VM →
paste verbatim output into gate files under `memory/v8_run/implementer/`.

---

## Machine-readable summary

```
CC-001 Pattern D finding: CLOSED (execl VA fix via shm_open + cxl_init.so)
CC-001 CHECK 3.1:          UPGRADED to SOURCE PASS (full)
New FINDING:               ring write-ordering (non-blocking)
BG1 hardcoding FINDING:    UNCHANGED from CC-001 (non-blocking)
Primary blocker:           UNCHANGED — no run, no gate evidence
v8_run/implementer/:       MISSING
GCP instance:              UNREACHABLE
```
