// oracle_tx_gen.cpp — P2-R15b oracle-vector generator. Builds a REAL, wire-valid, fully-encoded
// PUSCH-like UL transmission for one of the MVP's three MCS points, entirely from real linked
// OCUDU components (no hand-crafted/guessed bytes anywhere in the TX chain), and packs it into a
// real .pcap file (14 eCPRI+O-RAN U-plane frames, one per OFDM symbol) plus a JSON sidecar
// recording the known ground-truth transport block. `pcap_packer.py` invokes this binary; it does
// not reimplement any of this logic in Python (the wire-encoding logic already exists correctly
// here and in OCUDU, re-implementing it in a second, unverified language would be exactly the kind
// of avoidable provenance/correctness risk this project has repeatedly chosen not to take).
//
// TX-chain recipe (grounded, not guessed): mirrors lib/phy/upper/channel_processors/pdsch/
// pdsch_encoder_impl.cpp (segment -> per-CB LDPC-encode -> rate-match -> concatenate) and
// pdsch_modulator_impl.cpp (scramble -> modulate) read-only, since OCUDU has no PUSCH *encoder*
// (it is gNB-side/RX-only for the uplink) but the TS 38.212/38.211 procedures PDSCH's real TX
// chain implements are the same bit-level procedures a UE's PUSCH TX chain would use — consulted
// read-only for the call sequence (HLD D9 precedent, same as K1's read-only consultation of
// OCUDU's O-RAN decoder for field semantics), not ported code. Every individual primitive
// (segmenter, encoder, rate matcher, scrambler, modulator, eCPRI/O-RAN builders) is the real,
// unmodified, factory-constructed OCUDU implementation — nothing here reimplements a 3GPP
// procedure by hand. Segmentation sizing itself reuses this project's own already-verified
// oi_p2_cb_segment (30/30 assertions, see VERIFICATION.md) rather than re-deriving base_graph.
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
#include <random>
#include <string>
#include <vector>

#include "../src/host/oi_p2_cb_segment.h"
#include "../../p2d-k2-k3/src/host/oi_dmrs_ref_seq.h"
#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"

#include "ocudu/adt/bf16.h"
#include "ocudu/adt/complex.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/bit.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/ecpri/ecpri_factories.h"
#include "ocudu/ofh/serdes/ofh_serdes_factories.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_segmenter_buffer.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"

using namespace ocudu;
using namespace ocudu::ofh;

namespace {

// This MVP's fixed dimensions (must match p2a-scaffold/src/host/oi_p2_buffers.h and
// oi_p2_host.cpp's kMcsTable/kDmrsSymbols/kDataSymbols exactly -- duplicated here per this
// project's existing convention of small, clearly cross-referenced constant tables in each
// oracle/test file rather than a shared header, same as cb_segment_test.cpp's own kNofDataRe).
constexpr unsigned kNofPrb = 51;
constexpr unsigned kNofSubcarriers = kNofPrb * 12;  // 612
constexpr unsigned kNofSymbols = 14;
constexpr unsigned kDmrsSymbols[3] = {2, 7, 11};
constexpr unsigned kDataSymbols[11] = {0, 1, 3, 4, 5, 6, 8, 9, 10, 12, 13};
constexpr unsigned kNofDataRe = 11 * kNofSubcarriers;  // 6732

struct McsPoint {
  uint32_t mcs_index;
  uint32_t qm;
  uint32_t tbs_bits;
  float code_rate;
};
constexpr McsPoint kMcsTable[3] = {
    {4, 2, 4608, 0.3008f},
    {13, 4, 14600, 0.4785f},
    {21, 6, 27656, 0.6016f},
};

const McsPoint* find_mcs(uint32_t mcs_index) {
  for (const auto& m : kMcsTable) {
    if (m.mcs_index == mcs_index) return &m;
  }
  return nullptr;
}

modulation_scheme qm_to_scheme(uint32_t qm) {
  return (qm == 2) ? modulation_scheme::QPSK : (qm == 4) ? modulation_scheme::QAM16 : modulation_scheme::QAM64;
}

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
// generalized to take real (not random) IQ.
std::vector<uint8_t> build_wire_frame(uplane_message_builder& uplane_builder, ecpri::packet_builder& ecpri_builder,
                                      uint8_t symbol_id, const std::vector<cbf16_t>& iq) {
  ru_compression_params compr_params{compression_type::none, 16};
  units::bytes oran_header_size = uplane_builder.get_header_size(compr_params);
  std::vector<uint8_t> oran_buf(oran_header_size.value() + kNofSubcarriers * 4, 0);

  uplane_message_params params{};
  params.direction = data_direction::uplink;
  params.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, 0);
  params.filter_index = filter_index_type::standard_channel_filter;
  params.start_prb = 0;
  params.nof_prb = kNofPrb;
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

