// pcap_comparator.cpp — M4: P3-U1/P3-R3's byte comparator. Reads a bridge pcap captured during
// an injection run, extracts each U-plane frame's real IQ payload bytes (via the SAME shared
// oi_oran_preparse_frame helper M3 uses -- no second parser), computes file_idx from slot_id and
// a calibrated phase offset (oi_harness_calibrate.h), and asserts byte-for-byte equality against
// the corresponding .osg file's grid_payload slice for that symbol. Emits a JSON mismatch report;
// exit 0 iff 0 mismatches AND every frame's file_idx was determinable (calibration succeeded).
//
// Usage: pcap_comparator <pcap_path> <osg_dir_with_slot_NNNN.osg files> <slots_per_frame> <ru_mac>
//                        <udcomphdr_bytes>
//   <ru_mac>: "xx:xx:xx:xx:xx:xx" -- REQUIRED as of the 2026-07-26 direction-filter fix (real bug
//   found live on GCP: the bridge tap runs in hub mode -- see p1's ageing_time=0 workaround, HLD
//   D-notes -- so the captured pcap contains BOTH directions of fronthaul traffic (DU->RU DL and
//   RU->DU UL), not just the RU-emulator-sourced UL traffic this comparator's oracle files
//   describe. oi_frame_desc/oi_oran_preparse_frame have no direction field (O-RAN U-plane section
//   headers are direction-symmetric at the wire-format level -- see oi_frame_desc.h's own
//   "reserved[7]... future eAxC/BFP fields" comment), so this comparator was calibrating against
//   whichever eCPRI frame it saw FIRST in capture order -- in a hub-flooded capture, overwhelmingly
//   a DL frame, which no oracle file (UL-only content) could ever match. Confirmed via direct raw
//   byte search: the real injected UL payload WAS present, byte-for-byte, in the capture (1495
//   occurrences of one symbol's exact 2448-byte payload) while the very first ethertype=0xAEFE
//   frame in the capture had src=DU_MAC, dst=RU_MAC -- i.e. injection was already correct; this
//   comparator was comparing the wrong frames. Fix: skip any frame whose Ethernet source address
//   isn't the RU emulator's own MAC (bytes 6..11, before the optional VLAN tag, so no eth_hdr_len
//   ambiguity) before it ever reaches preparse/calibration.
//   <udcomphdr_bytes>: 0 or 2 -- REQUIRED as of the SECOND real bug found chasing the SAME live
//   symptom (found immediately after the direction-filter fix above, since the very first
//   RU-sourced frame STILL failed calibration): this comparator used
//   OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len) directly, which silently assumes the O-RAN
//   udCompHdr+reserved field is always absent (the static-compression builder's layout). Real
//   ru_emulator-sourced frames always carry it (2 bytes) -- confirmed byte-for-byte against two
//   independent real corpora, see oi_oran_wire_layout.h's header comment. The real rig's value is
//   OI_WIRE_UDCOMPHDR_BYTES_PRESENT (2); never sniffed/assumed here.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/host/oi_harness_calibrate.h"
#include "../src/host/oi_osg_format.h"
#include "../../p2a-scaffold/src/host/oi_oran_preparse.h"
#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"

