#include "oi_oran_preparse.h"

#include <string.h>

#include "oi_oran_wire_layout.h"

// Real bit-packed O-RAN CUS U-plane + eCPRI layout (see oi_oran_wire_layout.h for full derivation
// and source citations -- this supersedes the earlier byte-aligned placeholder; Q2 resolved for
// the eCPRI/O-RAN layers, AND for the Ethernet layer as of 2026-07-24 (see below and
// oi_oran_wire_layout.h's header note): p1's ru_emulator grounding found --vlan_tag is
// CLI::Range(1,65536) with no untagged option, so the real wire always carries a tag -- rather
// than bake in either answer, this function detects the tag per-frame and the rest of the
// pipeline (K1's kernel) consumes the result instead of assuming or re-deriving it.
//
// udcomphdr_bytes (2026-07-26, see oi_oran_wire_layout.h's header comment): unlike eth_hdr_len,
// the udCompHdr+reserved field's presence (0 or 2 bytes) is NOT autodetected from the frame's own
// bytes -- it reads as 0x00 0x00 for the none/16 config either way, so "present with zero content"
// and "absent" are indistinguishable by content alone. The caller supplies it; this function only
// folds it into the final payload_byte_off, alongside eth_hdr_len, so downstream consumers never
// re-derive either half themselves.