  const McsPoint* mcs = find_mcs(mcs_index);
  if (!mcs) {
    std::fprintf(stderr, "oracle_tx_gen: unknown mcs_index %u (expected 4, 13, or 21)\n", mcs_index);
    return 2;
  }

  // --- 1. Random transport block ---
  unsigned tb_bytes = (mcs->tbs_bits + 7) / 8;
  std::vector<uint8_t> tb(tb_bytes);
  std::mt19937 rgen(seed);
  for (auto& b : tb) b = (uint8_t)(rgen() & 0xFF);

  // --- 2. Segmentation sizing (this project's own verified module, not re-derived here) ---
  oi_p2_cb_segment_params seg{};
  oi_p2_cb_segment_compute(mcs->tbs_bits, mcs->code_rate, &seg);

  // --- 3. Real OCUDU segmenter/encoder/rate-matcher chain (grounded in pdsch_encoder_impl.cpp,
  // see file header) ---
  auto crc_factory = create_crc_calculator_factory_sw("generic");
  auto seg_factory = create_ldpc_segmenter_tx_factory_sw(crc_factory);
  auto segmenter = seg_factory->create();
  auto enc_factory = create_ldpc_encoder_factory_sw("generic");
  auto encoder = enc_factory->create();
  auto rm_factory = create_ldpc_rate_matcher_factory_sw();
  auto rate_matcher = rm_factory->create();

