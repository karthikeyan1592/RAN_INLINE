# Course correction 001 (v7) — 2026-06-26

## Step 1 — What's new

v7 spec (`telemetry_v7.md`) established. Scope: GCP n2-standard-4 with
`--enable-nested-virtualization`, real KVM inside QEMU, bpftime + PoCL + CXL
all in ONE process tree — the three root causes of every prior failure
addressed simultaneously.

Prior v6 state carried forward:
- Gates 0–3 CONFIRMED (DO droplet, QEMU-TCG)
- Gate 2 PARTIAL PASS: (a)(b)(c)(e) MET; (d) NOT MET (DEV-032, oracle deferred)
- DEV log ends at DEV-032

v7 introduces gates 0–5 (new numbering per spec). DEV sequence continues from
DEV-033.

---

## STOP — v7 implementer work not submitted

**`memory/v7_run/implementer/` does not exist.**

Exhaustive search of the repository found no v7 gate files anywhere:

```
find /root/linux_env/cxl/memory -type d | sort
→ memory/v4_run/, memory/v5_run/, memory/v6_run/  (no v7_run)

find /root/linux_env/cxl -name "gate_*.md" (not in third_party)
→ All results are v4/v5/v6 gates or the DO droplet archive
```

The only candidate location was `paper/results/droplet/gates/`, which contains
gate_0.md through gate_3.md — these are **v6 DO droplet gates** (identical
content to `memory/v6_run/implementer/`), not v7 GCP gates. Key identifiers:

- gate_0.md: `OpenCL device: cpu-haswell-DO-Regular` — DigitalOcean droplet
- gate_1.md: offset `0x35280` — DO droplet build (GCP build derived `0x30cf0`,
  per SETUP_RUNBOOK.md Layer 3)
- gate_2.md: `emulation_mode=qemu_cxl_node1_move_pages` with
  `uprobe_hits=1150`, PARTIAL PASS — this is the v6 re-test result
- DEVIATIONS.md: ends at DEV-032 — the v6 final state

No e2e_gcp.csv exists in paper/results/.

---

## What WAS completed toward v7

The following infrastructure work was completed (this session and prior):

| Item | Status | Location |
|------|--------|----------|
| GCP instance created (n2-standard-4, nested KVM) | DONE | SETUP_RUNBOOK.md Layer 0 |
| Ubuntu 24.04 packages installed | DONE | install_deps.sh run |
| Source rsync'd to GCP host | DONE | SETUP_RUNBOOK.md Layer 2 |
| bpftime built with DEV-035 cmake patch | DONE | SETUP_RUNBOOK.md Layer 3 |
| srsRAN ldpc_decoder_benchmark built on GCP host | DONE | SETUP_RUNBOOK.md Layer 4 |
| QEMU VM disk (Ubuntu 22.04 jammy) | DONE | Layer 5; cidata.iso created |
| QEMU CXL VM launched with -enable-kvm | DONE | SETUP_RUNBOOK.md Layer 6 |
| VM kernel upgraded 5.15 → 6.8 HWE | DONE | SETUP_RUNBOOK.md Layer 7 |
| ndctl v80 built + installed in VM | DONE | vm_install_ndctl80.sh |
| CXL topology (system-ram, NUMA node 1) | DONE | vm_cxl_setup.sh; SETUP_RUNBOOK.md Layer 9 |
| GCP ops scripts (provision, install_deps, prepare_vm, build_tools, run_e2e_test) | DONE | ops/gcp-cxl-lab/scripts/ |
| **Gate programs run (gate_0 through gate_5)** | **NOT DONE** | No gate files exist |
| **memory/v7_run/implementer/ directory** | **NOT DONE** | Missing |
| **DEV-033+ log** | **NOT DONE** | Missing |
| **e2e_gcp.csv** | **NOT DONE** | Not in paper/results/ |

---

## What the v7 spec requires (gates 0–5)

Per `telemetry_v7.md`:

| Gate | What | Key evidence required |
|------|------|-----------------------|
| 0 | KVM confirmed | `dmesg \| grep -i "Hypervisor detected"` inside VM → "KVM" (not TCG) |
| 1 | Uprobe fires on srsRAN in VM | uprobe_hits > 0 with GCP-build offset (0x30cf0, not 0x35280) |
| 2 | CXL topology (system-ram, node 1) | numactl --hardware → 2 nodes; mbind smoke test |
| 3 | LLR mover: bpftime handler writes LLR to shared CXL memfd | LLR[0..2] ∈ ±5..±20; cxl_llr_buf non-zero; shared memfd confirmed |
| 4 | E2E assembled run | (a)–(e) all assessed; NOT arithmetic sum; pstree shows simultaneous processes |
| 5 | GCP instance deleted | `gcloud compute instances list` showing cxl-systems-lab absent |

