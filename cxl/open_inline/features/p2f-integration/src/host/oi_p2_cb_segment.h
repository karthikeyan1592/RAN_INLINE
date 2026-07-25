/* oi_p2_cb_segment.h — TS 38.212 SS5.2.2 codeblock (de)segmentation sizing + TB reassembly, for
 * the CPU tail (P2-R10).
 *
 * Port grounding: include/ocudu/ran/sch/sch_segmentation.h (compute_tb_crc_size,
 * compute_nof_codeblocks, compute_lifting_size, compute_codeblock_size -- all four ported
 * verbatim, formulas confirmed by reading the real header directly, not derived by hand),
 * include/ocudu/ran/sch/ldpc_base_graph.h (get_ldpc_base_graph), and
 * lib/phy/upper/channel_coding/ldpc/ldpc_segmenter_tx_impl.cpp's offset/filler/zero-pad
 * bookkeeping (new_transmission()) -- the RX/desegmentation direction ported here is the logical
 * inverse of that TX-direction logic (same sizing math both ways; only which end reads vs writes
 * differs). third_party/ocudu, BSD-3, release_26_04.
 *
 * Per-CB layout produced by this sizing (every CB, non-last and last alike, totals
 * `segment_length` bits): [cb_info_bits (payload; on the last CB this is
 * cb_info_bits_last real TB bits + tb_crc_bits + zero_pad, NOT all real data)]
 * [nof_crc_bits (CRC24B, only if nof_segments>1)][nof_filler_bits (LLR_INFTY-derived zeros from
 * K6/LDPC, always at the tail)].
 */
#ifndef OI_P2_CB_SEGMENT_H
#define OI_P2_CB_SEGMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t base_graph;         // 1 or 2
  uint32_t nof_segments;       // C
  uint32_t lifting_size;       // Zc
  uint32_t segment_length;     // K, bits per CB (info + CB-CRC + filler) -- the LDPC DECODER's
                               // output length. NOT the same as codeword_length below; K6's
                               // `full_length` argument wants codeword_length (N), not this.
  uint32_t codeword_length;    // N = 66*Zc (BG1) or 50*Zc (BG2) -- the LDPC codeword length
                               // (K's systematic+parity columns before shortening/puncturing),
                               // i.e. K6's rate-dematch OUTPUT size == LDPC decoder's INPUT size.
                               // Ported: compute_full_codeblock_size (ldpc.h), N = K * (BG1?3:5).
  uint32_t nof_filler_bits;    // F, same for every CB
  uint32_t nof_crc_bits;       // 0 or 24 (CRC24B, only when nof_segments > 1)
  uint32_t tb_crc_bits;        // 16 or 24 (TB-level CRC)
  uint32_t cb_info_bits;       // payload-region bit count, every CB except the last
  uint32_t cb_info_bits_last;  // last CB's REAL-TB-data bit count (excludes tb_crc_bits + zero_pad)
  uint32_t zero_pad;           // zero-padding bits appended after the TB CRC, into the last CB's payload region
} oi_p2_cb_segment_params;

/// Computes segmentation sizing for a transport block of `tb_size_bits` bits (B, excluding CRC)
/// at the given code rate `code_rate` (R, e.g. 0.4785 for MCS 13) -- code_rate is only used to
/// select the base graph (TS 38.212 SS7.2.2), faithfully ported rather than assuming BG1.
void oi_p2_cb_segment_compute(uint32_t tb_size_bits, float code_rate, oi_p2_cb_segment_params* out);

typedef enum {
  OI_P2_DESEG_OK = 0,
  OI_P2_DESEG_ERR_CB_CRC = 1,  // a per-CB CRC24B check failed
  OI_P2_DESEG_ERR_TB_CRC = 2,  // the reassembled TB-level CRC check failed
} oi_p2_deseg_status;

/// Reassembles the original tb_size_bits-bit transport block from `params->nof_segments` decoded
/// codeblocks (each a byte-packed, MSB-first buffer of at least ceil(segment_length/8) bytes, as
/// produced by the LDPC decoder -- filler bits already stripped is NOT assumed, this function
/// strips them itself per the layout above). Verifies per-CB CRC24B (if segmented) and the TB CRC
/// (always). `out_tb_bytes` must have room for ceil(tb_size_bits/8) bytes.
oi_p2_deseg_status oi_p2_cb_desegment(const oi_p2_cb_segment_params* params, uint32_t tb_size_bits,
                                      const uint8_t* const* decoded_cbs, uint8_t* out_tb_bytes);

/// Computes the rate-matched output length (bits) K6 must produce for codeblock `cb_index`, per
/// TS 38.212 SS5.4.2.1. Port grounding: ldpc_segmenter_helpers.h's compute_rm_length() -- the
/// `nof_short_segments` CBs (indices 0..nof_short_segments-1) get floor(nof_symbols_per_layer /
/// nof_segments) symbols each; the rest get ceil(...); every CB's rm_length = its symbol count *
/// qm (nof_layers=1 throughout this MVP, so the real formula's *nof_layers term is omitted here).
/// `nof_symbols_per_layer` = the number of data-RE modulation symbols carrying this TB (this
/// MVP's fixed full-band/single-layer allocation: always N_data_re = 6732).
uint32_t oi_p2_compute_rm_length(uint32_t nof_symbols_per_layer, uint32_t nof_segments, uint32_t qm,
                                 uint32_t cb_index);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_P2_CB_SEGMENT_H */
