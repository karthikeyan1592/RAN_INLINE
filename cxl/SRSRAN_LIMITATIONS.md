# srsRAN Limitations — Consolidated Notes

**Date:** 2026-07-19 · **Scope:** everything under `/root/linux_env/cxl` that touches srsRAN —
[`cxl_ran_poc/`](cxl_ran_poc/) (lookaside LDPC PoC, hits these live) and [`open_inline/`](open_inline/)
(GPU-resident PHY, hits these in the feasibility study). Consolidates scattered findings from run
ledgers into one place so they don't get re-discovered.

**Local checkout:** `third_party/srsRAN_Project` only. **No `srsRAN_4G`/`srsLTE`/`srsUE` checkout
exists anywhere in this tree** — relevant to §3 below.

---

## 1. `ldpc_decoder_benchmark` has no ground-truth LLR (cxl_ran_poc, DEV-044)

**Source:** [`cxl_ran_poc/memory/v8_run/implementer/DEVIATIONS.md`](memory/v8_run/implementer/DEVIATIONS.md) DEV-044.

srsRAN's own `ldpc_decoder_benchmark`, run in its default mode (no `-C` flag → `use_crc=false`),
feeds the decoder **purely random LLR** (`(rgen() & 1) * 20 - 10`) — not LLR derived from a real
encoded message. Any uprobe/hook on `ldpc_decoder_impl::decode()` during a live benchmark run sees
statistically-plausible-looking but *ground-truthless* data: there is no original bit sequence to
diff the decoder's output against.

**What we did about it:** built a separate offline oracle (`bit_diff_test.cpp`, using srsRAN's own
encoder) and confirmed the identical kernel is bit-exact there (0/10,240,000 mismatches). This
proves the **kernel** is correct; it does not prove correctness on the **live-captured** codeblocks
(labeled `bit_diff=-1 DEFERRED`, honestly, in the run — this is a benchmark-design property, not
a bug).

**Closing this properly needs one of:**
- `-C` flag (real encode+CRC mode) plus a second uprobe capturing pre-encode message bits as ground
  truth, or
- an external, protocol-real traffic generator that produces genuinely encoded uplink transport
  blocks the harness can also observe pre-encode — this is exactly what §3 (ZMQ+srsUE) could
  provide, with caveats.

## 2. Benchmark's corner configs look like garbage until you read the source (DEV-043)

**Source:** same DEVIATIONS.md, DEV-043.

`ldpc_decoder_benchmark` exercises a deliberate `min_cb_length_bg` case (`n_vn_eff=24` for BG1,
`n_vn_eff=12` for BG2 — heavily punctured, zero parity bits transmitted). Our first BG-detection
logic didn't recognize these and silently skipped 2970/4000 CBs (74%) as "unsupported." Fixed by
reading `ldpc_decoder_benchmark.cpp:127-141` and extending BG detection to accept both the
min-length and max-length configs per base graph. **Lesson:** don't assume unrecognized benchmark
output shapes are bugs in the benchmark — read the harness source before discarding data.

## 3. `srsRAN_Project` ships no UE — `srsUE` lives in the separate, older `srsRAN_4G` repo

