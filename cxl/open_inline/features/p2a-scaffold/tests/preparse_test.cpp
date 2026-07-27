// preparse_test.cpp — proves oi_oran_preparse_frame's wrap-detection slot_id derivation is
// correct (the fully-specified part), using frames built at the REAL eCPRI + O-RAN CUS byte
// offsets (oi_oran_wire_layout.h) -- Q2 resolved for these layers, see that header's derivation
// comment. Stronger, protocol-level cross-validation (real OCUDU encoder/decoder) lives in
// p2c-k1/tests/k1_test.cpp; this test only needs self-consistent frames for the wrap-detection
// logic, which is independent of exact field values.
//
// 2026-07-24: also proves the VLAN-tag detection this function now does (both branches -- see
// oi_oran_preparse.cpp/oi_oran_wire_layout.h's header notes, triggered by p1's ru_emulator finding
// that --vlan_tag has no untagged option). Ethernet bytes are no longer "not parsed" as this
// file's header used to say -- EtherType (and, when present, the 802.1Q TPID) at bytes 12/16 are
// now read and validated; make_frame() below sets them for real instead of leaving them zero.
#include "../src/host/oi_oran_preparse.h"
#include "../src/host/oi_oran_wire_layout.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

// Builds a minimal synthetic frame at the REAL wire offsets: a real (untagged or 802.1Q-tagged)
// Ethernet header with a valid EtherType 0xAEFE, valid eCPRI (revision=1, msg_type=iq_data), valid
// O-RAN version/filter_index, given symbol_id/section_id, nof_prb fixed at 51 (MVP full-band).
// `vlan_tci`, when nonzero, is written into the tag's TCI field (any value -- this project's
// classifier accepts any VLAN ID, only the TPID 0x8100 + the EtherType past it are checked; see
// oi_oran_preparse.cpp's own comment on why VID itself is out of scope here). `udcomphdr_bytes`
// (default ABSENT, matching every pre-existing call site's frame size unchanged) pads the frame
// with that many extra zero bytes past the section header, simulating the real ru_emulator wire
// layout (2026-07-26 fix, see oi_oran_wire_layout.h's header comment).
static std::vector<uint8_t> make_frame(uint8_t symbol_id, uint16_t section_id = 0,
                                        uint8_t filter_index = 0, bool vlan_tagged = false,
                                        uint16_t vlan_tci = 1,
                                        unsigned udcomphdr_bytes = OI_WIRE_UDCOMPHDR_BYTES_ABSENT) {
  uint32_t eth_hdr_len = vlan_tagged ? OI_WIRE_ETH_HEADER_BYTES_TAGGED : OI_WIRE_ETH_HEADER_BYTES_UNTAGGED;
  std::vector<uint8_t> f(OI_WIRE_TOTAL_HEADER_BYTES(eth_hdr_len) + udcomphdr_bytes, 0);

  if (vlan_tagged) {
    f[OI_WIRE_OFF_VLAN_TPID] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q >> 8);
    f[OI_WIRE_OFF_VLAN_TPID + 1] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q & 0xFFu);
    f[OI_WIRE_OFF_VLAN_TPID + 2] = (uint8_t)(vlan_tci >> 8);
    f[OI_WIRE_OFF_VLAN_TPID + 3] = (uint8_t)(vlan_tci & 0xFFu);
    f[OI_WIRE_OFF_ETHERTYPE_TAGGED] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN >> 8);
    f[OI_WIRE_OFF_ETHERTYPE_TAGGED + 1] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN & 0xFFu);
  } else {
    f[OI_WIRE_OFF_ETHERTYPE_UNTAGGED] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN >> 8);
    f[OI_WIRE_OFF_ETHERTYPE_UNTAGGED + 1] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN & 0xFFu);
  }

  f[OI_WIRE_OFF_ECPRI_REV_TYPE(eth_hdr_len)] = (1u << 4);  // revision=1, is_last_packet bit clear (=true)
  f[OI_WIRE_OFF_ECPRI_MSG_TYPE(eth_hdr_len)] = 0x00;       // message_type::iq_data
  f[OI_WIRE_OFF_ORAN_DIR_VER_FILTER(eth_hdr_len)] =
      static_cast<uint8_t>((OI_WIRE_MVP_PAYLOAD_VERSION << 4) | (filter_index & 0x0Fu));
  f[OI_WIRE_OFF_ORAN_SLOTLO_SYMBOL(eth_hdr_len)] = static_cast<uint8_t>(symbol_id & 0x3Fu);
  f[OI_WIRE_OFF_ORAN_SECTIONID_HI(eth_hdr_len)] = static_cast<uint8_t>((section_id >> 4) & 0xFFu);
  f[OI_WIRE_OFF_ORAN_SECTIONID_LO_FLAGS_PRBHI(eth_hdr_len)] = static_cast<uint8_t>((section_id & 0x0Fu) << 4);
  f[OI_WIRE_OFF_ORAN_STARTPRB_LO(eth_hdr_len)] = 0;
  f[OI_WIRE_OFF_ORAN_NOF_PRB(eth_hdr_len)] = 51;
  return f;
}

