// oi_osg_format.cpp — see oi_osg_format.h for the format/sharing rationale.
//
// Portability note: every multi-byte header field is packed/unpacked byte-by-byte (not via
// reinterpret_cast/memcpy of a packed struct onto the file), so this reader/writer is correct
// regardless of host endianness or struct padding -- matches this project's lint_portability.py
// discipline (already enforced elsewhere, e.g. oi_frame_desc's own byte-precise (de)serialization).
#include "oi_osg_format.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include <zlib.h>

namespace oi_osg {

namespace {

constexpr size_t kFixedHeaderBytes = 48;  // offsets 0-47, LLD §3.1
constexpr size_t kTrailerBytes = 4;       // file_crc32

void put_u16le(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back((uint8_t)(v & 0xFF));
  buf.push_back((uint8_t)((v >> 8) & 0xFF));
}
void put_u32le(std::vector<uint8_t>& buf, uint32_t v) {
  for (int i = 0; i < 4; i++) buf.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void put_u64le(std::vector<uint8_t>& buf, uint64_t v) {
  for (int i = 0; i < 8; i++) buf.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
uint16_t get_u16le(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t get_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

}  // namespace

const char* osg_status_str(osg_status s) {
  switch (s) {
    case osg_status::ok: return "ok";
    case osg_status::err_open: return "could not open file";
    case osg_status::err_too_short: return "file too short for fixed header+trailer";
    case osg_status::err_bad_magic: return "bad magic (expected 'OSG1')";
    case osg_status::err_bad_version: return "unsupported format_version (expected 1)";
    case osg_status::err_bad_iq_format: return "non-zero iq_format (only uncompressed 16-bit supported at MVP)";
    case osg_status::err_bad_crc32: return "file_crc32 trailer mismatch";
    case osg_status::err_grid_payload_len_mismatch:
      return "grid_payload_len_bytes != nof_symbols_per_slot*nof_prb*12*4";
    case osg_status::err_truncated_payload: return "declared lengths exceed actual file size";
  }
  return "unknown osg_status";
}

uint64_t osg_expected_grid_payload_len(uint8_t nof_symbols_per_slot, uint16_t nof_prb) {
  return (uint64_t)nof_symbols_per_slot * (uint64_t)nof_prb * 12ull * 4ull;
}

bool osg_write(const std::string& path, const osg_file& f) {
  std::vector<uint8_t> buf;
  buf.reserve(kFixedHeaderBytes + f.grid_payload.size() + f.tb_payload.size() + kTrailerBytes);

  // offset 0: magic "OSG1"
  buf.push_back('O');
  buf.push_back('S');
  buf.push_back('G');
  buf.push_back('1');
  put_u16le(buf, f.format_version);          // 4
  buf.push_back(f.iq_format);                // 6
  buf.push_back(f.numerology_mu);            // 7
  put_u16le(buf, f.nof_prb);                 // 8
  buf.push_back(f.nof_symbols_per_slot);     // 10
  buf.push_back(f.nof_eaxc);                 // 11
  put_u16le(buf, f.eaxc_id);                 // 12
  put_u16le(buf, 0);                         // 14: reserved
  put_u64le(buf, f.tb_len_bytes);            // 16
  put_u32le(buf, f.tb_crc_ok);               // 24
  put_u32le(buf, f.rnti);                    // 28
  put_u32le(buf, f.harq_id);                 // 32
  put_u32le(buf, f.mcs_index);                // 36
  put_u64le(buf, f.grid_payload.size());     // 40
  // offset 48 must be reached exactly here.
  if (buf.size() != kFixedHeaderBytes) {
    std::fprintf(stderr, "oi_osg_format: internal error, header size %zu != %zu\n", buf.size(), kFixedHeaderBytes);
    return false;
  }
  buf.insert(buf.end(), f.grid_payload.begin(), f.grid_payload.end());
  buf.insert(buf.end(), f.tb_payload.begin(), f.tb_payload.end());

  uint32_t crc = (uint32_t)crc32(0L, buf.data(), (uInt)buf.size());
  put_u32le(buf, crc);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write((const char*)buf.data(), (std::streamsize)buf.size());
  return out.good();
}

osg_status osg_read(const std::string& path, osg_file* out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return osg_status::err_open;
  std::streamsize size = in.tellg();
  if (size < (std::streamsize)(kFixedHeaderBytes + kTrailerBytes)) return osg_status::err_too_short;
  in.seekg(0);
  std::vector<uint8_t> buf((size_t)size);
  in.read((char*)buf.data(), size);
  if (!in) return osg_status::err_open;

  if (!(buf[0] == 'O' && buf[1] == 'S' && buf[2] == 'G' && buf[3] == '1')) return osg_status::err_bad_magic;

  osg_file f{};
  f.format_version = get_u16le(&buf[4]);
  if (f.format_version != kFormatVersion) return osg_status::err_bad_version;
  f.iq_format = buf[6];
  if (f.iq_format != kIqFormatUncompressed16) return osg_status::err_bad_iq_format;
  f.numerology_mu = buf[7];
  f.nof_prb = get_u16le(&buf[8]);
  f.nof_symbols_per_slot = buf[10];
  f.nof_eaxc = buf[11];
  f.eaxc_id = get_u16le(&buf[12]);
  // bytes 14-15 reserved, ignored on read
  f.tb_len_bytes = get_u64le(&buf[16]);
  f.tb_crc_ok = get_u32le(&buf[24]);
  f.rnti = get_u32le(&buf[28]);
  f.harq_id = get_u32le(&buf[32]);
  f.mcs_index = get_u32le(&buf[36]);
  uint64_t grid_len = get_u64le(&buf[40]);

  uint64_t expected_grid_len = osg_expected_grid_payload_len(f.nof_symbols_per_slot, f.nof_prb);
  if (grid_len != expected_grid_len) return osg_status::err_grid_payload_len_mismatch;

  uint64_t need = kFixedHeaderBytes + grid_len + f.tb_len_bytes + kTrailerBytes;
  if (need > (uint64_t)size) return osg_status::err_truncated_payload;

  size_t grid_off = kFixedHeaderBytes;
  size_t tb_off = grid_off + (size_t)grid_len;
  size_t crc_off = tb_off + (size_t)f.tb_len_bytes;

  uint32_t file_crc = get_u32le(&buf[crc_off]);
  uint32_t computed_crc = (uint32_t)crc32(0L, buf.data(), (uInt)crc_off);
  if (file_crc != computed_crc) return osg_status::err_bad_crc32;

  f.grid_payload.assign(buf.begin() + (long)grid_off, buf.begin() + (long)tb_off);
  f.tb_payload.assign(buf.begin() + (long)tb_off, buf.begin() + (long)crc_off);

  *out = std::move(f);
  return osg_status::ok;
}

}  // namespace oi_osg
