/* oi_p2_tb_record.h — I8 TB+CRC record (LLD §4.7), host tail output, consumed by p4-phy-l2-seam.
 *
 * Byte layout is provisional (parent LLD Open Question Q3: field widths to be re-verified once
 * oi_p2_cb_segment's TS 38.212 §5.2.2 arithmetic is implemented and the three MVP TB sizes are
 * known — that's p2f-integration's job). Defined here since the struct itself is part of the
 * stable API surface (I9, P2-R17) that p2a's oi_p2_drain signature already commits to.
 */
#ifndef OI_P2_TB_RECORD_H
#define OI_P2_TB_RECORD_H

#include <cstdint>
#include <vector>

namespace oi_p2 {

constexpr uint32_t kTbRecordSchema = 0x00020001;  // "oi-p2-tb/1" (LLD §4.7)

// Fixed-size header per LLD §4.7 offsets 0-15; tb_data/crc24a are variable-length tails appended
// by the producer (oi_p2_drain), not part of this fixed struct -- callers read tb_size_bytes to
// know how many bytes of tb_data follow.
#pragma pack(push, 1)
struct TbRecordHeader {
  uint32_t schema;         // offset  0: kTbRecordSchema
  uint32_t slot_id;        // offset  4: matches oi_p2_drain's slot_id argument
  uint32_t tb_size_bytes;  // offset  8: TS 38.212 §5.1 TB size (MVP config + MCS derived)
  uint8_t  nof_cb;         // offset 12: 1 if TB fits without segmentation, else per §5.2.2
  uint8_t  base_graph;     // offset 13: 1 or 2
  uint8_t  crc24a_ok;      // offset 14: bool, 1 = TB CRC24A pass
  uint8_t  mcs_index;      // offset 15: 4, 13, or 21 (MVP set)
};
#pragma pack(pop)

static_assert(sizeof(TbRecordHeader) == 16, "TbRecordHeader must be exactly 16 bytes (LLD §4.7)");

// Full record: fixed header + variable-length tb_data + the 3-byte crc24a value.
struct TbRecord {
  TbRecordHeader header{};
  std::vector<uint8_t> tb_data;      // header.tb_size_bytes bytes, byte-aligned MSB-first
  uint8_t crc24a[3] = {0, 0, 0};     // the 24-bit CRC24A value itself (debug/oracle-tap use)
};

}  // namespace oi_p2

#endif /* OI_P2_TB_RECORD_H */
