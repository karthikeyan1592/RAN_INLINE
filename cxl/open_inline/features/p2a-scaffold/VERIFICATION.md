# p2a-scaffold — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22), same convention as `p0-rig-scaffold/README.md`.

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| Abstraction header | `src/kernels/oi_kernel_compat.h` | Empty per MVP; exists as the one lint-exempt location | — |
| Provenance schema + checker | `src/host/oi_p2_provenance.h`, `helpers/provenance_check.py` | Run against the real (currently kernel-less) p2* tree | PASS, 0 kernel files found (correct — none exist yet) |
| Portability lint | `helpers/lint_portability.py` | Run against the real tree | PASS, 1 file scanned (the stub `.cl`), no banned patterns |
| AGPL denylist | `helpers/agpl_denylist.py` | Run against the **real repo root** (`third_party/srsRAN_Project` genuinely present on disk) + a planted-violation negative test | PASS on real repo; correctly FAILs on a planted `pusch_decoder_test_data.h` outside `third_party/` |
| No-perf lint | `helpers/lint_no_perf.sh` | Run against the real tree + a planted-violation negative test | PASS clean; correctly FAILs on a planted `latency_threshold` assert — **caught and fixed a real `find`-operator-precedence bug in the process** (see below) |
| Config validator | `src/host/oi_p2_config.{h,cpp}` | `tests/config_test.cpp`, compiled and run for real: exact MVP YAML, 8 single-field perturbations, missing-required-path case, optional-field-absent case | **20/20 real assertions pass** |
| Buffer pool | `src/host/oi_p2_buffers.{h,cpp}` | Compiles clean (`-Wall -Wextra`, zero warnings); exercised transitively by the host API test | — |
| `oi_frame_desc` | `src/host/oi_frame_desc.h` | `static_assert(sizeof == 32)`, compiled and run | Confirmed exactly 32 bytes |
| `oi_p2_tb_record` | `src/host/oi_p2_tb_record.h` | `static_assert(sizeof(header) == 16)`, compiled | Confirmed |
| Host orchestration API | `src/host/oi_p2_host.{h,cpp}` | `tests/host_api_test.cpp`: real OpenCL context on PoCL, real 7-stage stub kernel chain with real `cl_event` dependencies, real readback | **20/20 real assertions pass**, including proof the in-order event chain genuinely serializes (stage 6 provably overwrites stage 5's output on the *same* buffer, not a race) |

## Real bugs found and fixed during this pass

1. **`lint_no_perf.sh`: `find` operator-precedence bug.** `-path A -o -path B -o -path C -type f ...`
   only attaches `-type f`/the name filters/`-print0` to the *last* alternative in GNU find's
   expression grammar — `src/` and `tests/` were silently never scanned, only `helpers/`. Fixed by
   wrapping the path alternatives in their own `\( ... \)` group. Caught by planting a real
   violation in `src/` and confirming the *unfixed* script missed it before fixing.
2. **`oi_p2_host.h` claimed C-compatibility it didn't have.** Used `<cstddef>`/`<cstdint>` (C++-only
   headers) despite the doc comment stating "OpenCL-host-callable from C or C++." Fixed to
   `<stddef.h>`/`<stdint.h>`; verified by compiling the header under both `gcc -std=c11` and
   `g++ -std=c++17`.
3. **Forward-reference bug in the same header** — `oi_p2_tb_record_c` was used in a function
   signature before its own typedef appeared later in the file (compiles in some contexts by luck
   of C++ two-phase lookup, but not reliably, and not in C at all). Fixed by moving the typedef
   before its first use.

## Design gap — RESOLVED (2026-07-22, same session)

