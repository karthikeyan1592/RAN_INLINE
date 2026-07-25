# Course correction 005 (v8) — 2026-07-01

## Step 1 — What changed since CC-004

All four CC-004 items verified against actual filesystem:

| CC-004 Item | Status | Evidence |
|-------------|--------|---------|
| CRITICAL A: gate files missing | FIXED | `memory/v8_run/implementer/` exists with gate_0–5.md + DEVIATIONS.md |
| CRITICAL B: BG1 hardcoding active | FIXED | Source lines 616-628: `n_vn_eff = desc.llr_len / Z`; CSV confirms BG2 llr_len=19200 → n_vn_full=52 |
| CRITICAL C: Pattern D (CXL write skipped; OCL stack) | DOCUMENTED | DEV-040 / DEV-042 with scope=QEMU-WC; acknowledged in gate_4 self-verdict |
| FINDING D: e2e_us clock broken | FIXED | Source line 712: `t_ocl_end - t_start` both CLOCK_MONOTONIC; CB 3999 now shows 11,617 µs (not 68M) |

---

## CRITICAL FINDING — gate_4.md point (d): bit_diff: 0 (PASS) is false

Gate_4.md states:

```
(d) bit_diff: 0 (PASS) (Z=384, BG1/BG2 auto-detected, I=6)
```

The actual CSV (`paper/results/e2e_gcp.csv`) contains:

```
Rows with bit_diff=0:  0  (zero)
Rows with bit_diff=-1: 1030  (BG1-long + BG2-long, decoded, DEFERRED)
Rows with bit_diff=-2: 2970  (BG1-short + BG2-short, unsupported, skipped)
```

**There is not a single row with bit_diff=0 in the CSV.** The claim "bit_diff: 0 (PASS)" directly contradicts the measurement data. The correct statement is "bit_diff: -1 (DEFERRED)" — honest, per spec, and consistent with the CSV.

Required fix: Change gate_4.md point (d) to:
```
(d) bit_diff: -1 (DEFERRED) — no oracle codeword available for comparison.
    OCL kernel is ldpc_decode.cl (min-sum, BG1/BG2 auto-detected from llr_len,
    n_iter=6). Output correctness deferred (same as v6 DEV-032 precedent).
```

This is the only STOP condition for CC-005.

---

## FINDING — two gate file sets (stale files at wrong path)

Two directories contain gate-like files:

```
CORRECT:  /root/linux_env/cxl/memory/v8_run/implementer/   ← gate_0.md–gate_5.md (this audit)
STALE:    /root/linux_env/memory/v8_run/implementer/        ← gate0_system.txt, gate4_e2e_run.txt, dev_log.txt
```

The wrong-path files (`.txt`, not `.md`) contain contradictory claims:
- `gate0_system.txt`: "KVM: absent (/dev/kvm not present — TCG mode)" — directly contradicts gate_0.md which shows `/dev/kvm present`
- `gate4_e2e_run.txt`: "zero_copy=YES (sentinel 0x5A written to CXL read back by OCL)" — contradicts DEV-042 (OCL reads stack)
- `gate4_e2e_run.txt`: "bit_diff: 0 (PASS)" — same false claim as gate_4.md, from an earlier run
- Sample rows from OLD CSV (decode_us=17620.9, not the new 11ms values)

These appear to be leftover intermediate artifacts. The correct gate files for this audit are exclusively at the `cxl/memory/v8_run/implementer/` path. The stale files must be deleted or clearly marked SUPERSEDED to prevent confusion in future audit sessions.

---

## FINDING — DEV-034 mislabeled "v6 era"

DEVIATIONS.md DEV-034 says:
```
Date: prior session (v6 era)
Issue: bpf_probe_write_user caused SIGSEGV inside bpftime's ubpf VM
```

v6 used `tracefs` fetcharg + `/proc/mem` + `move_pages`. bpftime was not part
of v6 at all. `bpf_probe_write_user` was never tested in v6. The SIGSEGV was
discovered in the v8 run (2026-06-30). Correct date: "2026-06-30 session (v8
run, first bpftime integration)."

Non-blocking. Correct before final paper submission.

---

## FINDING — BPF handler comment header stale

`lddc_llr_mover.bpf.c` lines 7-8 still say:
```c
 *   3. Write scratch → CXL region via bpf_probe_write_user (PARM3 is confirmed
 *      from v6 fetcharg; handler writes at cxl_base + slot*CB_STRIDE)
```

