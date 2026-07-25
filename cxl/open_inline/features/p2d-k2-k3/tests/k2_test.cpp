// k2_test.cpp — P2-R4 + Q1 (float-tolerance threshold measurement): the ACTUAL k2a_chanest_symbol
// + k2b_chanest_combine OpenCL kernels (via PoCL) vs the REAL linked OCUDU dmrs_pusch_estimator
// (td_interpolation_strategy=interpolate, fd_smoothing_strategy=filter — matching the MVP scope
// this slice committed to). Builds a synthetic resource grid (known channel + DM-RS + noise),
// feeds the SAME grid to both, and measures NRMSE(ch_est) / relative-error(noise_var, epre)
// across several (channel, noise) configurations — this sweep IS Q1's "record NRMSE
// distributions" task; the max across configs (with margin) is what parent LLD §7 records as
// T_K2/T_K2n.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/host/oi_dmrs_ref_seq.h"

#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include "ocudu/support/executors/task_executor.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

namespace {

constexpr unsigned kNofPrb = 51;
constexpr unsigned kNofSubcarriers = kNofPrb * 12;  // 612
constexpr unsigned kNofPilots = kNofPrb * 6;         // 306
constexpr unsigned kNofDataRe = 11 * kNofSubcarriers;

// Trivial synchronous executor: runs the task inline. dmrs_pusch_estimator_impl checks
// executor.defer()'s return; if the task hasn't already run inline, it invokes it directly (see
// dmrs_pusch_estimator_impl.cpp's constructor callback dispatch) -- returning false here is the
// simplest correct choice, matching that fallback path exactly.
class oi_inline_executor : public ocudu::task_executor {
 public:
  bool execute(ocudu::unique_task task) override {
    task();
    return true;
  }
  bool defer(ocudu::unique_task /*task*/) override { return false; }
};

struct Metrics {
  double nrmse_ch_est;
  double noise_var_rel_err;
  double epre_rel_err;
};

}  // namespace

