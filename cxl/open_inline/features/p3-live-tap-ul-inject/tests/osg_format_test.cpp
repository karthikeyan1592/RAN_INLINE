// osg_format_test.cpp — .osg file format unit tests (P3-R2/P3-R3/P3-R6's format-level pieces):
// round-trip (write then read back byte-identical), CRC32 trailer correctly rejects a corrupted
// file, magic/version/iq_format validation, grid_payload_len_bytes cross-check, and the P3-R6
// schedule formula's own self-consistency (assert file_idx matches the documented modulo rule
// for a range of (sfn, slot) pairs spanning >N, per LLD §Test plan P3-R6).
//
// No OCUDU linkage needed here (pure file-format logic, no wire-encoding) -- this is deliberately
// a fast, dependency-light unit test distinct from osg_gen's own self-check (which DOES exercise
// the real OCUDU TX chain via oi_oracle_pack, see osg_gen.cpp).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/host/oi_osg_format.h"
#include "../src/host/oi_osg_schedule.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

static oi_osg::osg_file make_sample_file() {
  oi_osg::osg_file f{};
  f.iq_format = oi_osg::kIqFormatUncompressed16;
  f.numerology_mu = 1;
  f.nof_prb = 51;
  f.nof_symbols_per_slot = 14;
  f.nof_eaxc = 1;
  f.eaxc_id = 0;
  f.tb_crc_ok = 1;
  f.rnti = 0x4601;
  f.harq_id = 0;
  f.mcs_index = 4;
  f.grid_payload.resize(oi_osg::osg_expected_grid_payload_len(f.nof_symbols_per_slot, f.nof_prb));
  for (size_t i = 0; i < f.grid_payload.size(); i++) f.grid_payload[i] = (uint8_t)(i * 7 + 3);
  f.tb_payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
  f.tb_len_bytes = f.tb_payload.size();
  return f;
}

