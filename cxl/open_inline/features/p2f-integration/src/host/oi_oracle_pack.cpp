// oi_oracle_pack.cpp — shared packer implementation. Moved here verbatim from
// oracle_tx_gen.cpp's original steps 1-6 (see git history / p2f-integration/VERIFICATION.md for
// the extraction record); oracle_tx_gen.cpp now calls this instead of inlining the logic.
//
// TX-chain recipe (grounded, not guessed): mirrors lib/phy/upper/channel_processors/pdsch/
// pdsch_encoder_impl.cpp (segment -> per-CB LDPC-encode -> rate-match -> concatenate) and
// pdsch_modulator_impl.cpp (scramble -> modulate) read-only, since OCUDU has no PUSCH *encoder*
// (it is gNB-side/RX-only for the uplink) but the TS 38.212/38.211 procedures PDSCH's real TX
// chain implements are the same bit-level procedures a UE's PUSCH TX chain would use — consulted
// read-only for the call sequence (HLD D9 precedent), not ported code. Every individual primitive
// (segmenter, encoder, rate matcher, scrambler, modulator, iq_compressor) is the real, unmodified,
// factory-constructed OCUDU implementation — nothing here reimplements a 3GPP procedure by hand.
// Segmentation sizing reuses this project's own already-verified oi_p2_cb_segment (30/30
// assertions) rather than re-deriving base_graph.
#include "oi_oracle_pack.h"

#include <cstring>
#include <random>

#include "oi_p2_cb_segment.h"
#include "../../../p2d-k2-k3/src/host/oi_dmrs_ref_seq.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/bit.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_segmenter_buffer.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"

using namespace ocudu;
using namespace ocudu::ofh;

namespace oi_oracle {

namespace {
constexpr mcs_point kMcsTable[3] = {
    {4, 2, 4608, 0.3008f},
    {13, 4, 14600, 0.4785f},
    {21, 6, 27656, 0.6016f},
};

modulation_scheme qm_to_scheme(uint32_t qm) {
  return (qm == 2) ? modulation_scheme::QPSK : (qm == 4) ? modulation_scheme::QAM16 : modulation_scheme::QAM64;
}
}  // namespace

const mcs_point* find_mcs(uint32_t mcs_index) {
  for (const auto& m : kMcsTable) {
    if (m.mcs_index == mcs_index) return &m;
  }
  return nullptr;
}

packed_tb pack_tb(uint32_t mcs_index, uint32_t seed, uint32_t rnti, uint32_t n_id, uint32_t nslot) {
  const mcs_point* mcs = find_mcs(mcs_index);
  if (!mcs) {
    std::fprintf(stderr, "oi_oracle_pack: unknown mcs_index %u (expected 4, 13, or 21)\n", mcs_index);
    std::abort();
  }

  // --- 1. Random transport block ---
  unsigned tb_bytes = (mcs->tbs_bits + 7) / 8;
  std::vector<uint8_t> tb(tb_bytes);
  std::mt19937 rgen(seed);
  for (auto& b : tb) b = (uint8_t)(rgen() & 0xFF);

  // --- 2. Segmentation sizing (this project's own verified module, not re-derived here) ---
  oi_p2_cb_segment_params seg{};
  oi_p2_cb_segment_compute(mcs->tbs_bits, mcs->code_rate, &seg);

  // --- 3. Real OCUDU segmenter/encoder/rate-matcher chain ---
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
    std::fprintf(stderr, "oi_oracle_pack: real segmenter nof_codeblocks=%u != oi_p2_cb_segment's %u\n",
                 seg_buf.get_nof_codeblocks(), seg.nof_segments);
    std::abort();
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
    std::fprintf(stderr, "oi_oracle_pack: concatenated codeword length %u != expected %u\n", offset,
                 kNofDataRe * mcs->qm);
    std::abort();
  }