int main() {
  using namespace ocudu;

  // --- Build the real OCUDU DM-RS PUSCH estimator once (interpolate/filter, matching this
  // slice's committed scope) ---
  auto prg_factory = create_pseudo_random_generator_sw_factory();
  auto low_papr_factory = create_low_papr_sequence_generator_sw_factory();
  auto dft_factory = create_dft_processor_factory_generic();
  auto ta_factory = create_time_alignment_estimator_dft_factory(dft_factory);
  auto ch_estimator_factory = create_port_channel_estimator_factory_sw(ta_factory);
  check(prg_factory && low_papr_factory && dft_factory && ta_factory && ch_estimator_factory,
        "real OCUDU sub-factories created");

  oi_inline_executor executor;
  auto dmrs_estimator_factory = create_dmrs_pusch_estimator_factory_sw(
      prg_factory, low_papr_factory, ch_estimator_factory, executor, /*nof_rx_ports=*/1,
      port_channel_estimator_fd_smoothing_strategy::filter,
      port_channel_estimator_td_interpolation_strategy::interpolate, /*compensate_cfo=*/false);
  check(dmrs_estimator_factory != nullptr, "real OCUDU dmrs_pusch_estimator_factory created");
  auto estimator = dmrs_estimator_factory->create();
  check(estimator != nullptr, "real OCUDU dmrs_pusch_estimator created");

  auto grid_factory = create_resource_grid_factory();

  // --- OpenCL/PoCL setup + build both kernels once ---
  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  auto load_src = [](const char* path) {
    FILE* f = fopen(path, "r");
    std::vector<char> buf;
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      buf.resize(sz + 1, 0);
      size_t nread = fread(buf.data(), 1, sz, f);
      (void)nread;
      fclose(f);
    }
    return buf;
  };
  auto build_kernel = [&](const char* path, const char* name) {
    std::vector<char> src = load_src(path);
    const char* src_ptr = src.data();
    cl_int e = CL_SUCCESS;
    cl_program prog = clCreateProgramWithSource(ctx, 1, &src_ptr, nullptr, &e);
    e = clBuildProgram(prog, 1, &device, "-cl-std=CL1.2 -I../../p2a-scaffold/src/kernels", nullptr, nullptr);
    if (e != CL_SUCCESS) {
      size_t log_len = 0;
      clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
      std::vector<char> log(log_len + 1, 0);
      clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
      std::fprintf(stderr, "kernel build failed (%s):\n%s\n", name, log.data());
    }
    check(e == CL_SUCCESS, std::string(name) + " kernel builds on PoCL");
    cl_kernel k = clCreateKernel(prog, name, &e);
    check(e == CL_SUCCESS, std::string(name) + " kernel created");
    return k;
  };
  cl_kernel k2a = build_kernel("../src/kernels/k2a_chanest_symbol.cl", "k2a_chanest_symbol");
  cl_kernel k2b = build_kernel("../src/kernels/k2b_chanest_combine.cl", "k2b_chanest_combine");

  const uint32_t n_id = 1, n_scid = 0, nslot = 0;

  // One full run of the real estimator + both kernels for a given (channel, noise) config.
  auto run_config = [&](cf_t h_true, float noise_sigma, unsigned rng_seed) -> Metrics {
    auto grid = grid_factory->create(1, 14, kNofSubcarriers);
    grid->set_all_zero();

    std::vector<oi_cf32> ref_seq2(kNofPilots), ref_seq7(kNofPilots), ref_seq11(kNofPilots);
    oi_dmrs_ref_seq_generate(nslot, 2, n_id, n_scid, ref_seq2.data(), kNofPrb);
    oi_dmrs_ref_seq_generate(nslot, 7, n_id, n_scid, ref_seq7.data(), kNofPrb);
    oi_dmrs_ref_seq_generate(nslot, 11, n_id, n_scid, ref_seq11.data(), kNofPrb);

    std::mt19937 rgen(rng_seed);
    std::normal_distribution<float> noise_dist(0.0f, noise_sigma);
    auto make_row = [&](const std::vector<oi_cf32>& ref_seq) {
      std::vector<cf_t> row(kNofSubcarriers, cf_t(0.0f, 0.0f));
      for (unsigned p = 0; p < kNofPilots; p++) {
        cf_t ref(ref_seq[p].re, ref_seq[p].im);
        row[2 * p] = h_true * ref + cf_t(noise_dist(rgen), noise_dist(rgen));
      }
      return row;
    };
    std::vector<cf_t> row2 = make_row(ref_seq2), row7 = make_row(ref_seq7), row11 = make_row(ref_seq11);
    grid->get_writer().put(0, 2, 0, span<const cf_t>(row2));
    grid->get_writer().put(0, 7, 0, span<const cf_t>(row7));
    grid->get_writer().put(0, 11, 0, span<const cf_t>(row11));

    dmrs_pusch_estimator::configuration cfg{};
    cfg.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, nslot);
    dmrs_pusch_estimator::pseudo_random_sequence_configuration seq_cfg{};
    seq_cfg.type = dmrs_type::TYPE1;
    seq_cfg.nof_tx_layers = 1;
    seq_cfg.scrambling_id = n_id;
    seq_cfg.n_scid = (n_scid != 0);
    cfg.sequence_config = seq_cfg;
    cfg.scaling = 1.0f;  // 0 dB DMRS-to-data power ratio (MVP simplification)
    cfg.c_prefix = cyclic_prefix::NORMAL;
    cfg.symbols_mask.resize(14);
    cfg.symbols_mask.set(2);
    cfg.symbols_mask.set(7);
    cfg.symbols_mask.set(11);
    cfg.rb_mask.resize(kNofPrb);
    cfg.rb_mask.fill(0, kNofPrb);
    cfg.first_symbol = 0;
    cfg.nof_symbols = 14;
    cfg.rx_ports = {0};

    class : public dmrs_pusch_estimator_notifier {
     public:
      const dmrs_pusch_estimator_results* results = nullptr;
      void on_estimation_complete(const dmrs_pusch_estimator_results& r) override { results = &r; }
    } notifier;

    estimator->estimate(notifier, grid->get_reader(), cfg);
    check(notifier.results != nullptr, "real OCUDU estimator completed (notifier called)");
    if (notifier.results == nullptr) {
      return {1e9, 1e9, 1e9};
    }
    float real_noise_var = notifier.results->get_noise_variance(0);
    float real_epre = notifier.results->get_epre(0);

    std::vector<cl_float2> re_grid_host(14 * kNofSubcarriers, cl_float2{{0.0f, 0.0f}});
    auto fill_row = [&](unsigned symbol, const std::vector<cf_t>& row) {
      for (unsigned re = 0; re < kNofSubcarriers; re++) re_grid_host[symbol * kNofSubcarriers + re] = {{row[re].real(), row[re].imag()}};
    };
    fill_row(2, row2);
    fill_row(7, row7);
    fill_row(11, row11);

    cl_mem re_grid_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        re_grid_host.size() * sizeof(cl_float2), re_grid_host.data(), &err);
    uint32_t bitmap_host = (1u << 2) | (1u << 7) | (1u << 11);
    cl_mem bitmap_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(uint32_t), &bitmap_host, &err);

    auto ref_seq_to_cl = [](const std::vector<oi_cf32>& s) {
      std::vector<cl_float2> out(s.size());
      for (size_t i = 0; i < s.size(); i++) out[i] = {{s[i].re, s[i].im}};
      return out;
    };
    std::vector<cl_float2> ref2_cl = ref_seq_to_cl(ref_seq2), ref7_cl = ref_seq_to_cl(ref_seq7),
                           ref11_cl = ref_seq_to_cl(ref_seq11);
    cl_mem ref2_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, kNofPilots * sizeof(cl_float2),
                                     ref2_cl.data(), &err);
    cl_mem ref7_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, kNofPilots * sizeof(cl_float2),
                                     ref7_cl.data(), &err);
    cl_mem ref11_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, kNofPilots * sizeof(cl_float2),
                                      ref11_cl.data(), &err);

    auto run_k2a = [&](uint32_t symbol_idx, cl_mem ref_buf, cl_mem* fd_est_out, cl_mem* filtered_pilot_out,
                       float* epre_partial_out) {
      cl_mem fd_est_buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, kNofSubcarriers * sizeof(cl_float2), nullptr, &err);
      cl_mem filtered_pilot_buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, kNofPilots * sizeof(cl_float2), nullptr, &err);
      cl_mem epre_buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(float), nullptr, &err);
      float beta_scaling = 1.0f;
      clSetKernelArg(k2a, 0, sizeof(cl_mem), &re_grid_buf);
      clSetKernelArg(k2a, 1, sizeof(cl_mem), &bitmap_buf);
      clSetKernelArg(k2a, 2, sizeof(cl_mem), &ref_buf);
      clSetKernelArg(k2a, 3, sizeof(cl_uint), &symbol_idx);
      clSetKernelArg(k2a, 4, sizeof(cl_float), &beta_scaling);
      clSetKernelArg(k2a, 5, sizeof(cl_mem), &fd_est_buf);
      clSetKernelArg(k2a, 6, sizeof(cl_mem), &filtered_pilot_buf);
      clSetKernelArg(k2a, 7, sizeof(cl_mem), &epre_buf);
      size_t one = 1;
      clEnqueueNDRangeKernel(queue, k2a, 1, nullptr, &one, nullptr, 0, nullptr, nullptr);
      clEnqueueReadBuffer(queue, epre_buf, CL_TRUE, 0, sizeof(float), epre_partial_out, 0, nullptr, nullptr);
      clReleaseMemObject(epre_buf);
      *fd_est_out = fd_est_buf;
      *filtered_pilot_out = filtered_pilot_buf;
    };

    cl_mem fd_est2, fd_est7, fd_est11, filt2, filt7, filt11;
    float epre_p2, epre_p7, epre_p11;
    run_k2a(2, ref2_buf, &fd_est2, &filt2, &epre_p2);
    run_k2a(7, ref7_buf, &fd_est7, &filt7, &epre_p7);
    run_k2a(11, ref11_buf, &fd_est11, &filt11, &epre_p11);

    cl_mem ch_est_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, kNofDataRe * sizeof(cl_float2), nullptr, &err);
    cl_mem noise_var_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(float), nullptr, &err);
    cl_mem epre_out_buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(float), nullptr, &err);

    clSetKernelArg(k2b, 0, sizeof(cl_mem), &fd_est2);
    clSetKernelArg(k2b, 1, sizeof(cl_mem), &fd_est7);
    clSetKernelArg(k2b, 2, sizeof(cl_mem), &fd_est11);
    clSetKernelArg(k2b, 3, sizeof(cl_mem), &filt2);
    clSetKernelArg(k2b, 4, sizeof(cl_mem), &filt7);
    clSetKernelArg(k2b, 5, sizeof(cl_mem), &filt11);
    clSetKernelArg(k2b, 6, sizeof(cl_mem), &ref2_buf);
    clSetKernelArg(k2b, 7, sizeof(cl_mem), &ref7_buf);
    clSetKernelArg(k2b, 8, sizeof(cl_mem), &ref11_buf);
    clSetKernelArg(k2b, 9, sizeof(cl_mem), &re_grid_buf);
    clSetKernelArg(k2b, 10, sizeof(cl_float), &epre_p2);
    clSetKernelArg(k2b, 11, sizeof(cl_float), &epre_p7);
    clSetKernelArg(k2b, 12, sizeof(cl_float), &epre_p11);
    clSetKernelArg(k2b, 13, sizeof(cl_mem), &ch_est_buf);
    clSetKernelArg(k2b, 14, sizeof(cl_mem), &noise_var_buf);
    clSetKernelArg(k2b, 15, sizeof(cl_mem), &epre_out_buf);
    size_t one = 1;
    clEnqueueNDRangeKernel(queue, k2b, 1, nullptr, &one, nullptr, 0, nullptr, nullptr);

    std::vector<cl_float2> kernel_ch_est(kNofDataRe);
    float kernel_noise_var = 0.0f, kernel_epre = 0.0f;
    clEnqueueReadBuffer(queue, ch_est_buf, CL_TRUE, 0, kNofDataRe * sizeof(cl_float2), kernel_ch_est.data(), 0,
                        nullptr, nullptr);
    clEnqueueReadBuffer(queue, noise_var_buf, CL_TRUE, 0, sizeof(float), &kernel_noise_var, 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, epre_out_buf, CL_TRUE, 0, sizeof(float), &kernel_epre, 0, nullptr, nullptr);

    const int data_symbols[11] = {0, 1, 3, 4, 5, 6, 8, 9, 10, 12, 13};
    double sq_err_sum = 0.0, sq_ref_sum = 0.0;
    for (unsigned lin = 0; lin < 11; lin++) {
      int abs_symbol = data_symbols[lin];
      std::vector<cbf16_t> real_est(kNofSubcarriers);
      notifier.results->get_symbol_ch_estimate(span<cbf16_t>(real_est), abs_symbol, 0, 0);
      for (unsigned re = 0; re < kNofSubcarriers; re++) {
        cf_t real_val = to_cf(real_est[re]);
        cl_float2 kv = kernel_ch_est[lin * kNofSubcarriers + re];
        double dre = kv.s[0] - real_val.real();
        double dim = kv.s[1] - real_val.imag();
        sq_err_sum += dre * dre + dim * dim;
        sq_ref_sum += real_val.real() * real_val.real() + real_val.imag() * real_val.imag();
      }
    }

    clReleaseMemObject(re_grid_buf);
    clReleaseMemObject(bitmap_buf);
    clReleaseMemObject(ref2_buf);
    clReleaseMemObject(ref7_buf);
    clReleaseMemObject(ref11_buf);
    clReleaseMemObject(fd_est2);
    clReleaseMemObject(fd_est7);
    clReleaseMemObject(fd_est11);
    clReleaseMemObject(filt2);
    clReleaseMemObject(filt7);
    clReleaseMemObject(filt11);
    clReleaseMemObject(ch_est_buf);
    clReleaseMemObject(noise_var_buf);
    clReleaseMemObject(epre_out_buf);

    Metrics m;
    m.nrmse_ch_est = std::sqrt(sq_err_sum / sq_ref_sum);
    m.noise_var_rel_err = std::fabs(kernel_noise_var - real_noise_var) / std::max(1e-9f, real_noise_var);
    m.epre_rel_err = std::fabs(kernel_epre - real_epre) / std::max(1e-9f, real_epre);
    return m;
  };

  // Sweep: several (channel, noise) points spanning low/high SNR, to build the NRMSE
  // distribution Q1 asks for (not a single point).
  struct Config {
    const char* label;
    cf_t h_true;
    float noise_sigma;
    unsigned seed;
  };
  const std::vector<Config> configs = {
      {"moderate SNR", cf_t(0.8f, 0.3f), 0.02f, 42},
      {"low SNR", cf_t(0.1f, 1.2f), 0.15f, 1234},
      {"high SNR", cf_t(-0.5f, -0.9f), 0.005f, 777},
  };

  double max_nrmse = 0.0, max_noise_var_err = 0.0, max_epre_err = 0.0;
  for (const auto& c : configs) {
    Metrics m = run_config(c.h_true, c.noise_sigma, c.seed);
    std::printf("[%s] ch_est NRMSE=%.6g noise_var_rel_err=%.6g epre_rel_err=%.6g\n", c.label, m.nrmse_ch_est,
               m.noise_var_rel_err, m.epre_rel_err);
    max_nrmse = std::max(max_nrmse, m.nrmse_ch_est);
    max_noise_var_err = std::max(max_noise_var_err, m.noise_var_rel_err);
    max_epre_err = std::max(max_epre_err, m.epre_rel_err);
  }

  std::printf("\nT_K2 gate (max NRMSE across sweep, must be < 0.05 per parent LLD §7): %.6g\n", max_nrmse);
  std::printf("T_K2n gate (max noise_var rel. error across sweep, must be < 0.05 per parent LLD §7): %.6g\n",
             max_noise_var_err);
  std::printf("epre max rel. error across sweep (diagnostic only, not gated): %.6g\n", max_epre_err);
  check(max_nrmse < 0.05, "ch_est NRMSE within T_K2 = 5% (parent LLD §7, Q1 resolved)");
  check(max_noise_var_err < 0.05, "noise_var relative error within T_K2n = 5% (parent LLD §7, Q1 resolved)");

  clReleaseKernel(k2a);
  clReleaseKernel(k2b);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k2_test: ALL PASS" : "k2_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