int main() {
  const std::string path = "/tmp/osg_format_test_sample.osg";

  // --- Round-trip: write then read back byte-identical ---
  oi_osg::osg_file written = make_sample_file();
  bool wrote_ok = oi_osg::osg_write(path, written);
  check(wrote_ok, "osg_write succeeds for a well-formed file");

  oi_osg::osg_file readback{};
  oi_osg::osg_status st = oi_osg::osg_read(path, &readback);
  check(st == oi_osg::osg_status::ok, std::string("osg_read succeeds on a freshly-written file (") +
                                          oi_osg::osg_status_str(st) + ")");
  check(readback.format_version == written.format_version, "round-trip: format_version preserved");
  check(readback.iq_format == written.iq_format, "round-trip: iq_format preserved");
  check(readback.numerology_mu == written.numerology_mu, "round-trip: numerology_mu preserved");
  check(readback.nof_prb == written.nof_prb, "round-trip: nof_prb preserved");
  check(readback.nof_symbols_per_slot == written.nof_symbols_per_slot, "round-trip: nof_symbols_per_slot preserved");
  check(readback.eaxc_id == written.eaxc_id, "round-trip: eaxc_id preserved");
  check(readback.tb_len_bytes == written.tb_len_bytes, "round-trip: tb_len_bytes preserved");
  check(readback.tb_crc_ok == written.tb_crc_ok, "round-trip: tb_crc_ok preserved");
  check(readback.rnti == written.rnti, "round-trip: rnti preserved");
  check(readback.harq_id == written.harq_id, "round-trip: harq_id preserved");
  check(readback.mcs_index == written.mcs_index, "round-trip: mcs_index preserved");
  check(readback.grid_payload == written.grid_payload, "round-trip: grid_payload byte-identical");
  check(readback.tb_payload == written.tb_payload, "round-trip: tb_payload byte-identical");

  // --- CRC32 trailer catches corruption (flip one payload byte, leave the trailer alone) ---
  {
    std::vector<uint8_t> raw;
    {
      FILE* f = std::fopen(path.c_str(), "rb");
      check(f != nullptr, "corruption test: can reopen the written file raw");
      std::fseek(f, 0, SEEK_END);
      long sz = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      raw.resize((size_t)sz);
      size_t rd = std::fread(raw.data(), 1, (size_t)sz, f);
      check(rd == (size_t)sz, "corruption test: read whole file");
      std::fclose(f);
    }
    raw[48] ^= 0xFF;  // flip the first grid_payload byte
    const std::string corrupt_path = "/tmp/osg_format_test_corrupt.osg";
    FILE* cf = std::fopen(corrupt_path.c_str(), "wb");
    check(cf != nullptr, "corruption test: can open corrupt-file output");
    std::fwrite(raw.data(), 1, raw.size(), cf);
    std::fclose(cf);

    oi_osg::osg_file corrupt_readback{};
    oi_osg::osg_status cst = oi_osg::osg_read(corrupt_path, &corrupt_readback);
    check(cst == oi_osg::osg_status::err_bad_crc32, "corrupted payload byte -> osg_read reports err_bad_crc32");
  }

  // --- Bad magic rejected ---
  {
    oi_osg::osg_file bad_magic_file = make_sample_file();
    const std::string bm_path = "/tmp/osg_format_test_badmagic.osg";
    oi_osg::osg_write(bm_path, bad_magic_file);
    // Corrupt the magic bytes directly (write valid file, then flip byte 0).
    std::vector<uint8_t> raw;
    FILE* f = std::fopen(bm_path.c_str(), "r+b");
    check(f != nullptr, "bad-magic test: can reopen for patch");
    std::fseek(f, 0, SEEK_SET);
    uint8_t bad = 'X';
    std::fwrite(&bad, 1, 1, f);
    std::fclose(f);
    oi_osg::osg_file out{};
    oi_osg::osg_status st2 = oi_osg::osg_read(bm_path, &out);
    check(st2 == oi_osg::osg_status::err_bad_magic, "corrupted magic byte -> osg_read reports err_bad_magic");
  }

  // --- Non-zero iq_format rejected (MVP pins uncompressed-16-bit only, LLD §3.1) ---
  {
    oi_osg::osg_file f = make_sample_file();
    f.iq_format = 1;  // reserved, not the pinned MVP value
    const std::string p = "/tmp/osg_format_test_badiqfmt.osg";
    oi_osg::osg_write(p, f);
    oi_osg::osg_file out{};
    oi_osg::osg_status st2 = oi_osg::osg_read(p, &out);
    check(st2 == oi_osg::osg_status::err_bad_iq_format, "non-zero iq_format -> osg_read reports err_bad_iq_format");
  }

  // --- grid_payload_len_bytes mismatch rejected (LLD §Error handling) ---
  {
    oi_osg::osg_file f = make_sample_file();
    f.grid_payload.resize(f.grid_payload.size() - 4);  // now inconsistent with nof_symbols*nof_prb*12*4
    const std::string p = "/tmp/osg_format_test_badlen.osg";
    oi_osg::osg_write(p, f);
    oi_osg::osg_file out{};
    oi_osg::osg_status st2 = oi_osg::osg_read(p, &out);
    check(st2 == oi_osg::osg_status::err_grid_payload_len_mismatch,
          "shrunk grid_payload -> osg_read reports err_grid_payload_len_mismatch");
  }

  // --- Too-short file rejected ---
  {
    const std::string p = "/tmp/osg_format_test_tooshort.osg";
    FILE* f = std::fopen(p.c_str(), "wb");
    check(f != nullptr, "too-short test: can create a tiny file");
    const char junk[8] = {0};
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);
    oi_osg::osg_file out{};
    oi_osg::osg_status st2 = oi_osg::osg_read(p, &out);
    check(st2 == oi_osg::osg_status::err_too_short, "8-byte file -> osg_read reports err_too_short");
  }

  // --- P3-R6 schedule formula self-consistency: file_idx matches the documented modulo rule for
  // a range of (sfn, slot) spanning > N, and (trivially, same function) agrees with itself --
  // the LLD's real requirement is that the PATCH's internal computation and the HARNESS's
  // independent computation agree; since both would call this identical formula (the patch's
  // copy is textually identical per oi_osg_schedule.h's header comment), this test at least
  // proves the formula itself is well-defined and matches the documented rule by direct
  // construction for every case, including wraparound past N.
  {
    const uint64_t N = 20;
    const uint64_t slots_per_frame = oi_osg::kSlotsPerFrameMu1;
    bool all_ok = true;
    for (uint64_t sfn = 0; sfn < 5; sfn++) {          // spans multiple frames
      for (uint64_t slot = 0; slot < slots_per_frame; slot++) {
        uint64_t got = oi_osg::osg_schedule_file_idx(sfn, slot, slots_per_frame, N);
        uint64_t expected = (sfn * slots_per_frame + slot) % N;
        if (got != expected) all_ok = false;
      }
    }
    check(all_ok, "P3-R6: file_idx matches (sfn*slots_per_frame+slot) mod N across sfn=0..4, slot=0..19");

    // N == slots_per_frame special case (osg_gen's own design constraint, see osg_gen.cpp header):
    // file_idx must equal slot exactly, independent of sfn, so DMRS nslot stays consistent.
    bool aligned_ok = true;
    for (uint64_t sfn = 0; sfn < 10; sfn++) {
      for (uint64_t slot = 0; slot < slots_per_frame; slot++) {
        uint64_t got = oi_osg::osg_schedule_file_idx(sfn, slot, slots_per_frame, slots_per_frame);
        if (got != slot) aligned_ok = false;
      }
    }
    check(aligned_ok, "P3-R6: N==slots_per_frame makes file_idx==slot for every sfn (osg_gen's DMRS-safety invariant)");

    check(oi_osg::osg_schedule_file_idx(0, 0, slots_per_frame, 0) == 0,
          "P3-R6: N==0 (empty file list) doesn't crash (osg_gen never produces this; defensive only)");
  }

  if (g_fail == 0) {
    std::printf("\nosg_format_test: ALL PASS\n");
  } else {
    std::fprintf(stderr, "\nosg_format_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
