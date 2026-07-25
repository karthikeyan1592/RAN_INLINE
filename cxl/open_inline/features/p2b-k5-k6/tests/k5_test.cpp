// k5_test.cpp — P2-R7 dual validation: (1) my host-side gold-init reference vs the REAL linked
// OCUDU library (strongest possible oracle — the actual reference implementation, not just a
// vector file); (2) the actual OpenCL kernel (run via PoCL) vs the same real library. Both must
// be bit-exact, all three MCS's Qm-worth of LLR data, plus the LLR_INFTY boundary case (P2-R7:
// "input LLR = +-LLR_INFTY passes through sign-flipped, never reinterpreted").
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../src/host/oi_p2_gold_init.h"

// Real OCUDU library (BSD-3, release_26_04) — the oracle.
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

// Host-side reference stepping loop (mirrors k5_descrambler.cl exactly) using the pre-warmed
// state from oi_gold_init(). Used to sanity-check the warm-up math independent of any GPU/PoCL
// involvement, before also checking the real kernel.
static void host_reference_descramble(uint32_t c_init, ocudu::span<int8_t> llr) {
  oi_gold_state st = oi_gold_init(c_init);
  uint32_t x1 = st.x1, x2 = st.x2;
  for (size_t n = 0; n < llr.size(); n++) {
    uint32_t c = x1 ^ x2;
    uint32_t f1 = x1 ^ (x1 << 3U);
    uint32_t f2 = x2 ^ (x2 << 1U) ^ (x2 << 2U) ^ (x2 << 3U);
    uint32_t mask = 0x80000000u;
    f1 = (f1 & mask) >> 30U;
    f2 = (f2 & mask) >> 30U;
    x1 = (x1 << 1U) ^ f1;
    x2 = (x2 << 1U) ^ f2;
    if (c & 0x80000000u) {
      llr[n] = static_cast<int8_t>(-static_cast<int>(llr[n]));
    }
  }
}

