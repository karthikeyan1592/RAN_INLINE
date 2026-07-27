/* oi_osg_format.h — M2's ".osg" (Oracle Slot Grid v1) file format reader/writer, byte-precise per
 * p3-live-tap-ul-inject/spec/LLD.md §3.1. Shared between:
 *  - tools/osg_gen.cpp (p3's oracle-file generator, writes .osg files from the shared
 *    oi_oracle_pack library's packed_tb output — LLD Q1's ".osg-file writer" front end)
 *  - the ru_emulator patch's M2 loader (reads offsets 0-47 + grid_payload only, never
 *    tb_payload — LLD §3.1's "loader ... MAY skip reading it" note)
 *  - the p3 harness (M4/M5, reads the FULL file including tb_payload, to compute the expected TB
 *    independently of the live decode)
 *
 * This header defines ONE parser/writer pair used by all three front ends above, so the byte
 * layout cannot drift between what osg_gen.cpp writes and what the loader/harness read (same
 * discipline as oi_oracle_pack.h's "one shared implementation" rationale).
 *
 * CRC32: real zlib crc32() (IEEE 802.3 / CRC-32/ISO-HDLC polynomial 0xEDB88320) — LLD §3.1
 * pins "IEEE 802.3 poly" exactly, and zlib's crc32() implements that standard poly; using the
 * well-tested library function rather than hand-rolling a CRC32 table.
 */
#ifndef OI_OSG_FORMAT_H
#define OI_OSG_FORMAT_H

#include <cstdint>
#include <string>
#include <vector>

namespace oi_osg {

constexpr uint32_t kMagic = 0x4753'4F31u;       // "OSG1" as a little-endian u32 read from bytes 'O','S','G','1'
constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kIqFormatUncompressed16 = 0;  // pinned MVP value (LLD §3.1); any other value rejected

// One .osg file's full parsed contents (LLD §3.1's every field). `grid_payload`/`tb_payload` are
// read as raw bytes -- interpretation (big-endian int16 I/Q for grid_payload; TS 38.212
// bit-packed bytes for tb_payload) is the caller's job, this module only handles file I/O +
// structural validation (magic/version/CRC32/length-consistency), matching M2's "parse+validate"
// scope (LLD §Public APIs, load_oracle_grid_set).
struct osg_file {
  uint16_t format_version = kFormatVersion;
  uint8_t iq_format = kIqFormatUncompressed16;
  uint8_t numerology_mu = 1;
  uint16_t nof_prb = 51;
  uint8_t nof_symbols_per_slot = 14;
  uint8_t nof_eaxc = 1;
  uint16_t eaxc_id = 0;
  uint64_t tb_len_bytes = 0;
  uint32_t tb_crc_ok = 1;
  uint32_t rnti = 0;
  uint32_t harq_id = 0;
  uint32_t mcs_index = 0;
  std::vector<uint8_t> grid_payload;  // wire-format IQ, big-endian int16 pairs (LLD §3.1)
  std::vector<uint8_t> tb_payload;    // ground-truth TB bytes (TS 38.212 bit-to-byte packing)
};

enum class osg_status {
  ok = 0,
  err_open,             // couldn't open the file for read/write
  err_too_short,        // file smaller than the fixed 48-byte header + trailer
  err_bad_magic,
  err_bad_version,
  err_bad_iq_format,    // non-zero iq_format (reserved, MVP loader rejects)
  err_bad_crc32,        // file_crc32 trailer mismatch
  err_grid_payload_len_mismatch,  // grid_payload_len_bytes != nof_symbols_per_slot*nof_prb*12*4
  err_truncated_payload,          // declared lengths exceed actual file size
};

const char* osg_status_str(osg_status s);

// Computes the expected grid_payload length for the given dimensions -- LLD §3.1's
// "nof_symbols_per_slot * nof_prb * 12 * 4" formula, exposed so callers (loader, generator, tests)
// share one formula instance instead of re-deriving it.
uint64_t osg_expected_grid_payload_len(uint8_t nof_symbols_per_slot, uint16_t nof_prb);

// Writes a fully-populated osg_file to `path`, computing and appending the real file_crc32
// trailer. Fails (returns false) only on an I/O error opening `path` for write -- callers are
// responsible for populating fields consistently (this is the generator side, not the validating
// loader side; osg_gen.cpp always builds internally-consistent files by construction).
bool osg_write(const std::string& path, const osg_file& file);

// Reads and fully validates a .osg file: magic, version, iq_format (rejects non-zero at MVP),
// grid_payload_len_bytes consistency, and the file_crc32 trailer (computed over every byte
// preceding the trailer itself). Returns osg_status::ok and populates `out` only if every check
// passes -- "no silent fallback" per LLD §Error handling: any check failure is reported via the
// specific osg_status, never a partial/best-effort parse.
osg_status osg_read(const std::string& path, osg_file* out);

}  // namespace oi_osg

#endif  // OI_OSG_FORMAT_H
