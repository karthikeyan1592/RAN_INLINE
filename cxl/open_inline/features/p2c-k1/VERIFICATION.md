# p2c-k1 — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22).

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| Wire layout (Q2, eCPRI/O-RAN layers) | `../p2a-scaffold/src/host/oi_oran_wire_layout.h` | Derived from reading real OCUDU decode logic (`ecpri_packet_decoder_impl.cpp`, `ofh_uplane_message_decoder_impl.cpp`, `ofh_uplane_message_decoder_static_compression_impl.cpp`), cross-validated empirically below | Confirmed against the real OCUDU encoder+decoder round-trip |
| O-RAN pre-parse (Q2 resolution) | `../p2a-scaffold/src/host/oi_oran_preparse.cpp` (rewritten, real bit-packed layout replacing the earlier byte-aligned placeholder) | `../p2a-scaffold/tests/preparse_test.cpp` (15/15, updated frame-builder for the real layout) + this slice's `tests/k1_test.cpp` (field-by-field cross-check against the real decoder) | All pass |
| K1 kernel | `src/kernels/k1_depacketizer.cl` | `tests/k1_test.cpp`: real wire frames built with the REAL OCUDU encoder (`create_static_compr_method_ofh_user_plane_packet_builder` + `create_ecpri_packet_builder`), decoded three ways — real OCUDU decoder (ground truth), `oi_oran_preparse_frame`, and the actual kernel via PoCL — cross-checked against each other | **57/57 real assertions pass** |

## Real bugs found and fixed during this pass

1. **`oi_frame_desc.h` couldn't compile as an OpenCL C kernel header.** Used `<stdint.h>` types
   (`uint32_t` etc.), but OpenCL C kernel compilers (PoCL included) have no host libc `<stdint.h>`
   — it's a freestanding device dialect with its own native scalar types. Latent since K1 is the
   first kernel to actually `#include` this header (K5/K6 only use the empty
   `oi_kernel_compat.h`). Fixed with an `__OPENCL_C_VERSION__`-gated typedef layer
   (`oi_u8`/`oi_u16`/`oi_u32`/`oi_u64`) selecting device-native types (`uchar`/`ushort`/`uint`/
   `ulong`) vs `<stdint.h>` types depending on which compiler is processing the file.
2. **Parent LLD §4.1's IQ conversion constant was wrong: `/32768.0f` vs the real `/32767.0f`.**
   OCUDU's actual quantizer (`lib/ofh/compression/quantizer.h`) uses
   `gain = (1 << (bit_width-1)) - 1.0f = 32767.0f`, not the clean Q15 power-of-2 the LLD assumed
   (never checked against a real encoder before this slice). Confirmed by round-tripping through
   the real encoder+decoder in `tests/k1_test.cpp`. Fixed in the kernel and in the parent LLD
   (§4.1, with a correction note); also matters for consistency with K4's `RANGE_LIMIT_FLOAT`
   constants, themselves sourced from real OCUDU code calibrated against this same gain.
3. **OCUDU's real U-plane builder hardcodes `direction=downlink`**, ignoring
   `uplane_message_params::direction` entirely (`encode_data_direction()` in
   `ofh_uplane_message_builder_impl.cpp` — this builder is meant for O-DU→O-RU transmission only;
   OCUDU's own code never needs to *build* an uplink message, only decode a received one). The
   decoder used as oracle unconditionally rejects non-uplink frames. Not a bug in K1 or in
   OCUDU — a real asymmetry in OCUDU's public API surface for this specific direction. Worked
   around in the test by flipping the one known bit (byte0, bit7) after building, per the exact
   bit position read from `encode_data_direction()` — not guessed.
4. **Test comparison used exact float equality against a bf16-rounded reference, initially
   failing all 5 symbol_id cases with ~1e-5-level mismatches.** `uplane_message_decoder`'s
   `decompress()` stores its result as `cbf16_t` (bf16-rounded, 7 mantissa bits) — a real,
   already-documented divergence from K1's own design (parent LLD HLD D5: "fp32 RE grid
   internally, never OCUDU's `cbf16_t`" — precisely because bf16 is lossier). Verified by
   direct calculation (not assumed) that K1's raw fp32 value round-trips to bf16 and produces
   the exact decoder-stored value bit-for-bit — i.e., K1's fp32 output is provably correct to
   the full precision the decoder's own storage format can express, and the earlier "mismatch"
   was an artifact of comparing against the wrong precision, not a kernel bug. Fixed the test to
   compare `to_bf16(kernel_value) == decoder_stored_value` instead of raw float equality.

## Known-open item (not this pass's bug, flagged for later)

**`oi_frame_desc::section_id` truncation risk.** The real wire field is 12 bits (0-4095,
confirmed via `ofh_uplane_message_decoder_impl.cpp`), but the struct's `section_id` is `uint8_t`
(chosen before the real field width was known). MVP's single-section-per-frame scope means
`section_id` is always 0 in practice, so this doesn't bite today — but it's a real latent
truncation bug if that assumption changes. Same class of decision as the `oi_p2_feed` ABI
amendment (p2a-scaffold `VERIFICATION.md`) — flagged for explicit reconciliation, not silently
patched, since widening the field changes `oi_frame_desc`'s frozen 32-byte layout.

## Q2 status update (2026-07-24: Ethernet layer now handled, both branches)

Parent LLD Open Question Q2 ("`oi_frame_desc` byte layout... not yet cross-checked against real
wire bytes") was resolved for the eCPRI and O-RAN CUS layers on 2026-07-22 (derived from real
OCUDU decode logic, cross-validated against the real OCUDU encoder+decoder). The remaining piece —
the Ethernet II framing sitting underneath — is now handled for both possible answers rather than
left as a single-guess placeholder:

**Trigger**: `p1-ran-baseline`'s `ru_emu.yml` grounding found `ru_emulator_cli11_schema.cpp`'s
`--vlan_tag` option is `CLI::Range(1, 65536)` — no untagged value exists, so the real fronthaul
wire is expected to always carry an 802.1Q tag, contradicting this slice's original "14 bytes, no
VLAN" placeholder. Rather than flip to the opposite guess, the fix makes K1's kernel handle
whichever header length the frame actually has.

**Fix**: `k1_depacketizer.cl` no longer uses the fixed `OI_WIRE_TOTAL_HEADER_BYTES` constant — it
reads `desc.eth_hdr_len` (set by `oi_oran_preparse_frame`'s new per-frame VLAN detection, see
`../p2a-scaffold/VERIFICATION.md` for that half of the fix) and computes
`OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len)` for both its bounds check and its IQ-payload
offset. The kernel does **not** re-detect the tag itself — one parser (host-side preparse) decides,
the kernel obeys the descriptor, so the two can never disagree (same single-source-of-truth
reasoning as the `oi_p2_feed` ABI reconciliation).

**Verified**: `tests/k1_test.cpp` extended with a full VLAN-tagged round-trip test — a real
OCUDU-encoded U-plane+eCPRI payload wrapped in a real 4-byte 802.1Q tag (TPID `0x8100`, matching
p1's fronthaul plan's VID=1), decoded via the real OCUDU decoder as ground truth, run through the
actual `k1_depacketize` kernel on PoCL. Result: bit-exact RE-grid match (mod bf16 storage), proving
K1 genuinely consumed the dynamic `eth_hdr_len` rather than a hardcoded 14 — **all pre-existing
assertions still pass unchanged** (they use the untagged path, which is unaffected), plus the new
tagged case. Full p2 re-verification sweep (every p2a-p2f test suite + lint/provenance checks)
re-run clean.

`p3-live-tap-ul-inject`'s LLD was also updated (spec note, not yet implemented — that feature
hasn't started): its af_packet BPF filter must match `0xAEFE` at byte 12 OR byte 16 (not a single
fixed offset), plus a documented caveat that af_packet can deliver VLAN tags out-of-band via
`PACKET_AUXDATA` rather than inline.

