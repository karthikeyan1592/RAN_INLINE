// ldpc_decode_test.cpp — P2-R9: oi_p2_ldpc_decode_cb (this slice's hookup, including its internal
// 2*Z zero-padding bridge) vs the real linked OCUDU ldpc_encoder for ground truth, mirroring
// p0-rig-scaffold's own bit_diff_test.cpp methodology but exercising THIS module's public API
// (which takes K6's actual n_short*Z-sized output, not a pre-padded n_vn_full*Z buffer) rather
// than reconstructing the padded buffer inline.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../src/host/oi_p2_ldpc_decode.h"

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ocudu/ran/sch/ldpc_base_graph.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

static constexpr int8_t kLlrsAmpl = 10;  // matches bit_diff_test.cpp's noiseless-channel amplitude

struct Case {
  const char* label;
  int bg;
  unsigned ls;
  unsigned n_short;
  unsigned bg_k;  // n_vn_info
};

int main() {
  using namespace ocudu;
  using namespace ocudu::ldpc;

  auto enc_factory = create_ldpc_encoder_factory_sw("generic");
  check(enc_factory != nullptr, "real OCUDU ldpc_encoder_factory created");
  auto encoder = enc_factory->create();
  check(encoder != nullptr, "real OCUDU ldpc_encoder created");

  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available");
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  oi_p2_ldpc_decoder dec{};
  int rc = oi_p2_ldpc_decoder_init(&dec, ctx, queue, "../../p0-rig-scaffold/docker/gpu-phy/ldpc_suite/ldpc_decode.cl");
  check(rc == 0, "oi_p2_ldpc_decoder_init succeeds (builds p0's unmodified kernel)");

  // Matches this project's MVP MCS points' actual (base_graph, lifting_size) pairs (MCS4: BG1/224,
  // MCS13: BG1/352, MCS21: BG1/320 -- see STATUS.md / VERIFICATION.md), plus p0's own original
  // BG1/BG2 x LS=384/256 baseline for continuity with the existing gate.
  std::vector<Case> cases = {
      {"MCS4 (BG1/Zc=224)", 1, 224, 66, 22}, {"MCS13 (BG1/Zc=352)", 1, 352, 66, 22},
      {"MCS21 (BG1/Zc=320)", 1, 320, 66, 22}, {"BG1/Zc=384 (p0 baseline)", 1, 384, 66, 22},
      {"BG2/Zc=384 (p0 baseline)", 2, 384, 50, 10}, {"BG1/Zc=256 (p0 baseline)", 1, 256, 66, 22},
      {"BG2/Zc=256 (p0 baseline)", 2, 256, 50, 10},
  };

  std::mt19937 rgen(99);

  for (const auto& c : cases) {
    unsigned msg_bits = c.bg_k * c.ls;
    unsigned short_bits = c.n_short * c.ls;

    dynamic_bit_buffer msg_buf(msg_bits);
    for (unsigned i = 0; i < msg_bits; i++) {
      msg_buf.insert((uint8_t)(rgen() & 1), i, 1);
    }

    ldpc_encoder::configuration enc_cfg;
    enc_cfg.base_graph = (c.bg == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
    enc_cfg.lifting_size = static_cast<lifting_size_t>(c.ls);
    const ldpc_encoder_buffer& rm_buf = encoder->encode(msg_buf, enc_cfg);

    std::vector<uint8_t> codeword(short_bits, 0);
    rm_buf.write_codeblock(codeword, 0);

    // Build K6's-shape LLR input: n_short*Z bytes, noiseless channel (bit=0 -> +ampl, bit=1 -> -ampl).
    std::vector<int8_t> cb_llr(short_bits);
    for (unsigned j = 0; j < short_bits; j++) {
      cb_llr[j] = (codeword[j] == 0) ? kLlrsAmpl : (int8_t)-kLlrsAmpl;
    }

    unsigned out_bytes = (msg_bits + 7) / 8;
    std::vector<uint8_t> decoded(out_bytes, 0xAA);
    rc = oi_p2_ldpc_decode_cb(&dec, cb_llr.data(), (uint32_t)c.bg, c.ls, /*n_iter=*/6, decoded.data());
    char label[128];
    std::snprintf(label, sizeof(label), "%s: oi_p2_ldpc_decode_cb succeeds", c.label);
    check(rc == 0, label);

    // Compare decoded bits to the original message bits (MSB-first packing, matching msg_buf's own
    // convention -- bit_buffer::insert(bit, offset, 1) packs MSB-first, same as this project's
    // convention throughout).
    bool match = true;
    unsigned first_mismatch = 0;
    for (unsigned i = 0; i < msg_bits; i++) {
      uint8_t expected = msg_buf.extract<uint8_t>(i, 1);
      uint8_t got = (decoded[i / 8] >> (7 - (i % 8))) & 1u;
      if (expected != got) {
        match = false;
        first_mismatch = i;
        break;
      }
    }
    std::snprintf(label, sizeof(label), "%s: decoded bits bit-exact vs original message (%u bits)", c.label,
                 msg_bits);
    check(match, label);
    if (!match) {
      std::fprintf(stderr, "  first mismatch at bit %u\n", first_mismatch);
    }
  }

  oi_p2_ldpc_decoder_destroy(&dec);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "ldpc_decode_test: ALL PASS" : "ldpc_decode_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