int main() {
  oi_oran_preparse_state state{};
  oi_frame_desc desc{};

  // 1. First frame: no prior symbol, no wrap possible.
  {
    auto frame = make_frame(/*symbol_id=*/2);
    auto st = oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    check(st == OI_PREPARSE_OK, "first frame (symbol 2) parses OK");
    check(desc.symbol_id == 2, "first frame symbol_id == 2");
    check(desc.slot_id == 0, "first frame slot_id starts at 0");
    check(desc.eth_hdr_len == OI_WIRE_ETH_HEADER_BYTES_UNTAGGED, "first frame (untagged) eth_hdr_len == 14");
    check(desc.payload_byte_off == OI_WIRE_TOTAL_HEADER_BYTES(OI_WIRE_ETH_HEADER_BYTES_UNTAGGED),
          "first frame (untagged, udcomphdr_bytes=ABSENT) payload_byte_off == 30");
  }

  // 2. Monotonically increasing symbols within the same slot: no wrap, slot_id unchanged.
  {
    for (uint8_t sym : {7, 11, 13}) {
      auto frame = make_frame(sym);
      auto st = oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
      check(st == OI_PREPARSE_OK, "increasing-symbol frame parses OK");
      check(desc.slot_id == 0, "slot_id stays 0 while symbol_id increases (no wrap yet)");
    }
  }

  // 3. Wrap (13 -> 0): slot_id must increment exactly once.
  {
    auto frame = make_frame(/*symbol_id=*/0);
    auto st = oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    check(st == OI_PREPARSE_OK, "wrap frame (13->0) parses OK");
    check(desc.slot_id == 1, "slot_id increments to 1 exactly once on wrap");
  }

  // 4. Repeat one full 0..13 cycle, confirm slot_id reaches 2.
  {
    for (uint8_t sym = 1; sym <= 13; sym++) {
      auto frame = make_frame(sym);
      oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    }
    auto frame = make_frame(0);
    oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    check(desc.slot_id == 2, "second full symbol cycle advances slot_id to 2");
  }

  // 5. Truncated frame -> ERR_TRUNCATED, out_desc left untouched (still whatever it was before).
  {
    oi_frame_desc before = desc;
    std::vector<uint8_t> short_frame(4, 0);
    auto st = oi_oran_preparse_frame(&state, short_frame.data(),
                                     static_cast<uint32_t>(short_frame.size()),
                                     OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    check(st == OI_PREPARSE_ERR_TRUNCATED, "frame shorter than min header -> ERR_TRUNCATED");
    check(std::memcmp(&before, &desc, sizeof(desc)) == 0,
          "truncated-frame error leaves out_desc unmodified");
  }

  // 6. Malformed symbol_id (>13) -> ERR_MALFORMED.
  {
    auto frame = make_frame(/*symbol_id=*/14);  // out of range (valid: 0-13)
    auto st = oi_oran_preparse_frame(&state, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc);
    check(st == OI_PREPARSE_ERR_MALFORMED, "symbol_id==14 (out of 0-13 range) -> ERR_MALFORMED");
  }

  // 7. Independent stream state: a second state object starts fresh at slot_id 0 even though the
  // first state has already advanced -- proves state is per-stream, not global/static.
  {
    oi_oran_preparse_state state2{};
    oi_frame_desc desc2{};
    auto frame = make_frame(5);
    oi_oran_preparse_frame(&state2, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc2);
    check(desc2.slot_id == 0, "a fresh preparse_state starts at slot_id 0 independent of others");
  }

  // 8. VLAN-tagged frame (2026-07-24): 802.1Q tag present -> eth_hdr_len == 18, everything past
  // the tag parses identically to the untagged case (same symbol_id/section_id/nof_prb logic,
  // just at a 4-byte-shifted offset -- this is the "K1 obeys the descriptor, never re-parses"
  // contract's whole point: preparse's own two code paths must agree on everything except offset).
  {
    oi_oran_preparse_state state3{};
    oi_frame_desc desc3{};
    auto frame = make_frame(/*symbol_id=*/9, /*section_id=*/0, /*filter_index=*/0,
                           /*vlan_tagged=*/true, /*vlan_tci=*/1);
    auto st = oi_oran_preparse_frame(&state3, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc3);
    check(st == OI_PREPARSE_OK, "VLAN-tagged frame parses OK");
    check(desc3.eth_hdr_len == OI_WIRE_ETH_HEADER_BYTES_TAGGED, "VLAN-tagged frame eth_hdr_len == 18");
    check(desc3.symbol_id == 9, "VLAN-tagged frame symbol_id parses correctly past the tag");
    check(desc3.nof_prbs == 51, "VLAN-tagged frame nof_prbs parses correctly past the tag");
  }

  // 9. VLAN tag with a DIFFERENT VID still parses -- this project's classifier accepts any VLAN
  // ID (only the TPID + the EtherType past it are validated; VID filtering is a switch/NIC-level
  // concern below this function's scope, not something K1/preparse enforces).
  {
    oi_oran_preparse_state state4{};
    oi_frame_desc desc4{};
    auto frame = make_frame(/*symbol_id=*/3, /*section_id=*/0, /*filter_index=*/0,
                           /*vlan_tagged=*/true, /*vlan_tci=*/42);
    auto st = oi_oran_preparse_frame(&state4, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc4);
    check(st == OI_PREPARSE_OK, "VLAN-tagged frame with a non-default VID (42) still parses OK");
  }

  // 10. Tagged-looking frame whose real EtherType (past the tag) is wrong -> ERR_MALFORMED, not
  // silently accepted as if it were untagged.
  {
    oi_oran_preparse_state state5{};
    oi_frame_desc desc5{};
    auto frame = make_frame(/*symbol_id=*/1, /*section_id=*/0, /*filter_index=*/0,
                           /*vlan_tagged=*/true);
    frame[OI_WIRE_OFF_ETHERTYPE_TAGGED] = 0x08;
    frame[OI_WIRE_OFF_ETHERTYPE_TAGGED + 1] = 0x00;  // 0x0800 (IPv4), not 0xAEFE
    auto st = oi_oran_preparse_frame(&state5, frame.data(), frame.size(), OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc5);
    check(st == OI_PREPARSE_ERR_MALFORMED, "802.1Q-tagged frame with wrong post-tag EtherType -> ERR_MALFORMED");
  }

  // 11. Truncated exactly at the VLAN boundary: TPID present but not enough bytes for the real
  // EtherType past it -> ERR_TRUNCATED, not an out-of-bounds read.
  {
    oi_oran_preparse_state state6{};
    oi_frame_desc desc6{};
    std::vector<uint8_t> short_tagged(15, 0);  // 12 (dst+src) + 2 (TPID) + 1 byte of TCI -- cut short
    short_tagged[OI_WIRE_OFF_VLAN_TPID] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q >> 8);
    short_tagged[OI_WIRE_OFF_VLAN_TPID + 1] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q & 0xFFu);
    auto st = oi_oran_preparse_frame(&state6, short_tagged.data(),
                                     static_cast<uint32_t>(short_tagged.size()),
                                     OI_WIRE_UDCOMPHDR_BYTES_ABSENT, &desc6);
    check(st == OI_PREPARSE_ERR_TRUNCATED, "802.1Q tag present but frame cut short before the real EtherType -> ERR_TRUNCATED");
  }

  // 12. udcomphdr_bytes=PRESENT (2026-07-26 fix): untagged frame with the real ru_emulator-style
  // 2-byte udCompHdr+reserved gap present -- payload_byte_off must be 2 bytes past the ABSENT case
  // (test 1), not just eth_hdr_len + eCPRI/O-RAN header bytes. This is the exact field the real bug
  // (pcap_comparator/K1 silently assuming ABSENT) got wrong.
  {
    oi_oran_preparse_state state7{};
    oi_frame_desc desc7{};
    auto frame = make_frame(/*symbol_id=*/4, /*section_id=*/0, /*filter_index=*/0,
                           /*vlan_tagged=*/false, /*vlan_tci=*/1, OI_WIRE_UDCOMPHDR_BYTES_PRESENT);
    auto st = oi_oran_preparse_frame(&state7, frame.data(), static_cast<uint32_t>(frame.size()),
                                     OI_WIRE_UDCOMPHDR_BYTES_PRESENT, &desc7);
    check(st == OI_PREPARSE_OK, "udcomphdr_bytes=PRESENT, untagged frame parses OK");
    check(desc7.symbol_id == 4, "udcomphdr_bytes=PRESENT frame symbol_id parses correctly");
    check(desc7.payload_byte_off == OI_WIRE_TOTAL_HEADER_BYTES(OI_WIRE_ETH_HEADER_BYTES_UNTAGGED) + 2,
          "udcomphdr_bytes=PRESENT (untagged) payload_byte_off == 32 (2 bytes past the ABSENT case)");
  }

  // 13. udcomphdr_bytes=PRESENT + VLAN-tagged together (additive, per Step 3's design): matches the
  // EXACT real byte offset (36) confirmed against real captured wire frames on the GCP VM (18
  // eth_hdr_len + 8 eCPRI + 8 O-RAN msg/section + 2 udCompHdr/reserved).
  {
    oi_oran_preparse_state state8{};
    oi_frame_desc desc8{};
    auto frame = make_frame(/*symbol_id=*/6, /*section_id=*/0, /*filter_index=*/0,
                           /*vlan_tagged=*/true, /*vlan_tci=*/1, OI_WIRE_UDCOMPHDR_BYTES_PRESENT);
    auto st = oi_oran_preparse_frame(&state8, frame.data(), static_cast<uint32_t>(frame.size()),
                                     OI_WIRE_UDCOMPHDR_BYTES_PRESENT, &desc8);
    check(st == OI_PREPARSE_OK, "udcomphdr_bytes=PRESENT + VLAN-tagged frame parses OK");
    check(desc8.eth_hdr_len == OI_WIRE_ETH_HEADER_BYTES_TAGGED, "udcomphdr_bytes=PRESENT + VLAN-tagged eth_hdr_len == 18");
    check(desc8.payload_byte_off == 36,
          "udcomphdr_bytes=PRESENT + VLAN-tagged payload_byte_off == 36 -- matches the exact real "
          "byte offset confirmed against real captured wire frames on the GCP VM");
  }

  std::printf("\n%s\n", g_fail == 0 ? "preparse_test: ALL PASS" : "preparse_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