**Original finding:** `oi_p2_feed`'s LLD-specified signature was `(pipeline, slot_id, arena_offset,
len)` — three scalars, no room for a caller to pass in already-parsed O-RAN header fields. This
conflicted with `p3-live-tap-ul-inject`'s reconciliation (an earlier session), which put the parse
in p3's ingest wrapper *before* calling `feed()` — implying feed() receives pre-derived data it had
no parameters to accept.

**Resolution (user-directed, three independent reasons, all confirmed correct):**
1. **The parse-inside-feed position was internally circular.** `slot_id` is derived from watching
   `symbol_id` wrap 13→0 (LLD §4.1) — you cannot know `slot_id` without having already parsed the
   header. p2a's original placeholder only avoided this by fabricating `symbol_id` instead of
   parsing; the moment real parsing landed, `feed()` would need `slot_id` before it could
   legitimately derive one.
2. **Parse-inside-feed cannot work for PHYSICAL.** p6's dmabuf ingest lands frame bytes in
   GPU-visible memory the host cannot cheaply read from inside the p2 pipeline — parsing has to be
   an ingest_backend capability, not a p2-internal one.
3. **Error accounting (`parse_failed`) already lives in p3's ingest wrapper**, per its
   already-approved spec — moving parsing into p2 would have forced reopening both p3's and p6's
   specs to relocate that counter.

**Fix applied:**
- `oi_p2_feed`'s signature changed to `oi_p2_status oi_p2_feed(oi_p2_pipeline*, const oi_frame_desc* desc)`
  — caller supplies a fully-populated descriptor; `feed()` only validates ring capacity and copies
  it in. `oi_frame_desc` itself moved from a C++-namespaced type to a plain C typedef (it now
  crosses the C/C++ boundary via this signature, and the header needed genuine C-compatibility,
  not just a claim of it — see bug #2 below).
- New shared helper `oi_oran_preparse.h/.cpp` (`oi_oran_preparse_frame`, `oi_oran_preparse_state`):
  owns the symbol-wrap slot_id derivation state machine (fully real, fully tested — 15/15
  assertions in `tests/preparse_test.cpp`, including multi-cycle wrap advancement, truncated/
  malformed-frame rejection, and per-stream state independence) plus a clearly-labeled placeholder
  byte-layout parse (Q2 still open — not yet confirmed against real wire bytes). Called by ingest
  backends (p3; p6 not yet, separately flagged), never by `feed()`.
- Rebuilt and reran `host_api_test.cpp` against the new signature: **20/20 assertions still pass**,
  unchanged pipeline behavior, only the feed-call site updated to build a descriptor directly.
- Docs updated: `p2-phy-kernels` LLD §2 (signature + rationale), §4.1 (field-ownership note,
  retitled "filled by ingest_backend"), §1 module breakdown (added `oi_frame_desc.h`,
  `oi_oran_preparse.h/.cpp`); `p3-live-tap-ul-inject` LLD (M3 now calls the shared helper instead of
  parsing bespoke, corrected an overclaim about p6 already doing the same — p6 does not yet, that's
  a separate, still-open item, not resolved by this fix).

**Timing note:** done before any other slice (p2b onward) links against `feed()`, and before p3/p6
implementations exist — exactly the cheapest point to change a signature P2-R17 promises stability
for going forward.

## Cross-slice updates driven by p2c-k1 (2026-07-22, same day)

Implementing K1 (the depacketizer) required resolving Q2 for real, which meant touching two files
this feature owns. Recorded here since the edits happened in p2c-k1's working session, not this
one — full detail in `../p2c-k1/VERIFICATION.md`.

- **`src/host/oi_oran_preparse.cpp` rewritten**: the byte-aligned placeholder layout (explicitly
  flagged as such when this file was first written) is replaced with the real bit-packed eCPRI +
  O-RAN CUS layout, derived from reading OCUDU's actual decode logic and cross-validated against
  the real OCUDU encoder+decoder in `../p2c-k1/tests/k1_test.cpp`. `tests/preparse_test.cpp`'s
  frame-builder was updated to match; **all 15 assertions still pass**.
- **New file `src/host/oi_oran_wire_layout.h`**: the shared byte-offset constants both
  `oi_oran_preparse.cpp` and `../p2c-k1/src/kernels/k1_depacketizer.cl` use, so the two don't carry
  independently-drifting copies of the same numbers.
- **`src/host/oi_frame_desc.h` fixed for OpenCL C compatibility**: used `<stdint.h>` types, which
  don't exist in OpenCL C's freestanding device dialect. Latent since no kernel had `#include`d
  this header before K1 (K5/K6 only use the empty `oi_kernel_compat.h`). Fixed with an
  `__OPENCL_C_VERSION__`-gated typedef layer selecting device-native types
  (`uchar`/`ushort`/`uint`/`ulong`) vs `<stdint.h>` types. No layout/size change — `oi_frame_desc`
  is still exactly 32 bytes on both sides.
- **Newly flagged, not fixed**: `oi_frame_desc::section_id` is `uint8_t`, but the real wire field
  is 12 bits (0-4095) — a latent truncation risk, harmless under the MVP's single-section-per-frame
  scope. Same class of decision as the `oi_p2_feed` signature change above (changes this struct's
  frozen 32-byte layout) — flagged for explicit reconciliation, not silently patched.

## VLAN handling: `eth_hdr_len` added, real 802.1Q detection (2026-07-24)

**Trigger**: implementing `p1-ran-baseline`'s `ru_emu.yml` grounding found `ru_emulator_cli11_schema.cpp`'s
`--vlan_tag` option is `CLI::Range(1, 65536)` — there is no untagged value, so the real fronthaul
wire is expected to always carry an 802.1Q tag. `oi_oran_wire_layout.h`'s Ethernet-layer assumption
(hardcoded 14-byte header, "no VLAN") had been a flagged, unconfirmed placeholder since p2c-k1; this
finding meant the placeholder's default guess was very likely the wrong one, and — more importantly
— the code had never been written to handle the other case at all. Rather than flip the placeholder
to the opposite guess (still a guess, just a different one) or wait for a live GCP capture to settle
it, the fix makes the code handle both cases, so the eventual capture confirms a branch instead of
being a hard dependency.

