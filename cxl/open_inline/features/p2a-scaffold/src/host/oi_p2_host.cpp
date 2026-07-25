#include "oi_p2_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "oi_frame_desc.h"
#include "oi_p2_buffers.h"
#include "oi_p2_config.h"
#include "oi_p2_tb_record.h"

// Real kernels + host helpers, referenced directly from their owning sub-features (single source
// of truth, same reuse convention as every cross-slice reference this project has used since K2's
// DMRS ref-seq reused K5's Gold-sequence module). 2026-07-23, p2f-integration: replaces p2a's
// original stub kernel chain -- see VERIFICATION.md "stub replacement" section for the full
// rationale and the real gaps found while wiring this (data-RE compaction step, per-CB rm_length
// heterogeneity, MCS conveyance).
#include "../../../p2b-k5-k6/src/host/oi_p2_gold_init.h"
#include "../../../p2d-k2-k3/src/host/oi_dmrs_ref_seq.h"
#include "../../../p2f-integration/src/host/oi_p2_cb_segment.h"
#include "../../../p2f-integration/src/host/oi_p2_crc.h"
#include "../../../p2f-integration/src/host/oi_p2_ldpc_decode.h"

namespace {

// Kernel source paths, relative to the CWD this pipeline is run from (tests/ of whichever feature
// links this file -- same CWD-relative convention every p2* test in this project already uses for
// its own kernel). KNOWN FRAGILITY (flagged, not silently assumed correct): this only resolves
// correctly when run from p2a-scaffold/tests/; oi_p2_setup's signature is frozen (P2-R17), so this
// couldn't be made a parameter without breaking that promise -- see VERIFICATION.md.
constexpr const char* kK1Src = "../../p2c-k1/src/kernels/k1_depacketizer.cl";
constexpr const char* kK2aSrc = "../../p2d-k2-k3/src/kernels/k2a_chanest_symbol.cl";
constexpr const char* kK2bSrc = "../../p2d-k2-k3/src/kernels/k2b_chanest_combine.cl";
constexpr const char* kK3Src = "../../p2d-k2-k3/src/kernels/k3_equalizer.cl";
constexpr const char* kK4Src = "../../p2e-k4/src/kernels/k4_demapper.cl";
constexpr const char* kK5Src = "../../p2b-k5-k6/src/kernels/k5_descrambler.cl";
constexpr const char* kK6Src = "../../p2b-k5-k6/src/kernels/k6_rate_dematcher.cl";
constexpr const char* kLdpcSrc = "../../p0-rig-scaffold/docker/gpu-phy/ldpc_suite/ldpc_decode.cl";

// Kernel-compat/frame-desc/wire-layout include dirs each kernel's #include needs at build time.
constexpr const char* kBuildOpts =
    "-cl-std=CL1.2 -I../../p2a-scaffold/src/kernels -I../../p2a-scaffold/src/host";

// MVP MCS points (TS 38.214 Table 5.1.3.1-1 indices 4/13/21) -- real TBS/code-rate values
// confirmed via a real OCUDU tbs_calculator run (see STATUS.md), not the general TS 38.214
// SS5.1.3.2 TBS-quantization procedure re-derived at runtime. This is a deliberate MVP-fixed-
// config simplification, same class as this project's other "fixed-config, closed set, don't
// re-derive the general procedure" choices (K1's hardcoded 51 PRB, oi_p2_buffers' fixed
// dimensions) -- flagged in VERIFICATION.md, not silently assumed.
struct McsParams {
  uint32_t mcs_index;
  uint32_t qm;
  uint32_t tbs_bits;
  float code_rate;
};
constexpr McsParams kMcsTable[3] = {
    {4, 2, 4608, 0.3008f},
    {13, 4, 14600, 0.4785f},
    {21, 6, 27656, 0.6016f},
};

const McsParams* find_mcs(uint32_t mcs_index) {
  for (const auto& m : kMcsTable) {
    if (m.mcs_index == mcs_index) return &m;
  }
  return nullptr;
}

// DMRS symbol positions, this MVP's fixed config (SPEC "Fixed MVP configuration": DMRS type1,
// additional_positions=2 -> symbols {2,7,11}).
constexpr uint32_t kDmrsSymbols[3] = {2, 7, 11};
constexpr uint32_t kDataSymbols[11] = {0, 1, 3, 4, 5, 6, 8, 9, 10, 12, 13};

cl_program build_program(cl_context ctx, cl_device_id device, const char* path, cl_int* out_err) {
  FILE* f = fopen(path, "r");
  if (!f) {
    std::fprintf(stderr, "oi_p2_host: cannot open kernel source '%s'\n", path);
    *out_err = -1;
    return nullptr;
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
  cl_program prog = clCreateProgramWithSource(ctx, 1, &src_ptr, nullptr, &err);
  if (err != CL_SUCCESS) {
    *out_err = err;
    return nullptr;
  }
  err = clBuildProgram(prog, 1, &device, kBuildOpts, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t log_len = 0;
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
    std::vector<char> log(log_len + 1, 0);
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
    std::fprintf(stderr, "oi_p2_host: kernel build failed for '%s':\n%s\n", path, log.data());
    clReleaseProgram(prog);
    *out_err = err;
    return nullptr;
  }
  *out_err = CL_SUCCESS;
  return prog;
}

struct SlotState {
  bool launched = false;
  bool drained = false;
  uint32_t mcs_index = 0;
  cl_event final_event = nullptr;  // K5 completion (last fully GPU-resident stage)
};

}  // namespace

struct oi_p2_pipeline {
  cl_context ctx = nullptr;
  cl_command_queue queue = nullptr;
  cl_device_id device = nullptr;
  oi_p2::MvpConfig config;
  oi_p2::BufferPool buffers;

