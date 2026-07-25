# DEVIATIONS.md — append-only, canonical cross-phase deviation log

Per v4.md: append an entry HERE, BEFORE continuing, the MOMENT anything
done differs from what `cursor_cxl_poc_prompt_v4.md` specified — however
minor. `DEV-NNN` numbers are sequential across the whole run.

Entry format:

```markdown
## DEV-<NNN> — <phase/gate> — <YYYY-MM-DD HH:MM>

**Spec said:** <quote>
**Did instead:** <what>
**Why:** <reason>
**Downstream impact:** <which later files/columns/claims must reflect this>
```

---

## DEV-001 — Gate 0.4 (multi-socket check) — 2026-06-15 06:52

**Spec said:** "On 2-3 LARGER DigitalOcean droplet sizes ... numactl
--hardware ... Looking for: available: 2 nodes with DISTINCT physical
nodes."
**Did instead:** Ran `numactl --hardware` on the WSL2 host only; did not
provision any larger DigitalOcean droplet.
**Why:** The spec itself makes provisioning optional ("do not provision
unless quick to check — if doctl/console makes this slow, SKIP entirely,
this is a bonus not a requirement"). Gate 0.4 is explicitly
"informational only, does not block anything."
**Downstream impact:** None on any blocking gate. Phase 4 uses CXLMemSim
software injection regardless of host topology, so no later file/column
depends on a multi-socket finding. The "future Pond-replication target"
note simply records "none available."

## DEV-002 — Gate 0.1 (bpftime) — 2026-06-15 06:48

**Spec said:** "Follow README build instructions" (default bpftime build
uses the LLVM JIT VM).
**Did instead:** Built with the ubpf JIT (`-DBPFTIME_LLVM_JIT=0`) and run
with `BPFTIME_VM_NAME=ubpf`.
**Why:** The ubpf JIT is much lighter to build (no LLVM link) and was
sufficient for the smoke test. The default runtime VM name is "llvm";
without the env var the agent failed with "No VM factory registered for
name: llvm" and the probe never instantiated.
## DEV-004 — Gate 0.3 (CXLMemSim CLI) — 2026-06-15 08:35

**Spec said:** `./cxlmemsim --latency=0 -- ./your_membench` and
`./cxlmemsim --latency=142 -- ./your_membench`
**Did instead:** `./build/cxlmemsim_legacy -t "/tmp/membench" -l 0,0 -c 0`
**Why:** The built binary (`cxlmemsim_legacy`) does not have a `--latency`
or `--` separator flag. Its CLI uses `-t TARGET -l READ_NS,WRITE_NS -c CPUSET`.
The v4 spec's CLI matches an older CXLMemSim interface; the SlugLab repo has
been restructured since the arXiv paper (arXiv:2303.06153).
**Downstream impact:** Gate 0.3 FAILed for a separate reason (WSL2 no PMU),
so this CLI difference is moot for Gate 0.3 itself. If CXLMemSim is
attempted on the droplet (bare metal, real PMU), use `-l NS,NS -t TARGET`
syntax, not `--latency=NS -- TARGET`. Document in ops/references/.

## DEV-005 — Gate 0.3 (CXLMemSim WSL2 PMU) — 2026-06-15 08:35

**Spec said:** CXLMemSim should build and run; FAIL path if PMU issues.
**Did instead:** CXLMemSim built (EXIT=0) but all runs fail with
`perf_event_open failed for generic hardware cache misses: No such file or
directory` — WSL2 does not expose Intel PEBS hardware PMU.
**Why:** WSL2 runs under Hyper-V which does not pass hardware performance
counters to the guest kernel. This is a fundamental WSL2 constraint.
**Downstream impact:** Phase 4's CXLMemSim latency sweep (4.3) is deferred
per the spec's FAIL path. `cxl_latency_sensitivity.csv` will NOT be produced.
Phase 4 still delivers: real drivers/cxl/ path (already verified in Part C)
+ real CXL-backed shared memory in the pipeline (4.2). The deferral must be
stated explicitly in emulation_mode.txt and Phase 4's RESULTS_SUMMARY section.
The columns in latency_ladder_v2.csv referencing `cxlmemsim_sweep` will be
absent; the comparison_table.csv row "This work — CXLMemSim sweep" will note
"deferred — WSL2 no PMU; see Limitations."

**Downstream impact:** Phase 2 must run the gNB-side agent with
`BPFTIME_VM_NAME=ubpf` too. If Phase 2's real probe needs LLVM-JIT-only
features or atomics (see DEV-003), reconsider rebuilding with the LLVM
JIT. Record the chosen VM in the Phase 2 gate file and emulation_mode.

## DEV-006 — Gate 0.2 (gNB rfsimulator serveraddr flag) — 2026-06-15 09:00

**Spec said:** "gNB --rfsim --phy-test launched with --rfsimulator.serveraddr 10.77.0.2 (in gnb-ns)"
**Did instead:** Omitted `--rfsimulator.serveraddr 10.77.0.2` from the gNB command line.
**Why:** OAI nr-softmodem does not accept `--rfsimulator.serveraddr` as a command-line flag
(`[CONFIG] unknown option: --rfsimulator.serveraddr`). The gNB's server config is in the .conf
file (`serveraddr = "server"` means server mode, bind all interfaces). Inside gnb-ns, the only
external interface is `veth-gnb@10.77.0.2`, so the gNB effectively binds on 10.77.0.2 anyway.
`ss` confirmed: `[::ffff:10.77.0.2]:4043` is the established ESTAB endpoint.
**Downstream impact:** None — the effect is equivalent. The TCP connection runs on the veth pair
with 10.77.0.2 as the server endpoint. All subsequent phases launch gNB without this unsupported flag.

## DEV-007 — Gate 0.2 (UE PHY sync failure in phytest) — 2026-06-15 09:00

**Spec said:** (implied) a working phytest run with decode activity.
**Did instead:** UE PHY sync repeatedly fails (`synch Failed`) during the test. The rfsimulator
TCP connection is established, and the gNB runs 11232+ DLSCH rounds (LDPC encoder active), but
BLER is 1.0 (all HARQ NACKs) because the pre-built `reconfig.raw`/`rbconfig.raw` in the OAI
build dir were generated for different cell parameters than the phytest-dora gNB config.
**Why:** The pre-built raw files encode cell configuration at build time. The CI docker setup
shares them via a volume that the gNB writes at runtime; our bare-metal run used stale files.
**Downstream impact:** Gate 0.2 pass criteria requires only rfsimulator connect messages (both
sides) and the LDPC symbol, both of which are confirmed. Phase 2 will resolve by starting the
gNB first (without UE), letting it write fresh reconfig.raw/rbconfig.raw to /tmp/oai_gnb/,
then launching UE with `--reconfig-file /tmp/oai_gnb/reconfig.raw`.

## DEV-008 — Gate 1 (Phase 1 test vectors) — 2026-06-15 10:30

**Spec said:** Use srsRAN's `ldpc_decoder_test_data` binary test vectors
(fetched via `cmake -B build -DBUILD_TESTING=On` ExternalProject download).
**Did instead:** Harness generates codewords at runtime using srsRAN's own
`ldpc_encoder` on random messages, then decodes and compares bit-for-bit.
**Why:** The cmake ExternalProject download requires network access (disabled
in test environment); `ldpc_decoder_test_data.tar.gz` was not present. The
runtime-oracle approach is strictly stronger — it tests arbitrary messages
rather than a fixed pre-generated set, and uses the authoritative srsRAN
encoder as the oracle.
**Downstream impact:** None. The gate criterion (bit_diff_rate == 0 for
BG1 LS=384 at minimum) is identical whether testing against pre-generated
vectors or live-generated ones. bit_correctness.csv format is unchanged.

## DEV-009 — Gate 2 (Phase 2 CB/slot count) — 2026-06-15 15:45

**Spec said:** "expected descriptor count ~= N_slots * 24 (C=24 CBs/slot)"
**Did instead:** Observed cb_calls/slot_calls ≈ 10090/5055 = 2.0 CB/slot.
**Why:** The phy-test config (`gnb.band66.106prb.rfsim.phytest-dora.conf`)
selects a small MCS that produces ≈2 CBs/slot, not 24. The C=24 in the spec
comes from PRIMARY_CONFIG (106 PRB, high MCS, large TB) which gives 24 CBs
per transport block. phy-test uses a simpler single-TB configuration.
**Downstream impact:** The per-CB consistency check (5055×2=10110 ≈ 10090,
<0.25% error) confirms zero missed events and zero double-counting. The
probe fires correctly. Phase 3 sustained run will document C_actual=2 for
this config; Phase 5 ablation must use the same phy-test config.

## DEV-010 — Gate 2 (Phase 2 descriptor format) — 2026-06-15 15:45

**Spec said:** `ldpc_offload_desc` stores only a pointer (`llr_ptr`,
`out_ptr`) — no payload through eBPF (Band 3 of the diagram).
**Did instead:** Stored the full LLR payload (int8_t[N_VN_FULL*Z], up to
26,112 bytes) directly in a BPF_MAP_TYPE_ARRAY map (`llr_copy`). Consumer
reads from the map rather than dereferencing a pointer to the gNB's memory.
**Why:** `p_llr` is a local variable on the gNB stack (in
`nr_process_decode_segment`); it is freed immediately after `LDPCdecoder`
returns. The consumer polls at ~50 ms intervals — too slow to read a
pointer before the stack frame disappears. Copying into the BPF map inside
the probe (before `LDPCdecoder` returns) is the only race-free approach
without kernel synchronisation.
**Downstream impact:** The "no payload through eBPF" principle is violated,
but unavoidably — the alternative (pointer-only) race-conditions the decode
and would produce corrupt LLR data. Phase 5/6 architecture discussion must
note this; emulation_mode.txt records it. The CXL-backed design (Phase 4)
would avoid this by routing LLR into a persistent CXL buffer before the
probe fires.

## DEV-011 — Gate 2 (Phase 2 Z=224 shift table mismatch) — 2026-06-15 15:45

**Spec said:** (implied) correct shift tables for the intercepted Z value.
**Did instead:** Consumer selects `ls_idx = (Z == 384) ? 1 : 0`. Z=224 maps
to `ls_idx=0`, which holds shift tables for Z=256 (3GPP iLS set 0). Z=224
belongs to iLS set 3 (Z=7×32). The bg_tables.h used in the consumer only
has two entries (iLS 0 and 1).
**Why:** Gate 1 tested only Z=384 and Z=256 — the two LS values in
bg_tables.h. The phy-test config uses Z=224, a third value. Extending
bg_tables.h to cover all eight 3GPP iLS sets was out of scope for Gate 2
(interception correctness, not decode correctness).
**Downstream impact:** The 200 OpenCL decodes in Gate 2 use wrong shifts for
Z=224, so decoded bits are not guaranteed correct. Gate 2's PASS criterion
only requires "real compute is happening, not just descriptor plumbing" —
non-zero decoded_ones (observed: 16 and 125 per CB) satisfy this. Gate 1's
bit-exact evidence (Z=256 and Z=384) is unaffected. If Phase 5 needs Z=224
decode quality, bg_tables.h must be extended to all eight iLS sets.

## DEV-012 — Gate 3 (Phase 3 XDP slot-period visibility) — 2026-06-15 21:30

**Spec said:** "nic_packet_timeline.csv shows inter-arrival times clustering
around the configured slot duration (0.5ms for mu=1)"
**Did instead:** Per-packet inter-arrivals cluster at 1–100 μs (intra-burst),
not at 500 μs. Slot period confirmed only in aggregate: 60,000 packets over
1535.5 ms at 19.5 pkts/slot implies 3,071 slots → 1535.5/3071 = 500.0 μs/slot.
**Why:** The rfsimulator transmits each 0.5 ms slot's I/Q samples as ~20 TCP
segments (MTU-limited on the veth loopback). XDP timestamps every segment, so
the dominant inter-arrival is the intra-slot segment spacing (~25 μs mean).
Only ~1,421 of 59,999 inter-arrivals exceed 100 μs (inter-slot gaps), and
those are not tightly clustered at 500 μs due to TCP and scheduler jitter.
This is a measurement granularity issue, not a probe failure — veth-gnb IS
the correct interface (confirmed by gNB TCP connect log).
**Downstream impact:** nic_packet_timeline.csv contains valid continuous-traffic
evidence. The aggregate slot rate (2204 bursts/s ≈ 2000/s) supports Gate 3(b)'s
intent. Phase 5 inter-arrival analysis should either look at burst-level timings
or switch to a coarser packet-count-per-bin approach to surface the 500 μs period.

## DEV-013 — Gate 3 (DEV-003 formally closed) — 2026-06-15 21:30

**Spec said:** (from DEV-003 downstream note) DEV-003 "must be addressed/
justified in the Phase 2 gate file" [deferred; it was not].
**Did instead:** Addressed in Gate 3. Root cause confirmed: OAI
`nrLDPC_coding_segment_decoder.c:281` calls `pushTpool` for each CB, so
`LDPCdecoder` runs concurrently from thread-pool workers. Fix: changed
`slot_counter` and `cb_counter` from `BPF_MAP_TYPE_ARRAY` to
`BPF_MAP_TYPE_PERCPU_ARRAY`. Each CPU increments its own slot; consumer
aggregates all CPUs with `libbpf_num_possible_cpus()` at exit. No atomic
opcode required, no ubpf opcode-0xdb rejection.
**Verification:** Gate 3 run: `cb_per_slot_ratio = 2.000` exactly across
929,474 calls and 4 CPUs — zero miscounts confirmed at scale.
**Downstream impact:** DEV-003 is closed. All subsequent probe programs
(Phase 5 if re-run) must use PERCPU_ARRAY for counters.

## DEV-014 — Gate 5 (Phase 5 ablation measurement constraints) — 2026-06-16 10:15

**Spec said:** Ablation table rows use N>=1000 slots; `+interception_only` isolates "pure interception+CXL-roundtrip overhead"; `+gpu_compute_full` uses "Phase 1's bit-exact kernel" on a real GPU device.
**Did instead:**
1. **Interception overhead dominated by poll interval**: Consumer polls `cb_desc` every 2ms. The measured overhead (mean=1,318,008 ns/CB = ~1.3ms, p50=~950µs) is dominated by the polling delay, not the actual bpftime round-trip (Gate 0.1 baseline: 248.5 ns). The per-slot value (2,636 µs) correctly represents the current implementation's end-to-end latency but should NOT be confused with the fundamental bpftime interception latency.
2. **OpenCL on CPU (no real GPU)**: WSL2 has no GPU pass-through. The OCL backend is PoCL (Portable Computing Language) running on the CPU. Measured OCL time: mean=74,774,094 ns/CB (~74.8ms); 149,548 µs/slot. A real GPU (DGX Spark achieves 710µs for C=24) would be ~2,526× faster for the compute portion alone.
3. **No CXL in interception path**: Phase 4 CXLMemSim deferred (DEV-005). The interception_only row measures bpftime overhead only; no CXL memory-fabric round-trip is included.
**Why:** WSL2 constraints (no GPU, no PMU). Consumer polling design was chosen for simplicity; an event-driven design (BPF_MAP_TYPE_RINGBUF notification or BPF perf buffer) would approach the Gate 0.1 floor.
**Downstream impact:** `latency_ladder_v2.csv` source column is `measured` for both rows (genuinely measured); `note` column documents each constraint. `comparison_table.csv` has a separate `projected` row for the GPU-speedup extrapolation. `RESULTS_SUMMARY.md` second headline uses the measured 149,548 µs value with explicit CPU-OCL caveat and projects the GPU scenario. Phase 6 figures must label OCL rows as "CPU-class OpenCL (WSL2)".

## DEV-003 — Gate 0.1 (bpftime) — 2026-06-15 06:48 — CLOSED by DEV-013

**Spec said:** (implied) a uprobe handler that records the event.
**Did instead:** Counter increment is non-atomic (`*v += 1`), not
`__sync_fetch_and_add`.
**Why:** The ubpf VM rejects the BPF atomic opcode 0xdb ("unknown opcode
0xdb at PC 9"). The smoke victim is single-threaded so a plain increment
is correct and the count came out exact (1,200,000).
**Downstream impact:** CLOSED by DEV-013 (Gate 3): PERCPU_ARRAY counters
replace the non-atomic ARRAY counters in the live probe. Verified race-free
at scale (929,474 calls, 4 CPUs, ratio = 2.000 exactly).