int main() {
  using ocudu::log_likelihood_ratio;

  // ---- Test 1: host reference vs the REAL linked OCUDU library ----
  auto factory = ocudu::create_pseudo_random_generator_sw_factory();
  check(factory != nullptr, "OCUDU pseudo_random_generator_sw_factory created");

  std::mt19937 rgen(1234);
  const std::vector<uint32_t> test_c_inits = {
      0x0004601u * 32768u + 1u,  // RNTI=0x4601, n_ID=1 (MVP config, SPEC scrambling table)
      1u, 0xFFFFFFFEu, 12345u,
  };
  const std::vector<size_t> test_lengths = {8448, 3840, 100, 1};  // MVP TB-sized + tiny edge case

  for (uint32_t c_init : test_c_inits) {
    for (size_t len : test_lengths) {
      // Real OCUDU: generate random LLRs, apply_xor via the actual library.
      std::vector<int8_t> real_llr(len);
      for (auto& v : real_llr) v = static_cast<int8_t>((rgen() % 241) - 120);  // [-120,120]
      std::vector<log_likelihood_ratio> ocudu_in(real_llr.begin(), real_llr.end());
      std::vector<log_likelihood_ratio> ocudu_out(len);

      auto gen = factory->create();
      gen->init(c_init);
      gen->apply_xor(ocudu::span<log_likelihood_ratio>(ocudu_out),
                     ocudu::span<const log_likelihood_ratio>(ocudu_in));

      // My host reference: same input, same c_init.
      std::vector<int8_t> my_llr = real_llr;
      host_reference_descramble(c_init, ocudu::span<int8_t>(my_llr));

      bool match = true;
      for (size_t i = 0; i < len; i++) {
        if (static_cast<int8_t>(ocudu_out[i].to_value_type()) != my_llr[i]) {
          match = false;
          break;
        }
      }
      char label[256];
      std::snprintf(label, sizeof(label),
                     "host reference bit-exact vs real OCUDU (c_init=%u, len=%zu)", c_init, len);
      check(match, label);
    }
  }

  // LLR_INFTY boundary case: +-127 must sign-flip cleanly, never reinterpreted.
  {
    uint32_t c_init = 12345;
    std::vector<int8_t> real_llr = {127, -127, 127, -127, 0, 120, -120};
    std::vector<log_likelihood_ratio> ocudu_in(real_llr.begin(), real_llr.end());
    std::vector<log_likelihood_ratio> ocudu_out(real_llr.size());
    auto gen = factory->create();
    gen->init(c_init);
    gen->apply_xor(ocudu::span<log_likelihood_ratio>(ocudu_out),
                   ocudu::span<const log_likelihood_ratio>(ocudu_in));

    std::vector<int8_t> my_llr = real_llr;
    host_reference_descramble(c_init, ocudu::span<int8_t>(my_llr));

    bool match = true;
    for (size_t i = 0; i < real_llr.size(); i++) {
      if (static_cast<int8_t>(ocudu_out[i].to_value_type()) != my_llr[i]) match = false;
    }
    check(match, "LLR_INFTY (+-127) boundary values sign-flip bit-exact vs real OCUDU");
  }

  // ---- Test 2: the ACTUAL OpenCL kernel (k5_descramble, via PoCL) vs the real library ----
  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");

  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  FILE* kf = fopen("../src/kernels/k5_descrambler.cl", "r");
  check(kf != nullptr, "k5_descrambler.cl opened");
  std::vector<char> src_buf;
  if (kf) {
    fseek(kf, 0, SEEK_END);
    long sz = ftell(kf);
    fseek(kf, 0, SEEK_SET);
    src_buf.resize(sz + 1, 0);
    size_t nread = fread(src_buf.data(), 1, sz, kf);
    (void)nread;
    fclose(kf);
  }
  const char* src_ptr = src_buf.data();
  cl_program prog = clCreateProgramWithSource(ctx, 1, &src_ptr, nullptr, &err);
  // include path so k5's #include "oi_kernel_compat.h" resolves (p2a-scaffold owns it).
  err = clBuildProgram(prog, 1, &device,
                        "-cl-std=CL1.2 -I../../p2a-scaffold/src/kernels -I../src/kernels", nullptr,
                        nullptr);
  if (err != CL_SUCCESS) {
    size_t log_len = 0;
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
    std::vector<char> log(log_len + 1, 0);
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
    std::fprintf(stderr, "kernel build failed:\n%s\n", log.data());
  }
  check(err == CL_SUCCESS, "k5_descramble kernel builds on PoCL");

  cl_kernel kernel = clCreateKernel(prog, "k5_descramble", &err);
  check(err == CL_SUCCESS, "k5_descramble kernel created");

  for (uint32_t c_init : {test_c_inits[0], test_c_inits[2]}) {
    for (size_t len : {size_t(8448), size_t(100)}) {
      std::vector<int8_t> real_llr(len);
      for (auto& v : real_llr) v = static_cast<int8_t>((rgen() % 241) - 120);
      std::vector<log_likelihood_ratio> ocudu_in(real_llr.begin(), real_llr.end());
      std::vector<log_likelihood_ratio> ocudu_out(len);
      auto gen = factory->create();
      gen->init(c_init);
      gen->apply_xor(ocudu::span<log_likelihood_ratio>(ocudu_out),
                     ocudu::span<const log_likelihood_ratio>(ocudu_in));

      cl_mem llr_buf =
          clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, len, real_llr.data(), &err);
      cl_uint nof_llrs_arg = static_cast<cl_uint>(len);
      clSetKernelArg(kernel, 0, sizeof(cl_mem), &llr_buf);
      clSetKernelArg(kernel, 1, sizeof(cl_uint), &c_init);
      clSetKernelArg(kernel, 2, sizeof(cl_uint), &nof_llrs_arg);
      size_t global_size = 1;  // one work-item = one block (this test: one block)
      err = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr,
                                    nullptr);
      std::vector<int8_t> kernel_out(len);
      clEnqueueReadBuffer(queue, llr_buf, CL_TRUE, 0, len, kernel_out.data(), 0, nullptr, nullptr);
      clReleaseMemObject(llr_buf);

      bool match = true;
      for (size_t i = 0; i < len; i++) {
        if (static_cast<int8_t>(ocudu_out[i].to_value_type()) != kernel_out[i]) {
          match = false;
          break;
        }
      }
      char label[256];
      std::snprintf(label, sizeof(label),
                     "REAL KERNEL bit-exact vs real OCUDU (c_init=%u, len=%zu)", c_init, len);
      check(match, label);
    }
  }

  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k5_test: ALL PASS" : "k5_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