**Fix**:
- `oi_frame_desc` (frozen 32-byte layout, LLD §4.1) gains `uint8_t eth_hdr_len` (14 or 18), taken
  from 1 of the 8 `reserved` bytes — exactly the use that field's own comment already anticipated
  ("future eAxC/VLAN/BFP fields land here"). `reserved` shrinks to `[7]`; struct size unchanged
  (verified: `static_assert(sizeof(oi_frame_desc) == 32)` still holds, compiles clean).
- `oi_oran_wire_layout.h`'s `OI_WIRE_OFF_*`/`OI_WIRE_TOTAL_HEADER_BYTES` macros are now
  parameterized on `eth_len` instead of hardcoding `OI_WIRE_ETH_HEADER_BYTES` — every offset past
  the Ethernet header shifts correctly for either header length. `OI_WIRE_ETH_HEADER_BYTES` itself
  is kept as a backward-compatible alias for the untagged value (14), so call sites that only ever
  build untagged frames (p2f's `oracle_tx_gen.cpp`, most of `k1_test.cpp`) needed no changes.
- `oi_oran_preparse_frame` now detects the tag for real: reads the EtherType at byte 12; if it's
  the 802.1Q TPID (`0x8100`), reads the real EtherType 4 bytes further out (byte 16) instead.
  Rejects (`ERR_MALFORMED`) only if the FINAL EtherType isn't `0xAEFE` — a frame with a bogus tag
  or a bogus untagged EtherType is malformed either way, not "assumed fine." VLAN ID itself is
  **not** validated (any TCI is accepted) — VID filtering is a switch/NIC-level concern below this
  function's scope, not something K1 or its ingest path enforces.
- One parser decides, the kernel obeys: K1's kernel (`p2c-k1/src/kernels/k1_depacketizer.cl`) does
  **not** re-detect the tag — it reads `desc.eth_hdr_len` (set by preparse) and uses
  `OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len)` for both its bounds check and its IQ-payload
  offset. Same single-source-of-truth reasoning as the `oi_p2_feed` reconciliation above: the two
  code paths (host preparse, device kernel) can never disagree about where the payload starts,
  because only one of them ever makes that decision.

**Verified**: `tests/preparse_test.cpp` extended with 6 new assertions (untagged/tagged parse,
tagged frame's fields parsing correctly past the shifted offset, a non-default VLAN ID still
parsing, a tagged-looking frame with a wrong post-tag EtherType correctly rejected, and a
frame truncated exactly at the VLAN boundary correctly rejected without an out-of-bounds read) —
**23/23 assertions pass**. `../p2c-k1/tests/k1_test.cpp` extended with a full VLAN-tagged
round-trip case (real OCUDU-encoded payload wrapped in a real 802.1Q header, decoded via the real
OCUDU decoder as ground truth, run through the actual PoCL kernel) — bit-exact RE-grid match,
proving K1 genuinely used the dynamic offset rather than a hardcoded 14. Full p2 re-verification
sweep (every p2a-p2f test suite + `lint_portability`/`provenance_check`/`lint_no_perf`) re-run
clean after this change — see `../p2c-k1/VERIFICATION.md` for that slice's own record.

## Stub chain stage count: 7 -> 8 (2026-07-22, driven by p2d-k2-k3)

`oi_p2_host.cpp`'s `oi_p2_launch_slot` stub kernel chain grew from 7 to 8 stages: p2d-k2-k3 split
K2 into K2a (per-DMRS-symbol FD estimate) + K2b (cross-symbol time-combine) — see
`../p2d-k2-k3/VERIFICATION.md` for why (K2's committed single-launch prototype couldn't produce a
time-interpolated estimate on its own; K1's own citation-checking discipline caught a wrong port
citation during that same decision, worth noting since it's what made the split's justification
solid rather than assumed). This is an internal implementation-detail edit, not an ABI change: no
other slice's spec references the stage *count* as a contract (p3/p6 build against `oi_p2_feed`'s
signature, not this stub's internals), and the host API's whole purpose is to let real kernels
plug into this chain as sub-features land — unlike the `oi_p2_feed` signature amendment, this
needed no cross-slice negotiation, just an array-length edit + updating two stage-marker
assertions in `tests/host_api_test.cpp` (`crc24a_ok`'s "every byte == 7" -> "== 8", and I6 cb_llr's
tap-marker check "6" -> "7", since the K5/K6 pair shifted from stages 5/6 to 6/7). **Rebuilt and
reran `host_api_test.cpp`: all 20 assertions still pass** with the 8-stage chain.
