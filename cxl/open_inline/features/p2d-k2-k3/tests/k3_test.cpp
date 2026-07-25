// k3_test.cpp — P2-R5: the ACTUAL k3_equalize OpenCL kernel (via PoCL) vs the REAL linked OCUDU
// channel_equalizer_generic_impl (MMSE type, which for 1 Tx layer literally IS the ZF formula —
// confirmed by reading the source, see k3_equalizer.cl's header). Covers normal REs across a
// range of tx_scaling/noise_var values, plus the degenerate cases the real equalizer's own doc
// comment calls out: |h|==0 and noise_var<=0/inf (both must zero the output + set noise to +inf).
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "ocudu/adt/bf16.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/dynamic_ch_est_list.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

// Tolerance set to accommodate a real, understood precision difference (not a bug): OCUDU's
// equalize_zf_1xn.h uses an approximate SIMD reciprocal instruction (ocudu_simd_f_rcp, ~12-bit
// hardware approximation) on its vectorized fast path, while this kernel does exact division —
// confirmed by reading equalize_zf_1xn.h lines ~87-103 after seeing ~1.5e-4 relative mismatches
// that didn't fit a bf16-precision or formula explanation. The kernel is arguably MORE precise
// here, not less; P2-R2 already bans porting SIMD-width-specific tricks into kernels, so matching
// OCUDU's approximation exactly would be both against the rules and a step backward.
static bool close_enough(float a, float b, float tol = 2e-3f) {
  if (std::isinf(a) && std::isinf(b)) return true;
  return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b));
}

