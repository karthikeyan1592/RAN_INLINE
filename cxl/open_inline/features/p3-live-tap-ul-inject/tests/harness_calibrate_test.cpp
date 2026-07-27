// harness_calibrate_test.cpp — unit tests for M5's slot_id->file_idx phase calibration (see
// oi_harness_calibrate.h for the real reconciliation finding this resolves). Fully local, no
// live rig needed: uses a synthetic "oracle set" of 20 distinct fake TB byte patterns and checks
// calibration recovers the correct phase offset for a range of (unknown-to-the-algorithm) true
// offsets, plus the failure path when no offset matches.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/host/oi_harness_calibrate.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

int main() {
  constexpr uint32_t N = 20;

  // Synthetic oracle set: file[i]'s "TB" is just the byte value i (distinct per file).
  std::vector<uint8_t> fake_oracle_tb(N);
  for (uint32_t i = 0; i < N; i++) fake_oracle_tb[i] = (uint8_t)i;

  // For each POSSIBLE true phase offset, simulate: host's first slot_id is some arbitrary value
  // (e.g. 137, mimicking "ingest joined mid-stream"), the wire's real slot-within-frame at that
  // moment is (137 + true_offset) mod 20, so the TB actually decoded is fake_oracle_tb[that index].
  for (uint32_t true_offset = 0; true_offset < N; true_offset++) {
    uint64_t first_slot_id = 137;
    uint32_t real_file_idx = oi_harness::file_idx_for(first_slot_id, true_offset, N);
    uint8_t decoded_tb_byte = fake_oracle_tb[real_file_idx];

    auto tb_matches_file = [&](uint32_t candidate_file_idx) {
      return fake_oracle_tb[candidate_file_idx] == decoded_tb_byte;
    };

    std::optional<uint32_t> recovered = oi_harness::calibrate_phase_offset(first_slot_id, N, tb_matches_file);
    check(recovered.has_value(), "calibration finds a matching offset for true_offset=" + std::to_string(true_offset));
    if (recovered.has_value()) {
      // Since fake_oracle_tb's bytes are all distinct (0..19), the match is unique -- recovered
      // offset must equal the true offset used to construct the scenario.
      check(*recovered == true_offset,
            "calibration recovers the EXACT true offset (not just any matching one), true_offset=" +
                std::to_string(true_offset));
    }
  }

  // Post-calibration: file_idx_for with the locked offset must reproduce the same mapping for
  // later slot_ids too (spanning multiple wraps past N).
  {
    uint32_t locked_offset = 5;
    bool consistent = true;
    for (uint64_t slot_id = 137; slot_id < 137 + 5 * N; slot_id++) {
      uint32_t fi = oi_harness::file_idx_for(slot_id, locked_offset, N);
      if (fi >= N) consistent = false;
    }
    check(consistent, "file_idx_for stays in [0,N) across many slot_ids with a locked offset, spanning multiple wraps");
  }

  // Failure path: no candidate offset matches (simulates a genuine first-slot decode bug) ->
  // calibrate_phase_offset must return nullopt, not silently default to offset 0.
  {
    auto never_matches = [](uint32_t) { return false; };
    std::optional<uint32_t> result = oi_harness::calibrate_phase_offset(137, N, never_matches);
    check(!result.has_value(), "calibration correctly reports failure (nullopt) when no offset matches, not a silent default");
  }

  if (g_fail == 0) {
    std::printf("\nharness_calibrate_test: ALL PASS\n");
  } else {
    std::fprintf(stderr, "\nharness_calibrate_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
