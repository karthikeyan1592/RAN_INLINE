// k6_test.cpp — P2-R7/R8 validation: the ACTUAL k6_rate_dematch OpenCL kernel (run via PoCL)
// vs the REAL linked OCUDU library's ldpc_rate_dematcher_impl (generic variant, forced via the
// "generic" factory string per D3 — never avx2/avx512/neon). Covers both base graphs, all three
// MVP MCS's Qm values (2, 4, 6), and rm_length both within the systematic region only and
// spanning into parity.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// Real OCUDU library (BSD-3, release_26_04) — the oracle.
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

struct TestCase {
  const char* label;
  ocudu::ldpc_base_graph_type base_graph;
  ocudu::ldpc::lifting_size_t lifting_size;
  unsigned nof_filler_bits;
  unsigned modulation_order;  // Qm: 2 (QPSK), 4 (16QAM), 6 (64QAM) — MVP's MCS set {4,13,21}
  unsigned rm_length;
};

int main() {
  using ocudu::log_likelihood_ratio;
  using namespace ocudu::ldpc;

  auto dematcher_factory = ocudu::create_ldpc_rate_dematcher_factory_sw("generic");
  check(dematcher_factory != nullptr, "OCUDU ldpc_rate_dematcher_factory_sw(generic) created");

  // ---- Set up PoCL/OpenCL context and build the actual kernel once ----
  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");

  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  FILE* kf = fopen("../src/kernels/k6_rate_dematcher.cl", "r");
  check(kf != nullptr, "k6_rate_dematcher.cl opened");
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
  check(err == CL_SUCCESS, "k6_rate_dematch kernel builds on PoCL");

  cl_kernel kernel = clCreateKernel(prog, "k6_rate_dematch", &err);
  check(err == CL_SUCCESS, "k6_rate_dematch kernel created");

  std::mt19937 rgen(4242);

  const std::vector<TestCase> cases = {
      // BG1, LS8 -> full_length = 66*8 = 528, nof_systematic = (22-2)*8 = 160.
      {"BG1/LS8/QPSK/systematic-only", ocudu::ldpc_base_graph_type::BG1, LS8, 8, 2, 96},
      {"BG1/LS8/QAM16/into-parity", ocudu::ldpc_base_graph_type::BG1, LS8, 8, 4, 300},
      // 516 = largest multiple of Qm=6 that stays within the single-pass capacity boundary
      // (full_length(528) - nof_filler_bits(8) = 520; see k6_rate_dematcher.cl's file header on
      // the wraparound/repetition limitation). rm_length must be a multiple of Qm (real library
      // precondition: E bits map Qm-per-modulated-symbol).
      {"BG1/LS8/QAM64/full-span", ocudu::ldpc_base_graph_type::BG1, LS8, 8, 6, 516},
      // BG2, LS8 -> full_length = 50*8 = 400, nof_systematic = (10-2)*8 = 64.
      {"BG2/LS8/QPSK/systematic-only", ocudu::ldpc_base_graph_type::BG2, LS8, 4, 2, 40},
      {"BG2/LS8/QAM16/into-parity", ocudu::ldpc_base_graph_type::BG2, LS8, 4, 4, 200},
      {"BG2/LS8/QAM64/full-span", ocudu::ldpc_base_graph_type::BG2, LS8, 4, 6, 396},
  };

  for (const auto& tc : cases) {
    unsigned BG_N_SHORT = (tc.base_graph == ocudu::ldpc_base_graph_type::BG1) ? 66 : 50;
    unsigned full_length = BG_N_SHORT * static_cast<unsigned>(tc.lifting_size);

    // Real OCUDU: build rate-matched input LLRs and run the real generic dematcher.
    std::vector<int8_t> real_in(tc.rm_length);
    for (auto& v : real_in) v = static_cast<int8_t>((rgen() % 241) - 120);  // [-120,120]
    std::vector<log_likelihood_ratio> ocudu_in(real_in.begin(), real_in.end());
    std::vector<log_likelihood_ratio> ocudu_out(full_length);

    ocudu::codeblock_metadata cfg{};
    cfg.tb_common.base_graph = tc.base_graph;
    cfg.tb_common.lifting_size = tc.lifting_size;
    cfg.tb_common.rv = 0;  // P2-R8: rv=0 always
    cfg.tb_common.mod = (tc.modulation_order == 2)   ? ocudu::modulation_scheme::QPSK
                        : (tc.modulation_order == 4) ? ocudu::modulation_scheme::QAM16
                                                      : ocudu::modulation_scheme::QAM64;
    cfg.tb_common.Nref = 0;  // unlimited buffer (Nref/HARQ out of scope)
    cfg.tb_common.cw_length = full_length;
    cfg.cb_specific.full_length = full_length;
    cfg.cb_specific.rm_length = tc.rm_length;
    cfg.cb_specific.nof_filler_bits = tc.nof_filler_bits;
    cfg.cb_specific.cw_offset = 0;

    auto dematcher = dematcher_factory->create();
    dematcher->rate_dematch(ocudu::span<log_likelihood_ratio>(ocudu_out),
                            ocudu::span<const log_likelihood_ratio>(ocudu_in),
                            /*new_data=*/true, cfg);

    // The actual kernel, via PoCL.
    cl_mem in_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, tc.rm_length,
                                    real_in.data(), &err);
    cl_mem out_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, full_length, nullptr, &err);
    cl_uint rm_length_arg = tc.rm_length;
    cl_uint full_length_arg = full_length;
    cl_uint nof_filler_arg = tc.nof_filler_bits;
    cl_uint shift_k0_arg = 0;  // rv=0 always -> shift_k0=0 always (P2-R8)
    cl_uint mod_order_arg = tc.modulation_order;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &in_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_uint), &rm_length_arg);
    clSetKernelArg(kernel, 2, sizeof(cl_uint), &full_length_arg);
    clSetKernelArg(kernel, 3, sizeof(cl_uint), &nof_filler_arg);
    clSetKernelArg(kernel, 4, sizeof(cl_uint), &shift_k0_arg);
    clSetKernelArg(kernel, 5, sizeof(cl_uint), &mod_order_arg);
    clSetKernelArg(kernel, 6, sizeof(cl_mem), &out_buf);
    size_t global_size = 1;  // one work-item = one CB (this test: one CB)
    err = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr,
                                  nullptr);
    std::vector<int8_t> kernel_out(full_length);
    clEnqueueReadBuffer(queue, out_buf, CL_TRUE, 0, full_length, kernel_out.data(), 0, nullptr,
                        nullptr);
    clReleaseMemObject(in_buf);
    clReleaseMemObject(out_buf);

    bool match = true;
    size_t first_mismatch = 0;
    for (size_t i = 0; i < full_length; i++) {
      if (static_cast<int8_t>(ocudu_out[i].to_value_type()) != kernel_out[i]) {
        match = false;
        first_mismatch = i;
        break;
      }
    }
    char label[256];
    std::snprintf(label, sizeof(label), "REAL KERNEL bit-exact vs real OCUDU (%s)", tc.label);
    check(match, label);
    if (!match) {
      std::fprintf(stderr, "  first mismatch at %zu: ocudu=%d kernel=%d\n", first_mismatch,
                   static_cast<int8_t>(ocudu_out[first_mismatch].to_value_type()),
                   kernel_out[first_mismatch]);
    }
  }

  // Filler bits check (P2-R8): must always be exactly LLR_INFTY (+127), never anything else.
  {
    const TestCase& tc = cases[1];  // BG1/QAM16/into-parity
    unsigned full_length = 66 * static_cast<unsigned>(tc.lifting_size);
    unsigned nof_systematic_bits = (22 - 2) * static_cast<unsigned>(tc.lifting_size);
    unsigned nof_info_bits = nof_systematic_bits - tc.nof_filler_bits;

    std::vector<int8_t> real_in(tc.rm_length);
    for (auto& v : real_in) v = static_cast<int8_t>((rgen() % 241) - 120);

    cl_mem in_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, tc.rm_length,
                                    real_in.data(), &err);
    cl_mem out_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, full_length, nullptr, &err);
    cl_uint rm_length_arg = tc.rm_length;
    cl_uint full_length_arg = full_length;
    cl_uint nof_filler_arg = tc.nof_filler_bits;
    cl_uint shift_k0_arg = 0;
    cl_uint mod_order_arg = tc.modulation_order;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &in_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_uint), &rm_length_arg);
    clSetKernelArg(kernel, 2, sizeof(cl_uint), &full_length_arg);
    clSetKernelArg(kernel, 3, sizeof(cl_uint), &nof_filler_arg);
    clSetKernelArg(kernel, 4, sizeof(cl_uint), &shift_k0_arg);
    clSetKernelArg(kernel, 5, sizeof(cl_uint), &mod_order_arg);
    clSetKernelArg(kernel, 6, sizeof(cl_mem), &out_buf);
    size_t global_size = 1;
    clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    std::vector<int8_t> kernel_out(full_length);
    clEnqueueReadBuffer(queue, out_buf, CL_TRUE, 0, full_length, kernel_out.data(), 0, nullptr,
                        nullptr);
    clReleaseMemObject(in_buf);
    clReleaseMemObject(out_buf);

    bool all_infty = true;
    for (unsigned k = 0; k < tc.nof_filler_bits; k++) {
      if (kernel_out[nof_info_bits + k] != 127) all_infty = false;
    }
    check(all_infty, "filler-bit region is exactly +LLR_INFTY (127) in kernel output");
  }

  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k6_test: ALL PASS" : "k6_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
