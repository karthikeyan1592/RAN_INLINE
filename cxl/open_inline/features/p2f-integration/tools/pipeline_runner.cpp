// pipeline_runner.cpp — feeds one .pcap's worth of real O-RAN U-plane frames through the REAL
// oi_p2_host pipeline (setup/feed/launch_slot/drain, P2-R17's stable surface) exactly the way a
// production ingest_backend (p3/p6) would: parse each frame via the SAME shared
// oi_oran_preparse_frame() helper, place its bytes in the arena via oi_p2_write_arena (the
// 2026-07-23 additive API -- see oi_p2_host.h), then oi_p2_feed the resulting descriptor. Prints
// the resulting TB record as JSON on stdout for pipeline_test.py (Python) to compare against a
// pcap's ground-truth sidecar (class-b, oracle-packed) or to check structurally (class-a,
// P1-captured, no ground truth -- DEV-044). This binary does not know or care which pcap class it
// was given; that classification (and what to assert about the result) is pipeline_test.py's job,
// per P2-R15's own two-class gate design.
//
// This is also this project's P2-R17 proof in the form the LLD actually specifies: a caller
// (this binary) that only ever calls oi_p2_setup/feed/launch_slot/drain/tap/teardown -- the exact
// same sequence p3's live-tap ingest_backend will call once it exists -- switching this binary's
// pcap-file input for a live capture requires no change to any call it makes into oi_p2_host.h.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../p2a-scaffold/src/host/oi_frame_desc.h"
#include "../../p2a-scaffold/src/host/oi_oran_preparse.h"
#include "../../p2a-scaffold/src/host/oi_p2_host.h"

namespace {

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xF]);
  }
  return out;
}

