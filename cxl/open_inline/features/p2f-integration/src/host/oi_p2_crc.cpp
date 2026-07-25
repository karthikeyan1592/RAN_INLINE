#include "oi_p2_crc.h"

namespace {

struct PolyDef {
  uint32_t order;
  uint64_t polynom;
};

PolyDef poly_def(oi_p2_crc_poly poly) {
  switch (poly) {
    case OI_P2_CRC24A:
      return {24, 0x1864cfbu};
    case OI_P2_CRC24B:
      return {24, 0x1800063u};
    case OI_P2_CRC16:
    default:
      return {16, 0x11021u};
  }
}

// Extracts one bit (MSB-first within each byte) at absolute bit position `bit_idx`.
inline uint32_t get_bit(const uint8_t* data, uint32_t bit_idx) {
  uint32_t byte_idx = bit_idx / 8u;
  uint32_t bit_pos = 7u - (bit_idx % 8u);
  return (data[byte_idx] >> bit_pos) & 1u;
}

}  // namespace

extern "C" uint32_t oi_p2_crc_order(oi_p2_crc_poly poly) { return poly_def(poly).order; }

extern "C" uint32_t oi_p2_crc_calculate(oi_p2_crc_poly poly, const uint8_t* data, uint32_t bit_offset,
                                        uint32_t bit_len) {
  PolyDef def = poly_def(poly);
  uint64_t highbit = 1ull << def.order;
  uint64_t remainder = 0;

  for (uint32_t i = 0; i < bit_len; i++) {
    uint64_t bit = get_bit(data, bit_offset + i);
    remainder = (remainder << 1u) | bit;
    if (remainder & highbit) {
      remainder ^= def.polynom;
    }
  }
  for (uint32_t i = 0; i < def.order; i++) {
    remainder = remainder << 1u;
    if (remainder & highbit) {
      remainder ^= def.polynom;
    }
  }
  return static_cast<uint32_t>(remainder & (highbit - 1));
}
