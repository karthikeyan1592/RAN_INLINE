// synth_ecpri_gen.cpp — builds a small, canned eCPRI+O-RAN CUS pcap with a KNOWN frame
// composition, for assert_ecpri.sh's classifier to be unit-tested against (P1-R8's test plan:
// "classifier unit-tested against a canned pcap with known composition, tagged and untagged
// variants"). Real eCPRI/O-RAN CUS frames throughout via OCUDU's own real, factory-constructed
// builders (ecpri::packet_builder, ofh::uplane_message_builder, ofh::cplane_message_builder) --
// no hand-rolled protocol bytes, same discipline as p2f-integration/tools/oracle_tx_gen.cpp
// (nothing here is real O-RAN payload correctness, just enough of a real, well-formed frame for
// the eCPRI-common-header + O-RAN direction-field bytes the classifier actually reads to be
// genuine, not guessed).
//
// Composition (fixed, documented, asserted by the caller — tests/fixtures/synth_ecpri.json):
//   2x C-plane DL (msg type 0x02, src=DU MAC)
//   2x U-plane DL (msg type 0x00, src=DU MAC), untagged
//   1x U-plane DL (msg type 0x00, src=DU MAC), 802.1Q VLAN-tagged (TCI=1) -- exercises Q1's
//     "classifier handles 802.1Q" requirement
//   2x U-plane UL (msg type 0x00, src=RU MAC), untagged
//   0x C-plane UL (Q3: "c_ul is observed-and-recorded only" -- this fixture intentionally has
//     none, so the classifier's negative path (a class genuinely absent) is exercised too)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ocudu/adt/bf16.h"
#include "ocudu/adt/complex.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/ecpri/ecpri_factories.h"
#include "ocudu/ofh/serdes/ofh_serdes_factories.h"

using namespace ocudu;
using namespace ocudu::ofh;

namespace {

constexpr unsigned kNofPrb = 51;
constexpr unsigned kNofSubcarriers = kNofPrb * 12;
// Fronthaul plan MACs (IF-P1-FRONTHAUL, matches docker/configs/*.yml).
const uint8_t kDuMac[6] = {0x02, 0x6f, 0x69, 0x00, 0x01, 0x02};
const uint8_t kRuMac[6] = {0x02, 0x6f, 0x69, 0x00, 0x01, 0x01};

void write_eth_header(std::vector<uint8_t>& frame, const uint8_t* dst, const uint8_t* src, bool vlan_tagged) {
  frame.insert(frame.end(), dst, dst + 6);
  frame.insert(frame.end(), src, src + 6);
  if (vlan_tagged) {
    frame.push_back(0x81);
    frame.push_back(0x00);  // TPID 0x8100
    frame.push_back(0x00);
    frame.push_back(0x01);  // TCI: PCP=0, DEI=0, VID=1 (matches vlan_tag_cp/up=1 in configs)
  }
  frame.push_back(0xAE);
  frame.push_back(0xFE);  // EtherType 0xAEFE
}

std::vector<uint8_t> build_uplane_frame(uplane_message_builder& uplane_builder, ecpri::packet_builder& ecpri_builder,
                                        bool downlink, bool vlan_tagged, uint16_t seq_id) {
  std::vector<cbf16_t> iq(kNofSubcarriers, cbf16_t(0.1f, -0.1f));  // content irrelevant to the classifier

  ru_compression_params compr_params{compression_type::none, 16};
  units::bytes oran_header_size = uplane_builder.get_header_size(compr_params);
  std::vector<uint8_t> oran_buf(oran_header_size.value() + kNofSubcarriers * 4, 0);

  uplane_message_params params{};
  params.direction = downlink ? data_direction::downlink : data_direction::uplink;
  params.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, 0);
  params.filter_index = filter_index_type::standard_channel_filter;
  params.start_prb = 0;
  params.nof_prb = kNofPrb;
  params.symbol_id = 0;
  params.sect_type = section_type::type_1;
  params.compression_params = compr_params;
  unsigned oran_bytes = uplane_builder.build_message(span<uint8_t>(oran_buf), span<const cbf16_t>(iq), params);
  // NOTE: unlike p2f's oracle_tx_gen.cpp, no direction-bit workaround is needed here -- this
  // fixture is never decoded by OCUDU's own uplane_message_decoder (which hardcodes rejecting
  // non-uplink frames), only by assert_ecpri.sh's byte-offset classifier, which reads the
  // eCPRI/Ethernet layers, not the O-RAN direction bit.

  std::vector<uint8_t> frame;
  write_eth_header(frame, downlink ? kRuMac : kDuMac, downlink ? kDuMac : kRuMac, vlan_tagged);

  units::bytes ecpri_header_size = ecpri_builder.get_header_size(ecpri::message_type::iq_data);
  size_t eth_len = frame.size();
  frame.resize(eth_len + ecpri_header_size.value() + oran_bytes, 0);
  std::memcpy(frame.data() + eth_len + ecpri_header_size.value(), oran_buf.data(), oran_bytes);

  ecpri::iq_data_parameters iq_params{/*pc_id=*/1, seq_id};
  ecpri_builder.build_data_packet(span<uint8_t>(frame).subspan(eth_len, ecpri_header_size.value() + oran_bytes),
                                  iq_params);
  return frame;
}

