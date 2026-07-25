// cb_segment_test.cpp — P2-R10: oi_p2_cb_segment_compute's sizing vs the real linked OCUDU
// ldpc_segmenter_tx (which computes the identical TS 38.212 SS5.2.2 numbers for its own,
// TX-direction purposes), plus a full round-trip test: real segmenter builds valid CBs (with
// real embedded CRC24B/TB-CRC/filler) for a random TB, oi_p2_cb_desegment reassembles them and
// must recover the exact original TB bytes. Covers all three MVP MCS points (C=1, 2, 4) plus a
// corruption-detection case.
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../src/host/oi_p2_cb_segment.h"

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_segmenter_buffer.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

struct McsPoint {
  const char* label;
  uint32_t tbs_bits;
  float code_rate;
  uint32_t qm;
};

int main() {
  using namespace ocudu;

  auto crc_factory = create_crc_calculator_factory_sw("generic");
  check(crc_factory != nullptr, "real OCUDU crc_calculator_factory created");
  auto seg_factory = create_ldpc_segmenter_tx_factory_sw(crc_factory);
  check(seg_factory != nullptr, "real OCUDU ldpc_segmenter_tx_factory created");
  auto segmenter = seg_factory->create();
  check(segmenter != nullptr, "real OCUDU ldpc_segmenter_tx created");

  // Real TBS/rate values for the MVP's three MCS points (MCS 4/13/21), confirmed via a real
  // tbs_calculator run: see p2f-integration/VERIFICATION.md.
  std::vector<McsPoint> points = {
      {"MCS4 (C=1)", 4608, 0.3008f, 2},
      {"MCS13 (C=2)", 14600, 0.4785f, 4},
      {"MCS21 (C=4)", 27656, 0.6016f, 6},
  };
  constexpr uint32_t kNofDataRe = 6732;  // this MVP's fixed N_data_re (11 symbols * 612 subcarriers)

  std::mt19937 rgen(555);

  for (const auto& pt : points) {
    // --- Sizing cross-check ---
    oi_p2_cb_segment_params params;
    oi_p2_cb_segment_compute(pt.tbs_bits, pt.code_rate, &params);

    segmenter_config cfg{};
    cfg.base_graph = (params.base_graph == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
    cfg.rv = 0;
    cfg.mod = (pt.qm == 2)  ? modulation_scheme::QPSK
             : (pt.qm == 4) ? modulation_scheme::QAM16
                           : modulation_scheme::QAM64;
    cfg.Nref = 0;
    cfg.nof_layers = 1;
    cfg.nof_ch_symbols = kNofDataRe;  // this MVP's real, fixed full-band/single-layer allocation

    unsigned tb_bytes = (pt.tbs_bits + 7) / 8;
    std::vector<uint8_t> tb(tb_bytes);
    for (auto& b : tb) b = (uint8_t)(rgen() & 0xFF);

    const ldpc_segmenter_buffer& seg_buf = segmenter->new_transmission(span<const uint8_t>(tb), cfg);

    char label[128];
    std::snprintf(label, sizeof(label), "%s: nof_segments matches real segmenter", pt.label);
    check(params.nof_segments == seg_buf.get_nof_codeblocks(), label);
    std::snprintf(label, sizeof(label), "%s: segment_length matches real segmenter", pt.label);
    check(params.segment_length == seg_buf.get_segment_length().value(), label);
    std::snprintf(label, sizeof(label), "%s: nof_filler_bits matches real segmenter", pt.label);
    check(params.nof_filler_bits == seg_buf.get_nof_filler_bits().value(), label);
    std::snprintf(label, sizeof(label), "%s: zero_pad matches real segmenter", pt.label);
    check(params.zero_pad == seg_buf.get_zero_pad().value(), label);
    std::snprintf(label, sizeof(label), "%s: tb_crc_bits matches real segmenter", pt.label);
    check(params.tb_crc_bits == seg_buf.get_tb_crc_bits().value(), label);
    // get_cb_info_bits(0) returns the LAST-cb value whenever nof_segments==1 (index 0 IS the
    // last/only CB in that case) -- only meaningful to compare against the non-last `cb_info_bits`
    // field when there's more than one segment.
    if (params.nof_segments > 1) {
      std::snprintf(label, sizeof(label), "%s: cb_info_bits (non-last) matches real segmenter", pt.label);
      check(params.cb_info_bits == seg_buf.get_cb_info_bits(0).value(), label);
    }
    std::snprintf(label, sizeof(label), "%s: cb_info_bits_last matches real segmenter", pt.label);
    check(params.cb_info_bits_last == seg_buf.get_cb_info_bits(params.nof_segments - 1).value(), label);

    for (uint32_t i = 0; i < params.nof_segments; i++) {
      uint32_t rm = oi_p2_compute_rm_length(kNofDataRe, params.nof_segments, pt.qm, i);
      std::snprintf(label, sizeof(label), "%s: rm_length[%u] matches real segmenter", pt.label, i);
      check(rm == seg_buf.get_rm_length(i), label);
    }

    // --- Round-trip: real segmenter builds valid CBs, our desegmenter reassembles the TB ---
    std::vector<std::vector<uint8_t>> cb_storage(params.nof_segments);
    std::vector<dynamic_bit_buffer> cb_bits;
    std::vector<const uint8_t*> cb_ptrs(params.nof_segments);
    cb_bits.reserve(params.nof_segments);
    for (uint32_t i = 0; i < params.nof_segments; i++) {
      cb_bits.emplace_back(params.segment_length);
      seg_buf.read_codeblock(cb_bits.back(), span<const uint8_t>(tb), i);
      cb_ptrs[i] = cb_bits.back().get_buffer().data();
    }

    std::vector<uint8_t> reassembled(tb_bytes, 0);
    oi_p2_deseg_status st = oi_p2_cb_desegment(&params, pt.tbs_bits, cb_ptrs.data(), reassembled.data());
    std::snprintf(label, sizeof(label), "%s: desegment returns OK", pt.label);
    check(st == OI_P2_DESEG_OK, label);
    std::snprintf(label, sizeof(label), "%s: reassembled TB bit-exact vs original", pt.label);
    check(std::memcmp(reassembled.data(), tb.data(), tb_bytes) == 0, label);

    // --- Corruption detection: flip a bit in the first CB's payload, expect a CRC failure ---
    {
      std::vector<uint8_t> corrupt_buf(cb_bits[0].get_buffer().begin(), cb_bits[0].get_buffer().end());
      corrupt_buf[0] ^= 0x01;
      std::vector<const uint8_t*> corrupt_ptrs = cb_ptrs;
      corrupt_ptrs[0] = corrupt_buf.data();
      std::vector<uint8_t> reassembled2(tb_bytes, 0);
      oi_p2_deseg_status st2 = oi_p2_cb_desegment(&params, pt.tbs_bits, corrupt_ptrs.data(), reassembled2.data());
      std::snprintf(label, sizeof(label), "%s: corrupted CB detected (CRC failure, not silently accepted)", pt.label);
      check(st2 != OI_P2_DESEG_OK, label);
    }
  }

  std::printf("\n%s\n", g_fail == 0 ? "cb_segment_test: ALL PASS" : "cb_segment_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
