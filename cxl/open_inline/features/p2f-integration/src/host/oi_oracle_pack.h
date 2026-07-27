/* oi_oracle_pack.h — shared "known TB -> RE grid -> wire-format IQ bytes" packer.
 *
 * LLD Q1 resolution (p3-live-tap-ul-inject/spec/LLD.md §Open questions): p2f-integration's
 * oracle_tx_gen.cpp (P2-R15b, pcap-frame front end) and p3-live-tap-ul-inject's M2 .osg-file
 * generator both need a "known TB -> valid U-plane wire bytes" packer implementing the exact same
 * "51 PRB x 14 symbols x 12 subcarriers x (16-bit I, 16-bit Q)" byte layout. Rather than two
 * independent implementations that could silently drift apart, this header is the ONE shared
 * library; oracle_tx_gen.cpp and p3's .osg generator are its two front ends (p2f wraps the output
 * in eCPRI/O-RAN/pcap framing for a pipeline_runner.cpp replay target; p3 concatenates it directly
 * into the flat .osg grid_payload section, since ru_emulator's own real uplane_message_builder
 * does the eCPRI/O-RAN wrapping at injection time -- p3 never needs a pcap frame).
 *
 * Every step below is the same real, linked OCUDU chain oracle_tx_gen.cpp already used and
 * self-verified (segment/LDPC-encode/rate-match -> scramble -> modulate -> RE-map -> real
 * iq_compression_none_impl::compress() for the wire-byte conversion) -- moved here verbatim, not
 * reimplemented. See oi_oracle_pack.cpp's header comment for the per-step grounding citations
 * (mirrors oracle_tx_gen.cpp's original file-header comment, which this supersedes).
 *
 * Port grounding for the wire-byte step specifically: iq_compression_none_impl::compress()
 * (third_party/ocudu/lib/ofh/compression/iq_compression_none_impl.cpp) iterates PRB-major, and its
 * pack_bytes() (packing_utils_generic.cpp) packs each 16-bit sample high-byte-first -- i.e. real,
 * confirmed big-endian, PRB-major/subcarrier-minor, (I,Q) per subcarrier -- exactly the byte order
 * both oracle_tx_gen.cpp's pcap frames and p3's .osg §3.1 format require. Calling the real
 * compressor (not hand-packing bytes) is the whole point of sharing this function: it is
 * impossible for the two front ends' byte layouts to drift apart if they both call the same
 * function backed by the same real OCUDU component.
 */
#ifndef OI_ORACLE_PACK_H
#define OI_ORACLE_PACK_H

#include <cstdint>
#include <vector>

#include "ocudu/adt/bf16.h"
#include "ocudu/adt/complex.h"

namespace oi_oracle {

// MVP's fixed grid dimensions (p2-phy-kernels SPEC.md "Fixed MVP configuration" table, shared
// across P1/P2/P3 by design -- single source of truth now that this header exists; previously
// duplicated per-file, per this project's "small local constant table" convention for files that
// don't share logic -- this header is exactly the exception where sharing was warranted, LLD Q1).
constexpr unsigned kNofPrb = 51;
constexpr unsigned kNofSubcarriers = kNofPrb * 12;  // 612
constexpr unsigned kNofSymbols = 14;
constexpr unsigned kDmrsSymbols[3] = {2, 7, 11};
constexpr unsigned kDataSymbols[11] = {0, 1, 3, 4, 5, 6, 8, 9, 10, 12, 13};
constexpr unsigned kNofDataRe = 11 * kNofSubcarriers;  // 6732

struct mcs_point {
  uint32_t mcs_index;
  uint32_t qm;
  uint32_t tbs_bits;
  float code_rate;
};

// MVP MCS table: {4, 13, 21} (p2 MVP MCS set, p2-phy-kernels SPEC.md). Returns nullptr if
// mcs_index isn't one of the three pinned points.
const mcs_point* find_mcs(uint32_t mcs_index);

// Result of packing a known TB into a fully-encoded RE grid (steps 1-6 of the original
// oracle_tx_gen.cpp recipe -- TB gen, segment/encode/rate-match, scramble, modulate, RE-map).
struct packed_tb {
  std::vector<uint8_t> tb_bytes;
  unsigned nof_cb;
  unsigned base_graph;
  // re_grid[symbol][subcarrier], kNofSymbols rows x kNofSubcarriers cols. Data-symbol rows carry
  // the modulated/scaled codeword; DMRS-symbol rows carry the comb-2 pilot pattern (every other
  // subcarrier; the other half stays 0, matching K2a's read pattern -- see .cpp).
  std::vector<std::vector<ocudu::cbf16_t>> re_grid;
};

// Steps 1-6: build a real, self-consistent, fully-encoded transport block for one of the three
// MVP MCS points, using the real linked OCUDU segmenter/LDPC-encoder/rate-matcher/scrambler/
// modulator chain (TX-chain recipe grounded in pdsch_encoder_impl.cpp/pdsch_modulator_impl.cpp,
// read-only consultation since OCUDU has no PUSCH encoder -- see .cpp header). `seed` drives the
// deterministic random TB content (std::mt19937); `rnti`/`n_id`/`nslot` drive scrambling exactly
// as pdsch_modulator_impl.cpp's c_init formula does. Includes the real 0.9x TX amplitude scale
// finding (see .cpp) applied identically to data and DMRS.
packed_tb pack_tb(uint32_t mcs_index, uint32_t seed, uint32_t rnti, uint32_t n_id, uint32_t nslot);

// One symbol's RE row (kNofSubcarriers cbf16_t values, PRB-major/subcarrier-minor as already laid
// out by pack_tb's re_grid) -> raw wire-format IQ bytes: uncompressed 16-bit signed I/Q, network
// byte order (big-endian), PRB-major/subcarrier-minor, (I,Q) per subcarrier. Exactly
// kNofSubcarriers * 4 bytes (12 subcarriers/PRB * 4 bytes/subcarrier * 51 PRB = 2448). This is the
// real OCUDU iq_compression_none_impl::compress() call, not a hand-rolled packer (see file header).
std::vector<uint8_t> pack_symbol_to_wire_iq(const std::vector<ocudu::cbf16_t>& symbol_grid);

}  // namespace oi_oracle

#endif  // OI_ORACLE_PACK_H
