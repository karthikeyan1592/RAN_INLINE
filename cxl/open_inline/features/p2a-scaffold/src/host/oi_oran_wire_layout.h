/* oi_oran_wire_layout.h — real (not placeholder) byte offsets for the MVP's fixed fronthaul wire
 * format: Ethernet II + eCPRI IQ-data + O-RAN CUS U-plane (static/no-compression profile, one
 * section per frame). Resolves parent LLD Open Question Q2 for the eCPRI/O-RAN layers.
 *
 * Derived by READING (not guessing) the real OCUDU decode/encode implementations -- not ported
 * code (T4 fresh per P2-R3), but the byte offsets below are cross-checked against:
 *   - lib/ofh/ecpri/ecpri_packet_decoder_impl.cpp (deserialize_header, ECPRI_COMMON_HEADER_SIZE=4,
 *     ECPRI_IQ_DATA_PACKET_FIELDS_SIZE=4)
 *   - lib/ofh/serdes/ofh_uplane_message_decoder_impl.cpp (decode_header, decode_section_header;
 *     NOF_BYTES_UP_HEADER=4, SECTION_ID_HEADER_NO_COMPRESSION_SIZE=4)
 *   - lib/ofh/serdes/ofh_uplane_message_decoder_static_compression_impl.cpp (confirms: for
 *     compression_type::none with the STATIC profile, decode_compression_header reads ZERO wire
 *     bytes -- udCompHdr is configured out-of-band, not transmitted per-section -- and
 *     decode_compression_length also reads zero bytes for type::none, so the section header is
 *     exactly 4 bytes with no compression-parameter tail)
 * and cross-validated empirically in tests/k1_test.cpp against the REAL OCUDU encoder
 * (create_static_compr_method_ofh_user_plane_packet_builder) + REAL OCUDU decoder
 * (create_static_compr_method_ofh_user_plane_packet_decoder), not just derived by hand.
 *
 * ETHERNET LAYER — RESOLVED 2026-07-24 (superseding the earlier "no VLAN" placeholder): p1's
 * ru_emulator config grounding found `--vlan_tag` is `CLI::Range(1, 65536)` (real
 * ru_emulator_cli11_schema.cpp) — there is no untagged option, so the real wire always carries a
 * 4-byte 802.1Q tag. Rather than bake in "always 14" or "always 18" on an inference, every offset
 * below is now parameterized on `eth_len` (14 or 18), computed per-frame by
 * oi_oran_preparse_frame() from the real EtherType position and carried in
 * oi_frame_desc::eth_hdr_len — the code handles both cases so a live capture confirms which
 * branch the real rig exercises rather than gating on a guess (see p2a/p2c VERIFICATION.md).
 *
 * COMPRESSION HEADER (udCompHdr) — RESOLVED 2026-07-26, second live-capture surprise (p3's P3-I1
 * gate on the GCP VM): OI_WIRE_TOTAL_HEADER_BYTES(eth_len) assumed the O-RAN U-plane section
 * header is immediately followed by IQ data, matching
 * ofh_uplane_message_builder_static_compression_impl::serialize_compression_header
 * (ofh_uplane_message_builder_static_compression_impl.cpp:9-14 — "When the static IQ format is
 * configured the udCompHdr and reserved fields are absent", 0 bytes written) and confirmed by
 * k1_test.cpp's own round-trip, which uses exactly that builder
 * (create_static_compr_method_ofh_user_plane_packet_builder). But real captured UL frames from
 * apps/examples/ofh/ru_emulator.cpp's own hand-rolled frame construction (NOT the OCUDU library's
 * builder classes at all — see set_static_header_params()/generate_test_data(),
 * ru_emulator.cpp:195-244) unconditionally reserve and emit 2 extra bytes matching
 * ofh_uplane_message_builder_dynamic_compression_impl::serialize_compression_header
 * (ofh_uplane_message_builder_dynamic_compression_impl.cpp:10-23 — 1 byte
 * `data_width<<4 | compression_type` + 1 reserved byte), regardless of the configured
 * ul_compr_method/ul_compr_bitwidth (those values only change the BYTE CONTENTS, not whether the
 * 2 bytes are present at all — ru_emulator.cpp:228-232 always writes frame[34]; the reserved byte
 * at frame[35] is always 0 from its own hdr_template default). Confirmed byte-for-byte against two
 * independent real corpora: p3's oracle-injection GCP capture (none/16 config, 1495 occurrences of
 * an exact 2448-byte oracle payload, gap bytes == 0x00 0x00) and p1's baseline corpus
 * (artifacts/p1/pcaps/20260725T180323Z, bfp/9 config, 163,268/163,268 real UL frames, gap bytes ==
 * 0x91 0x00 == (9<<4)|1 exactly as predicted). Since this project's only real RU emulator (the
 * upstream OCUDU example app) always uses this 2-byte layout, and p2f's oracle_tx_gen.cpp /
 * p2c-k1's k1_test.cpp both use the STATIC builder (0 bytes) for their own synthetic fixtures,
 * BOTH modes are real and coexist in this codebase — this is why OI_WIRE_UDCOMPHDR_BYTES_ABSENT/
 * _PRESENT exist as an explicit, caller-supplied parameter (oi_oran_preparse_frame's
 * `udcomphdr_bytes` argument) rather than being sniffed from the two bytes' content (0x00 0x00 is
 * indistinguishable from "absent" by content alone when data_width=16/type=none).
 */