extern "C" oi_preparse_status oi_oran_preparse_frame(oi_oran_preparse_state* state,
                                                     const uint8_t* frame_bytes,
                                                     uint32_t frame_len,
                                                     uint8_t udcomphdr_bytes,
                                                     oi_frame_desc* out_desc) {
  // Ethernet-layer EtherType/VLAN detection (2026-07-24): read EtherType at byte 12; if it's the
  // 802.1Q TPID (0x8100), the real EtherType sits 4 bytes further out, at byte 16. Reject only if
  // the FINAL EtherType (whichever offset it landed at) isn't 0xAEFE -- a frame is malformed
  // either way if that check fails, not merely "assumed untagged".
  if (frame_len < OI_WIRE_OFF_ETHERTYPE_UNTAGGED + 2u) {
    return OI_PREPARSE_ERR_TRUNCATED;
  }
  uint16_t ethertype_or_tpid = static_cast<uint16_t>((frame_bytes[OI_WIRE_OFF_VLAN_TPID] << 8) |
                                                     frame_bytes[OI_WIRE_OFF_VLAN_TPID + 1]);
  uint32_t eth_hdr_len;
  if (ethertype_or_tpid == OI_WIRE_VLAN_TPID_8021Q) {
    eth_hdr_len = OI_WIRE_ETH_HEADER_BYTES_TAGGED;
    if (frame_len < OI_WIRE_OFF_ETHERTYPE_TAGGED + 2u) {
      return OI_PREPARSE_ERR_TRUNCATED;
    }
    uint16_t real_ethertype = static_cast<uint16_t>((frame_bytes[OI_WIRE_OFF_ETHERTYPE_TAGGED] << 8) |
                                                    frame_bytes[OI_WIRE_OFF_ETHERTYPE_TAGGED + 1]);
    if (real_ethertype != OI_WIRE_ETHERTYPE_ORAN) {
      return OI_PREPARSE_ERR_MALFORMED;
    }
  } else {
    eth_hdr_len = OI_WIRE_ETH_HEADER_BYTES_UNTAGGED;
    if (ethertype_or_tpid != OI_WIRE_ETHERTYPE_ORAN) {
      return OI_PREPARSE_ERR_MALFORMED;
    }
  }

  uint32_t payload_byte_off = OI_WIRE_PAYLOAD_OFFSET(eth_hdr_len, udcomphdr_bytes);
  if (frame_len < payload_byte_off) {
    return OI_PREPARSE_ERR_TRUNCATED;
  }

  // eCPRI common header sanity (mirrors ecpri_packet_decoder_impl::is_header_valid): revision
  // must be 1, message type must be iq_data (0x00). Malformed otherwise.
  uint8_t ecpri_rev_type = frame_bytes[OI_WIRE_OFF_ECPRI_REV_TYPE(eth_hdr_len)];
  uint8_t ecpri_revision = ecpri_rev_type >> 4;
  uint8_t ecpri_msg_type = frame_bytes[OI_WIRE_OFF_ECPRI_MSG_TYPE(eth_hdr_len)];
  if (ecpri_revision != 1 || ecpri_msg_type != 0x00) {
    return OI_PREPARSE_ERR_MALFORMED;
  }

  // O-RAN U-plane message header (ofh_uplane_message_decoder_impl::decode_header).
  uint8_t dir_ver_filter = frame_bytes[OI_WIRE_OFF_ORAN_DIR_VER_FILTER(eth_hdr_len)];
  uint8_t oran_version = (dir_ver_filter >> 4) & 0x07u;
  uint8_t filter_index = dir_ver_filter & 0x0Fu;
  if (oran_version != OI_WIRE_MVP_PAYLOAD_VERSION || filter_index != OI_WIRE_MVP_FILTER_INDEX) {
    return OI_PREPARSE_ERR_MALFORMED;
  }

  uint8_t slot_and_symbol = frame_bytes[OI_WIRE_OFF_ORAN_SLOTLO_SYMBOL(eth_hdr_len)];
  uint8_t symbol_id = slot_and_symbol & 0x3Fu;
  if (symbol_id > 13) {
    return OI_PREPARSE_ERR_MALFORMED;
  }

  // O-RAN section header (ofh_uplane_message_decoder_impl::decode_section_header).
  uint8_t section_id_hi = frame_bytes[OI_WIRE_OFF_ORAN_SECTIONID_HI(eth_hdr_len)];
  uint8_t section_lo_flags_prbhi = frame_bytes[OI_WIRE_OFF_ORAN_SECTIONID_LO_FLAGS_PRBHI(eth_hdr_len)];
  uint16_t section_id = static_cast<uint16_t>((static_cast<uint16_t>(section_id_hi) << 4) |
                                              (section_lo_flags_prbhi >> 4));
  uint8_t is_every_rb_used = (((section_lo_flags_prbhi >> 3) & 1u) == 0) ? 1 : 0;
  uint8_t use_current_symbol_number = (((section_lo_flags_prbhi >> 2) & 1u) == 0) ? 1 : 0;
  uint16_t start_prb = static_cast<uint16_t>(
      (static_cast<uint16_t>(section_lo_flags_prbhi & 0x03u) << 8) |
      frame_bytes[OI_WIRE_OFF_ORAN_STARTPRB_LO(eth_hdr_len)]);
  uint16_t nof_prb = frame_bytes[OI_WIRE_OFF_ORAN_NOF_PRB(eth_hdr_len)];
  if (nof_prb == 0) {
    // O-RAN.WG4.CUS: 0 signals "more than 255 PRBs" / "all configured PRBs" -- MVP: 51, always
    // full-band (D-something, matches SPEC MVP fixed config).
    nof_prb = 51;
    start_prb = 0;
  }

  // Wrap-detection slot_id derivation (LLD §4.1: "host slot counter... incremented whenever
  // symbol_id decreases (wrap 13->0)"). Unchanged from the original implementation -- fully
  // specified and real, independent of the wire byte offsets above.
  if (state->has_last_symbol && symbol_id < state->last_symbol_id) {
    state->running_slot_id++;
  }
  state->last_symbol_id = symbol_id;
  state->has_last_symbol = 1;

  out_desc->slot_id = state->running_slot_id;
  out_desc->symbol_id = symbol_id;
  // TRUNCATION RISK (found while resolving Q2, not previously flagged): the real wire field is
  // 12 bits (0-4095, ofh_uplane_message_decoder_impl::decode_section_header), but oi_frame_desc's
  // section_id is only uint8_t (LLD §4.1, offset 17, chosen before the real field width was
  // known). MVP's single-section-per-frame scope means section_id is always 0 in practice, so
  // this doesn't bite today -- but it's a real, silent-truncation latent bug if that assumption
  // ever changes. Not fixed here: widening the field changes oi_frame_desc's 32-byte layout, the
  // same class of cross-slice ABI decision the oi_p2_feed signature change was (p2a
  // VERIFICATION.md) -- flagged for explicit reconciliation, not silently patched.
  out_desc->section_id = static_cast<uint8_t>(section_id);
  out_desc->filter_index = filter_index;
  out_desc->flags = static_cast<uint8_t>((is_every_rb_used ? 0x01u : 0u) |
                                         (use_current_symbol_number ? 0x02u : 0u));
  out_desc->start_prb = start_prb;
  out_desc->nof_prbs = nof_prb;
  out_desc->eth_hdr_len = static_cast<uint8_t>(eth_hdr_len);
  out_desc->payload_byte_off = static_cast<uint8_t>(payload_byte_off);
  memset(out_desc->reserved, 0, sizeof(out_desc->reserved));
  // arena_offset/frame_len are intentionally NOT set here -- the caller (ingest_backend) already
  // knows them (it placed the bytes), and sets them on out_desc itself before/after this call.

  return OI_PREPARSE_OK;
}
