#include "oi_p2_ldpc_decode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// p0-rig-scaffold's own prior work, reused directly (not copied) -- see this module's header
// comment. bg_tables.h defines BG1_M/BG1_N/BG2_M/BG2_N/NO_EDGE/MAX_LS/LS_TO_IDX/BG1_SHIFTS/
// BG2_SHIFTS at file scope; #include at file scope is required (matches bit_diff_test.cpp's own
// usage convention).
#include "../../../p0-rig-scaffold/docker/gpu-phy/ldpc_suite/bg_tables.h"

namespace {

// n_short (66/50), n_vn_info (22/10, includes the 2 always-punctured info VNs) per base graph.
struct BgShape {
  uint32_t n_vn_full;
  uint32_t n_cn;
  uint32_t n_vn_info;
  uint32_t n_short;
  const unsigned short* shifts_table;  // &BG{1,2}_SHIFTS[0][0][0]
};

BgShape bg_shape(uint32_t base_graph) {
  if (base_graph == 1) {
    return {BG1_N, BG1_M, 22u, 66u, &BG1_SHIFTS[0][0][0]};
  }
  return {BG2_N, BG2_M, 10u, 50u, &BG2_SHIFTS[0][0][0]};
}

}  // namespace

extern "C" int oi_p2_ldpc_decoder_init(oi_p2_ldpc_decoder* dec, cl_context ctx, cl_command_queue queue,
                                       const char* ldpc_decode_cl_path) {
  dec->ctx = ctx;
  dec->queue = queue;

  FILE* f = fopen(ldpc_decode_cl_path, "r");
  if (!f) {
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<char> src(sz + 1, 0);
  size_t nread = fread(src.data(), 1, sz, f);
  (void)nread;
  fclose(f);

  cl_int err = CL_SUCCESS;
  const char* src_ptr = src.data();
  dec->program = clCreateProgramWithSource(ctx, 1, &src_ptr, nullptr, &err);
  if (err != CL_SUCCESS) return (int)err;

  cl_device_id device = nullptr;
  clGetContextInfo(ctx, CL_CONTEXT_DEVICES, sizeof(device), &device, nullptr);

  // Matches bit_diff_test.cpp's own build options exactly (NO_EDGE/MAX_LS from bg_tables.h).
  char opts[256];
  std::snprintf(opts, sizeof(opts), "-DNO_EDGE=%#x -DMAX_LS=%d -cl-std=CL1.2", NO_EDGE, MAX_LS);
  err = clBuildProgram(dec->program, 1, &device, opts, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t log_len = 0;
    clGetProgramBuildInfo(dec->program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
    std::vector<char> log(log_len + 1, 0);
    clGetProgramBuildInfo(dec->program, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
    std::fprintf(stderr, "oi_p2_ldpc_decode: kernel build failed:\n%s\n", log.data());
    return (int)err;
  }

  dec->kernel = clCreateKernel(dec->program, "ldpc_decode", &err);
  return (int)err;
}

extern "C" void oi_p2_ldpc_decoder_destroy(oi_p2_ldpc_decoder* dec) {
  if (dec->kernel) clReleaseKernel(dec->kernel);
  if (dec->program) clReleaseProgram(dec->program);
}

extern "C" int oi_p2_ldpc_decode_cb(oi_p2_ldpc_decoder* dec, const int8_t* cb_llr, uint32_t base_graph,
                                    uint32_t lifting_size, uint32_t n_iter, uint8_t* out_bits) {
  BgShape shape = bg_shape(base_graph);
  uint32_t z = lifting_size;
  uint8_t ls_idx = LS_TO_IDX[z];
  if (ls_idx == 255) {
    return -2;  // not one of the 51 standard TS 38.212 lifting sizes
  }

  // Build the padded llr_input: 2*Z always-punctured (neutral, LLR=0) columns, then K6's real
  // n_short*Z transmitted LLRs unchanged (see this module's header comment for why this padding
  // is required and is not a P2-R9 violation).
  uint32_t full_bits = shape.n_vn_full * z;
  uint32_t short_bits = shape.n_short * z;
  std::vector<int8_t> llr_full(full_bits, 0);
  std::memcpy(llr_full.data() + 2u * z, cb_llr, short_bits);

  size_t shift_bytes = (size_t)shape.n_cn * shape.n_vn_full * sizeof(unsigned short);
  size_t c2v_bytes = (size_t)shape.n_cn * shape.n_vn_full * z * sizeof(int8_t);
  uint32_t out_bytes = (shape.n_vn_info * z + 7u) / 8u;

  cl_int err = CL_SUCCESS;
  cl_mem cl_llr =
      clCreateBuffer(dec->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, full_bits, llr_full.data(), &err);
  if (err != CL_SUCCESS) return (int)err;

  std::vector<uint8_t> out_zero(out_bytes, 0);
  cl_mem cl_out =
      clCreateBuffer(dec->ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, out_bytes, out_zero.data(), &err);

  const unsigned short* shifts_row = shape.shifts_table + (size_t)ls_idx * shape.n_cn * shape.n_vn_full;
  cl_mem cl_shifts =
      clCreateBuffer(dec->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, shift_bytes, (void*)shifts_row, &err);

  std::vector<int8_t> c2v_zero(c2v_bytes, 0);
  cl_mem cl_c2v = clCreateBuffer(dec->ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, c2v_bytes, c2v_zero.data(), &err);

  cl_int n_vn_full_a = (cl_int)shape.n_vn_full;
  cl_int n_cn_a = (cl_int)shape.n_cn;
  cl_int n_vn_info_a = (cl_int)shape.n_vn_info;
  cl_int ls_a = (cl_int)z;
  cl_int n_iter_a = (cl_int)n_iter;
  cl_int cb_off_a = 0;

  clSetKernelArg(dec->kernel, 0, sizeof(cl_mem), &cl_llr);
  clSetKernelArg(dec->kernel, 1, sizeof(cl_mem), &cl_out);
  clSetKernelArg(dec->kernel, 2, sizeof(cl_mem), &cl_shifts);
  clSetKernelArg(dec->kernel, 3, sizeof(cl_mem), &cl_c2v);
  clSetKernelArg(dec->kernel, 4, sizeof(cl_int), &n_vn_full_a);
  clSetKernelArg(dec->kernel, 5, sizeof(cl_int), &n_cn_a);
  clSetKernelArg(dec->kernel, 6, sizeof(cl_int), &n_vn_info_a);
  clSetKernelArg(dec->kernel, 7, sizeof(cl_int), &ls_a);
  clSetKernelArg(dec->kernel, 8, sizeof(cl_int), &n_iter_a);
  clSetKernelArg(dec->kernel, 9, sizeof(cl_int), &cb_off_a);

  size_t gs = 1, lws = 1;
  err = clEnqueueNDRangeKernel(dec->queue, dec->kernel, 1, nullptr, &gs, &lws, 0, nullptr, nullptr);
  if (err == CL_SUCCESS) {
    clFinish(dec->queue);
    err = clEnqueueReadBuffer(dec->queue, cl_out, CL_TRUE, 0, out_bytes, out_bits, 0, nullptr, nullptr);
  }

  clReleaseMemObject(cl_llr);
  clReleaseMemObject(cl_out);
  clReleaseMemObject(cl_shifts);
  clReleaseMemObject(cl_c2v);

  return (int)err;
}
