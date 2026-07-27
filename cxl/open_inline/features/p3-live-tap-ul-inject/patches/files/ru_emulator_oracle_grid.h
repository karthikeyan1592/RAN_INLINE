// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// ru_emulator_oracle_grid.h — M2's oracle-grid loader (p3-live-tap-ul-inject, LLD §Public APIs).
// New file added by the oracle-injection patch series; not present upstream.
//
// Reads the ".osg" (Oracle Slot Grid v1) file format, LLD §3.1, byte-precise. This parser is a
// deliberately independent implementation from open_inline/features/p3-live-tap-ul-inject/src/
// host/oi_osg_format.h (the project-side writer/reader used by osg_gen and the harness) --
// ru_emulator becomes its own standalone binary once this patch is built into the OCUDU image,
// with no dependency on this project's build system (required for the file to be upstreamable as
// a clean, self-contained OCUDU contribution, HLD D7). The two implementations are kept in sync
// by construction (both read/write the identical byte layout documented in LLD §3.1) and cross-
// checked by p3's own test suite (osg_loader_crosscheck_test.cpp): a real .osg file written by
// oi_osg_format.h's osg_write() is parsed by THIS file's load_oracle_grid_set() and the fields
// must agree exactly. If LLD §3.1 ever changes, both copies must be updated together.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ocudu/adt/span.h"
#include "ocudu/ofh/compression/compression_params.h"

namespace ocudu {

/// One loaded, in-memory oracle grid (LLD §3.1 section 1) plus its sidecar metadata.
struct oracle_grid_entry {
  uint16_t nof_prb;
  uint8_t nof_symbols;
  uint8_t eaxc_id;
  std::vector<uint8_t> grid_payload;  // raw wire-format IQ, symbol-major (LLD §3.1 section 1)
  uint64_t tb_len_bytes;
  bool tb_crc_ok;
  uint32_t rnti;
  uint32_t harq_id;
  uint32_t mcs_index;
  std::vector<uint8_t> tb_payload;    // ground truth; ru_emulator's TX path never reads this
};

/// Loaded once at ru_emulator startup, from the config's oracle_injection.files list.
struct oracle_grid_set {
  std::vector<oracle_grid_entry> entries;   // ordered, index = schedule position (P3-R6)
  ofh::ru_compression_params iq_format;          // parsed from entries[0], validated equal across all entries
};

enum class oracle_grid_load_status {
  ok = 0,
  err_open,
  err_bad_magic,
  err_bad_version,
  err_bad_iq_format,
  err_bad_crc32,
  err_grid_payload_len_mismatch,
  err_truncated,
  err_empty_file_list,
  err_inconsistent_iq_format_across_files,
};

const char* oracle_grid_load_status_str(oracle_grid_load_status s);

/// Parses+validates every file in `file_list` (magic, version, CRC32 trailer, iq_format
/// consistency across all files, grid_payload_len == nof_symbols*nof_prb*12*4 for the pinned
/// uncompressed 16-bit format). Returns a non-ok status (not partial success) on any malformed
/// file (P3-R2/R3, LLD §Error handling) -- `*out` is left empty on failure. An empty `file_list`
/// is itself an error (err_empty_file_list): P3-R2 requires `oracle_injection.enabled: true` to
/// always have at least one file; the modulo schedule (P3-R6) is undefined for N==0.
oracle_grid_load_status load_oracle_grid_set(span<const std::string> file_list, oracle_grid_set* out);

/// select_symbol_payload: the per-burst patch point call (LLD §Public APIs).
/// file_idx = (sfn * slots_per_frame + slot) mod grids.entries.size()   -- P3-R6, exact rule
/// returns entries[file_idx].grid_payload sliced to [symbol_index]'s byte range.
span<const uint8_t> select_symbol_payload(const oracle_grid_set& grids, unsigned sfn, unsigned slot,
                                          unsigned slots_per_frame, uint8_t eaxc_id, unsigned symbol_index);

}  // namespace ocudu
