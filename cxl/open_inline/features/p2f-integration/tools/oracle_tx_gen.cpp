// oracle_tx_gen.cpp — P2-R15b oracle-vector generator. Builds a REAL, wire-valid, fully-encoded
// PUSCH-like UL transmission for one of the MVP's three MCS points and packs it into a real .pcap
// file (14 eCPRI+O-RAN U-plane frames, one per OFDM symbol) plus a JSON sidecar recording the
// known ground-truth transport block. `pcap_packer.py` invokes this binary.
//
// 2026-07-26 refactor (p3-live-tap-ul-inject build, LLD Q1 resolution): the TB->RE-grid packing
// (former steps 1-6: random TB, segment/LDPC-encode/rate-match, scramble, modulate, RE-map) moved
// to the shared oi_oracle_pack library (src/host/oi_oracle_pack.{h,cpp}) so p3's .osg oracle-grid
// generator uses the exact same real-OCUDU-grounded logic instead of a second, potentially-
// drifting implementation of the same "51 PRB x 14 symbols x 12 subcarriers x (16-bit I, 16-bit
// Q)" byte layout. This file now owns only the pcap-frame-specific front end: building full
// eCPRI+O-RAN wire frames from oi_oracle::pack_tb's RE grid (below), self-verifying them by real
// decode, and writing the .pcap + JSON sidecar. See oi_oracle_pack.h/.cpp for the TX-chain
// grounding citations (mirrors this file's original header, extraction is verbatim, not
// reimplemented).
//
// Self-verification (built into this binary, not deferred to pipeline_test.py): after building
// the 14 wire frames, decodes every one of them with the REAL OCUDU eCPRI+O-RAN decoder and
// checks the recovered IQ (data REs and DMRS pilot REs) matches what was encoded, mod bf16
// storage rounding (same comparison basis as p2c-k1/tests/k1_test.cpp). Exits nonzero if this
// self-check fails — an oracle that hasn't verified its own wire frames is not trustworthy enough
// to gate anything downstream.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/host/oi_oracle_pack.h"
#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"

#include "ocudu/adt/bf16.h"
#include "ocudu/adt/complex.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/ecpri/ecpri_factories.h"
#include "ocudu/ofh/serdes/ofh_serdes_factories.h"

using namespace ocudu;
using namespace ocudu::ofh;

namespace {

std::string to_hex(const std::vector<uint8_t>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xF]);
  }
  return out;
}

// Builds one real wire-valid frame (Ethernet(placeholder, see oi_oran_wire_layout.h) + eCPRI(real)
// + O-RAN CUS static/none(real)) for the given symbol_id, carrying the given 612 IQ values.
// Mirrors p2c-k1/tests/k1_test.cpp's build_real_frame exactly (including its direction-bit
// workaround, read verbatim from ofh_uplane_message_builder_impl.cpp's encode_data_direction()),
// generalized to take real (not random) IQ. Not part of the shared oi_oracle_pack library: this is
// full eCPRI+O-RAN+pcap framing, which only p2f needs (p3's ru_emulator patch does this framing
// itself, via its own real uplane_message_builder, at injection time — see p3 HLD D1/D2).
std::vector<uint8_t> build_wire_frame(uplane_message_builder& uplane_builder, ecpri::packet_builder& ecpri_builder,
                                      uint8_t symbol_id, const std::vector<cbf16_t>& iq) {
  ru_compression_params compr_params{compression_type::none, 16};
  units::bytes oran_header_size = uplane_builder.get_header_size(compr_params);
  std::vector<uint8_t> oran_buf(oran_header_size.value() + oi_oracle::kNofSubcarriers * 4, 0);

  uplane_message_params params{};
  params.direction = data_direction::uplink;
  params.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, 0);
  params.filter_index = filter_index_type::standard_channel_filter;
  params.start_prb = 0;
  params.nof_prb = oi_oracle::kNofPrb;
  params.symbol_id = symbol_id;
  params.sect_type = section_type::type_1;
  params.compression_params = compr_params;

  unsigned oran_bytes = uplane_builder.build_message(span<uint8_t>(oran_buf), span<const cbf16_t>(iq), params);

  // WORKAROUND (verbatim from k1_test.cpp): the real builder hardcodes direction=downlink in the
  // wire byte regardless of params.direction (it is meant for O-DU->O-RU tx only); flip the one
  // known bit (byte0, bit7) so the uplink-only decoder we self-verify against accepts it.
  oran_buf[0] &= 0x7Fu;

  units::bytes ecpri_header_size = ecpri_builder.get_header_size(ecpri::message_type::iq_data);
  std::vector<uint8_t> full(OI_WIRE_ETH_HEADER_BYTES + ecpri_header_size.value() + oran_bytes, 0);
  std::memcpy(full.data() + OI_WIRE_ETH_HEADER_BYTES + ecpri_header_size.value(), oran_buf.data(), oran_bytes);

  ecpri::iq_data_parameters iq_params{/*pc_id=*/1, /*seq_id=*/symbol_id};
  ecpri_builder.build_data_packet(
      span<uint8_t>(full).subspan(OI_WIRE_ETH_HEADER_BYTES, ecpri_header_size.value() + oran_bytes), iq_params);

  full[12] = 0xAE;
  full[13] = 0xFE;  // EtherType (placeholder Ethernet layer, see oi_oran_wire_layout.h)
  return full;
}

