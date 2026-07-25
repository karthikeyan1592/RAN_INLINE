/* oi_p2_crc.h — CRC16/CRC24A/CRC24B (TS 38.212 SS5.1), for CB/TB verification in the CPU tail.
 *
 * Port grounding: lib/phy/upper/channel_coding/crc_calculator_generic_impl.{h,cpp} (BSD-3,
 * release_26_04) -- a plain bit-by-bit LFSR long-division CRC with the standard 3GPP generator
 * polynomials g_CRC24A(D)/g_CRC24B(D)/g_CRC16(D) (TS 38.212 SS5.1), no lookup-table/SIMD variant
 * (P2-R2/D3: those exist as speed optimizations of the same math, not a different algorithm).
 *
 * Operates on an arbitrary bit-offset/bit-length window of a byte-packed (MSB-first) buffer,
 * matching the real calculate(bit_buffer)'s bit-level generality -- CB segment boundaries are not
 * guaranteed byte-aligned in general (TS 38.212 SS5.2.2's K/F/CRC split), even though the MVP's
 * three actual configurations happen to have byte-aligned K.
 */
#ifndef OI_P2_CRC_H
#define OI_P2_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  OI_P2_CRC24A = 0,
  OI_P2_CRC24B = 1,
  OI_P2_CRC16 = 2,
} oi_p2_crc_poly;

/// Returns the checksum order in bits (24, 24, or 16).
uint32_t oi_p2_crc_order(oi_p2_crc_poly poly);

/// Computes the CRC checksum over bit_len bits starting at bit_offset within data (MSB-first
/// byte packing, matching the rest of this pipeline's bit_buffer convention). Returns the
/// checksum right-justified in the low `order` bits of the return value.
uint32_t oi_p2_crc_calculate(oi_p2_crc_poly poly, const uint8_t* data, uint32_t bit_offset, uint32_t bit_len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_P2_CRC_H */
