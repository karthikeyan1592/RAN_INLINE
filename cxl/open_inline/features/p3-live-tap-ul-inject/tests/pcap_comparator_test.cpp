// pcap_comparator_test.cpp — M4 unit test. Hand-builds synthetic wire frames (same technique as
// p2a-scaffold/tests/preparse_test.cpp: direct byte placement at oi_oran_wire_layout.h's real
// offsets, not a full O-RAN builder) carrying REAL oracle-grid IQ payload bytes across multiple
// slots (via symbol-wrap), writes them to a pcap, and invokes the real built pcap_comparator
// binary as a subprocess -- checking it reports 0 mismatches for a byte-identical, correctly-
// injected stream, and >=1 mismatch when one payload byte is deliberately corrupted.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/host/oi_osg_format.h"
#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"

// Same pinned MACs used throughout p3 (ingest_af_packet_test.cpp, compose.p3.yml's ru_mac arg).
static const uint8_t kRuMac[6] = {0x02, 0x6f, 0x69, 0x00, 0x01, 0x01};
static const uint8_t kDuMac[6] = {0x02, 0x6f, 0x69, 0x00, 0x01, 0x02};
static const char* kRuMacStr = "02:6f:69:00:01:01";

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

static void write_pcap(const std::string& path, const std::vector<std::vector<uint8_t>>& frames) {
  std::ofstream f(path, std::ios::binary);
  uint32_t magic = 0xa1b2c3d4u;
  uint16_t vmaj = 2, vmin = 4;
  int32_t tz = 0;
  uint32_t sig = 0, snap = 65535, net = 1;
  f.write((const char*)&magic, 4);
  f.write((const char*)&vmaj, 2);
  f.write((const char*)&vmin, 2);
  f.write((const char*)&tz, 4);
  f.write((const char*)&sig, 4);
  f.write((const char*)&snap, 4);
  f.write((const char*)&net, 4);
  for (size_t i = 0; i < frames.size(); i++) {
    uint32_t ts_sec = 0, ts_usec = (uint32_t)i, len = (uint32_t)frames[i].size();
    f.write((const char*)&ts_sec, 4);
    f.write((const char*)&ts_usec, 4);
    f.write((const char*)&len, 4);
    f.write((const char*)&len, 4);
    f.write((const char*)frames[i].data(), frames[i].size());
  }
}

// Builds one untagged wire frame carrying `iq_payload` (must be exactly nof_prb*12*4 bytes) at
// the given symbol_id, matching preparse_test.cpp's own hand-building convention. `src_mac` is
// embedded at bytes 6..11 (real Ethernet source-address position, ahead of any VLAN tag) -- as of
// the 2026-07-26 direction-filter fix, pcap_comparator only considers frames sourced from the RU
// emulator's own MAC, so every test frame must carry a real, deliberate source address rather than
// the all-zero default. `udcomphdr_bytes` (default ABSENT, matching every pre-existing call site's
// unchanged behavior) inserts that many zero bytes between the section header and the IQ payload,
// simulating the real ru_emulator wire layout (OI_WIRE_UDCOMPHDR_BYTES_PRESENT) that exposed the
// second real bug this file regresses -- see the new udcomphdr_bytes_present_matches_real_rig case.
static std::vector<uint8_t> build_frame(uint8_t symbol_id, const std::vector<uint8_t>& iq_payload,
                                        const uint8_t src_mac[6] = kRuMac,
                                        unsigned udcomphdr_bytes = OI_WIRE_UDCOMPHDR_BYTES_ABSENT) {
  unsigned eth_hdr_len = OI_WIRE_ETH_HEADER_BYTES_UNTAGGED;
  unsigned total_hdr = OI_WIRE_TOTAL_HEADER_BYTES(eth_hdr_len) + udcomphdr_bytes;
  std::vector<uint8_t> f(total_hdr + iq_payload.size(), 0);

  std::memcpy(f.data() + 6, src_mac, 6);
  f[OI_WIRE_OFF_ETHERTYPE_UNTAGGED] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN >> 8);
  f[OI_WIRE_OFF_ETHERTYPE_UNTAGGED + 1] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN & 0xFFu);
  f[OI_WIRE_OFF_ECPRI_REV_TYPE(eth_hdr_len)] = (1u << 4);
  f[OI_WIRE_OFF_ECPRI_MSG_TYPE(eth_hdr_len)] = 0x00;
  f[OI_WIRE_OFF_ORAN_DIR_VER_FILTER(eth_hdr_len)] =
      (uint8_t)((OI_WIRE_MVP_PAYLOAD_VERSION << 4) | OI_WIRE_MVP_FILTER_INDEX);
  f[OI_WIRE_OFF_ORAN_SLOTLO_SYMBOL(eth_hdr_len)] = symbol_id & 0x3Fu;
  f[OI_WIRE_OFF_ORAN_SECTIONID_HI(eth_hdr_len)] = 0;
  f[OI_WIRE_OFF_ORAN_SECTIONID_LO_FLAGS_PRBHI(eth_hdr_len)] = 0;
  f[OI_WIRE_OFF_ORAN_STARTPRB_LO(eth_hdr_len)] = 0;
  f[OI_WIRE_OFF_ORAN_NOF_PRB(eth_hdr_len)] = 51;

  std::memcpy(f.data() + total_hdr, iq_payload.data(), iq_payload.size());
  return f;
}

