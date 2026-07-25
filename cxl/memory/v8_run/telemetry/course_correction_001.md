# Course correction 001 (v8) — 2026-06-26

## Step 1 — What's new

v8 spec (`telemetry_v8.md`) established. Scope: audit of actual gate evidence
from a GCP KVM run — not source code review. The spec defines a structured
check list (CHECK 0.1 through CHECK X.4) and five failure patterns (A–E) to
catch.

v8 source deliveries (new files written this session):

| File | What |
|------|------|
| `phase5_cxl/lddc_llr_mover.bpf.c` | BPF uprobe handler — reads LLR via PARM3, writes to CXL via `bpf_probe_write_user` |
| `phase5_cxl/llr_consumer_v8.c` | Consumer — fork/exec benchmark with agent, BPF load, CXL memfd, bit-exact OCL |
| `phase5_cxl/run_e2e_v8.sh` | Inside-VM orchestration script |
| Makefile `gate-v8` target | Builds `lddc_llr_mover.bpf.o` + `llr_consumer_v8` |

---

## STOP — no gate evidence submitted

**`memory/v8_run/implementer/` does not exist.**

The spec audit requires gate evidence files with raw terminal output. None was
provided. The v7_run/implementer/ gate files (gate_0.md–gate_5.md) all have
empty Raw evidence sections (`PENDING`) — they have never been updated with
actual output from a run.

Root cause: GCP instance (`34.131.224.105`) timed out before any gate program
was executed. No run has occurred under either v7 or v8 programs.

---

## Source code pre-audit (informational — not a substitute for run evidence)

The spec requires evidence, not source review. The following findings are
provisional — they establish what can be verified without a run, and what
**cannot** be cleared until raw evidence exists.

### CHECK 3.1 — writes CXL not BPF map: SOURCE PASS

`lddc_llr_mover.bpf.c` data path:
1. `bpf_probe_read_user(s->data, len, llr_ptr)` — LLR → `scratch_map`
2. `bpf_probe_write_user(cxl_base + slot*CB_STRIDE, s->data, len)` — scratch → CXL

