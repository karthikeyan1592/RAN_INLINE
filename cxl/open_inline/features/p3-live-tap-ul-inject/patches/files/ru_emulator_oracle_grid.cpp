// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// ru_emulator_oracle_grid.cpp — see ru_emulator_oracle_grid.h for the format/sharing rationale.
//
// CRC32 (IEEE 802.3 / CRC-32/ISO-HDLC, polynomial 0xEDB88320): hand-rolled table-based
// implementation, not a new external dependency -- this file must stay self-contained to remain
// upstreamable (HLD D7); the project-side oi_osg_format.cpp uses zlib's crc32() instead (this
// project's own build already links zlib), but ru_emulator's build should not gain a new required
// library just for this feature. Standard, textbook reflected-polynomial table generation --
// same class of "plain, standardized algorithm, safe to write directly" as oracle_tx_gen.cpp's
// own pcap writer (see that file's header comment for the precedent).
#include "ru_emulator_oracle_grid.h"

#include <array>
#include <cstdio>
#include <fstream>

namespace ocudu {

namespace {

constexpr size_t kFixedHeaderBytes = 48;
constexpr size_t kTrailerBytes = 4;

uint16_t get_u16le(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t get_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

std::array<uint32_t, 256> make_crc32_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}

uint32_t crc32_ieee(const uint8_t* data, size_t len) {
  static const std::array<uint32_t, 256> table = make_crc32_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint64_t expected_grid_payload_len(uint8_t nof_symbols_per_slot, uint16_t nof_prb) {
  return (uint64_t)nof_symbols_per_slot * (uint64_t)nof_prb * 12ull * 4ull;
}

}  // namespace

const char* oracle_grid_load_status_str(oracle_grid_load_status s) {
  switch (s) {
    case oracle_grid_load_status::ok: return "ok";
    case oracle_grid_load_status::err_open: return "could not open file";
    case oracle_grid_load_status::err_bad_magic: return "bad magic (expected 'OSG1')";
    case oracle_grid_load_status::err_bad_version: return "unsupported format_version (expected 1)";
    case oracle_grid_load_status::err_bad_iq_format: return "non-zero iq_format (only uncompressed 16-bit supported)";
    case oracle_grid_load_status::err_bad_crc32: return "file_crc32 trailer mismatch";
    case oracle_grid_load_status::err_grid_payload_len_mismatch:
      return "grid_payload_len_bytes != nof_symbols_per_slot*nof_prb*12*4";
    case oracle_grid_load_status::err_truncated: return "declared lengths exceed actual file size";
    case oracle_grid_load_status::err_empty_file_list: return "oracle_injection.files is empty";
    case oracle_grid_load_status::err_inconsistent_iq_format_across_files:
      return "not every .osg file in the list declares the same iq_format";
  }
  return "unknown oracle_grid_load_status";
}

oracle_grid_load_status load_oracle_grid_set(span<const std::string> file_list, oracle_grid_set* out) {
  *out = oracle_grid_set{};
  if (file_list.empty()) {
    return oracle_grid_load_status::err_empty_file_list;
  }

  for (const std::string& path : file_list) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return oracle_grid_load_status::err_open;
    std::streamsize size = in.tellg();
    if (size < (std::streamsize)(kFixedHeaderBytes + kTrailerBytes)) return oracle_grid_load_status::err_truncated;
    in.seekg(0);
    std::vector<uint8_t> buf((size_t)size);
    in.read((char*)buf.data(), size);
    if (!in) return oracle_grid_load_status::err_open;

    if (!(buf[0] == 'O' && buf[1] == 'S' && buf[2] == 'G' && buf[3] == '1')) {
      return oracle_grid_load_status::err_bad_magic;
    }
    uint16_t format_version = get_u16le(&buf[4]);
    if (format_version != 1) return oracle_grid_load_status::err_bad_version;
    uint8_t iq_format = buf[6];
    if (iq_format != 0) return oracle_grid_load_status::err_bad_iq_format;  // MVP pin, LLD §3.1

    oracle_grid_entry entry{};
    entry.nof_prb = get_u16le(&buf[8]);
    entry.nof_symbols = buf[10];
    entry.eaxc_id = (uint8_t)get_u16le(&buf[12]);
    entry.tb_len_bytes = get_u64le(&buf[16]);
    entry.tb_crc_ok = get_u32le(&buf[24]) != 0;
    entry.rnti = get_u32le(&buf[28]);
    entry.harq_id = get_u32le(&buf[32]);
    entry.mcs_index = get_u32le(&buf[36]);
    uint64_t grid_len = get_u64le(&buf[40]);

    if (grid_len != expected_grid_payload_len(entry.nof_symbols, entry.nof_prb)) {
      return oracle_grid_load_status::err_grid_payload_len_mismatch;
    }
    uint64_t need = kFixedHeaderBytes + grid_len + entry.tb_len_bytes + kTrailerBytes;
    if (need > (uint64_t)size) return oracle_grid_load_status::err_truncated;

    size_t grid_off = kFixedHeaderBytes;
    size_t tb_off = grid_off + (size_t)grid_len;
    size_t crc_off = tb_off + (size_t)entry.tb_len_bytes;

    uint32_t file_crc = get_u32le(&buf[crc_off]);
    uint32_t computed_crc = crc32_ieee(buf.data(), crc_off);
    if (file_crc != computed_crc) return oracle_grid_load_status::err_bad_crc32;

    entry.grid_payload.assign(buf.begin() + (long)grid_off, buf.begin() + (long)tb_off);
    entry.tb_payload.assign(buf.begin() + (long)tb_off, buf.begin() + (long)crc_off);

    out->entries.push_back(std::move(entry));
  }

  // All files must declare the same iq_format (they all did, checked above -- iq_format==0,
  // uncompressed 16-bit -- the per-file check above already enforces this implicitly since only
  // one value is accepted at MVP; this loop confirms nof_prb/nof_symbols consistency too, which
  // the per-file check does NOT already guarantee).
  const oracle_grid_entry& first = out->entries.front();
  for (const auto& e : out->entries) {
    if (e.nof_prb != first.nof_prb || e.nof_symbols != first.nof_symbols) {
      return oracle_grid_load_status::err_inconsistent_iq_format_across_files;
    }
  }
  out->iq_format = ofh::ru_compression_params{ofh::compression_type::none, 16};
  return oracle_grid_load_status::ok;
}

span<const uint8_t> select_symbol_payload(const oracle_grid_set& grids, unsigned sfn, unsigned slot,
                                          unsigned slots_per_frame, uint8_t eaxc_id, unsigned symbol_index) {
  ocudu_assert(!grids.entries.empty(), "select_symbol_payload called with an empty oracle_grid_set");
  size_t n = grids.entries.size();
  size_t file_idx = (size_t)(((uint64_t)sfn * slots_per_frame + slot) % n);  // P3-R6, exact rule
  const oracle_grid_entry& entry = grids.entries[file_idx];
  // eaxc_id consistency is validated once at config-load time (single-eAxC MVP, LLD §Q3), not
  // re-checked per call here -- (void) to document the parameter is intentionally unused for that.
  (void)eaxc_id;
  size_t symbol_bytes = (size_t)entry.nof_prb * 12u * 4u;
  size_t offset = (size_t)symbol_index * symbol_bytes;
  ocudu_assert(offset + symbol_bytes <= entry.grid_payload.size(), "symbol_index out of range for this oracle grid");
  return span<const uint8_t>(entry.grid_payload).subspan(offset, symbol_bytes);
}

}  // namespace ocudu