  // --- 4. Scramble (real OCUDU pseudo_random_generator; c_init = rnti*32768 + n_id, matching
  // pdsch_modulator_impl.cpp's c_init = rnti<<15 + q<<14 + n_id with q=0) ---
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
    std::fprintf(stderr, "oi_oracle_pack: nof_re %u != kNofDataRe %u\n", nof_re, kNofDataRe);
    std::abort();
  }
  std::vector<cf_t> symbols(nof_re);
  modulator->modulate(span<cf_t>(symbols), scrambled, qm_to_scheme(mcs->qm));

  // Real, found-not-guessed finding (originally discovered building oracle_tx_gen.cpp): OCUDU's
  // real "static compression, none" wire codec (iq_compression_none_impl, iq_scaling=1.0)
  // quantizes floats to fixed-point assuming a unit full-scale range; 64-QAM's outer constellation
  // points (amplitude 7/sqrt(42) ~= 1.0801) exceed that range and get silently clipped to 1.0 on
  // decode. Fix: a conservative TX amplitude scale applied to BOTH data and DMRS, matching a real
  // system's transmit power scaling -- NOT a change to K3's tx_scaling (equalizer is blind to a
  // common data/DMRS gain via pilot-based channel estimation, see oracle_tx_gen.cpp git history
  // for the full algebraic argument this comment originally carried).
  constexpr float kTxAmplitudeScale = 0.9f;  // 0.9 * 1.0801 = 0.972 < 1.0, safe margin
  for (auto& sym : symbols) sym *= kTxAmplitudeScale;

  // --- 6. RE-map: 11 data-symbol rows (symbol-major/subcarrier-minor) + 3 DMRS rows (comb-2) ---
  packed_tb result;
  result.tb_bytes = std::move(tb);
  result.nof_cb = seg.nof_segments;
  result.base_graph = seg.base_graph;
  result.re_grid.assign(kNofSymbols, std::vector<cbf16_t>(kNofSubcarriers, cbf16_t(0.0f, 0.0f)));

  for (unsigned lin = 0; lin < 11; lin++) {
    for (unsigned sc = 0; sc < kNofSubcarriers; sc++) {
      result.re_grid[kDataSymbols[lin]][sc] = cbf16_t(symbols[lin * kNofSubcarriers + sc]);
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
      result.re_grid[kDmrsSymbols[i]][2 * p] = cbf16_t(dmrs_ref[i][p].re, dmrs_ref[i][p].im);
    }
  }
  return result;
}

std::vector<uint8_t> pack_symbol_to_wire_iq(const std::vector<cbf16_t>& symbol_grid) {
  if (symbol_grid.size() != kNofSubcarriers) {
    std::fprintf(stderr, "oi_oracle_pack: pack_symbol_to_wire_iq expected %u REs, got %zu\n", kNofSubcarriers,
                 symbol_grid.size());
    std::abort();
  }
  ru_compression_params compr_params{compression_type::none, 16};
  // Real OCUDU compressor (not a hand-rolled byte packer) -- see file header. Output size for
  // "none"/16-bit is get_compressed_prb_size() * nof_prbs = (12 REs * 2 * 16 bits = 384 bits =
  // 48 bytes/PRB) * 51 PRB = 2448 bytes, i.e. kNofSubcarriers * 4 (4 bytes/RE: 2 bytes I + 2 Q).
  static thread_local ocudulog::basic_logger* logger_ptr = nullptr;
  if (!logger_ptr) {
    ocudulog::init();
    logger_ptr = &ocudulog::fetch_basic_logger("ORACLE_PACK");
    logger_ptr->set_level(ocudulog::basic_levels::error);
  }
  auto compressor = create_iq_compressor(compression_type::none, *logger_ptr, 1.0f, "generic");
  std::vector<uint8_t> out(kNofSubcarriers * 4);
  compressor->compress(span<uint8_t>(out), span<const cbf16_t>(symbol_grid), compr_params);
  return out;
}

}  // namespace oi_oracle