**What's accurate in the pasted ZMQ suggestion:** `ENABLE_ZEROMQ` is a real CMake option
(confirmed: `third_party/srsRAN_Project/CMakeLists.txt:76`, default `OFF`); `lib/radio/zmq/` is a
real, built-out RF backend in our checkout; the general mechanism (ZMQ carries baseband IQ between
two processes, forcing the gNB's real PUSCH chain — including `ldpc_decoder_impl::decode()` — to
run on genuinely encoded uplink data) is a real and commonly used srsRAN testing pattern, confirmed
against public docs and CI fixtures (`.gitlab/ci/e2e/retina_request_zmq_srsue.yml` exists in our
checkout, confirming srsRAN Project's own CI does exactly this pairing).

**What's misleading:** the suggestion says "recompile srsRAN with ZMQ enabled" as if `srsUE` were
part of the same build. It is not. `srsUE` (and the ZMQ-based 5G-SA UE stack) ships in the separate,
older **`srsRAN_4G`** repository — a different codebase from `srsRAN_Project` with its own build.
An E2E loopback needs **three** components: `srsRAN_Project` gNB (have it), Open5GS (not checked
out here either), and `srsRAN_4G`'s `srsUE` (**not present in this tree at all**). This is a
materially bigger integration lift than "flip one flag."

**Relevance per project:**

| Project | Would ZMQ+srsUE help? | Caveat |
|---|---|---|
| `cxl_ran_poc` (lookaside LDPC) | **Yes, meaningfully** — real encoded uplink TBs through the real PHY chain would close most of DEV-044's gap (genuinely non-random LLR reaching the hooked `decode()` call). Still need a capture point for pre-encode message bits to get a true bit-level diff; app-level (iperf3 payload survives end-to-end) is a weaker CRC-only check. | Needs building/integrating `srsRAN_4G` (new dependency); **QEMU/TCG risk carries over from DEV-039/DEV-041**: ZMQ RF is a real-time interface — the gNB must process each slot within its wall-clock deadline or the link stalls. Our CXL PoC VM already hit ~23µs/byte TCG device-emulation slow-path costs (DEV-039) and a 30ms/call `waitpid` cost under TCG (DEV-041); ZMQ's real-time pacing is a similar class of risk inside the same QEMU-TCG environment used for CXL emulation. Untested — would need its own day-1 probe before relying on it. |
| `open_inline` (GPU PHY, split 7.2x fronthaul) | **No, wrong layer.** ZMQ carries time-domain baseband IQ directly between an integrated DU+RU gNB process and a UE process — there is no O-RAN split, no eCPRI, no NIC in the loop, and the split-7.2x RE-grid boundary (what we need to tap/inject) isn't exposed on the wire in this mode, only as an internal function call inside the gNB binary. This doesn't touch the M1 ingest mechanism or the fronthaul protocol at all. | Already correctly scoped out in [`open_inline/research/phase1_feasibility_cloud_hw.md`](open_inline/research/phase1_feasibility_cloud_hw.md) §2.3 — `ru_emulator` (DPDK, real eCPRI) + gNB test mode remain the right tool for that project; ZMQ is a different, unrelated srsRAN testing mode, not a stronger version of it. |

## 4. `open_inline`'s own findings (already in the feasibility study, cross-referenced here)

**Source:** [`open_inline/research/phase1_feasibility_cloud_hw.md`](open_inline/research/phase1_feasibility_cloud_hw.md) §2.3, risk register #6–#7.

- No open tool gives a **live, protocol-attached UE through eCPRI without RF hardware** — cloud
  end-to-end is necessarily protocol-real/data-synthetic. A live UE attach needs SDRs (out of cloud
  scope, M6 stretch).
- `ru_emulator` sends **static UL IQ by default** — the oracle-grid injection patch
  (feature `p3-live-tap-ul-inject`) is required, not optional, to get bit-exact ground truth on the
  live path. This is the same *class* of problem as DEV-044 above (benchmark/emulator defaults to
  synthetic data; real ground truth has to be engineered in deliberately) — worth remembering as a
  recurring srsRAN-ecosystem pattern: **default test/bench modes are convenience stubs, not
  ground-truth generators; always check what's actually driving the LLR/IQ before trusting a
  "live" capture.**
- gNB test mode emulates UEs at **MAC level only** — real L2/L3/core behavior, but no real PHY-layer
  UE signal ever exists in that mode either.

---

## 5. The recurring pattern (why this file exists)

Every srsRAN limitation found so far is the same shape: **a component that looks like it's carrying
"real" data by default is actually carrying a synthetic/random/static stand-in**, and the fix is
always to either (a) find the flag/patch that switches it to real encoded/generated data, or (b)
build an external oracle and accept a documented, honest gap. Before trusting any new srsRAN
test/bench/emulator surface as a ground-truth source, check its default data path first.

## References
- [`cxl_ran_poc/GAPS.md`](cxl_ran_poc/GAPS.md) GAP-14 (historical — no integration attempted yet)
- [`cxl_ran_poc/memory/v8_run/implementer/DEVIATIONS.md`](memory/v8_run/implementer/DEVIATIONS.md) DEV-039, DEV-041, DEV-043, DEV-044
- [`cxl_ran_poc/memory/v6_run/implementer/DEVIATIONS.md`](memory/v6_run/implementer/DEVIATIONS.md) DEV-030, DEV-032
- [`open_inline/research/phase1_feasibility_cloud_hw.md`](open_inline/research/phase1_feasibility_cloud_hw.md) §2.3, §5
- srsRAN Project ZMQ radio backend: `third_party/srsRAN_Project/lib/radio/zmq/`, `CMakeLists.txt:76`
- srsRAN docs — gNB+srsUE tutorial: docs.srsran.com/projects/project/en/latest/tutorials/source/srsUE
- srsRAN 4G docs — 5G SA E2E w/ ZMQ: docs.srsran.com/projects/4g/en/latest/app_notes/source/5g_sa_E2E