// Reads a raw libpcap file (global header + per-packet records) written by oracle_tx_gen.cpp / any
// standard pcap producer. Returns one entry per packet's raw Ethernet-frame bytes, in file order.
bool read_pcap(const std::string& path, std::vector<std::vector<uint8_t>>* out_frames) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  uint8_t global_hdr[24];
  f.read((char*)global_hdr, 24);
  if (!f) return false;
  while (true) {
    uint8_t rec_hdr[16];
    f.read((char*)rec_hdr, 16);
    if (!f) break;  // EOF
    uint32_t incl_len;
    std::memcpy(&incl_len, rec_hdr + 8, 4);  // little-endian host assumed (matches writer)
    std::vector<uint8_t> frame(incl_len);
    f.read((char*)frame.data(), incl_len);
    if (!f) return false;
    out_frames->push_back(std::move(frame));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: pipeline_runner <config_yaml> <pcap_path> <mcs_index>\n");
    return 2;
  }
  std::string config_yaml = argv[1];
  std::string pcap_path = argv[2];
  uint32_t mcs_index = (uint32_t)std::atoi(argv[3]);

  std::vector<std::vector<uint8_t>> frames;
  if (!read_pcap(pcap_path, &frames)) {
    std::fprintf(stderr, "pipeline_runner: failed to read pcap '%s'\n", pcap_path.c_str());
    return 2;
  }

  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  if (nof_platforms == 0) {
    std::fprintf(stderr, "pipeline_runner: no OpenCL platform\n");
    return 2;
  }
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  oi_p2_pipeline* pipeline = nullptr;
  oi_p2_status st = oi_p2_setup(config_yaml.c_str(), ctx, queue, &pipeline);
  if (st != OI_P2_OK) {
    std::fprintf(stderr, "{\"error\": \"oi_p2_setup failed\", \"status\": %d}\n", (int)st);
    return 1;
  }

  // Same ingest_backend responsibility oi_oran_preparse.h documents: one state per fronthaul
  // stream (MVP: one stream), advanced across every frame in arrival (here: pcap file) order.
  oi_oran_preparse_state pstate{};
  uint64_t arena_cursor = 0;
  bool have_slot_id = false;
  uint32_t slot_id = 0;
  uint32_t nof_fed = 0, nof_dropped = 0;

  for (auto& frame : frames) {
    oi_frame_desc desc{};
    oi_preparse_status pst = oi_oran_preparse_frame(&pstate, frame.data(), (uint32_t)frame.size(), &desc);
    if (pst != OI_PREPARSE_OK) {
      // Malformed/truncated frame -> drop, not fatal (same tolerance K1/p3's ingest layer already
      // specify; class-a P1 pcaps may contain frames this MVP's fixed-shape parser rejects).
      nof_dropped++;
      continue;
    }
    desc.arena_offset = arena_cursor;
    desc.frame_len = (uint32_t)frame.size();

    st = oi_p2_write_arena(pipeline, arena_cursor, frame.data(), frame.size());
    if (st != OI_P2_OK) {
      std::fprintf(stderr, "{\"error\": \"oi_p2_write_arena failed\", \"status\": %d}\n", (int)st);
      oi_p2_teardown(pipeline);
      return 1;
    }
    arena_cursor += frame.size();

    st = oi_p2_feed(pipeline, &desc);
    if (st != OI_P2_OK) {
      std::fprintf(stderr, "{\"error\": \"oi_p2_feed failed\", \"status\": %d}\n", (int)st);
      oi_p2_teardown(pipeline);
      return 1;
    }
    if (!have_slot_id) {
      slot_id = desc.slot_id;
      have_slot_id = true;
    }
    nof_fed++;
  }

  if (!have_slot_id) {
    std::fprintf(stderr, "{\"error\": \"no valid frames parsed from pcap\", \"nof_dropped\": %u}\n", nof_dropped);
    oi_p2_teardown(pipeline);
    return 1;
  }

  st = oi_p2_launch_slot(pipeline, slot_id, mcs_index);
  if (st != OI_P2_OK) {
    std::fprintf(stderr, "{\"error\": \"oi_p2_launch_slot failed\", \"status\": %d}\n", (int)st);
    oi_p2_teardown(pipeline);
    return 1;
  }

  oi_p2_tb_record_c record{};
  st = oi_p2_drain(pipeline, slot_id, &record);
  if (st != OI_P2_OK) {
    std::fprintf(stderr, "{\"error\": \"oi_p2_drain failed\", \"status\": %d}\n", (int)st);
    oi_p2_teardown(pipeline);
    return 1;
  }

  // P2-R1 tap check: I2..I5 must be readable after a real run (same check host_api_test.cpp makes
  // per-slice; re-asserted here at the integration level over real oracle-packed data).
  bool taps_ok = true;
  for (int stage : {OI_P2_STAGE_I2_RE_GRID, OI_P2_STAGE_I3_CHAN_EST, OI_P2_STAGE_I4_EQ_OUT, OI_P2_STAGE_I5_LLR}) {
    std::vector<uint8_t> tap_buf(64, 0);
    if (oi_p2_tap(pipeline, slot_id, stage, tap_buf.data(), tap_buf.size()) != OI_P2_OK) {
      taps_ok = false;
    }
  }

  std::printf("{\n");
  std::printf("  \"schema\": \"oi-p2-runner-result/1\",\n");
  std::printf("  \"slot_id\": %u,\n", record.slot_id);
  std::printf("  \"mcs_index\": %u,\n", record.mcs_index);
  std::printf("  \"nof_cb\": %u,\n", record.nof_cb);
  std::printf("  \"base_graph\": %u,\n", record.base_graph);
  std::printf("  \"crc24a_ok\": %u,\n", record.crc24a_ok);
  std::printf("  \"tb_size_bytes\": %u,\n", record.tb_size_bytes);
  std::printf("  \"nof_frames_fed\": %u,\n", nof_fed);
  std::printf("  \"nof_frames_dropped\": %u,\n", nof_dropped);
  std::printf("  \"taps_ok\": %s,\n", taps_ok ? "true" : "false");
  std::printf("  \"tb_data_hex\": \"%s\"\n", to_hex(record.tb_data, record.tb_size_bytes).c_str());
  std::printf("}\n");

  oi_p2_teardown(pipeline);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);
  return 0;
}
