// osg_gen.cpp — p3's ".osg" oracle-grid-set generator (LLD M2's file-format writer front end,
// LLD Q1's shared-packer resolution: this tool calls the SAME oi_oracle_pack library
// p2f-integration/tools/oracle_tx_gen.cpp uses, just writes the grid to a flat .osg file (via
// oi_osg_format) instead of wrapping it in eCPRI/O-RAN/pcap framing — ru_emulator's own real
// uplane_message_builder does that wrapping at injection time (p3 HLD D1/D2), so this tool never
// needs to.
//
// REAL, GROUNDED DESIGN CONSTRAINT this generator satisfies (not in the LLD's illustrative "3
// example files" config snippet -- found by reading the actual decode-side DMRS code, not
// assumed): p2-phy-kernels' K2a channel estimation (oi_p2_host.cpp) computes
// `nslot = slot_id % 20` from the REAL wire slot_id of the frame being decoded, and
// oi_dmrs_ref_seq_generate's c_init formula (oi_dmrs_ref_seq.cpp:31) is directly a function of
// that nslot. If an .osg file's baked-in DMRS grid were generated with a DIFFERENT nslot than the
// real within-frame slot position it gets injected at, K2a's channel estimate would use the wrong
// reference sequence and corrupt decode -- silently, not as a crash. The P3-R6 schedule formula
// `file_idx = (sfn*slots_per_frame + slot) mod N` is nslot-safe ONLY when N == slots_per_frame
// (20 at the pinned mu=1 numerology): then file_idx == slot mod 20 for every sfn, so file[i] is
// ALWAYS injected at within-frame slot i, matching a fixed nslot=i baked in at generation time.
// Any other N would make some file get reused at multiple different real within-frame slots over
// a run, breaking DMRS correctness at every reuse after the first. This generator therefore always
// produces exactly `slots_per_frame` (20) files, one per nslot 0..19 -- not an arbitrary count.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/host/oi_osg_format.h"
#include "../src/host/oi_osg_schedule.h"
#include "../../p2f-integration/src/host/oi_oracle_pack.h"

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: osg_gen <mcs_index> <seed_base> <rnti> <harq_id> <out_dir>\n"
                 "  generates exactly %u files (slot_0000.osg .. slot_%04u.osg) into <out_dir>,\n"
                 "  one per within-frame slot nslot=0..%u (see file header for why this count is\n"
                 "  not arbitrary), each seeded seed_base+nslot for distinct-but-deterministic TB\n"
                 "  content per slot.\n",
                 oi_osg::kSlotsPerFrameMu1, oi_osg::kSlotsPerFrameMu1 - 1, oi_osg::kSlotsPerFrameMu1 - 1);
    return 2;
  }
  uint32_t mcs_index = (uint32_t)std::atoi(argv[1]);
  uint32_t seed_base = (uint32_t)std::atoi(argv[2]);
  uint32_t rnti = (uint32_t)std::strtoul(argv[3], nullptr, 0);
  uint32_t harq_id = (uint32_t)std::strtoul(argv[4], nullptr, 0);
  std::string out_dir = argv[5];

  const uint32_t n_id = 1u;  // matches oracle_tx_gen.cpp's pinned MVP scrambling identity

  const oi_oracle::mcs_point* mcs = oi_oracle::find_mcs(mcs_index);
  if (!mcs) {
    std::fprintf(stderr, "osg_gen: unknown mcs_index %u (expected 4, 13, or 21)\n", mcs_index);
    return 2;
  }

  for (unsigned nslot = 0; nslot < oi_osg::kSlotsPerFrameMu1; nslot++) {
    oi_oracle::packed_tb packed = oi_oracle::pack_tb(mcs_index, seed_base + nslot, rnti, n_id, nslot);

    oi_osg::osg_file f{};
    f.iq_format = oi_osg::kIqFormatUncompressed16;
    f.numerology_mu = 1;
    f.nof_prb = oi_oracle::kNofPrb;
    f.nof_symbols_per_slot = (uint8_t)oi_oracle::kNofSymbols;
    f.nof_eaxc = 1;
    f.eaxc_id = 0;
    f.tb_len_bytes = packed.tb_bytes.size();
    f.tb_crc_ok = 1;  // real, correctly-encoded TB via the real segmenter/encoder chain (always valid)
    f.rnti = rnti;
    f.harq_id = harq_id;
    f.mcs_index = mcs_index;
    f.tb_payload = packed.tb_bytes;

    f.grid_payload.reserve(oi_osg::osg_expected_grid_payload_len(f.nof_symbols_per_slot, f.nof_prb));
    for (unsigned s = 0; s < oi_oracle::kNofSymbols; s++) {
      std::vector<uint8_t> symbol_bytes = oi_oracle::pack_symbol_to_wire_iq(packed.re_grid[s]);
      f.grid_payload.insert(f.grid_payload.end(), symbol_bytes.begin(), symbol_bytes.end());
    }
    uint64_t expected_len = oi_osg::osg_expected_grid_payload_len(f.nof_symbols_per_slot, f.nof_prb);
    if (f.grid_payload.size() != expected_len) {
      std::fprintf(stderr, "osg_gen: internal error, grid_payload size %zu != expected %llu\n", f.grid_payload.size(),
                  (unsigned long long)expected_len);
      return 1;
    }

    char fname[64];
    std::snprintf(fname, sizeof(fname), "/slot_%04u.osg", nslot);
    std::string path = out_dir + fname;
    if (!oi_osg::osg_write(path, f)) {
      std::fprintf(stderr, "osg_gen: failed to write %s\n", path.c_str());
      return 1;
    }

    // Self-verify by reading back immediately (same discipline as oracle_tx_gen.cpp's self-check
    // -- an oracle file that hasn't verified its own round-trip is not trustworthy enough to gate
    // anything downstream).
    oi_osg::osg_file readback{};
    oi_osg::osg_status st = oi_osg::osg_read(path, &readback);
    if (st != oi_osg::osg_status::ok) {
      std::fprintf(stderr, "osg_gen self-check FAILED for %s: %s\n", path.c_str(), oi_osg::osg_status_str(st));
      return 1;
    }
    if (readback.grid_payload != f.grid_payload || readback.tb_payload != f.tb_payload) {
      std::fprintf(stderr, "osg_gen self-check FAILED for %s: readback payload mismatch\n", path.c_str());
      return 1;
    }
  }

  std::fprintf(stderr, "osg_gen: OK — MCS %u, wrote %u files to %s (nslot 0..%u), self-check passed\n", mcs_index,
              oi_osg::kSlotsPerFrameMu1, out_dir.c_str(), oi_osg::kSlotsPerFrameMu1 - 1);
  return 0;
}