  cl_program prog_k1 = nullptr, prog_k2a = nullptr, prog_k2b = nullptr, prog_k3 = nullptr;
  cl_program prog_k4 = nullptr, prog_k5 = nullptr, prog_k6 = nullptr;
  cl_kernel k1 = nullptr, k2a = nullptr, k2b = nullptr, k3 = nullptr, k4 = nullptr, k5 = nullptr, k6 = nullptr;
  oi_p2_ldpc_decoder ldpc{};

  // re_grid_compact: data-RE-linear view of I2's 11 data symbols (K3 expects this shape, matching
  // I3/I4's own layout -- K1 only produces the full 14x612 grid, so this bridges the gap; found
  // while wiring, see VERIFICATION.md "data-RE compaction gap").
  cl_mem re_grid_compact[2] = {nullptr, nullptr};

  std::map<uint32_t, SlotState> slots;
};

extern "C" {

oi_p2_status oi_p2_setup(const char* yaml_path, cl_context ctx, cl_command_queue q,
                          oi_p2_pipeline** out_pipeline) {
  *out_pipeline = nullptr;

  auto* p = new oi_p2_pipeline();
  p->ctx = ctx;
  p->queue = q;
  clGetContextInfo(ctx, CL_CONTEXT_DEVICES, sizeof(p->device), &p->device, nullptr);

  oi_p2::ConfigError err;
  oi_p2::ConfigStatus cst = oi_p2::oi_p2_config_load(yaml_path, &p->config, &err);
  if (cst != oi_p2::ConfigStatus::OK) {
    std::fprintf(stderr, "oi_p2_setup: config rejected — key='%s' got='%s' expected='%s'\n",
                 err.key.c_str(), err.got.c_str(), err.expected.c_str());
    delete p;
    return OI_P2_ERR_CONFIG_REJECTED;
  }

  cl_int cl_err = CL_SUCCESS;
  struct {
    cl_program* prog;
    cl_kernel* kern;
    const char* path;
    const char* name;
  } kernels[] = {
      {&p->prog_k1, &p->k1, kK1Src, "k1_depacketize"},
      {&p->prog_k2a, &p->k2a, kK2aSrc, "k2a_chanest_symbol"},
      {&p->prog_k2b, &p->k2b, kK2bSrc, "k2b_chanest_combine"},
      {&p->prog_k3, &p->k3, kK3Src, "k3_equalize"},
      {&p->prog_k4, &p->k4, kK4Src, "k4_demap"},
      {&p->prog_k5, &p->k5, kK5Src, "k5_descramble"},
      {&p->prog_k6, &p->k6, kK6Src, "k6_rate_dematch"},
  };
  for (auto& k : kernels) {
    *k.prog = build_program(ctx, p->device, k.path, &cl_err);
    if (cl_err != CL_SUCCESS) {
      delete p;
      return OI_P2_ERR_CL_BUILD_FAILED;
    }
    *k.kern = clCreateKernel(*k.prog, k.name, &cl_err);
    if (cl_err != CL_SUCCESS) {
      delete p;
      return OI_P2_ERR_CL_BUILD_FAILED;
    }
  }

  if (oi_p2_ldpc_decoder_init(&p->ldpc, ctx, q, kLdpcSrc) != 0) {
    delete p;
    return OI_P2_ERR_CL_BUILD_FAILED;
  }

  // Arena/desc-ring sizing: generous fixed placeholders (zero-alloc-after-setup rule, HLD §5).
  const size_t kArenaBytes = 4 * 1024 * 1024;
  const size_t kDescRingCapacity = 4096;
  if (!oi_p2::oi_p2_buffers_create(ctx, kArenaBytes, kDescRingCapacity, &p->buffers)) {
    delete p;
    return OI_P2_ERR_DEVICE;
  }

  for (auto& rg : p->re_grid_compact) {
    rg = clCreateBuffer(ctx, CL_MEM_READ_WRITE, oi_p2::kNofDataRe * sizeof(cl_float2), nullptr, &cl_err);
    if (cl_err != CL_SUCCESS) {
      delete p;
      return OI_P2_ERR_DEVICE;
    }
  }

  *out_pipeline = p;
  return OI_P2_OK;
}

oi_p2_status oi_p2_feed(oi_p2_pipeline* p, const oi_frame_desc* desc) {
  SlotState& slot = p->slots[desc->slot_id];
  (void)slot;
  static std::map<uint32_t, size_t> nof_descs_by_slot;
  size_t& nof_descs = nof_descs_by_slot[desc->slot_id];
  if (nof_descs >= p->buffers.desc_ring_capacity) {
    return OI_P2_ERR_ARENA_OVERFLOW;
  }
  cl_int err = clEnqueueWriteBuffer(p->queue, p->buffers.desc_ring, CL_TRUE,
                                    nof_descs * sizeof(*desc), sizeof(*desc), desc, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;
  nof_descs++;
  return OI_P2_OK;
}

oi_p2_status oi_p2_write_arena(oi_p2_pipeline* p, uint64_t offset, const void* data, size_t len) {
  if (offset + len > p->buffers.arena_bytes) {
    return OI_P2_ERR_ARENA_OVERFLOW;
  }
  cl_int err = clEnqueueWriteBuffer(p->queue, p->buffers.arena, CL_TRUE, (size_t)offset, len, data, 0, nullptr,
                                    nullptr);
  return (err == CL_SUCCESS) ? OI_P2_OK : OI_P2_ERR_DEVICE;
}

oi_p2_status oi_p2_launch_slot(oi_p2_pipeline* p, uint32_t slot_id, uint32_t mcs_index) {
  const McsParams* mcs = find_mcs(mcs_index);
  if (!mcs) {
    return OI_P2_ERR_CONFIG_REJECTED;
  }
  bool mcs_in_config = false;
  for (int m : p->config.mcs_set) {
    if ((uint32_t)m == mcs_index) mcs_in_config = true;
  }
  if (!mcs_in_config) {
    return OI_P2_ERR_CONFIG_REJECTED;
  }

  SlotState& slot = p->slots[slot_id];
  auto& bufs = p->buffers.slots[slot_id % 2];
  cl_int err = CL_SUCCESS;

  // --- K1: depacketize ---
  // nof_descs for this slot was tracked in oi_p2_feed via a static map keyed by slot_id; K1 reads
  // the WHOLE desc_ring and filters by slot_id internally, so nof_descs here is the ring's total
  // occupancy (an MVP simplification: one slot's worth of frames per drain cycle, ring not reused
  // across slots within a single pipeline run).
  cl_uint nof_descs = (cl_uint)p->buffers.desc_ring_capacity;
  cl_uint slot_id_arg = slot_id;
  clSetKernelArg(p->k1, 0, sizeof(cl_mem), &p->buffers.arena);
  clSetKernelArg(p->k1, 1, sizeof(cl_mem), &p->buffers.desc_ring);
  clSetKernelArg(p->k1, 2, sizeof(cl_uint), &nof_descs);
  clSetKernelArg(p->k1, 3, sizeof(cl_uint), &slot_id_arg);
  clSetKernelArg(p->k1, 4, sizeof(cl_mem), &bufs.re_grid);
  clSetKernelArg(p->k1, 5, sizeof(cl_mem), &bufs.symbol_bitmap);
  size_t k1_global = nof_descs;
  cl_event e_k1 = nullptr;
  err = clEnqueueNDRangeKernel(p->queue, p->k1, 1, nullptr, &k1_global, nullptr, 0, nullptr, &e_k1);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;

  // --- K2a x3 (one per DMRS symbol) ---
  cl_event e_k2a[3];
  float beta_scaling = 1.0f;  // 0 dB DMRS-to-data ratio (MVP simplification, p2d VERIFICATION.md)
  uint32_t n_id = (p->config.scrambling_n_id >= 0) ? (uint32_t)p->config.scrambling_n_id : 1u;
  uint32_t nslot = slot_id % 20u;  // proxy for the 3GPP frame-relative slot index (mu=1, 20 slots/frame)
  for (int i = 0; i < 3; i++) {
    std::vector<oi_cf32> ref_seq(306);
    oi_dmrs_ref_seq_generate(nslot, kDmrsSymbols[i], n_id, /*n_scid=*/0, ref_seq.data(), 51);
    clEnqueueWriteBuffer(p->queue, bufs.k2a[i].ref_seq, CL_TRUE, 0, 306 * sizeof(cl_float2), ref_seq.data(), 0,
                        nullptr, nullptr);

    cl_uint dmrs_sym = kDmrsSymbols[i];
    clSetKernelArg(p->k2a, 0, sizeof(cl_mem), &bufs.re_grid);
    clSetKernelArg(p->k2a, 1, sizeof(cl_mem), &bufs.symbol_bitmap);
    clSetKernelArg(p->k2a, 2, sizeof(cl_mem), &bufs.k2a[i].ref_seq);
    clSetKernelArg(p->k2a, 3, sizeof(cl_uint), &dmrs_sym);
    clSetKernelArg(p->k2a, 4, sizeof(cl_float), &beta_scaling);
    clSetKernelArg(p->k2a, 5, sizeof(cl_mem), &bufs.k2a[i].fd_est);
    clSetKernelArg(p->k2a, 6, sizeof(cl_mem), &bufs.k2a[i].filtered_pilot);
    clSetKernelArg(p->k2a, 7, sizeof(cl_mem), &bufs.k2a[i].epre_partial);
    size_t one = 1;
    err = clEnqueueNDRangeKernel(p->queue, p->k2a, 1, nullptr, &one, nullptr, 1, &e_k1, &e_k2a[i]);
    if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;
  }

  // --- data-RE compaction: copy the 11 non-DMRS symbol rows of the full I2 grid into
  // data-RE-linear order (K3 expects this shape, matching I3/I4 -- gap found while wiring, see
  // VERIFICATION.md). Fully device-side (clEnqueueCopyBuffer), no host round-trip.
  cl_event e_copy[11];
  for (int lin = 0; lin < 11; lin++) {
    size_t src_off = (size_t)kDataSymbols[lin] * oi_p2::kNofSubcarriers * sizeof(cl_float2);
    size_t dst_off = (size_t)lin * oi_p2::kNofSubcarriers * sizeof(cl_float2);
    size_t bytes = oi_p2::kNofSubcarriers * sizeof(cl_float2);
    err = clEnqueueCopyBuffer(p->queue, bufs.re_grid, p->re_grid_compact[slot_id % 2], src_off, dst_off, bytes, 1,
                             &e_k1, &e_copy[lin]);
    if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;
  }

  // --- K2b: cross-symbol combine ---
  cl_event e_k2b_wait[4] = {e_k2a[0], e_k2a[1], e_k2a[2], e_copy[10]};
  clSetKernelArg(p->k2b, 0, sizeof(cl_mem), &bufs.k2a[0].fd_est);
  clSetKernelArg(p->k2b, 1, sizeof(cl_mem), &bufs.k2a[1].fd_est);
  clSetKernelArg(p->k2b, 2, sizeof(cl_mem), &bufs.k2a[2].fd_est);
  clSetKernelArg(p->k2b, 3, sizeof(cl_mem), &bufs.k2a[0].filtered_pilot);
  clSetKernelArg(p->k2b, 4, sizeof(cl_mem), &bufs.k2a[1].filtered_pilot);
  clSetKernelArg(p->k2b, 5, sizeof(cl_mem), &bufs.k2a[2].filtered_pilot);
  clSetKernelArg(p->k2b, 6, sizeof(cl_mem), &bufs.k2a[0].ref_seq);
  clSetKernelArg(p->k2b, 7, sizeof(cl_mem), &bufs.k2a[1].ref_seq);
  clSetKernelArg(p->k2b, 8, sizeof(cl_mem), &bufs.k2a[2].ref_seq);
  clSetKernelArg(p->k2b, 9, sizeof(cl_mem), &bufs.re_grid);
  // epre_partial args (10,11,12) are scalars the kernel reads by value -- but our K2a wrote them
  // to device buffers; read back the 3 partial sums to host here (tiny, 3 floats) to pass as the
  // scalar kernel args K2b's signature expects.
  float epre_p[3];
  for (int i = 0; i < 3; i++) {
    clWaitForEvents(1, &e_k2a[i]);
    clEnqueueReadBuffer(p->queue, bufs.k2a[i].epre_partial, CL_TRUE, 0, sizeof(float), &epre_p[i], 0, nullptr,
                        nullptr);
  }
  clSetKernelArg(p->k2b, 10, sizeof(cl_float), &epre_p[0]);
  clSetKernelArg(p->k2b, 11, sizeof(cl_float), &epre_p[1]);
  clSetKernelArg(p->k2b, 12, sizeof(cl_float), &epre_p[2]);
  clSetKernelArg(p->k2b, 13, sizeof(cl_mem), &bufs.ch_est);
  clSetKernelArg(p->k2b, 14, sizeof(cl_mem), &bufs.noise_var);
  clSetKernelArg(p->k2b, 15, sizeof(cl_mem), &bufs.epre);
  size_t one = 1;
  cl_event e_k2b;
  err = clEnqueueNDRangeKernel(p->queue, p->k2b, 1, nullptr, &one, nullptr, 4, e_k2b_wait, &e_k2b);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;

  // --- K3: equalize ---
  float tx_scaling = 1.0f;  // unity (MVP simplification, no power-control scenario specified)
  clSetKernelArg(p->k3, 0, sizeof(cl_mem), &p->re_grid_compact[slot_id % 2]);
  clSetKernelArg(p->k3, 1, sizeof(cl_mem), &bufs.ch_est);
  clSetKernelArg(p->k3, 2, sizeof(cl_mem), &bufs.noise_var);
  clSetKernelArg(p->k3, 3, sizeof(cl_float), &tx_scaling);
  clSetKernelArg(p->k3, 4, sizeof(cl_mem), &bufs.eq_symbols);
  clSetKernelArg(p->k3, 5, sizeof(cl_mem), &bufs.eq_noise_var);
  size_t k3_global = oi_p2::kNofDataRe;
  cl_event e_k3;
  err = clEnqueueNDRangeKernel(p->queue, p->k3, 1, nullptr, &k3_global, nullptr, 1, &e_k2b, &e_k3);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;

  // --- K4: demap ---
  cl_uint qm_arg = mcs->qm;
  clSetKernelArg(p->k4, 0, sizeof(cl_mem), &bufs.eq_symbols);
  clSetKernelArg(p->k4, 1, sizeof(cl_mem), &bufs.eq_noise_var);
  clSetKernelArg(p->k4, 2, sizeof(cl_uint), &qm_arg);
  clSetKernelArg(p->k4, 3, sizeof(cl_mem), &bufs.llr);
  size_t k4_global = oi_p2::kNofDataRe;
  cl_event e_k4;
  err = clEnqueueNDRangeKernel(p->queue, p->k4, 1, nullptr, &k4_global, nullptr, 1, &e_k3, &e_k4);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;

  // --- K5: descramble (whole codeword, one Gold sequence per TB -- HLD D4) ---
  uint32_t rnti = p->config.scrambling_rnti;
  uint32_t c_init = (rnti * 32768u + n_id) & 0x7FFFFFFFu;
  cl_uint nof_llrs = oi_p2::kNofDataRe * mcs->qm;
  clSetKernelArg(p->k5, 0, sizeof(cl_mem), &bufs.llr);
  clSetKernelArg(p->k5, 1, sizeof(cl_uint), &c_init);
  clSetKernelArg(p->k5, 2, sizeof(cl_uint), &nof_llrs);
  size_t one_block = 1;
  cl_event e_k5;
  err = clEnqueueNDRangeKernel(p->queue, p->k5, 1, nullptr, &one_block, nullptr, 1, &e_k4, &e_k5);
  if (err != CL_SUCCESS) return OI_P2_ERR_DEVICE;

  for (auto& e : {e_k1, e_k2a[0], e_k2a[1], e_k2a[2], e_copy[0], e_copy[1], e_copy[2], e_copy[3], e_copy[4],
                 e_copy[5], e_copy[6], e_copy[7], e_copy[8], e_copy[9], e_copy[10], e_k2b, e_k3, e_k4}) {
    if (e) clReleaseEvent(e);
  }

  slot.final_event = e_k5;
  slot.mcs_index = mcs_index;
  slot.launched = true;
  return OI_P2_OK;
}

oi_p2_status oi_p2_drain(oi_p2_pipeline* p, uint32_t slot_id, oi_p2_tb_record_c* out_record) {
  auto it = p->slots.find(slot_id);
  if (it == p->slots.end() || !it->second.launched) {
    return OI_P2_ERR_SLOT_INCOMPLETE;
  }
  SlotState& slot = it->second;
  auto& bufs = p->buffers.slots[slot_id % 2];
  const McsParams* mcs = find_mcs(slot.mcs_index);

  clWaitForEvents(1, &slot.final_event);
  clReleaseEvent(slot.final_event);
  slot.final_event = nullptr;
  slot.drained = true;

  // --- Host-orchestrated tail: K6 (per-CB rate-dematch) + LDPC decode + CB desegmentation +
  // CRC (P2-R9/R10). Not fully GPU-resident (see VERIFICATION.md "host-orchestrated CPU tail"):
  // K6 needs a fresh device buffer per CB (heterogeneous rm_length across CBs within one TB rules
  // out a single uniform-stride launch), and oi_p2_ldpc_decode_cb already allocates its own
  // buffers per call -- both are per-call allocations, a documented deviation from HLD §5's
  // "zero device allocations after setup" for this specific (LDPC-adjacent) stage only.
  oi_p2_cb_segment_params seg{};
  oi_p2_cb_segment_compute(mcs->tbs_bits, mcs->code_rate, &seg);

  uint32_t total_llr_bytes = oi_p2::kNofDataRe * mcs->qm;
  std::vector<int8_t> llr_host(total_llr_bytes);
  clEnqueueReadBuffer(p->queue, bufs.llr, CL_TRUE, 0, total_llr_bytes, llr_host.data(), 0, nullptr, nullptr);

  std::vector<std::vector<uint8_t>> decoded_cbs(seg.nof_segments);
  std::vector<const uint8_t*> decoded_ptrs(seg.nof_segments);
  uint32_t cw_offset = 0;
  bool ldpc_ok = true;
  for (uint32_t cb = 0; cb < seg.nof_segments; cb++) {
    uint32_t rm_length = oi_p2_compute_rm_length(oi_p2::kNofDataRe, seg.nof_segments, mcs->qm, cb);

    // K6: rate-dematch this CB (fresh small device buffers -- see comment above). K6's
    // `full_length` argument is the CODEWORD length N = 66*Zc(BG1)/50*Zc(BG2) -- seg.codeword_length,
    // NOT seg.segment_length (which is K = 22*Zc/10*Zc, the LDPC decoder's own output length; a
    // real bug caught while writing this exact call, confusing the two -- see VERIFICATION.md).
    // K6's output (codeword_length-sized) is exactly oi_p2_ldpc_decode_cb's expected n_short*Z
    // input with no further extraction: codeword_length == n_short * lifting_size by construction.
    cl_int err = CL_SUCCESS;
    cl_mem in_buf = clCreateBuffer(p->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, rm_length,
                                   llr_host.data() + cw_offset, &err);
    std::vector<int8_t> cb_llr_zero(seg.codeword_length, 0);
    cl_mem out_buf = clCreateBuffer(p->ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, seg.codeword_length,
                                    cb_llr_zero.data(), &err);
    cl_uint rm_length_a = rm_length, full_length_a = seg.codeword_length, filler_a = seg.nof_filler_bits;
    cl_uint shift_k0_a = 0;  // rv=0 always (P2-R8)
    cl_uint qm_a = mcs->qm;
    clSetKernelArg(p->k6, 0, sizeof(cl_mem), &in_buf);
    clSetKernelArg(p->k6, 1, sizeof(cl_uint), &rm_length_a);
    clSetKernelArg(p->k6, 2, sizeof(cl_uint), &full_length_a);
    clSetKernelArg(p->k6, 3, sizeof(cl_uint), &filler_a);
    clSetKernelArg(p->k6, 4, sizeof(cl_uint), &shift_k0_a);
    clSetKernelArg(p->k6, 5, sizeof(cl_uint), &qm_a);
    clSetKernelArg(p->k6, 6, sizeof(cl_mem), &out_buf);
    size_t one = 1;
    clEnqueueNDRangeKernel(p->queue, p->k6, 1, nullptr, &one, nullptr, 0, nullptr, nullptr);
    std::vector<int8_t> cb_llr_host(seg.codeword_length);
    clEnqueueReadBuffer(p->queue, out_buf, CL_TRUE, 0, seg.codeword_length, cb_llr_host.data(), 0, nullptr, nullptr);
    clReleaseMemObject(in_buf);
    clReleaseMemObject(out_buf);

    // LDPC decode this CB. Output size = ceil(n_vn_info*Z / 8) bytes = ceil(segment_length / 8)
    // (segment_length == K == n_vn_info*Z exactly -- includes filler bits, which the LDPC decoder
    // does emit, packed alongside info+CRC bits; oi_p2_cb_desegment strips them via cb_info_bits).
    decoded_cbs[cb].resize((seg.segment_length + 7) / 8, 0);
    int rc = oi_p2_ldpc_decode_cb(&p->ldpc, cb_llr_host.data(), seg.base_graph, seg.lifting_size,
                                 /*n_iter=*/6, decoded_cbs[cb].data());
    if (rc != 0) ldpc_ok = false;
    decoded_ptrs[cb] = decoded_cbs[cb].data();

    cw_offset += rm_length;
  }

  uint8_t tb_bytes_buf[4096];  // see oi_p2_cb_segment.cpp's own scratch-sizing rationale (MVP bound)
  oi_p2_deseg_status dst = OI_P2_DESEG_ERR_TB_CRC;
  if (ldpc_ok) {
    dst = oi_p2_cb_desegment(&seg, mcs->tbs_bits, decoded_ptrs.data(), tb_bytes_buf);
  }

  static std::vector<uint8_t> tb_bytes_storage;  // owns the memory out_record->tb_data points to
  uint32_t tb_bytes = (mcs->tbs_bits + 7) / 8;
  tb_bytes_storage.assign(tb_bytes_buf, tb_bytes_buf + tb_bytes);

  out_record->schema = oi_p2::kTbRecordSchema;
  out_record->slot_id = slot_id;
  out_record->tb_size_bytes = tb_bytes;
  out_record->nof_cb = (uint8_t)seg.nof_segments;
  out_record->base_graph = (uint8_t)seg.base_graph;
  out_record->crc24a_ok = (ldpc_ok && dst == OI_P2_DESEG_OK) ? 1 : 0;
  out_record->mcs_index = (uint8_t)slot.mcs_index;
  out_record->tb_data = tb_bytes_storage.data();
  static const uint8_t kPlaceholderCrc[3] = {0, 0, 0};
  out_record->crc24a = kPlaceholderCrc;

  return OI_P2_OK;
}

oi_p2_status oi_p2_tap(oi_p2_pipeline* p, uint32_t slot_id, int stage_id, void* out_buf,
                       size_t out_buf_bytes) {
  auto& slot_bufs = p->buffers.slots[slot_id % 2];
  cl_mem src = nullptr;
  size_t src_bytes = 0;
  switch (stage_id) {
    case OI_P2_STAGE_I2_RE_GRID:
      src = slot_bufs.re_grid;
      src_bytes = oi_p2::kNofSymbols * oi_p2::kNofSubcarriers * sizeof(cl_float2);
      break;
    case OI_P2_STAGE_I3_CHAN_EST:
      src = slot_bufs.ch_est;
      src_bytes = oi_p2::kNofDataRe * sizeof(cl_float2);
      break;
    case OI_P2_STAGE_I4_EQ_OUT:
      src = slot_bufs.eq_symbols;
      src_bytes = oi_p2::kNofDataRe * sizeof(cl_float2);
      break;
    case OI_P2_STAGE_I5_LLR:
      src = slot_bufs.llr;
      src_bytes = oi_p2::kNofDataRe * oi_p2::kMaxQm * sizeof(cl_char);
      break;
    case OI_P2_STAGE_I6_CB_LLR:
      src = slot_bufs.cb_llr;
      src_bytes = (size_t)oi_p2::kMaxCbLlrLen * oi_p2::kMaxSegments * sizeof(cl_char);
      break;
    default:
      return OI_P2_ERR_DEVICE;
  }
  size_t n = std::min(out_buf_bytes, src_bytes);
  cl_int err = clEnqueueReadBuffer(p->queue, src, CL_TRUE, 0, n, out_buf, 0, nullptr, nullptr);
  return (err == CL_SUCCESS) ? OI_P2_OK : OI_P2_ERR_DEVICE;
}

void oi_p2_teardown(oi_p2_pipeline* p) {
  if (!p) return;
  for (auto& kv : p->slots) {
    if (kv.second.final_event) clReleaseEvent(kv.second.final_event);
  }
  oi_p2_ldpc_decoder_destroy(&p->ldpc);
  for (auto* k : {&p->k1, &p->k2a, &p->k2b, &p->k3, &p->k4, &p->k5, &p->k6}) {
    if (*k) clReleaseKernel(*k);
  }
  for (auto* pr : {&p->prog_k1, &p->prog_k2a, &p->prog_k2b, &p->prog_k3, &p->prog_k4, &p->prog_k5, &p->prog_k6}) {
    if (*pr) clReleaseProgram(*pr);
  }
  for (auto& rg : p->re_grid_compact) {
    if (rg) clReleaseMemObject(rg);
  }
  oi_p2::oi_p2_buffers_destroy(&p->buffers);
  delete p;
}

}  // extern "C"