**Critical distinguisher from v6:** Gate 3 (LLR mover) is entirely new and is
the decisive gate. v6 used tracefs fetcharg + move_pages (cross-process, no bpftime
handler writing to shared buffer). v7 requires bpftime handler to write LLR directly
into a shared CXL memfd — the mechanism that was deferred in every prior version.

---

## Persistent ghost status

Per spec, three ghosts must die in v7:

| Ghost | Status |
|-------|--------|
| DEV-011 (Z=224 wrong tables) | **OPEN** — no gate_4.md to assess |
| DEV-030 (numactl SIGILL, pmem=off fix) | **OPEN** — GCP has `pmem=off` per SETUP_RUNBOOK.md Layer 6; DEV-031 in SETUP_RUNBOOK.md notes SIGILL persists even with pmem=off. Gate 2 must show the mbind shim resolves it. |
| DEV-003 (non-atomic counter) | **OPEN** — no LLR mover source to assess |

Note on DEV-030/numbering: the SETUP_RUNBOOK.md uses "DEV-031" for the GCP
numactl SIGILL issue (pmem=off didn't fix it). But DEV-031 in the official log
is the v6 2/7-pages-EFAULT issue. The SETUP_RUNBOOK.md's local DEV numbering
is NOT consistent with the official DEVIATIONS.md. The v7 implementer must
reconcile: GCP numactl SIGILL is **DEV-033** (next available) in the official log.

---

## STOP / GO

**STOP** — no v7 gate work has been submitted.

Additionally: the GCP instance (`cxl-systems-lab`, `34.131.224.105`) deletion was
requested this session but `gcloud` is not installed on this local machine. The user
was instructed to run the deletion via Cloud Console or a machine with gcloud. If
the instance is still running, billing continues. **Confirm deletion.**

---

## Required actions before v7 audit can proceed

**Action 1 — Create memory/v7_run/implementer/ and run all six gates on the GCP KVM VM.**

If the GCP instance was deleted before running the gates: re-provision (provision.sh
is idempotent) and run the full sequence:

```
./provision.sh
./install_deps.sh
./rsync_source.sh
./build_tools.sh
./prepare_vm.sh
./launch_vm.sh       # handles 5.15→6.8 kernel upgrade + ndctl v80 + CXL setup
./run_e2e_test.sh    # runs gate 0/1/2 inside VM
```

Gate 3 (LLR mover) and Gate 4 (assembled run) require additional source code not
yet written: the bpftime handler that writes LLR to a shared CXL memfd. This is the
one program remaining.

**Action 2 — Submit gate files to memory/v7_run/implementer/gate_{0..5}.md.**

Each gate file must follow the established format: Spec, Commands, Raw evidence
(not description), Self-verdict with criterion table.

**Action 3 — Continue DEVIATIONS.md from DEV-033.**

Do NOT re-use DEV-030/031/032 for GCP-specific issues. Continue from DEV-033.
GCP numactl SIGILL (pmem=off, SIGILL persists) = DEV-033; ndctl 72.1 missing
`cxl create-region` = DEV-034 (already resolved by vm_install_ndctl80.sh);
bpftool cmake patch = DEV-035; grub UUID issue = DEV-036.

**Action 4 — Confirm GCP instance deleted.**

Append `gcloud compute instances list --project=cxl-systems-lab-26` output to
a gate_5.md showing `cxl-systems-lab` absent.

---

## Cross-cutting (Step 5) — pre-check

### PRIMARY_CONFIG anchor

```
calibration_check.txt:
  per_slot_latency_us: 11703   (487.6 × 24)
  overshoot_factor:    23.4    (11703 / 500)
```

UNCHANGED. ✓

### Old arithmetic numbers

No grep hits for 12036 or 11727 in paper/. ✓

### Arithmetic e2e check

Not assessable — no e2e_gcp.csv exists yet.

### GCP instance deleted

Unconfirmed. `gcloud` not available on local machine; user instructed to delete
via Cloud Console. Confirmation required.

---

## Machine-readable summary

```
CONFIRMED (carried from v6): Gates 0–3 (DO droplet)
v7 gate status: NOT_SUBMITTED
Last DEV in official log: DEV-032
Next available DEV: DEV-033
GCP instance deletion: UNCONFIRMED
Primary blocker: memory/v7_run/implementer/ does not exist —
  no gate programs have been run on the GCP KVM environment.
  Gate 3 (LLR mover bpftime handler) source code not yet written.
```