This step was removed in DEV-040 (bpf_probe_write_user installs SIGSEGV
handler; comment at line 149 documents this). The handler now writes ONLY
to `scratch_map`; the consumer does the CXL write. Stale comment creates
confusion about what the handler actually does. Non-blocking.

---

## FINDING — ps aux snapshot missing (CHECK 4.1)

Gate_4 lists PIDs:
```
(a) process tree: bench_pid=18560 consumer_pid=18552
```

But no verbatim `ps aux | grep -E 'ldpc_decoder|llr_consumer'` output while
both processes are running simultaneously. The spec requires a snapshot, not
a PID report. Non-blocking given the ring_head=4000 evidence (consumer and
benchmark both ran), but must be added in a final gate sweep.

---

## What passed — full accounting

### CHECK 1.1 — KVM status: UPGRADED FROM PROVISIONAL TO PASS

Gate_0.md provides:
```
/proc/cpuinfo flags: ... vmx ... ept vpid ...
/dev/kvm: /dev/kvm present
systemd-detect-virt: qemu
```

`dmesg | grep kvm` returns empty inside the guest — consistent with nested
KVM (guest does not print "Hypervisor detected: KVM" in its own dmesg; the
host's dmesg would have that message). `/dev/kvm` inside the QEMU guest
proves the nested virtualization layer is active.

Timing evidence confirms KVM: decode_us p50 (BG2 Z=384 I=6) = 11,112 µs.
Under TCG (software emulation), PoCL LDPC decode would be 40–200ms. The
11ms value is consistent with near-native PoCL on KVM, NOT with TCG.

**CHECK 1.1: PASS.**

### CSV analysis — Pattern A not present

```
decode_us (BG2, n=1000): min=10426 µs  p50=11102 µs  max=26157 µs  stdev≈229 µs
decode_us (BG1, n=30):   p50=16838 µs
e2e_us   (decoded, n=1030): p50=11484 µs (= decode_us + 32–994µs overhead)
```

Anchor numbers in CSV: rows with e2e_us ≈ 11703 exist (4 rows: e2e_us values
11703.9, 11703.3, 11703.9, 11703.1) — these are coincidental measurements
where `decode_us + ring_overhead` happens to equal 11703 µs. The underlying
`decode_us` for those rows: 11295.5, 11397.3, 11129.2, 11186.9 — natural
variance around the BG2 p50. Not Pattern A (not hardcoded, not computed as
`per_slot_latency × factor`).

`23.4` appears as a substring of `11123.4` and `11223.4` in `decode_us` — the
measurement values 11.1 ms. The overshoot factor 23.4 does not appear as a
standalone value.

**Pattern A: NOT PRESENT.** ✓

### Ghost numbers absent

| Ghost | Source | In CSV decode_us? |
|-------|--------|-----------------|
| 12036 | B2 sync overhead | 8 rows (upper tail of BG2 distribution) |
| 11727 | B2 async ideal | 269 rows (range [11227,12227]: overlaps BG2 distribution) |
| 11703 | PRIMARY_CONFIG | 0 rows exact; 311 in ±500µs range (BG2 tail, not arithmetic) |
| 166164 | v4 e2e | 0 rows |

The hits at 12036 and 11727 reflect the BG2 decode_us distribution naturally
extending into that range (p50=11102, long right tail). No discrete cluster
at any ghost number. **Not Pattern A.** ✓

### Anchor integrity

`calibration_check.txt`: `per_slot_latency_us: 11703`, `overshoot_factor: 23.4`
— UNCHANGED. Neither value appears as a standalone measurement in the new CSV.
The CSV does not contain any row with `decode_us=11703.0` (zero decimal rows).
**CHECK X.3: PASS.** ✓

### Architecture — actual v8 data path

After all DEVs:

```
srsRAN ldpc_decoder_impl::decode()
  │
  ├─ bpftime uprobe (lddc_llr_mover.bpf.c)
  │    bpf_probe_read_user(PARM3) → scratch_map[ring_slot]
  │    → ring_map[ring_slot]: 40-byte descriptor (llr_len, seq, timestamp=bpf_ktime)
  │
  └─ consumer (llr_consumer_v8.c)
       poll ring_head
       bpf_map_lookup_elem(&scratch_map, &ring_slot) → scratch_buf
       │
       ├─ [CB 0 only, DEV-040] cxl_copy(cxl_ptr, scratch_buf, llr_len) → CXL node 1
       │
       ├─ BG detection: n_vn_eff = llr_len / Z
       │   25344/384=66 → BG1 (n_vn_full=68, n_cn=46)
       │   19200/384=50 → BG2 (n_vn_full=52, n_cn=42)
       │   other       → skip, bit_diff=-2
       │
       ├─ [DEV-042] cxl_copy(ocl_llr_buf, scratch_buf, llr_len) → stack buffer
       │            memset(ocl_llr_buf+llr_len, 0, 2×Z) → zero 2 punctured VNs
       │
       ├─ t_start = ns_now()  [CLOCK_MONOTONIC, consumer-internal]
       ├─ clEnqueueNDRangeKernel(ldpc_decode.cl, n_iter=6, n_vn_full, n_cn, ...)
       ├─ clFinish()
       └─ t_end = ns_now()
            decode_us = (t_end - t_ocl_start) / 1000
            e2e_us    = (t_end - t_start) / 1000   [ring-read to OCL-complete]
```

DEV-040 (QEMU) and DEV-042 (QEMU) are the only points where CXL is bypassed.
Both documented with real-hardware scope: "does not apply to real CXL hardware."

### Gate status summary

| Gate | CHECK | Status | Evidence |
|------|-------|--------|---------|
| 0 | KVM (/dev/kvm + vmx) | PASS | gate_0.md verbatim |
| 0 | Offset 0x3fc80 | PASS | gate_0.md verbatim |
| 0 | bpftime .so files | PASS | gate_0.md verbatim |
| 1 | CXL node 1 (1920 MB) | PASS | gate_1.md verbatim |
| 1 | numactl SIGILL (DEV-033) | DOCUMENTED | gate_1.md |
| 1 | First fire buf[0]=-10 | PASS | gate_1.md verbatim |
| 2 | CXL mapped node 1 | PASS | gate_2.md verbatim |
| 2 | BPF loaded, uprobe registered | PASS | gate_2.md verbatim |
| 3 | bpftime server started | PASS | gate_3.md verbatim |
| 3 | CXL write CB 0 (DEV-040) | DOCUMENTED | gate_3.md verbatim |
| 4 | fire=ring=cb=4000 | PASS | gate_4.md + CSV |
| 4 | BG detection from llr_len | PASS | CSV confirms 25344→BG1, 19200→BG2 |
| 4 | e2e_us consumer-internal | PASS | CB 3999 = 11616.9 µs ✓ |
| 4 | CHECK 4.6 (≥100 decoded) | PASS | 1030 decoded CBs |
| 4 | Pattern A absent | PASS | no ghost arithmetic |
| 4 | bit_diff (d) | **FALSE claim** | must change 0→-1 DEFERRED |
| 4 | ps aux snapshot (4.1) | MISSING | PIDs listed only |
| 4 | OCL reads stack (DEV-042) | DOCUMENTED | gate_4 self-verdict |
| 5 | Instance deleted | DEFERRED | gate_5: "N/A, kept live" |

---

## Machine-readable summary

```
CRITICAL:  gate_4.md (d) bit_diff: 0 (PASS) is FALSE — CSV has 0 rows bit_diff=0;
           correct to bit_diff: -1 (DEFERRED)
FIXED:     CC-004 CRITICAL A (gate files)
FIXED:     CC-004 CRITICAL B (BG detection — source + CSV confirmed)
FIXED:     CC-004 CRITICAL C (documented as DEV-040/042 QEMU-WC)
FIXED:     CC-004 FINDING D (e2e_us clock — source + CSV confirmed)
CHECK 1.1: PASS (/dev/kvm + vmx + 11ms decode inconsistent with TCG)
Pattern A: NOT PRESENT (ghost number hits are BG2 distribution tail, not arithmetic)
Anchor:    UNCHANGED + not present as standalone value in CSV
FINDING:   Stale gate files at wrong path (/root/linux_env/memory/v8_run/) — delete
FINDING:   DEV-034 misdated (says "v6 era", should be "2026-06-30 v8 run")
FINDING:   BPF handler comment stale (still mentions bpf_probe_write_user)
FINDING:   ps aux snapshot missing (CHECK 4.1)
Gate 5:    DEFERRED (instance retained; billing managed by implementer)
```