// Minimal raw libpcap writer (global header + per-packet records) -- a plain, standardized binary
// container format, not OCUDU-adjacent expression; safe to write directly with no library.
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
    uint32_t ts_sec = 0, ts_usec = (uint32_t)i;  // deterministic, not wall-clock (reproducibility)
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
  if (argc < 5) {
    std::fprintf(stderr, "usage: oracle_tx_gen <mcs_index> <seed> <out_pcap> <out_json>\n");
    return 2;
  }
  uint32_t mcs_index = (uint32_t)std::atoi(argv[1]);
  uint32_t seed = (uint32_t)std::atoi(argv[2]);
  std::string out_pcap = argv[3];
  std::string out_json = argv[4];

  // Fixed MVP scrambling identity (fixtures/mvp_config.yaml: scrambling.rnti=0x4601, n_id=1);
  // nslot=0 -- matches a fresh oi_oran_preparse_state's first-slot derivation (slot_id starts at
  // 0, confirmed via p2a-scaffold/tests/preparse_test.cpp), so this oracle's frames are consumed
  // as slot_id 0 by the real pipeline with no extra bookkeeping needed.
  const uint32_t rnti = 0x4601u;
  const uint32_t n_id = 1u;
  const uint32_t nslot = 0u;

  const oi_oracle::mcs_point* mcs = oi_oracle::find_mcs(mcs_index);
  if (!mcs) {
    std::fprintf(stderr, "oracle_tx_gen: unknown mcs_index %u (expected 4, 13, or 21)\n", mcs_index);
    return 2;
  }

  // --- 1-6: shared packer (TB -> RE grid), see oi_oracle_pack.h ---
  oi_oracle::packed_tb packed = oi_oracle::pack_tb(mcs_index, seed, rnti, n_id, nslot);

  // --- 7. Build 14 real wire frames (pcap-specific front end) ---
  ocudulog::init();
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("ORACLE_TX_GEN");
  logger.set_level(ocudulog::basic_levels::error);
  auto compressor = create_iq_compressor(compression_type::none, logger, 1.0f, "generic");
  auto uplane_builder = create_static_compr_method_ofh_user_plane_packet_builder(logger, *compressor);
  auto ecpri_builder = ecpri::create_ecpri_packet_builder();

  std::vector<std::vector<uint8_t>> frames(oi_oracle::kNofSymbols);
  for (unsigned s = 0; s < oi_oracle::kNofSymbols; s++) {
    frames[s] = build_wire_frame(*uplane_builder, *ecpri_builder, (uint8_t)s, packed.re_grid[s]);
  }

  // --- 8. Self-verification: decode every frame with the REAL OCUDU decoder, check recovered IQ
  // matches what was encoded (mod bf16 storage), before trusting this oracle for anything else ---
  {
    auto decompressor = create_iq_decompressor(compression_type::none, logger, "generic");
    ru_compression_params compr_params{compression_type::none, 16};
    auto uplane_decoder = create_static_compr_method_ofh_user_plane_packet_decoder(
        logger, subcarrier_spacing::kHz30, cyclic_prefix{}, oi_oracle::kNofPrb, /*sector_id=*/0,
        std::move(decompressor), compr_params);
    auto ecpri_decoder = ecpri::create_ecpri_packet_decoder_ignoring_payload_size(logger, /*sector=*/0);

    for (unsigned s = 0; s < oi_oracle::kNofSymbols; s++) {
      span<const uint8_t> after_eth(frames[s].data() + OI_WIRE_ETH_HEADER_BYTES,
                                    frames[s].size() - OI_WIRE_ETH_HEADER_BYTES);
      ecpri::packet_parameters ecpri_params;
      span<const uint8_t> oran_payload = ecpri_decoder->decode(after_eth, ecpri_params);
      if (oran_payload.empty()) {
        std::fprintf(stderr, "oracle_tx_gen self-check FAILED: eCPRI decode empty at symbol %u\n", s);
        return 1;
      }
      uplane_message_decoder_results results;
      if (!uplane_decoder->decode(results, oran_payload) || results.sections.empty()) {
        std::fprintf(stderr, "oracle_tx_gen self-check FAILED: U-plane decode failed at symbol %u\n", s);
        return 1;
      }
      for (unsigned sc = 0; sc < oi_oracle::kNofSubcarriers; sc++) {
        cbf16_t expected = packed.re_grid[s][sc];
        cbf16_t got = results.sections[0].iq_samples[sc];
        if (got.real != expected.real || got.imag != expected.imag) {
          std::fprintf(stderr, "oracle_tx_gen self-check FAILED: IQ mismatch at symbol %u subcarrier %u\n", s, sc);
          return 1;
        }
      }
    }
  }

  // --- 9. Write outputs ---
  write_pcap(out_pcap, frames);

  std::ofstream jf(out_json);
  jf << "{\n"
    << "  \"schema\": \"oi-p2-oracle/1\",\n"
    << "  \"mcs_index\": " << mcs->mcs_index << ",\n"
    << "  \"qm\": " << mcs->qm << ",\n"
    << "  \"tbs_bits\": " << mcs->tbs_bits << ",\n"
    << "  \"tb_size_bytes\": " << packed.tb_bytes.size() << ",\n"
    << "  \"nof_cb\": " << packed.nof_cb << ",\n"
    << "  \"base_graph\": " << packed.base_graph << ",\n"
    << "  \"rnti\": " << rnti << ",\n"
    << "  \"n_id\": " << n_id << ",\n"
    << "  \"nslot\": " << nslot << ",\n"
    << "  \"seed\": " << seed << ",\n"
    << "  \"tb_bytes_hex\": \"" << to_hex(packed.tb_bytes) << "\"\n"
    << "}\n";

  std::fprintf(stderr, "oracle_tx_gen: OK — MCS %u, %u CB(s), BG%u, self-check passed, wrote %s + %s\n",
              mcs->mcs_index, packed.nof_cb, packed.base_graph, out_pcap.c_str(), out_json.c_str());
  return 0;
}