static std::string run_and_capture(const std::string& cmd, int* exit_code) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    *exit_code = -1;
    return out;
  }
  char buf[512];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  int rc = pclose(p);
  *exit_code = WEXITSTATUS(rc);
  return out;
}

int main() {
  const uint32_t N = 3;  // small oracle set for this test (does not need to equal 20 -- the
                        // comparator's calibration logic works for any N, verified generally by
                        // harness_calibrate_test.cpp; osg_gen's own N==20 constraint is specific
                        // to ru_emulator's real DMRS-safety concern, not the comparator's logic)

  // --- Build a small synthetic oracle set (3 files, distinct payload content per file) ---
  std::string osg_dir = "/tmp/pcap_comparator_test_osg";
  std::system(("mkdir -p " + osg_dir).c_str());
  std::vector<oi_osg::osg_file> oracle(N);
  for (uint32_t i = 0; i < N; i++) {
    oi_osg::osg_file& f = oracle[i];
    f.iq_format = oi_osg::kIqFormatUncompressed16;
    f.numerology_mu = 1;
    f.nof_prb = 51;
    f.nof_symbols_per_slot = 14;
    f.nof_eaxc = 1;
    f.eaxc_id = 0;
    f.tb_crc_ok = 1;
    f.rnti = 0x4601;
    f.mcs_index = 4;
    f.grid_payload.resize(oi_osg::osg_expected_grid_payload_len(14, 51));
    for (size_t b = 0; b < f.grid_payload.size(); b++) f.grid_payload[b] = (uint8_t)((b + i * 97) & 0xFF);
    f.tb_payload = {1, 2, 3};
    f.tb_len_bytes = 3;
    char fname[64];
    std::snprintf(fname, sizeof(fname), "/slot_%04u.osg", i);
    check(oi_osg::osg_write(osg_dir + fname, f), std::string("wrote synthetic oracle file ") + fname);
  }

  size_t symbol_bytes = 51u * 12u * 4u;

  // --- Build a synthetic pcap: 2 full slots (14 symbols each) + 1 partial, cycling file_idx =
  // slot_id mod N (phase offset 0, since this test controls both "sides") ---
  std::vector<std::vector<uint8_t>> frames;
  for (uint32_t slot = 0; slot < 5; slot++) {
    uint32_t file_idx = slot % N;
    for (uint8_t sym = 0; sym < 14; sym++) {
      std::vector<uint8_t> payload(oracle[file_idx].grid_payload.begin() + sym * symbol_bytes,
                                   oracle[file_idx].grid_payload.begin() + (sym + 1) * symbol_bytes);
      frames.push_back(build_frame(sym, payload));
    }
  }
  std::string pcap_path = "/tmp/pcap_comparator_test_clean.pcap";
  write_pcap(pcap_path, frames);

  // --- Run the real comparator binary against the clean pcap ---
  int rc = -1;
  std::string out = run_and_capture("./build/pcap_comparator " + pcap_path + " " + osg_dir + " " + std::to_string(N) +
                                    " " + kRuMacStr + " " + std::to_string(OI_WIRE_UDCOMPHDR_BYTES_ABSENT) + " 2>&1",
                                    &rc);
  check(rc == 0, "pcap_comparator exits 0 on a byte-identical injected stream (output: " + out + ")");
  check(out.find("\"mismatches\":0") != std::string::npos, "pcap_comparator reports 0 mismatches");
  check(out.find("\"non_ru_src\":0") != std::string::npos,
       "pcap_comparator reports 0 non-RU-sourced frames when every frame is RU-sourced (output: " + out + ")");

  // --- Corrupt one payload byte in one frame, confirm the comparator catches it ---
  frames[20][OI_WIRE_TOTAL_HEADER_BYTES(OI_WIRE_ETH_HEADER_BYTES_UNTAGGED)] ^= 0xFF;
  std::string corrupt_path = "/tmp/pcap_comparator_test_corrupt.pcap";
  write_pcap(corrupt_path, frames);
  int rc2 = -1;
  std::string out2 =
      run_and_capture("./build/pcap_comparator " + corrupt_path + " " + osg_dir + " " + std::to_string(N) + " " +
                     kRuMacStr + " " + std::to_string(OI_WIRE_UDCOMPHDR_BYTES_ABSENT) + " 2>&1",
                     &rc2);
  check(rc2 != 0, "pcap_comparator exits nonzero when a payload byte is corrupted");
  check(out2.find("\"mismatches\":0") == std::string::npos, "pcap_comparator reports a nonzero mismatch count for the corrupted stream");

  // --- Real bug regression (found live on GCP 2026-07-26): a hub-mode bridge tap captures BOTH
  // directions of fronthaul traffic. Prepend a DU-sourced (DL) frame whose payload deliberately
  // does NOT match any oracle file -- if the comparator calibrated against this frame (the pre-fix
  // behavior, since it took whichever eCPRI frame came first in capture order), it would report a
  // calibration failure despite every real RU-sourced frame being byte-perfect. Confirms the
  // src-MAC filter is what makes this stream pass, not incidental frame ordering. ---
  std::vector<uint8_t> foreign_payload(symbol_bytes, 0xEE);  // matches no oracle file's content
  std::vector<uint8_t> foreign_frame = build_frame(0, foreign_payload, kDuMac);
  // frames[] above already carries the corrupted byte from the previous case -- rebuild a clean
  // set so this case isolates the direction-filter behavior from the corruption behavior.
  std::vector<std::vector<uint8_t>> clean_frames;
  for (uint32_t slot = 0; slot < 5; slot++) {
    uint32_t file_idx = slot % N;
    for (uint8_t sym = 0; sym < 14; sym++) {
      std::vector<uint8_t> payload(oracle[file_idx].grid_payload.begin() + sym * symbol_bytes,
                                   oracle[file_idx].grid_payload.begin() + (sym + 1) * symbol_bytes);
      clean_frames.push_back(build_frame(sym, payload));
    }
  }
  std::vector<std::vector<uint8_t>> frames_dl_first;
  frames_dl_first.push_back(foreign_frame);
  frames_dl_first.insert(frames_dl_first.end(), clean_frames.begin(), clean_frames.end());
  std::string dl_noise_path = "/tmp/pcap_comparator_test_dl_noise.pcap";
  write_pcap(dl_noise_path, frames_dl_first);
  int rc3 = -1;
  std::string out3 = run_and_capture("./build/pcap_comparator " + dl_noise_path + " " + osg_dir + " " +
                                     std::to_string(N) + " " + kRuMacStr + " " + std::to_string(OI_WIRE_UDCOMPHDR_BYTES_ABSENT) + " 2>&1",
                                     &rc3);
  check(rc3 == 0,
       "pcap_comparator exits 0 on a stream with a leading non-matching-payload DU-sourced frame "
       "(direction filter must skip it, not calibrate against it) (output: " + out3 + ")");
  check(out3.find("\"mismatches\":0") != std::string::npos,
       "pcap_comparator reports 0 mismatches with DL noise present (output: " + out3 + ")");
  check(out3.find("\"non_ru_src\":1") != std::string::npos,
       "pcap_comparator's non_ru_src counter correctly counts exactly the one DU-sourced frame (output: " + out3 + ")");

  // --- Real bug regression #2 (found live on GCP 2026-07-26, immediately after the direction-
  // filter fix above): this comparator used OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len) directly,
  // silently assuming the udCompHdr+reserved field is always absent. Real ru_emulator-sourced
  // frames always carry it (OI_WIRE_UDCOMPHDR_BYTES_PRESENT, 2 bytes) -- confirmed byte-for-byte
  // against two independent real corpora (see oi_oran_wire_layout.h's header comment). Build a
  // stream with the 2-byte gap present and confirm the comparator (given the correct
  // udcomphdr_bytes=2 argument) still calibrates and reports 0 mismatches -- the exact scenario
  // that used to fail with "calibration failed on first U-plane frame" even after the direction
  // fix, since every candidate comparison was reading 2 bytes into what should have been payload.
  std::vector<std::vector<uint8_t>> dynamic_frames;
  for (uint32_t slot = 0; slot < 5; slot++) {
    uint32_t file_idx = slot % N;
    for (uint8_t sym = 0; sym < 14; sym++) {
      std::vector<uint8_t> payload(oracle[file_idx].grid_payload.begin() + sym * symbol_bytes,
                                   oracle[file_idx].grid_payload.begin() + (sym + 1) * symbol_bytes);
      dynamic_frames.push_back(build_frame(sym, payload, kRuMac, OI_WIRE_UDCOMPHDR_BYTES_PRESENT));
    }
  }
  std::string dynamic_path = "/tmp/pcap_comparator_test_dynamic.pcap";
  write_pcap(dynamic_path, dynamic_frames);
  int rc4 = -1;
  std::string out4 = run_and_capture("./build/pcap_comparator " + dynamic_path + " " + osg_dir + " " +
                                     std::to_string(N) + " " + kRuMacStr + " " +
                                     std::to_string(OI_WIRE_UDCOMPHDR_BYTES_PRESENT) + " 2>&1",
                                     &rc4);
  check(rc4 == 0,
       "pcap_comparator exits 0 against a stream with the real rig's 2-byte udCompHdr gap present, "
       "given the correct udcomphdr_bytes=2 argument (output: " + out4 + ")");
  check(out4.find("\"mismatches\":0") != std::string::npos,
       "pcap_comparator reports 0 mismatches with the udCompHdr gap correctly accounted for (output: " + out4 + ")");

  // Negative control: the SAME dynamic-layout stream, but told udcomphdr_bytes=0 (the pre-fix,
  // wrong assumption) -- must fail exactly the way the real bug did, proving this test would have
  // caught the original bug rather than just exercising the fixed code path by construction.
  int rc5 = -1;
  std::string out5 = run_and_capture("./build/pcap_comparator " + dynamic_path + " " + osg_dir + " " +
                                     std::to_string(N) + " " + kRuMacStr + " " +
                                     std::to_string(OI_WIRE_UDCOMPHDR_BYTES_ABSENT) + " 2>&1",
                                     &rc5);
  check(rc5 != 0,
       "pcap_comparator FAILS against the same stream when given the wrong (pre-fix) "
       "udcomphdr_bytes=0 -- proves this regression test actually exercises the bug (output: " + out5 + ")");

  if (g_fail == 0) {
    std::printf("\npcap_comparator_test: ALL PASS\n");
  } else {
    std::fprintf(stderr, "\npcap_comparator_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