#ifndef OI_ORAN_WIRE_LAYOUT_H
#define OI_ORAN_WIRE_LAYOUT_H

// --- Ethernet II ------------------------------------------------------------------------------
#define OI_WIRE_ETH_HEADER_BYTES_UNTAGGED 14u  // 6 dst MAC + 6 src MAC + 2 EtherType
#define OI_WIRE_ETH_HEADER_BYTES_TAGGED 18u     // + 4-byte 802.1Q tag (TPID 0x8100 + TCI)
// Historical default, still used by call sites that haven't been (or don't need to be) updated to
// carry a per-frame eth_len (e.g. tools built before this change) -- new code should use a real,
// per-frame eth_hdr_len instead of relying on this constant.
#define OI_WIRE_ETH_HEADER_BYTES OI_WIRE_ETH_HEADER_BYTES_UNTAGGED

// --- eCPRI (real; ecpri_constants.h + ecpri_packet_decoder_impl.cpp) ------------------------------
#define OI_WIRE_ECPRI_COMMON_HEADER_BYTES 4u    // revision/reserved/last-packet(1) + msg_type(1) + payload_size(2)
#define OI_WIRE_ECPRI_IQ_DATA_FIELDS_BYTES 4u    // pc_id(2) + seq_id(2)
#define OI_WIRE_ECPRI_HEADER_BYTES (OI_WIRE_ECPRI_COMMON_HEADER_BYTES + OI_WIRE_ECPRI_IQ_DATA_FIELDS_BYTES)

// --- O-RAN CUS U-plane (real; ofh_uplane_message_decoder_impl.cpp) -------------------------------
#define OI_WIRE_ORAN_MSG_HEADER_BYTES 4u      // direction/version/filter_index(1) + frame(1) + subframe/slot(1) + slot/symbol(1)
#define OI_WIRE_ORAN_SECTION_HEADER_BYTES 4u  // section_id(12b) + flags(2b) + start_prb(10b) + nof_prb(8b); NO comp-hdr/comp-param/comp-len bytes for MVP's static+none profile
#define OI_WIRE_ORAN_HEADER_BYTES (OI_WIRE_ORAN_MSG_HEADER_BYTES + OI_WIRE_ORAN_SECTION_HEADER_BYTES)

// --- Combined: bytes from arena_offset (start of raw Ethernet frame) to the first IQ sample byte --
// Parameterized on the ACTUAL per-frame Ethernet header length (14 or 18) -- callers pass
// desc->eth_hdr_len / desc.eth_hdr_len, never a compile-time guess.
#define OI_WIRE_TOTAL_HEADER_BYTES(eth_len) \
  ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + OI_WIRE_ORAN_HEADER_BYTES)  // = eth_len + 16

// --- udCompHdr + reserved: 0 or 2 bytes, present iff the frame's real builder is the "dynamic
// compression" profile (see header comment's 2026-07-26 finding) -- an explicit, caller-supplied
// fact, never sniffed from the bytes' content.
#define OI_WIRE_UDCOMPHDR_BYTES_ABSENT 0u   // static-compression builder / p2f's oracle_tx_gen.cpp
                                            // / p2c-k1's k1_test.cpp fixtures