namespace {

// Duplicated (not shared) across pcap_comparator.cpp / bit_exact_harness.cpp / gpu_phy_seam_bridge.c
// -- same "genuinely separate binaries" precedent this project already established.
bool parse_mac(const char* s, uint8_t out[6]) {
  unsigned b[6];
  if (std::sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
  return true;
}

// Ethernet source address is always at bytes 6..11 regardless of an 802.1Q tag (the tag sits
// after both MAC addresses), so this needs no eth_hdr_len/VLAN awareness.
bool src_mac_matches(const std::vector<uint8_t>& frame, const uint8_t ru_mac[6]) {
  if (frame.size() < 12) return false;
  return std::memcmp(frame.data() + 6, ru_mac, 6) == 0;
}

bool read_pcap_all(const std::string& path, std::vector<std::vector<uint8_t>>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  char global_hdr[24];
  f.read(global_hdr, 24);
  if (!f) return false;
  for (;;) {
    char rec_hdr[16];
    f.read(rec_hdr, 16);
    if (!f) break;
    uint32_t incl_len;
    std::memcpy(&incl_len, rec_hdr + 8, 4);
    std::vector<uint8_t> frame(incl_len);
    f.read((char*)frame.data(), incl_len);
    if (!f) break;
    out->push_back(std::move(frame));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: pcap_comparator <pcap_path> <osg_dir> <slots_per_frame> <ru_mac xx:xx:xx:xx:xx:xx> "
                        "<udcomphdr_bytes 0|2>\n");
    return 2;
  }
  std::string pcap_path = argv[1];
  std::string osg_dir = argv[2];
  uint32_t slots_per_frame = (uint32_t)std::atoi(argv[3]);
  uint8_t ru_mac[6];
  if (!parse_mac(argv[4], ru_mac)) {
    std::fprintf(stderr, "{\"check\":\"pcap_comparator\",\"error\":\"invalid ru_mac '%s', expected xx:xx:xx:xx:xx:xx\"}\n",
                argv[4]);
    return 2;
  }
  int udcomphdr_bytes_arg = std::atoi(argv[5]);
  if (udcomphdr_bytes_arg != (int)OI_WIRE_UDCOMPHDR_BYTES_ABSENT && udcomphdr_bytes_arg != (int)OI_WIRE_UDCOMPHDR_BYTES_PRESENT) {
    std::fprintf(stderr, "{\"check\":\"pcap_comparator\",\"error\":\"invalid udcomphdr_bytes '%s', expected 0 or 2\"}\n",
                argv[5]);
    return 2;
  }
  uint8_t udcomphdr_bytes = (uint8_t)udcomphdr_bytes_arg;

  std::vector<std::vector<uint8_t>> frames;
  if (!read_pcap_all(pcap_path, &frames) || frames.empty()) {
    std::fprintf(stderr, "{\"check\":\"pcap_comparator\",\"error\":\"could not read pcap or empty: %s\"}\n",
                pcap_path.c_str());
    return 2;
  }

  // Load exactly slots_per_frame oracle files: slot_0000.osg .. slot_{N-1}.osg (osg_gen's own
  // fixed naming convention).
  std::vector<oi_osg::osg_file> oracle_set(slots_per_frame);
  for (uint32_t i = 0; i < slots_per_frame; i++) {
    char fname[64];
    std::snprintf(fname, sizeof(fname), "/slot_%04u.osg", i);
    oi_osg::osg_status st = oi_osg::osg_read(osg_dir + fname, &oracle_set[i]);
    if (st != oi_osg::osg_status::ok) {
      std::fprintf(stderr, "{\"check\":\"pcap_comparator\",\"error\":\"failed to load %s: %s\"}\n", fname,
                  oi_osg::osg_status_str(st));
      return 2;
    }
  }

  oi_oran_preparse_state pstate{};
  uint64_t nof_uplane = 0, nof_parse_failed = 0, nof_mismatches = 0, nof_non_ru_src = 0;
  bool calibrated = false;
  uint32_t phase_offset = 0;

  for (auto& frame : frames) {
    if (!src_mac_matches(frame, ru_mac)) {
      nof_non_ru_src++;
      continue;
    }
    oi_frame_desc desc{};
    oi_preparse_status pst = oi_oran_preparse_frame(&pstate, frame.data(), (uint32_t)frame.size(), udcomphdr_bytes, &desc);
    if (pst != OI_PREPARSE_OK) {
      nof_parse_failed++;
      continue;
    }
    nof_uplane++;

    unsigned payload_off = desc.payload_byte_off;
    unsigned payload_len = (unsigned)desc.nof_prbs * OI_WIRE_RES_PER_PRB * OI_WIRE_BYTES_PER_RE;
    if (payload_off + payload_len > frame.size()) {
      nof_parse_failed++;
      continue;
    }
    const uint8_t* payload = frame.data() + payload_off;

    if (!calibrated) {
      // Calibration for this PAYLOAD comparator (distinct from M5's TB-level calibration, same
      // offset-search principle from oi_harness_calibrate.h): try every candidate file_idx,
      // compare THIS frame's payload against that candidate's grid_payload slice for
      // desc.symbol_id -- a payload-level comparator has no decoded TB to calibrate against.
      auto matches_candidate = [&](uint32_t candidate_file_idx) {
        const oi_osg::osg_file& f = oracle_set[candidate_file_idx];
        size_t symbol_bytes = (size_t)f.nof_prb * 12u * 4u;
        size_t off = (size_t)desc.symbol_id * symbol_bytes;
        if (off + payload_len > f.grid_payload.size()) return false;
        return std::memcmp(payload, f.grid_payload.data() + off, payload_len) == 0;
      };
      std::optional<uint32_t> offset = oi_harness::calibrate_phase_offset(desc.slot_id, slots_per_frame, matches_candidate);
      if (!offset.has_value()) {
        std::fprintf(stderr, "{\"check\":\"pcap_comparator\",\"error\":\"calibration failed on first U-plane frame "
                            "(slot_id=%u, symbol_id=%u) -- no oracle file's expected payload matched\"}\n",
                    desc.slot_id, desc.symbol_id);
        return 1;
      }
      phase_offset = *offset;
      calibrated = true;
    }

    uint32_t file_idx = oi_harness::file_idx_for(desc.slot_id, phase_offset, slots_per_frame);
    const oi_osg::osg_file& f = oracle_set[file_idx];
    size_t symbol_bytes = (size_t)f.nof_prb * 12u * 4u;
    size_t off = (size_t)desc.symbol_id * symbol_bytes;
    if (off + payload_len > f.grid_payload.size() ||
        std::memcmp(payload, f.grid_payload.data() + off, payload_len) != 0) {
      nof_mismatches++;
    }
  }

  std::printf(
      "{\"check\":\"pcap_comparator\",\"schema\":\"oi-p3-comparator/2\",\"total_frames\":%zu,"
      "\"non_ru_src\":%lu,\"uplane_frames\":%lu,\"non_uplane_or_malformed\":%lu,\"phase_offset\":%u,"
      "\"mismatches\":%lu}\n",
      frames.size(), (unsigned long)nof_non_ru_src, (unsigned long)nof_uplane,
      (unsigned long)nof_parse_failed, phase_offset, (unsigned long)nof_mismatches);

  return (nof_mismatches == 0 && calibrated) ? 0 : 1;
}