std::vector<uint8_t> build_cplane_frame(cplane_message_builder& cplane_builder, ecpri::packet_builder& ecpri_builder,
                                        uint16_t seq_id) {
  cplane_section_type1_parameters params{};
  params.radio_hdr.direction = data_direction::downlink;
  params.radio_hdr.filter_index = filter_index_type::standard_channel_filter;
  params.radio_hdr.start_symbol = 0;
  params.radio_hdr.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, 0);
  params.section_fields.common_fields.section_id = 0;
  params.section_fields.common_fields.prb_start = 0;
  params.section_fields.common_fields.nof_prb = kNofPrb;
  params.section_fields.common_fields.re_mask = 0xFFF;
  params.section_fields.common_fields.nof_symbols = 1;
  params.compr_params = ru_compression_params{compression_type::none, 16};

  std::vector<uint8_t> oran_buf(256, 0);  // generously sized; build_* returns the actual length
  unsigned oran_bytes = cplane_builder.build_dl_ul_radio_channel_message(span<uint8_t>(oran_buf), params);

  std::vector<uint8_t> frame;
  write_eth_header(frame, kRuMac, kDuMac, /*vlan_tagged=*/false);  // DL C-plane: DU->RU

  units::bytes ecpri_header_size = ecpri_builder.get_header_size(ecpri::message_type::rt_control_data);
  size_t eth_len = frame.size();
  frame.resize(eth_len + ecpri_header_size.value() + oran_bytes, 0);
  std::memcpy(frame.data() + eth_len + ecpri_header_size.value(), oran_buf.data(), oran_bytes);

  ecpri::realtime_control_parameters rtc_params{/*rtc_id=*/1, seq_id};
  ecpri_builder.build_control_packet(span<uint8_t>(frame).subspan(eth_len, ecpri_header_size.value() + oran_bytes),
                                     rtc_params);
  return frame;
}

void write_pcap(const std::string& path, const std::vector<std::vector<uint8_t>>& frames) {
  std::ofstream f(path, std::ios::binary);
  uint32_t magic = 0xa1b2c3d4u;
  uint16_t ver_major = 2, ver_minor = 4;
  int32_t thiszone = 0;
  uint32_t sigfigs = 0, snaplen = 65535, network = 1;  // LINKTYPE_ETHERNET
  f.write((const char*)&magic, 4);
  f.write((const char*)&ver_major, 2);
  f.write((const char*)&ver_minor, 2);
  f.write((const char*)&thiszone, 4);
  f.write((const char*)&sigfigs, 4);
  f.write((const char*)&snaplen, 4);
  f.write((const char*)&network, 4);
  for (size_t i = 0; i < frames.size(); i++) {
    uint32_t ts_sec = 0, ts_usec = (uint32_t)i;
    uint32_t incl_len = (uint32_t)frames[i].size(), orig_len = incl_len;
    f.write((const char*)&ts_sec, 4);
    f.write((const char*)&ts_usec, 4);
    f.write((const char*)&incl_len, 4);
    f.write((const char*)&orig_len, 4);
    f.write((const char*)frames[i].data(), frames[i].size());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: synth_ecpri_gen <out_pcap> <out_json>\n");
    return 2;
  }
  std::string out_pcap = argv[1];
  std::string out_json = argv[2];

  ocudulog::init();
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("SYNTH_ECPRI_GEN");
  logger.set_level(ocudulog::basic_levels::error);
  auto compressor = create_iq_compressor(compression_type::none, logger, 1.0f, "generic");
  auto uplane_builder = create_static_compr_method_ofh_user_plane_packet_builder(logger, *compressor);
  auto cplane_builder = create_ofh_control_plane_static_compression_message_builder();
  auto ecpri_builder = ecpri::create_ecpri_packet_builder();

  std::vector<std::vector<uint8_t>> frames;
  uint16_t seq = 0;
  for (int i = 0; i < 2; i++) frames.push_back(build_cplane_frame(*cplane_builder, *ecpri_builder, seq++));
  for (int i = 0; i < 2; i++) frames.push_back(build_uplane_frame(*uplane_builder, *ecpri_builder, true, false, seq++));
  frames.push_back(build_uplane_frame(*uplane_builder, *ecpri_builder, true, true, seq++));   // VLAN-tagged DL
  for (int i = 0; i < 2; i++) frames.push_back(build_uplane_frame(*uplane_builder, *ecpri_builder, false, false, seq++));

  write_pcap(out_pcap, frames);

  std::ofstream jf(out_json);
  jf << "{\n"
    << "  \"schema\": \"oi-p1-synth-ecpri/1\",\n"
    << "  \"frames\": " << frames.size() << ",\n"
    << "  \"c_dl\": 2,\n"
    << "  \"c_ul\": 0,\n"
    << "  \"u_dl\": 3,\n"
    << "  \"u_ul\": 2,\n"
    << "  \"vlan_tagged_frames\": 1\n"
    << "}\n";

  std::fprintf(stderr, "synth_ecpri_gen: OK — wrote %zu frames (2 c_dl, 3 u_dl [1 vlan-tagged], 2 u_ul) to %s + %s\n",
              frames.size(), out_pcap.c_str(), out_json.c_str());
  return 0;
}
