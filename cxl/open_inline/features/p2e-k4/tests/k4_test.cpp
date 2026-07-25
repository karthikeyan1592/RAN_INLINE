// k4_test.cpp — P2-R6: the ACTUAL k4_demap OpenCL kernel (via PoCL) vs the REAL linked OCUDU
// demodulation_mapper (scalar paths). K4's gate is BIT-EXACT int8 output, not a tolerance (parent
// LLD §7) -- all three MVP MCS's Qm values (2/4/6), plus explicit near-zero and saturation-
// boundary vectors.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <random>
#include <vector>

#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"

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
  using namespace ocudu;

  auto factory = create_demodulation_mapper_factory();
  check(factory != nullptr, "real OCUDU demodulation_mapper_factory created");
  auto mapper = factory->create();
  check(mapper != nullptr, "real OCUDU demodulation_mapper created");

  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  FILE* kf = fopen("../src/kernels/k4_demapper.cl", "r");
  check(kf != nullptr, "k4_demapper.cl opened");
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
  err = clBuildProgram(prog, 1, &device, "-cl-std=CL1.2 -I../../p2a-scaffold/src/kernels", nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t log_len = 0;
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
    std::vector<char> log(log_len + 1, 0);
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
    std::fprintf(stderr, "kernel build failed:\n%s\n", log.data());
  }
  check(err == CL_SUCCESS, "k4_demap kernel builds on PoCL");
  cl_kernel kernel = clCreateKernel(prog, "k4_demap", &err);
  check(err == CL_SUCCESS, "k4_demap kernel created");

  auto run_case = [&](const std::string& label, modulation_scheme mod, unsigned qm,
                      const std::vector<cf_t>& symbols, const std::vector<float>& noise_vars) {
    unsigned n = symbols.size();

    // Real OCUDU demapper.
    std::vector<log_likelihood_ratio> real_llrs(n * qm);
    mapper->demodulate_soft(span<log_likelihood_ratio>(real_llrs), span<const cf_t>(symbols),
                            span<const float>(noise_vars), mod);

    // The actual kernel via PoCL.
    std::vector<cl_float2> symbols_cl(n);
    for (unsigned i = 0; i < n; i++) symbols_cl[i] = {{symbols[i].real(), symbols[i].imag()}};
    cl_mem symbols_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(cl_float2), symbols_cl.data(), &err);
    cl_mem noise_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(float),
                                      (void*)noise_vars.data(), &err);
    cl_mem llr_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n * qm * sizeof(cl_char), nullptr, &err);
    cl_uint qm_arg = qm;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &symbols_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &noise_buf);
    clSetKernelArg(kernel, 2, sizeof(cl_uint), &qm_arg);
    clSetKernelArg(kernel, 3, sizeof(cl_mem), &llr_buf);
    size_t global_size = n;
    clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    std::vector<int8_t> kernel_llrs(n * qm);
    clEnqueueReadBuffer(queue, llr_buf, CL_TRUE, 0, n * qm, kernel_llrs.data(), 0, nullptr, nullptr);
    clReleaseMemObject(symbols_buf);
    clReleaseMemObject(noise_buf);
    clReleaseMemObject(llr_buf);

    bool match = true;
    size_t first_mismatch = 0;
    for (unsigned i = 0; i < n * qm; i++) {
      if (static_cast<int8_t>(real_llrs[i].to_value_type()) != kernel_llrs[i]) {
        match = false;
        first_mismatch = i;
        break;
      }
    }
    check(match, "REAL KERNEL bit-exact vs real OCUDU demapper (" + label + ")");
    if (!match) {
      std::fprintf(stderr, "  first mismatch at %zu: real=%d kernel=%d\n", first_mismatch,
                   static_cast<int8_t>(real_llrs[first_mismatch].to_value_type()), kernel_llrs[first_mismatch]);
    }
  };

  std::mt19937 rgen(2024);
  std::uniform_real_distribution<float> dist(-1.5f, 1.5f);
  std::uniform_real_distribution<float> nvar_dist(0.01f, 2.0f);

  // Normal random symbols, each MVP Qm.
  for (auto [mod, qm] : {std::pair{modulation_scheme::QPSK, 2u}, {modulation_scheme::QAM16, 4u},
                        {modulation_scheme::QAM64, 6u}}) {
    std::vector<cf_t> symbols(128);
    std::vector<float> noise_vars(128);
    for (auto& s : symbols) s = cf_t(dist(rgen), dist(rgen));
    for (auto& n : noise_vars) n = nvar_dist(rgen);
    char label[64];
    std::snprintf(label, sizeof(label), "random, qm=%u", qm);
    run_case(label, mod, qm, symbols, noise_vars);
  }

  // Saturation-boundary vectors: values right at and beyond +-range_limit (24 for QPSK, 20 for
  // 16/64QAM, applied to the RAW symbol amplitude before the modulation-specific scaling -- picking
  // symbol magnitudes large enough that l_value clearly exceeds range_limit regardless of scaling).
  for (auto [mod, qm] : {std::pair{modulation_scheme::QPSK, 2u}, {modulation_scheme::QAM16, 4u},
                        {modulation_scheme::QAM64, 6u}}) {
    std::vector<cf_t> symbols = {cf_t(50.0f, 50.0f), cf_t(-50.0f, -50.0f), cf_t(0.001f, 0.001f),
                                 cf_t(50.0f, -50.0f)};
    std::vector<float> noise_vars = {0.01f, 0.01f, 0.01f, 0.01f};
    char label[64];
    std::snprintf(label, sizeof(label), "saturation boundary, qm=%u", qm);
    run_case(label, mod, qm, symbols, noise_vars);
  }

  // Near-zero symbol (16QAM/64QAM's is_near_zero guard) -- QPSK has no such guard, skip it there.
  for (auto [mod, qm] : {std::pair{modulation_scheme::QAM16, 4u}, {modulation_scheme::QAM64, 6u}}) {
    std::vector<cf_t> symbols = {cf_t(0.0f, 0.0f), cf_t(1e-6f, 1e-6f)};
    std::vector<float> noise_vars = {0.5f, 0.5f};
    char label[64];
    std::snprintf(label, sizeof(label), "near-zero symbol, qm=%u", qm);
    run_case(label, mod, qm, symbols, noise_vars);
  }

  // Degenerate noise_var (<=0 or NaN) -> LLR must be exactly 0.
  for (auto [mod, qm] : {std::pair{modulation_scheme::QPSK, 2u}, {modulation_scheme::QAM16, 4u},
                        {modulation_scheme::QAM64, 6u}}) {
    std::vector<cf_t> symbols = {cf_t(0.5f, 0.5f), cf_t(0.5f, 0.5f)};
    std::vector<float> noise_vars = {0.0f, -1.0f};
    char label[64];
    std::snprintf(label, sizeof(label), "degenerate noise_var, qm=%u", qm);
    run_case(label, mod, qm, symbols, noise_vars);
  }

  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k4_test: ALL PASS" : "k4_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