`bpf_probe_write_user` IS implemented in bpftime. Confirmed:
```
runtime/src/bpf_helper.cpp:239 — int64_t bpftime_probe_write_user(uint64_t dst, ...)
runtime/src/bpf_helper.cpp:1253 — BPF_FUNC_probe_write_user registered
runtime/unit-test/test_probe.cpp — unit test for bpftime_probe_write_user
```
The implementation uses SIGSEGV-based safety (returns `-EFAULT` if dst is
invalid, does not crash). If `cxl_base` is a valid writable VA in the
benchmark process (which it is, because the consumer forks the benchmark after
mmap'ing the CXL region), the write proceeds.

**Intermediate scratch_map is NOT a concern for Pattern D**: `scratch_map` is
a bpftime BPF array map residing in POSIX shared memory (userspace) — NOT
kernel RAM. The final destination of the LLR is the CXL memfd region at
`cxl_base + llr_off`. This satisfies CHECK 3.1.

Cannot clear until runtime: confirm `bpf_probe_write_user` returned 0 (not
-EFAULT) in the actual run.

### CHECK 3.4 — descriptor is small: SOURCE PASS

`struct desc_t` = 40 bytes:
`{timestamp_ns(8), llr_offset(4), llr_len(4), out_offset(4), out_len(4), seq(4), _pad(4)}`

Ring carries byte offsets only. LLR payload (~26 KB) is NOT in the ring.

### CHECK 4.4 — bit-exact kernel, not stub: SOURCE PASS

`llr_consumer_v8.c` loads `ldpc_decode.cl`, calls kernel `"ldpc_decode"`,
with `n_iter=6`, `n_vn_full=68`, `n_cn=46`, `n_vn_info=22`, `Z=384`.
Kernel arg mapping verified against `ldpc_decode.cl` signature (10 args, all
aligned). This is the min-sum LDPC decoder from Phase 2, NOT `cxl_copy`, NOT
a hard-decision threshold.

### Pattern A (arithmetic) — NOT pre-clearable

The `bit_diff` column is recorded as `-1` (DEFERRED) with honest annotation.
The `e2e_us` column will be measured from `desc.timestamp_ns` (written at
uprobe fire time) to `ns_now()` after `clFinish()`. Whether `e2e_us` is a
real measurement or an arithmetic sum of prior numbers cannot be determined
from source code alone — it depends on actual PoCL JIT latency on KVM
hardware. Cannot clear until run produces a CSV with variance data.

### FINDING — hardcoded BG1 assumption

`llr_consumer_v8.c` hardcodes `n_vn_full=68, n_cn=46, n_vn_info=22` (BG1)
for the OCL kernel regardless of the captured LLR's actual graph size.

`ldpc_decoder_benchmark -L 384 -I 5 -T avx2 -R 1000` may interleave BG2
configurations (52 VNs × 384 = 19968 bytes). When a BG2 LLR is captured:
- Handler writes 19968 bytes to CXL slot (correct)
- `desc.llr_len = 19968`
- Consumer passes `n_vn_full=68` to OCL kernel → reads 6144 zero bytes at tail
- Decode uses wrong parity graph → output is incorrect

**Impact**: If BG2 CBs are captured, decode runs against wrong graph. For BG1-
only captures the issue doesn't arise. Recommend: infer `n_vn_full` from
`desc.llr_len` (26112 → BG1, 19968 → BG2). Non-blocking on gate pass if the
benchmark is confirmed BG1-only for `-L 384`.

---

## What each check requires from the run

The following cannot be cleared from source. They require raw terminal output
in gate files.

| Check | Requirement |
|-------|-------------|
| CHECK 0.1 | `grep -c vmx /proc/cpuinfo` non-zero + `/dev/kvm` exists — from GCP host |
| CHECK 0.2 | `nm ldpc_decoder_benchmark \| grep ldpc_decoder_impl` — offset NOT 0x35280 or 0x30cf0 (new build) |
| CHECK 0.3 | `ls -la libbpftime-agent.so libbpftime-syscall-server.so` file sizes |
| CHECK 0.4 | `clinfo` showing CPU device |
| CHECK 1.1 | `dmesg \| grep -i kvm` from INSIDE VM → "Hypervisor detected: KVM" |
| CHECK 1.2 | `numactl --hardware` → node 1 ≥ 1800 MB |
| CHECK 1.3 | `numactl --membind=1 ls` or mbind test — pass or documented DEV-033 |
| CHECK 2.1 | `get_mempolicy(MPOL_F_ADDR\|MPOL_F_NODE, cxl_base)` → node 1 |
| CHECK 2.2 | `clCreateBuffer` err=0 in `[v8]` output |
| CHECK 2.3 | `[v8] zero_copy=YES/NO` line with explicit documentation |
| CHECK 3.1 | `[v8] bpftime_uprobe_attached` + `bpf_probe_write_user` returning 0 |
| CHECK 3.2 | `[v8] first CB: ...` line (handler fired at least once) |
| CHECK 3.3 | `[v8] first CB: llr[0..4]=<v0> <v1> ...` — values in ±5..±20 range |
| CHECK 4.1 | `ps aux` snapshot while both `llr_consumer_v8` and `ldpc_decoder_benchmark` show in output |
| CHECK 4.2 | `[v8] first CB: llr_node=1` — CXL node confirmed on live descriptor address |
| CHECK 4.3 | `CL_MEM_USE_HOST_PTR` confirmed in `[v8]` output at `cxl_base` address |
| CHECK 4.5 | `e2e_gcp.csv` — rows have variance; mean ≠ any known arithmetic sum (11703+X, 166164, 12036, 11727) |
| CHECK 4.6 | `wc -l e2e_gcp.csv` ≥ 101 (100 data rows + header) |
| CHECK 5.1 | `gcloud compute instances list` showing instance absent |
| CHECK X.1 | Offset from Gate 0 nm == offset in `[v8] uprobe_offset=` line |
| CHECK X.2 | `cxl_base` in `config_map.cxl_base=` line + first CB `llr_offset` → `cxl_base + llr_offset` within [cxl_base, cxl_base+64M] |

---

## STOP / GO

**STOP** — no evidence submitted. All checks assessable only after a live run.

GCP instance is also unreachable (SSH timeout to 34.131.224.105). Must
reprovision before any run is possible.

---

## Required actions

**Action 1 — Reprovision GCP and run.**

```bash
cd /root/linux_env/cxl/ops/gcp-cxl-lab
bash scripts/provision.sh           # creates new instance, saves IP
bash scripts/install_deps.sh
bash scripts/rsync_source.sh        # syncs lddc_llr_mover.bpf.c, llr_consumer_v8.c, etc.
bash scripts/build_tools.sh         # bpftime + srsRAN bench + uprobe offset
bash scripts/prepare_vm.sh
bash scripts/launch_vm.sh           # 2-boot sequence, ndctl v80, CXL setup
# SSH to GCP host, SSH to VM:
bash run_e2e_v8.sh --phase all      # inside VM as root
```

`run_e2e_v8.sh --phase all` runs phases 0 (env checks), 1 (CXL NUMA), and
the full assembled run (phases 2+3+4) under `LD_PRELOAD=bpftime-syscall-server.so`.
Log is saved to `/tmp/v8_consumer.log` and CSV to `paper/results/e2e_gcp.csv`.

**Action 2 — Create memory/v8_run/implementer/ with gate files.**

Do NOT update the v7_run gate files — v8 is a separate run with separate
commands. Create fresh gate_0.md through gate_5.md under `v8_run/implementer/`,
paste verbatim output from `run_e2e_v8.sh --phase all` into Raw evidence.

The gate file format is: `## Spec`, `## Commands`, `## Raw evidence` (verbatim
paste, no paraphrase), `## Self-verdict` (criterion table with MET/NOT MET).

**Action 3 — State BG graph used explicitly in gate_4.md self-verdict.**

```
DEV-011 ghost: benchmark invoked with -L 384 -I 5 -T avx2.
If all captures show llr_len=26112 (BG1): DEV-011 not affected (no Z=224 table used).
If any capture shows llr_len=19968 (BG2 Z=384): note the hardcoded n_vn_full=68
assumption (FINDING above) and whether affected CBs were decoded correctly.
```

**Action 4 — Delete GCP instance last, capture output for gate_5.md.**

```bash
gcloud compute instances delete cxl-systems-lab \
  --project=cxl-systems-lab-26 --zone=asia-south2-a --quiet
gcloud compute instances list --project=cxl-systems-lab-26
```
Paste both commands + output into gate_5.md Raw evidence.

---

## Cross-cutting pre-check

### PRIMARY_CONFIG anchor

```
calibration_check.txt:
  per_slot_latency_us: 11703   (487.6 µs × 24 CB)
  overshoot_factor:    23.4    (11703 / 500)
```
UNCHANGED. ✓

### Ghost numbers absent

No hits for 12036, 11727, or 166164 in `paper/results/`. ✓

### GCP instance deleted

UNCONFIRMED — SSH timeout to 34.131.224.105.

---

## Machine-readable summary

```
v8 gate files:       NOT_SUBMITTED (memory/v8_run/implementer/ missing)
v7 gate files:       STALE — empty Raw evidence, v7 commands (not v8)
Source checks:
  CHECK 3.1:         SOURCE_PASS (bpf_probe_write_user confirmed in bpftime)
  CHECK 3.4:         SOURCE_PASS (descriptor 40 bytes, offsets only)
  CHECK 4.4:         SOURCE_PASS (ldpc_decode.cl, n_iter=6, BG1)
  Pattern A:         NOT_CLEARABLE (no run data)
  BG1 assumption:    FINDING (non-blocking if -L 384 benchmark is BG1-only)
Primary blocker:     GCP instance unreachable — no run has executed.
                     Reprovision → run run_e2e_v8.sh → submit v8 gate files.
Next DEV:            DEV-033 (official; do not reuse v7 SETUP_RUNBOOK labels)
```