  segmenter_config cfg{};
  cfg.base_graph = (seg.base_graph == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
  cfg.rv = 0;
  cfg.mod = qm_to_scheme(mcs->qm);
  cfg.Nref = 0;
  cfg.nof_layers = 1;
  cfg.nof_ch_symbols = kNofDataRe;

  const ldpc_segmenter_buffer& seg_buf = segmenter->new_transmission(span<const uint8_t>(tb), cfg);
  if (seg_buf.get_nof_codeblocks() != seg.nof_segments) {
    std::fprintf(stderr, "oracle_tx_gen: real segmenter nof_codeblocks=%u != oi_p2_cb_segment's %u\n",
                 seg_buf.get_nof_codeblocks(), seg.nof_segments);
    return 1;
  }

  std::vector<uint8_t> unpacked_codeword(kNofDataRe * mcs->qm, 0);
  unsigned offset = 0;
  for (unsigned cb = 0; cb < seg_buf.get_nof_codeblocks(); cb++) {
    codeblock_metadata cb_meta = seg_buf.get_cb_metadata(cb);

    dynamic_bit_buffer cb_data(seg_buf.get_segment_length().value());
    seg_buf.read_codeblock(cb_data, span<const uint8_t>(tb), cb);

    ldpc_encoder::configuration enc_cfg;
    enc_cfg.base_graph = cb_meta.tb_common.base_graph;
    enc_cfg.lifting_size = cb_meta.tb_common.lifting_size;
    enc_cfg.Nref = cb_meta.tb_common.Nref;
    const ldpc_encoder_buffer& rm_buf_enc = encoder->encode(cb_data, enc_cfg);

    unsigned rm_length = seg_buf.get_rm_length(cb);
    dynamic_bit_buffer rm_out(rm_length);
    rate_matcher->rate_match(rm_out, rm_buf_enc, cb_meta);

    span<uint8_t> dst = span<uint8_t>(unpacked_codeword).subspan(offset, rm_length);
    ocuduvec::bit_unpack(dst, rm_out);
    offset += rm_length;
  }
  if (offset != kNofDataRe * mcs->qm) {
    std::fprintf(stderr, "oracle_tx_gen: concatenated codeword length %u != expected %u\n", offset,
                 kNofDataRe * mcs->qm);
    return 1;
  }

  // --- 4. Scramble (real OCUDU pseudo_random_generator; c_init = rnti*32768 + n_id, matching
  // pdsch_modulator_impl.cpp's c_init = rnti<<15 + q<<14 + n_id with q=0 for our single codeword
  // -- the same formula already used and verified for K5's descrambling side, oi_p2_host.cpp) ---
  dynamic_bit_buffer packed(unpacked_codeword.size());
  ocuduvec::bit_pack(packed, span<const uint8_t>(unpacked_codeword));

  uint32_t c_init = (rnti * 32768u + n_id) & 0x7FFFFFFFu;
  auto rng_factory = create_pseudo_random_generator_sw_factory();
  auto scrambler = rng_factory->create();
  scrambler->init(c_init);
  dynamic_bit_buffer scrambled(unpacked_codeword.size());
  scrambler->apply_xor(scrambled, packed);

  // --- 5. Modulate (real OCUDU modulation_mapper) ---
  auto mod_factory = create_modulation_mapper_factory();
  auto modulator = mod_factory->create();
  unsigned nof_re = (unsigned)unpacked_codeword.size() / mcs->qm;
  if (nof_re != kNofDataRe) {
    std::fprintf(stderr, "oracle_tx_gen: nof_re %u != kNofDataRe %u\n", nof_re, kNofDataRe);
    return 1;
  }
  std::vector<cf_t> symbols(nof_re);
  modulator->modulate(span<cf_t>(symbols), scrambled, qm_to_scheme(mcs->qm));

  // Real, found-not-guessed finding: OCUDU's real "static compression, none" wire codec
  // (iq_compression_none_impl, constructed with iq_scaling=1.0 -- see file's build_wire_frame,
  // matching k1_test.cpp's own precedent) quantizes floats to fixed-point assuming a unit
  // full-scale range; 64-QAM's outer constellation points (amplitude 7/sqrt(42) ~= 1.0801) exceed
  // that range and get silently clipped to 1.0 on decode -- caught by this generator's own
  // self-check (section 8 below), not assumed away. QPSK (max ~0.707) and 16-QAM (max ~0.949)
  // never hit this, which is why no earlier test in this project (all using amplitudes < 1.0, e.g.
  // k1_test.cpp's dist(-0.9,0.9)) ever exercised it. Fix: apply a conservative TX amplitude scale
  // to BOTH data and DMRS before wire-encoding, matching a real system's transmit power scaling --
  // NOT a change to K3's tx_scaling/oi_p2_host.cpp's beta_scaling (those stay 1.0f): the equalizer
  // recovers the true symbol regardless of any common scale applied identically to data and DMRS,
  // because channel estimation (h_est = rx_pilot / known_unscaled_ref) absorbs the same factor the
  // scaled data carries, and K3's y*conj(h)/(tx_scaling*|h|^2) cancels it out algebraically -- this
  // is exactly the point of pilot-based channel estimation being blind to a common TX gain.
  constexpr float kTxAmplitudeScale = 0.9f;  // 0.9 * 1.0801 = 0.972 < 1.0, safe margin
  for (auto& sym : symbols) sym *= kTxAmplitudeScale;

  // --- 6. RE-map: 11 data-symbol rows (symbol-major/subcarrier-minor, matching K3's
  // re_grid_compact convention) + 3 DMRS rows (comb-2 pilot pattern, matching K2a's read pattern:
  // re_grid[dmrs_symbol][2*p] for pilot index p) ---
  std::vector<std::vector<cbf16_t>> re_grid(kNofSymbols, std::vector<cbf16_t>(kNofSubcarriers, cbf16_t(0.0f, 0.0f)));
  for (unsigned lin = 0; lin < 11; lin++) {
    for (unsigned sc = 0; sc < kNofSubcarriers; sc++) {
      re_grid[kDataSymbols[lin]][sc] = cbf16_t(symbols[lin * kNofSubcarriers + sc]);
    }
  }
  std::vector<std::vector<oi_cf32>> dmrs_ref(3, std::vector<oi_cf32>(306));
  for (int i = 0; i < 3; i++) {
    oi_dmrs_ref_seq_generate(nslot, kDmrsSymbols[i], n_id, /*n_scid=*/0, dmrs_ref[i].data(), kNofPrb);
    for (auto& v : dmrs_ref[i]) {
      v.re *= kTxAmplitudeScale;
      v.im *= kTxAmplitudeScale;
    }
    for (unsigned p = 0; p < 306; p++) {
      re_grid[kDmrsSymbols[i]][2 * p] = cbf16_t(dmrs_ref[i][p].re, dmrs_ref[i][p].im);
    }
  }

  // --- 7. Build 14 real wire frames ---
  ocudulog::init();
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("ORACLE_TX_GEN");
  logger.set_level(ocudulog::basic_levels::error);
  auto compressor = create_iq_compressor(compression_type::none, logger, 1.0f, "generic");
  auto uplane_builder = create_static_compr_method_ofh_user_plane_packet_builder(logger, *compressor);
  auto ecpri_builder = ecpri::create_ecpri_packet_builder();

  std::vector<std::vector<uint8_t>> frames(kNofSymbols);
  for (unsigned s = 0; s < kNofSymbols; s++) {
    frames[s] = build_wire_frame(*uplane_builder, *ecpri_builder, (uint8_t)s, re_grid[s]);
  }

  // --- 8. Self-verification: decode every frame with the REAL OCUDU decoder, check recovered IQ
  // matches what was encoded (mod bf16 storage), before trusting this oracle for anything else ---
  {
    auto decompressor = create_iq_decompressor(compression_type::none, logger, "generic");
    ru_compression_params compr_params{compression_type::none, 16};
    auto uplane_decoder = create_static_compr_method_ofh_user_plane_packet_decoder(
        logger, subcarrier_spacing::kHz30, cyclic_prefix{}, kNofPrb, /*sector_id=*/0, std::move(decompressor),
        compr_params);
    auto ecpri_decoder = ecpri::create_ecpri_packet_decoder_ignoring_payload_size(logger, /*sector=*/0);

    for (unsigned s = 0; s < kNofSymbols; s++) {
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
      for (unsigned sc = 0; sc < kNofSubcarriers; sc++) {
        cbf16_t expected = re_grid[s][sc];
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
    << "  \"tb_size_bytes\": " << tb_bytes << ",\n"
    << "  \"nof_cb\": " << seg.nof_segments << ",\n"
    << "  \"base_graph\": " << seg.base_graph << ",\n"
    << "  \"rnti\": " << rnti << ",\n"
    << "  \"n_id\": " << n_id << ",\n"
    << "  \"nslot\": " << nslot << ",\n"
    << "  \"seed\": " << seed << ",\n"
    << "  \"tb_bytes_hex\": \"" << to_hex(tb) << "\"\n"
    << "}\n";

  std::fprintf(stderr, "oracle_tx_gen: OK — MCS %u, %u CB(s), BG%u, self-check passed, wrote %s + %s\n",
              mcs->mcs_index, seg.nof_segments, seg.base_graph, out_pcap.c_str(), out_json.c_str());
  return 0;
}
