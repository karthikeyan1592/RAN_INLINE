// osg_loader_crosscheck_test.cpp — proves the ru_emulator patch's OWN independent .osg loader
// (patches/files/ru_emulator_oracle_grid.cpp, compiled directly from the patch's file content --
// not a copy) agrees byte-for-byte with the project-side writer (oi_osg_format.h's osg_write) and
// the project-side reader (oi_osg_format.h's osg_read). Also exercises the loader's own error
// paths (P3-R2/R3's "loader ... unit" test plan entry) and P3-R6's select_symbol_payload rule
// directly against the patch's own implementation (not just oi_osg_schedule.h's formula, which
// osg_format_test.cpp already covers) -- this is the strongest local proxy available for "the
// patch's internal computation and the harness's independent computation agree" without building
// the full ru_emulator binary (deferred, needs the live rig per DEFERRED_LIVE_GATES.md).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../patches/files/ru_emulator_oracle_grid.h"
#include "../src/host/oi_osg_format.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

static oi_osg::osg_file make_file(uint8_t eaxc_id, uint32_t mcs_index, uint8_t fill_seed) {
  oi_osg::osg_file f{};
  f.iq_format = oi_osg::kIqFormatUncompressed16;
  f.numerology_mu = 1;
  f.nof_prb = 51;
  f.nof_symbols_per_slot = 14;
  f.nof_eaxc = 1;
  f.eaxc_id = eaxc_id;
  f.tb_crc_ok = 1;
  f.rnti = 0x4601;
  f.harq_id = 0;
  f.mcs_index = mcs_index;
  f.grid_payload.resize(oi_osg::osg_expected_grid_payload_len(f.nof_symbols_per_slot, f.nof_prb));
  for (size_t i = 0; i < f.grid_payload.size(); i++) f.grid_payload[i] = (uint8_t)(i + fill_seed);
  f.tb_payload = {(uint8_t)(0x10 + fill_seed), 0x20, 0x30};
  f.tb_len_bytes = f.tb_payload.size();
  return f;
}