int main() {
  using namespace ocudu;

  auto factory = create_channel_equalizer_generic_factory(channel_equalizer_algorithm_type::mmse);
  check(factory != nullptr, "real OCUDU channel_equalizer_generic_factory(mmse) created");
  auto equalizer = factory->create();
  check(equalizer != nullptr, "real OCUDU equalizer created");

  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  FILE* kf = fopen("../src/kernels/k3_equalizer.cl", "r");
  check(kf != nullptr, "k3_equalizer.cl opened");
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
  check(err == CL_SUCCESS, "k3_equalize kernel builds on PoCL");
  cl_kernel kernel = clCreateKernel(prog, "k3_equalize", &err);
  check(err == CL_SUCCESS, "k3_equalize kernel created");

  double sq_err_sum_sym = 0.0, sq_ref_sum_sym = 0.0;
  double max_nvar_rel_err = 0.0;

  auto run_case = [&](const std::string& label, const std::vector<cf_t>& y_in, const std::vector<cf_t>& h_in,
                      float noise_var, float tx_scaling, bool accumulate_nrmse = true) {
    unsigned n = y_in.size();

    // ch_symbols/ch_estimates are stored as cbf16_t (bf16-rounded, 7 mantissa bits) -- both the
    // real equalizer and the kernel must see the SAME already-rounded values for an exact
    // comparison to be meaningful (same lesson as K1's RE-grid precision fix).
    std::vector<cf_t> y(n), h(n);
    for (unsigned i = 0; i < n; i++) {
      y[i] = to_cf(cbf16_t(y_in[i]));
      h[i] = to_cf(cbf16_t(h_in[i]));
    }

    // Real OCUDU equalizer.
    dynamic_re_buffer<cbf16_t> ch_symbols(1, n);
    dynamic_ch_est_list ch_estimates(n, 1, 1);
    for (unsigned i = 0; i < n; i++) {
      ch_symbols.get_slice(0)[i] = cbf16_t(y[i]);
      ch_estimates.get_channel(0, 0)[i] = cbf16_t(h[i]);
    }
    std::vector<cf_t> real_eq_symbols(n);
    std::vector<float> real_eq_noise_var(n);
    std::vector<float> noise_var_estimates = {noise_var};
    equalizer->equalize(span<cf_t>(real_eq_symbols), span<float>(real_eq_noise_var), ch_symbols, ch_estimates,
                        span<const float>(noise_var_estimates), tx_scaling);

    // The actual kernel via PoCL.
    std::vector<cl_float2> re_grid_in(n), ch_est_in(n);
    for (unsigned i = 0; i < n; i++) {
      re_grid_in[i] = {{y[i].real(), y[i].imag()}};
      ch_est_in[i] = {{h[i].real(), h[i].imag()}};
    }
    cl_mem re_grid_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(cl_float2), re_grid_in.data(), &err);
    cl_mem ch_est_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(cl_float2), ch_est_in.data(), &err);
    cl_mem noise_var_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float), &noise_var, &err);
    cl_mem eq_symbols_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n * sizeof(cl_float2), nullptr, &err);
    cl_mem eq_noise_var_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n * sizeof(float), nullptr, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &re_grid_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &ch_est_buf);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &noise_var_buf);
    clSetKernelArg(kernel, 3, sizeof(cl_float), &tx_scaling);
    clSetKernelArg(kernel, 4, sizeof(cl_mem), &eq_symbols_buf);
    clSetKernelArg(kernel, 5, sizeof(cl_mem), &eq_noise_var_buf);
    size_t global_size = n;
    clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    std::vector<cl_float2> kernel_eq_symbols(n);
    std::vector<float> kernel_eq_noise_var(n);
    clEnqueueReadBuffer(queue, eq_symbols_buf, CL_TRUE, 0, n * sizeof(cl_float2), kernel_eq_symbols.data(), 0,
                        nullptr, nullptr);
    clEnqueueReadBuffer(queue, eq_noise_var_buf, CL_TRUE, 0, n * sizeof(float), kernel_eq_noise_var.data(), 0,
                        nullptr, nullptr);
    clReleaseMemObject(re_grid_buf);
    clReleaseMemObject(ch_est_buf);
    clReleaseMemObject(noise_var_buf);
    clReleaseMemObject(eq_symbols_buf);
    clReleaseMemObject(eq_noise_var_buf);

    bool match = true;
    size_t first_mismatch = 0;
    for (unsigned i = 0; i < n; i++) {
      if (!close_enough(kernel_eq_symbols[i].s[0], real_eq_symbols[i].real()) ||
          !close_enough(kernel_eq_symbols[i].s[1], real_eq_symbols[i].imag()) ||
          !close_enough(kernel_eq_noise_var[i], real_eq_noise_var[i])) {
        match = false;
        first_mismatch = i;
        break;
      }
      if (accumulate_nrmse) {
        double dre = kernel_eq_symbols[i].s[0] - real_eq_symbols[i].real();
        double dim = kernel_eq_symbols[i].s[1] - real_eq_symbols[i].imag();
        sq_err_sum_sym += dre * dre + dim * dim;
        sq_ref_sum_sym += real_eq_symbols[i].real() * real_eq_symbols[i].real() +
                         real_eq_symbols[i].imag() * real_eq_symbols[i].imag();
        double nvar_err = std::fabs(kernel_eq_noise_var[i] - real_eq_noise_var[i]) /
                          std::max(1e-9f, real_eq_noise_var[i]);
        max_nvar_rel_err = std::max(max_nvar_rel_err, nvar_err);
      }
    }
    check(match, "REAL KERNEL matches real OCUDU equalizer (" + label + ")");
    if (!match) {
      std::fprintf(stderr, "  first mismatch at %zu: real=(%f,%f,nv=%f) kernel=(%f,%f,nv=%f)\n", first_mismatch,
                   real_eq_symbols[first_mismatch].real(), real_eq_symbols[first_mismatch].imag(),
                   real_eq_noise_var[first_mismatch], kernel_eq_symbols[first_mismatch].s[0],
                   kernel_eq_symbols[first_mismatch].s[1], kernel_eq_noise_var[first_mismatch]);
    }
  };

  std::mt19937 rgen(777);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  // Normal case: random y/h, several (noise_var, tx_scaling) combinations.
  for (float noise_var : {0.01f, 0.1f, 1.0f}) {
    for (float tx_scaling : {1.0f, 0.5f, 2.0f}) {
      std::vector<cf_t> y(64), h(64);
      for (auto& v : y) v = cf_t(dist(rgen), dist(rgen));
      for (auto& v : h) v = cf_t(dist(rgen) * 0.5f + 0.5f, dist(rgen) * 0.5f);  // avoid |h|~0 in the "normal" case
      char label[128];
      std::snprintf(label, sizeof(label), "normal, noise_var=%.2f, tx_scaling=%.2f", noise_var, tx_scaling);
      run_case(label, y, h, noise_var, tx_scaling);
    }
  }

  // Degenerate: |h| == 0 for every RE -> zero symbol, infinite noise var.
  {
    std::vector<cf_t> y(16), h(16, cf_t(0.0f, 0.0f));
    for (auto& v : y) v = cf_t(dist(rgen), dist(rgen));
    run_case("|h|=0", y, h, 0.5f, 1.0f, /*accumulate_nrmse=*/false);  // both sides are 0/+inf, not a meaningful NRMSE point
  }

  // Degenerate: noise_var == 0 -> zero symbol, infinite noise var (real equalizer's own doc: "ill-formed" noise var).
  {
    std::vector<cf_t> y(16), h(16);
    for (auto& v : y) v = cf_t(dist(rgen), dist(rgen));
    for (auto& v : h) v = cf_t(dist(rgen) * 0.5f + 0.5f, dist(rgen) * 0.5f);
    run_case("noise_var=0", y, h, 0.0f, 1.0f, /*accumulate_nrmse=*/false);
  }

  double nrmse_eq_symbols = std::sqrt(sq_err_sum_sym / sq_ref_sum_sym);
  std::printf("\nT_K3 gate (eq_symbols NRMSE, normal cases, must be < 0.005 per parent LLD §7): %.6g\n",
             nrmse_eq_symbols);
  std::printf("T_K3n gate (eq_noise_var max relative error, normal cases, must be < 0.005 per parent LLD §7): %.6g\n",
             max_nvar_rel_err);
  check(nrmse_eq_symbols < 0.005, "eq_symbols NRMSE within T_K3 = 0.5% (parent LLD §7, Q1 resolved)");
  check(max_nvar_rel_err < 0.005, "eq_noise_var relative error within T_K3n = 0.5% (parent LLD §7, Q1 resolved)");

  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k3_test: ALL PASS" : "k3_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