**Still genuinely open**: which branch the real, live rig actually exercises (tagged vs. untagged,
and if tagged, whether af_packet delivers it inline or via `PACKET_AUXDATA`) — that's exactly what
the GCP VM capture (P1's next step) will confirm. The code no longer depends on that answer either
way.

## udCompHdr compression-header offset (2026-07-26): the GCP capture above found a SECOND real gap

The eth_hdr_len/VLAN question above got its real-world answer once p3's live gate actually ran
against a real GCP capture — but that same capture surfaced a second, deeper real bug in this
kernel: `k1_depacketizer.cl` silently assumed the O-RAN U-plane section header is immediately
followed by IQ data (the OCUDU static-compression builder's layout, `ofh_uplane_message_builder_
static_compression_impl.cpp:9-14` — 0 bytes for udCompHdr/reserved), matching only the fixtures
this file's own `k1_test.cpp` builds. Real ru_emulator-sourced frames (this project's actual
SIM-tier RU emulator, `apps/examples/ofh/ru_emulator.cpp:195-244`) unconditionally use the OCUDU
dynamic-compression builder's layout instead (`ofh_uplane_message_builder_dynamic_compression_impl.
cpp:10-23` — 2 extra bytes: `data_width<<4|type` + reserved), confirmed byte-for-byte against two
independent real corpora (163,268/163,268 real UL frames matched in one of them). Full root-cause
account, citations, and corpus-validation numbers: `../p2a-scaffold/VERIFICATION.md`'s own
"udCompHdr compression-header offset" entry (that slice owns `oi_oran_preparse_frame`/
`oi_frame_desc`/`oi_oran_wire_layout.h`, the shared code this bug actually lived in).

**This kernel's own share of the fix**: `k1_depacketizer.cl` now reads `desc.payload_byte_off`
(the fully-resolved offset p2a-scaffold's preparse now computes) directly, instead of computing
`OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len)` itself — it still does not re-detect the VLAN tag,
and now also does not re-derive the compression-header mode; both facts are the host parser's job,
this kernel only ever obeys the descriptor (same single-source-of-truth reasoning as the VLAN fix
above).

**Verified**: `tests/k1_test.cpp` extended with a new Test 6 — a full dynamic-compression round-trip
using the REAL OCUDU dynamic-compression builder AND decoder
(`create_dynamic_compr_method_ofh_user_plane_packet_builder`/`_decoder`, an authentic OCUDU
round-trip, not hand-inserted padding), decoded via the real dynamic decoder as ground truth, run
through the actual `k1_depacketize` kernel on PoCL: bit-exact RE-grid match (mod bf16 storage),
proving K1 genuinely used `desc.payload_byte_off` rather than the old eth_hdr_len-only offset that
silently dropped the udCompHdr+reserved bytes. Also includes a negative control (re-parsing the
same frame with the wrong, pre-fix `udcomphdr_bytes=ABSENT` gives a `payload_byte_off` exactly 2
bytes short) proving this test would have caught the original bug. **All pre-existing assertions
(Tests 1-5, the static-compression and VLAN-tagged cases) still pass unchanged** — this is an
additive second mode, not a replacement. Full p2 re-verification sweep (every p2a-p2f test suite +
lint/provenance checks) re-run clean after this change.