int main() {
  // --- Cross-check: project-writer -> patch-loader ---
  const std::string path = "/tmp/osg_crosscheck_sample.osg";
  oi_osg::osg_file written = make_file(0, 13, 5);
  check(oi_osg::osg_write(path, written), "oi_osg_format::osg_write succeeds");

  std::vector<std::string> file_list = {path};
  ocudu::oracle_grid_set grids{};
  ocudu::oracle_grid_load_status st = ocudu::load_oracle_grid_set(ocudu::span<const std::string>(file_list), &grids);
  check(st == ocudu::oracle_grid_load_status::ok,
        std::string("patch's load_oracle_grid_set accepts a project-written .osg file (") +
            ocudu::oracle_grid_load_status_str(st) + ")");
  check(grids.entries.size() == 1, "patch's loader parsed exactly 1 entry");
  if (!grids.entries.empty()) {
    const auto& e = grids.entries[0];
    check(e.nof_prb == written.nof_prb, "crosscheck: nof_prb agrees between writer and patch-loader");
    check(e.nof_symbols == written.nof_symbols_per_slot, "crosscheck: nof_symbols agrees");
    check(e.eaxc_id == written.eaxc_id, "crosscheck: eaxc_id agrees");
    check(e.tb_len_bytes == written.tb_len_bytes, "crosscheck: tb_len_bytes agrees");
    check(e.tb_crc_ok == (written.tb_crc_ok != 0), "crosscheck: tb_crc_ok agrees");
    check(e.rnti == written.rnti, "crosscheck: rnti agrees");
    check(e.harq_id == written.harq_id, "crosscheck: harq_id agrees");
    check(e.mcs_index == written.mcs_index, "crosscheck: mcs_index agrees");
    check(e.grid_payload == written.grid_payload, "crosscheck: grid_payload byte-identical between writer and patch-loader");
    check(e.tb_payload == written.tb_payload, "crosscheck: tb_payload byte-identical between writer and patch-loader");
  }

  // --- Corrupted file rejected identically by both loaders (same CRC32 algorithm, independently
  // implemented -- zlib on the project side, hand-rolled table on the patch side) ---
  {
    std::vector<uint8_t> raw;
    FILE* f = std::fopen(path.c_str(), "rb");
    check(f != nullptr, "corruption crosscheck: can reopen written file");
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    raw.resize((size_t)sz);
    size_t rd = std::fread(raw.data(), 1, (size_t)sz, f);
    check(rd == (size_t)sz, "corruption crosscheck: read whole file");
    std::fclose(f);
    raw[100] ^= 0xFF;
    const std::string corrupt_path = "/tmp/osg_crosscheck_corrupt.osg";
    FILE* cf = std::fopen(corrupt_path.c_str(), "wb");
    std::fwrite(raw.data(), 1, raw.size(), cf);
    std::fclose(cf);

    std::vector<std::string> corrupt_list = {corrupt_path};
    ocudu::oracle_grid_set corrupt_grids{};
    ocudu::oracle_grid_load_status cst =
        ocudu::load_oracle_grid_set(ocudu::span<const std::string>(corrupt_list), &corrupt_grids);
    check(cst == ocudu::oracle_grid_load_status::err_bad_crc32,
          "corrupted file: patch's own independent CRC32 (hand-rolled table) also rejects it");

    oi_osg::osg_file project_readback{};
    oi_osg::osg_status pst = oi_osg::osg_read(corrupt_path, &project_readback);
    check(pst == oi_osg::osg_status::err_bad_crc32,
          "same corrupted file: project's own CRC32 (zlib) also rejects it -- two independent implementations agree");
  }

  // --- Empty file list rejected (P3-R2's "N==0 with injection enabled is a startup config error") ---
  {
    std::vector<std::string> empty_list;
    ocudu::oracle_grid_set empty_grids{};
    ocudu::oracle_grid_load_status est =
        ocudu::load_oracle_grid_set(ocudu::span<const std::string>(empty_list), &empty_grids);
    check(est == ocudu::oracle_grid_load_status::err_empty_file_list,
          "empty oracle_injection.files list -> load_oracle_grid_set reports err_empty_file_list");
  }

  // --- select_symbol_payload: P3-R6 schedule + payload slicing, against the patch's OWN
  // implementation (not oi_osg_schedule.h's copy) ---
  {
    // Build a small 3-file set with distinct, recognizable per-symbol content.
    std::vector<std::string> paths3;
    for (int i = 0; i < 3; i++) {
      oi_osg::osg_file f = make_file(0, 4, (uint8_t)(100 + i * 10));
      std::string p = "/tmp/osg_crosscheck_multi_" + std::to_string(i) + ".osg";
      oi_osg::osg_write(p, f);
      paths3.push_back(p);
    }
    ocudu::oracle_grid_set grids3{};
    ocudu::oracle_grid_load_status st3 =
        ocudu::load_oracle_grid_set(ocudu::span<const std::string>(paths3), &grids3);
    check(st3 == ocudu::oracle_grid_load_status::ok, "3-file set loads OK");
    check(grids3.entries.size() == 3, "3-file set: exactly 3 entries loaded");

    const unsigned slots_per_frame = 20;
    // file_idx(sfn=0, slot=0) = 0; (sfn=0, slot=1) = 1; (sfn=0, slot=2) = 2; (sfn=0, slot=3) = 3 mod 3 = 0
    struct Case {
      unsigned sfn, slot, expected_file_idx;
    };
    std::vector<Case> cases = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 0}, {1, 0, 20 % 3}, {2, 5, (2 * 20 + 5) % 3}};
    bool schedule_ok = true;
    for (const auto& c : cases) {
      ocudu::span<const uint8_t> payload =
          ocudu::select_symbol_payload(grids3, c.sfn, c.slot, slots_per_frame, /*eaxc_id=*/0, /*symbol_index=*/0);
      uint8_t expected_first_byte = (uint8_t)(100 + c.expected_file_idx * 10);
      if (payload.empty() || payload[0] != expected_first_byte) schedule_ok = false;
    }
    check(schedule_ok, "select_symbol_payload: file_idx=(sfn*slots_per_frame+slot) mod N routes to the right file "
                       "(verified via each file's distinguishable content), matching P3-R6");

    // Symbol-slicing: symbol 1's payload must start right after symbol 0's (2448 bytes/symbol at
    // 51 PRB uncompressed-16-bit), and be a different byte value (each symbol wasn't literally
    // identical filler in make_file's fill pattern: f.grid_payload[i] = i + fill_seed).
    ocudu::span<const uint8_t> sym0 = ocudu::select_symbol_payload(grids3, 0, 0, slots_per_frame, 0, 0);
    ocudu::span<const uint8_t> sym1 = ocudu::select_symbol_payload(grids3, 0, 0, slots_per_frame, 0, 1);
    check(sym0.size() == 51u * 12u * 4u, "select_symbol_payload: symbol byte length == nof_prb*12*4 (2448)");
    check(sym1.size() == sym0.size(), "select_symbol_payload: every symbol has the same byte length");
    check(sym1[0] != sym0[0], "select_symbol_payload: symbol 1's slice is genuinely offset from symbol 0's");
  }

  if (g_fail == 0) {
    std::printf("\nosg_loader_crosscheck_test: ALL PASS\n");
  } else {
    std::fprintf(stderr, "\nosg_loader_crosscheck_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