#define OI_WIRE_UDCOMPHDR_BYTES_PRESENT 2u  // dynamic-compression builder / ru_emulator.cpp's own
                                            // (unconditional) frame construction -- the real rig

// --- Final payload byte offset, folding in eth_len AND udcomphdr_bytes. This is what
// oi_oran_preparse_frame() computes once and writes into oi_frame_desc::payload_byte_off --
// downstream consumers (K1's kernel included) read that field directly rather than re-deriving it
// from this macro themselves (same "one parser decides" principle as eth_hdr_len).
#define OI_WIRE_PAYLOAD_OFFSET(eth_len, udcomphdr_bytes) \
  (OI_WIRE_TOTAL_HEADER_BYTES(eth_len) + (udcomphdr_bytes))

// --- Byte offsets of individual header fields, relative to arena_offset (i.e. frame start) --------
// All parameterized on `eth_len` (the real, per-frame Ethernet header length) -- see header note.
// eCPRI common header (starts right after the Ethernet header):
#define OI_WIRE_OFF_ECPRI_REV_TYPE(eth_len) ((eth_len) + 0u)      // revision(4b)<<4 | reserved(3b)<<1 | !is_last(1b)
#define OI_WIRE_OFF_ECPRI_MSG_TYPE(eth_len) ((eth_len) + 1u)
#define OI_WIRE_OFF_ECPRI_PAYLOAD_SIZE(eth_len) ((eth_len) + 2u)  // uint16 BE, 2 bytes
#define OI_WIRE_OFF_ECPRI_PCID(eth_len) ((eth_len) + 4u)          // uint16 BE
#define OI_WIRE_OFF_ECPRI_SEQID(eth_len) ((eth_len) + 6u)         // uint16 BE

// O-RAN U-plane message header (starts right after eCPRI, at eth_len + OI_WIRE_ECPRI_HEADER_BYTES):
#define OI_WIRE_OFF_ORAN_DIR_VER_FILTER(eth_len) ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + 0u)
#define OI_WIRE_OFF_ORAN_FRAME(eth_len) ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + 1u)
#define OI_WIRE_OFF_ORAN_SUBFRAME_SLOTHI(eth_len) ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + 2u)
#define OI_WIRE_OFF_ORAN_SLOTLO_SYMBOL(eth_len) ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + 3u)

// O-RAN section header (starts right after the U-plane message header):
#define OI_WIRE_OFF_ORAN_SECTIONID_HI(eth_len) \
  ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + OI_WIRE_ORAN_MSG_HEADER_BYTES + 0u)
#define OI_WIRE_OFF_ORAN_SECTIONID_LO_FLAGS_PRBHI(eth_len) \
  ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + OI_WIRE_ORAN_MSG_HEADER_BYTES + 1u)
#define OI_WIRE_OFF_ORAN_STARTPRB_LO(eth_len) \
  ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + OI_WIRE_ORAN_MSG_HEADER_BYTES + 2u)
#define OI_WIRE_OFF_ORAN_NOF_PRB(eth_len) \
  ((eth_len) + OI_WIRE_ECPRI_HEADER_BYTES + OI_WIRE_ORAN_MSG_HEADER_BYTES + 3u)

// --- Ethernet-layer EtherType/VLAN detection offsets (used by oi_oran_preparse_frame only) -------
#define OI_WIRE_OFF_ETHERTYPE_UNTAGGED 12u
#define OI_WIRE_OFF_VLAN_TPID 12u
#define OI_WIRE_OFF_ETHERTYPE_TAGGED 16u
#define OI_WIRE_ETHERTYPE_ORAN 0xAEFEu
#define OI_WIRE_VLAN_TPID_8021Q 0x8100u

// MVP fixed values (SPEC MVP table): filter_index must equal this or the frame is malformed
// (P2-R11 scope); O-RAN payload version must equal this (ofh_cuplane_constants.h OFH_PAYLOAD_VERSION).
#define OI_WIRE_MVP_FILTER_INDEX 0u  // filter_index_type::standard_channel_filter
#define OI_WIRE_MVP_PAYLOAD_VERSION 1u

// Uncompressed 16-bit IQ (D6): 2 bytes I + 2 bytes Q per RE, big-endian, 12 REs/PRB.
#define OI_WIRE_BYTES_PER_RE 4u
#define OI_WIRE_RES_PER_PRB 12u

#endif /* OI_ORAN_WIRE_LAYOUT_H */
