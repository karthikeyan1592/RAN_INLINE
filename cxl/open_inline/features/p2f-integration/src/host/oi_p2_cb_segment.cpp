#include "oi_p2_cb_segment.h"
#include "oi_p2_crc.h"

#include <string.h>

namespace {

// The 51 standard 3GPP lifting sizes (TS 38.212 Table 5.3.2-1), ordered ascending -- ported from
// include/ocudu/phy/upper/channel_coding/ldpc/ldpc.h's lifting_size_t enum values (a plain list of
// standard-mandated integers, not srsRAN-specific expression).
constexpr uint32_t kLiftingSizes[51] = {2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
                                        15,  16,  18,  20,  22,  24,  26,  28,  30,  32,  36,  40,  44,
                                        48,  52,  56,  60,  64,  72,  80,  88,  96,  104, 112, 120, 128,
                                        144, 160, 176, 192, 208, 224, 240, 256, 288, 320, 352, 384};

uint32_t compute_tb_crc_size(uint32_t tbs_bits) { return (tbs_bits <= 3824u) ? 16u : 24u; }

// TS 38.212 SS7.2.2 base-graph selection (include/ocudu/ran/sch/ldpc_base_graph.h).
uint32_t get_ldpc_base_graph(float r, uint32_t a_bits) {
  if (a_bits <= 292u || r <= 0.25f || (a_bits <= 3824u && r <= 0.67f)) {
    return 2;
  }
  return 1;
}

uint32_t compute_nof_codeblocks(uint32_t tbs_bits, uint32_t base_graph) {
  uint32_t tb_and_crc_bits = tbs_bits + compute_tb_crc_size(tbs_bits);
  uint32_t max_segment_length = (base_graph == 1) ? 8448u : 3840u;
  if (tb_and_crc_bits <= max_segment_length) {
    return 1;
  }
  uint32_t denom = max_segment_length - 24u;
  return (tb_and_crc_bits + denom - 1u) / denom;  // ceil
}

uint32_t compute_lifting_size(uint32_t tbs_bits, uint32_t base_graph, uint32_t nof_segments) {
  uint32_t nof_tb_bits_in = tbs_bits + compute_tb_crc_size(tbs_bits);

  uint32_t ref_length = 22;
  if (base_graph == 2) {
    if (nof_tb_bits_in > 640u) {
      ref_length = 10;
    } else if (nof_tb_bits_in > 560u) {
      ref_length = 9;
    } else if (nof_tb_bits_in > 192u) {
      ref_length = 8;
    } else {
      ref_length = 6;
    }
  }
  uint32_t total_ref_length = nof_segments * ref_length;

  uint32_t nof_tb_bits_out = nof_tb_bits_in;
  if (nof_segments > 1) {
    nof_tb_bits_out += 24u * nof_segments;
  }

  for (uint32_t ls : kLiftingSizes) {
    if ((uint64_t)ls * total_ref_length >= nof_tb_bits_out) {
      return ls;
    }
  }
  return 0;  // unreachable for valid inputs (matches ocudu_assert in the real code)
}

uint32_t compute_codeblock_size(uint32_t base_graph, uint32_t lifting_size) {
  uint32_t base_length = (base_graph == 1) ? 22u : 10u;
  return base_length * lifting_size;
}

inline uint32_t divide_ceil_u32(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Extracts one bit (MSB-first within each byte) at absolute bit position `bit_idx`.
inline uint32_t get_bit(const uint8_t* data, uint32_t bit_idx) {
  return (data[bit_idx / 8u] >> (7u - (bit_idx % 8u))) & 1u;
}

inline void set_bit(uint8_t* data, uint32_t bit_idx, uint32_t val) {
  uint32_t byte_idx = bit_idx / 8u;
  uint32_t bit_pos = 7u - (bit_idx % 8u);
  if (val) {
    data[byte_idx] |= (uint8_t)(1u << bit_pos);
  } else {
    data[byte_idx] &= (uint8_t) ~(1u << bit_pos);
  }
}

void copy_bits(uint8_t* dst, uint32_t dst_bit_off, const uint8_t* src, uint32_t src_bit_off, uint32_t nbits) {
  for (uint32_t i = 0; i < nbits; i++) {
    set_bit(dst, dst_bit_off + i, get_bit(src, src_bit_off + i));
  }
}

}  // namespace

extern "C" void oi_p2_cb_segment_compute(uint32_t tb_size_bits, float code_rate, oi_p2_cb_segment_params* out) {
  memset(out, 0, sizeof(*out));

  out->base_graph = get_ldpc_base_graph(code_rate, tb_size_bits);
  out->tb_crc_bits = compute_tb_crc_size(tb_size_bits);
  out->nof_segments = compute_nof_codeblocks(tb_size_bits, out->base_graph);
  out->lifting_size = compute_lifting_size(tb_size_bits, out->base_graph, out->nof_segments);
  out->segment_length = compute_codeblock_size(out->base_graph, out->lifting_size);
  out->codeword_length = out->segment_length * (out->base_graph == 1 ? 3u : 5u);
  out->nof_crc_bits = (out->nof_segments > 1) ? 24u : 0u;

  uint32_t nof_tb_bits_out = tb_size_bits + out->tb_crc_bits;
  if (out->nof_segments > 1) {
    nof_tb_bits_out += out->nof_segments * 24u;
  }
  out->cb_info_bits = divide_ceil_u32(nof_tb_bits_out, out->nof_segments) - out->nof_crc_bits;
  out->zero_pad = (out->cb_info_bits + out->nof_crc_bits) * out->nof_segments - nof_tb_bits_out;
  out->nof_filler_bits = out->segment_length - out->cb_info_bits - out->nof_crc_bits;
  out->cb_info_bits_last = out->cb_info_bits - out->tb_crc_bits - out->zero_pad;
}

extern "C" oi_p2_deseg_status oi_p2_cb_desegment(const oi_p2_cb_segment_params* p, uint32_t tb_size_bits,
                                                 const uint8_t* const* decoded_cbs, uint8_t* out_tb_bytes) {
  // 1. Per-CB CRC24B self-check (payload + its own embedded CRC, expect remainder == 0 -- same
  // convention as pusch_codeblock_decoder.cpp::decode()).
  if (p->nof_crc_bits > 0) {
    for (uint32_t i = 0; i < p->nof_segments; i++) {
      uint32_t crc = oi_p2_crc_calculate(OI_P2_CRC24B, decoded_cbs[i], 0, p->cb_info_bits + p->nof_crc_bits);
      if (crc != 0) {
        return OI_P2_DESEG_ERR_CB_CRC;
      }
    }
  }

  // 2. Reassemble the original tb_size_bits-bit transport block: every non-last CB contributes
  // its full cb_info_bits payload (pure TB data); the last CB contributes only its first
  // cb_info_bits_last bits (the remainder of its payload region is the TB CRC + zero padding,
  // not real TB data).
  uint32_t out_bit_pos = 0;
  for (uint32_t i = 0; i < p->nof_segments; i++) {
    bool last = (i == p->nof_segments - 1);
    uint32_t nbits = last ? p->cb_info_bits_last : p->cb_info_bits;
    copy_bits(out_tb_bytes, out_bit_pos, decoded_cbs[i], 0, nbits);
    out_bit_pos += nbits;
  }

  // 3. TB-level CRC self-check: recompute over [reassembled TB bits ++ TB-CRC bits], the latter
  // stored in the last CB immediately after cb_info_bits_last, expect remainder == 0.
  // Scratch sized for this MVP's real bound (max measured TBS = 3457 bytes at MCS 21, see
  // STATUS.md), not the absolute 3GPP ceiling (MAX_TBS = 1277992 bits in
  // ldpc_segmenter_tx_impl.cpp) -- avoids an ~160KB unconditional stack allocation for a pipeline
  // that never sees a TBS anywhere near that limit (P2-R11 rejects any config outside the fixed
  // MVP shape before reaching this code at all).
  uint8_t scratch[4096];
  uint32_t tb_bytes = divide_ceil_u32(tb_size_bits, 8u);
  memcpy(scratch, out_tb_bytes, tb_bytes);
  copy_bits(scratch, tb_size_bits, decoded_cbs[p->nof_segments - 1], p->cb_info_bits_last, p->tb_crc_bits);

  oi_p2_crc_poly tb_poly = (p->tb_crc_bits == 16u) ? OI_P2_CRC16 : OI_P2_CRC24A;
  uint32_t tb_crc = oi_p2_crc_calculate(tb_poly, scratch, 0, tb_size_bits + p->tb_crc_bits);
  if (tb_crc != 0) {
    return OI_P2_DESEG_ERR_TB_CRC;
  }

  return OI_P2_DESEG_OK;
}

extern "C" uint32_t oi_p2_compute_rm_length(uint32_t nof_symbols_per_layer, uint32_t nof_segments, uint32_t qm,
                                            uint32_t cb_index) {
  uint32_t nof_short_segments = nof_segments - (nof_symbols_per_layer % nof_segments);
  uint32_t nof_symbols_this_cb = (cb_index < nof_short_segments)
                                     ? (nof_symbols_per_layer / nof_segments)
                                     : divide_ceil_u32(nof_symbols_per_layer, nof_segments);
  return nof_symbols_this_cb * qm;
}
