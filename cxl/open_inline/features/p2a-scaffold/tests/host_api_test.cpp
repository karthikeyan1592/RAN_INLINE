// host_api_test.cpp — proves the oi_p2_host orchestration (setup/feed/launch_slot/drain/tap/
// teardown) runs the REAL 8-stage kernel chain (K1, K2a x3, K2b, K3, K4, K5, K6+LDPC tail)
// end-to-end without crashing, for arbitrary (including empty/malformed) input -- i.e. mechanical
// orchestration correctness (right buffer shapes, chain completes, taps readable), not full PHY
// correctness. Content correctness against a REAL, wire-valid, fully-encoded transmission is
// pipeline_test.py's job (P2-R1/R15), which can assert an actual CRC pass; this test's descriptors
// are deliberately not a physically valid encoded frame, so a CRC pass here isn't expected.
#include "../src/host/oi_p2_host.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

int main() {
  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "at least one OpenCL platform found");
  if (nof_platforms == 0) return 1;

  cl_device_id device = nullptr;
  cl_int err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  check(err == CL_SUCCESS, "device obtained from platform");

  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  check(err == CL_SUCCESS, "context created");

  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);
  check(err == CL_SUCCESS, "in-order command queue created");

  oi_p2_pipeline* pipeline = nullptr;
  oi_p2_status st = oi_p2_setup("fixtures/mvp_config.yaml", ctx, queue, &pipeline);
  check(st == OI_P2_OK, "oi_p2_setup succeeds with valid MVP config (builds all 8 real kernels)");
  check(pipeline != nullptr, "oi_p2_setup produces a non-null pipeline");

  // Negative: setup with a rejected config must not leak a pipeline handle.
  {
    oi_p2_pipeline* bad = nullptr;
    oi_p2_status bad_st = oi_p2_setup("fixtures/nonexistent.yaml", ctx, queue, &bad);
    check(bad_st != OI_P2_OK, "oi_p2_setup rejects a missing config file");
    check(bad == nullptr, "rejected setup leaves pipeline handle null");
  }

  const uint32_t slot_id = 42;

  // Feed a handful of already-parsed, but NOT physically-valid-frame, descriptors -- exercising
  // real full-encoding correctness is pipeline_test.py's job; this test only needs the chain to
  // run to completion without crashing on whatever K1 makes of these (per K1's own error-table
  // tolerance, malformed/incomplete frames are dropped, not fatal).
  for (size_t i = 0; i < 5; i++) {
    oi_frame_desc desc{};
    desc.arena_offset = i * 1024;
    desc.frame_len = 1024;
    desc.slot_id = slot_id;
    desc.symbol_id = static_cast<uint8_t>(i);
    desc.section_id = 0;
    desc.filter_index = 0;
    desc.flags = 0;
    desc.start_prb = 0;
    desc.nof_prbs = 51;
    st = oi_p2_feed(pipeline, &desc);
    check(st == OI_P2_OK, "oi_p2_feed accepts a fully-populated descriptor");
  }

  st = oi_p2_launch_slot(pipeline, slot_id, /*mcs_index=*/4);
  check(st == OI_P2_OK, "oi_p2_launch_slot enqueues the real 8-stage chain (MCS 4, C=1)");

  // Rejecting an mcs_index outside the configured mcs_set is itself part of the orchestration
  // contract (2026-07-23 addition -- see oi_p2_host.h's mcs_index doc comment).
  {
    oi_p2_status bad_mcs_st = oi_p2_launch_slot(pipeline, /*slot_id=*/43, /*mcs_index=*/9);
    check(bad_mcs_st == OI_P2_ERR_CONFIG_REJECTED, "oi_p2_launch_slot rejects an mcs_index outside {4,13,21}");
  }

  oi_p2_tb_record_c record{};
  st = oi_p2_drain(pipeline, slot_id, &record);
  check(st == OI_P2_OK, "oi_p2_drain succeeds for a launched slot (runs the real K6+LDPC+CRC tail)");
  check(record.schema == 0x00020001, "TB record schema is oi-p2-tb/1");
  check(record.slot_id == slot_id, "TB record slot_id matches");
  check(record.mcs_index == 4, "TB record mcs_index matches the value passed to launch_slot");
  check(record.nof_cb == 1, "TB record nof_cb == 1 for MCS 4 (C=1, confirmed via real OCUDU segmenter)");
  check(record.base_graph == 1, "TB record base_graph == 1 for MCS 4 (confirmed via real OCUDU base-graph selection)");
  // crc24a_ok is NOT asserted here: these descriptors don't decode from a real encoded
  // transmission, so a CRC pass isn't expected -- proving a REAL CRC pass end-to-end is
  // pipeline_test.py's job, exercised there against a real, fully-encoded oracle-packed frame.

  // Draining a slot that was never launched must fail distinctly.
  {
    oi_p2_tb_record_c bad_record{};
    oi_p2_status bad_st = oi_p2_drain(pipeline, /*slot_id=*/999, &bad_record);
    check(bad_st == OI_P2_ERR_SLOT_INCOMPLETE, "drain on never-launched slot -> SLOT_INCOMPLETE");
  }

  // Tap I2/I3/I4/I5 for this slot -- proves the real chain actually wrote each buffer (readback
  // succeeds), not that specific PHY values are correct (that's pipeline_test.py's job).
  for (int stage : {OI_P2_STAGE_I2_RE_GRID, OI_P2_STAGE_I3_CHAN_EST, OI_P2_STAGE_I4_EQ_OUT, OI_P2_STAGE_I5_LLR}) {
    std::vector<uint8_t> tap_buf(64, 0xAA);
    st = oi_p2_tap(pipeline, slot_id, stage, tap_buf.data(), tap_buf.size());
    char label[64];
    std::snprintf(label, sizeof(label), "oi_p2_tap reads back stage %d without error", stage);
    check(st == OI_P2_OK, label);
  }

  oi_p2_teardown(pipeline);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "host_api_test: ALL PASS" : "host_api_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
