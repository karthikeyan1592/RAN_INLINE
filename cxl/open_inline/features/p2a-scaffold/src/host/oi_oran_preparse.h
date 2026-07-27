/* oi_oran_preparse.h — shared O-RAN CUS header pre-parse helper for ingest_backend implementations.
 *
 * Owned by p2a (co-located with oi_frame_desc.h, whose fields this fills) but called ONLY by
 * ingest backends (p3-live-tap-ul-inject in SIM, p6-physical-m1-ingest in PHYSICAL) -- never by
 * oi_p2_feed() itself. One implementation, shared by both tiers, so the parsing logic (and its
 * eventual real byte-offset fixes once p2c-k1/Q2 confirms them against real wire bytes) lives in
 * exactly one place instead of being duplicated per backend.
 *
 * Why this is stateful: LLD §4.1 defines slot_id as a "host slot counter... incremented whenever
 * symbol_id decreases (wrap 13->0) within the same (eaxc, filter_index) stream" -- deriving it
 * requires remembering the previous frame's symbol_id per stream. Each ingest backend owns one
 * oi_oran_preparse_state per fronthaul stream it's demultiplexing (MVP: one stream, one state).
 *
 * Q2 STATUS (updated 2026-07-24): the eCPRI + O-RAN CUS byte offsets are derived from and
 * cross-validated against the real OCUDU encoder/decoder (see oi_oran_wire_layout.h's derivation
 * comment and p2c-k1/tests/k1_test.cpp). The Ethernet-layer VLAN question is now handled, not
 * assumed either way: this function detects the tag per-frame (EtherType at byte 12, or byte 16
 * if 0x8100 sits at 12) and records the resulting header length in oi_frame_desc::eth_hdr_len --
 * p1's ru_emulator grounding found --vlan_tag is CLI::Range(1,65536) with no untagged option, so
 * the real wire is expected to always carry a tag, but nothing downstream assumes that; a live
 * capture (GCP VM) will simply confirm which branch the real rig exercises. See
 * oi_oran_wire_layout.h and p2a/p2c VERIFICATION.md.
 */
#ifndef OI_ORAN_PREPARSE_H
#define OI_ORAN_PREPARSE_H

#include <stdint.h>

#include "oi_frame_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  OI_PREPARSE_OK = 0,
  OI_PREPARSE_ERR_TRUNCATED = 1,   // frame shorter than the minimum header size
  OI_PREPARSE_ERR_MALFORMED = 2,   // header fields out of the MVP's expected range
} oi_preparse_status;

// Per-(eaxc, filter_index) stream state. Zero-initialize before first use
// (`oi_oran_preparse_state state = {0};` in C, `oi_oran_preparse_state state{};` in C++).
typedef struct {
  int has_last_symbol;     // 0 until the first frame is parsed
  uint8_t last_symbol_id;
  uint32_t running_slot_id;
} oi_oran_preparse_state;

/// Parses frame_bytes[0..frame_len) and fills every field of *out_desc except arena_offset and
/// frame_len (the caller already knows those -- they describe where ingest already placed the
/// bytes, not something to re-derive from the bytes themselves). Advances *state's slot_id
/// counter on symbol-wrap detection. Returns non-OK without modifying *out_desc on a malformed/
/// truncated frame (caller's error-handling matches the existing "malformed frame -> drop, not
/// fatal" tolerance already specified for K1/p3's ingest layer).
///
/// `udcomphdr_bytes` (added 2026-07-26, see oi_oran_wire_layout.h's header comment for the full
/// citation trail): OI_WIRE_UDCOMPHDR_BYTES_ABSENT (0) or _PRESENT (2), an explicit fact the
/// caller already knows from its own rig/fixture (which OCUDU U-plane builder produced these
/// frames), never sniffed from the frame's own bytes -- the 2 candidate bytes read as 0x00 0x00
/// for the none/16 config and are NOT distinguishable from "absent" by content alone. Folded
/// together with eth_hdr_len into out_desc->payload_byte_off (OI_WIRE_PAYLOAD_OFFSET), so
/// downstream consumers (K1's kernel included) never re-derive the offset themselves.
oi_preparse_status oi_oran_preparse_frame(oi_oran_preparse_state* state,
                                          const uint8_t* frame_bytes, uint32_t frame_len,
                                          uint8_t udcomphdr_bytes,
                                          oi_frame_desc* out_desc);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_ORAN_PREPARSE_H */
