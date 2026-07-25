#include "oi_p2_buffers.h"

#include <cstring>

namespace oi_p2 {

namespace {

// Allocates one cl_mem, tracking failure so the caller can unwind cleanly.
bool alloc(cl_context ctx, size_t bytes, cl_mem_flags flags, cl_mem* out, bool* ok) {
  if (!*ok) return false;  // short-circuit once anything has failed
  cl_int err = CL_SUCCESS;
  *out = clCreateBuffer(ctx, flags, bytes, nullptr, &err);
  if (err != CL_SUCCESS || *out == nullptr) {
    *ok = false;
    return false;
  }
  return true;
}

void free_slot(SlotBufferSet* s) {
  if (s->re_grid) clReleaseMemObject(s->re_grid);
  if (s->symbol_bitmap) clReleaseMemObject(s->symbol_bitmap);
  for (auto& k2a : s->k2a) {
    if (k2a.ref_seq) clReleaseMemObject(k2a.ref_seq);
    if (k2a.fd_est) clReleaseMemObject(k2a.fd_est);
    if (k2a.filtered_pilot) clReleaseMemObject(k2a.filtered_pilot);
    if (k2a.epre_partial) clReleaseMemObject(k2a.epre_partial);
  }
  if (s->ch_est) clReleaseMemObject(s->ch_est);
  if (s->noise_var) clReleaseMemObject(s->noise_var);
  if (s->epre) clReleaseMemObject(s->epre);
  if (s->eq_symbols) clReleaseMemObject(s->eq_symbols);
  if (s->eq_noise_var) clReleaseMemObject(s->eq_noise_var);
  if (s->llr) clReleaseMemObject(s->llr);
  if (s->cb_llr) clReleaseMemObject(s->cb_llr);
  *s = SlotBufferSet{};
}

}  // namespace

bool oi_p2_buffers_create(cl_context ctx, size_t arena_bytes, size_t desc_ring_capacity,
                           BufferPool* out_pool) {
  *out_pool = BufferPool{};
  out_pool->ctx = ctx;
  bool ok = true;

  const size_t re_grid_bytes = kNofSymbols * kNofSubcarriers * sizeof(cl_float2);
  const size_t ch_est_bytes = kNofDataRe * sizeof(cl_float2);
  const size_t eq_noise_var_bytes = kNofDataRe * sizeof(cl_float);
  const size_t llr_bytes = kNofDataRe * kMaxQm * sizeof(cl_char);
  const size_t cb_llr_bytes = (size_t)kMaxCbLlrLen * kMaxSegments * sizeof(cl_char);
  const size_t ref_seq_bytes = kNofPilots * sizeof(cl_float2);
  const size_t fd_est_bytes = kNofSubcarriers * sizeof(cl_float2);
  const size_t filtered_pilot_bytes = kNofPilots * sizeof(cl_float2);

  for (auto& slot : out_pool->slots) {
    alloc(ctx, re_grid_bytes, CL_MEM_READ_WRITE, &slot.re_grid, &ok);
    alloc(ctx, sizeof(cl_uint), CL_MEM_READ_WRITE, &slot.symbol_bitmap, &ok);
    for (auto& k2a : slot.k2a) {
      alloc(ctx, ref_seq_bytes, CL_MEM_READ_WRITE, &k2a.ref_seq, &ok);
      alloc(ctx, fd_est_bytes, CL_MEM_READ_WRITE, &k2a.fd_est, &ok);
      alloc(ctx, filtered_pilot_bytes, CL_MEM_READ_WRITE, &k2a.filtered_pilot, &ok);
      alloc(ctx, sizeof(cl_float), CL_MEM_READ_WRITE, &k2a.epre_partial, &ok);
    }
    alloc(ctx, ch_est_bytes, CL_MEM_READ_WRITE, &slot.ch_est, &ok);
    alloc(ctx, sizeof(cl_float), CL_MEM_READ_WRITE, &slot.noise_var, &ok);
    alloc(ctx, sizeof(cl_float), CL_MEM_READ_WRITE, &slot.epre, &ok);
    alloc(ctx, ch_est_bytes, CL_MEM_READ_WRITE, &slot.eq_symbols, &ok);  // same shape as ch_est
    alloc(ctx, eq_noise_var_bytes, CL_MEM_READ_WRITE, &slot.eq_noise_var, &ok);
    alloc(ctx, llr_bytes, CL_MEM_READ_WRITE, &slot.llr, &ok);
    alloc(ctx, cb_llr_bytes, CL_MEM_READ_WRITE, &slot.cb_llr, &ok);
  }

  alloc(ctx, arena_bytes, CL_MEM_READ_ONLY, &out_pool->arena, &ok);
  out_pool->arena_bytes = arena_bytes;
  // oi_frame_desc is 32 bytes (LLD §4.1); descriptor ring is a flat byte buffer of that many slots.
  alloc(ctx, desc_ring_capacity * 32, CL_MEM_READ_ONLY, &out_pool->desc_ring, &ok);
  out_pool->desc_ring_capacity = desc_ring_capacity;

  if (!ok) {
    oi_p2_buffers_destroy(out_pool);
    return false;
  }
  return true;
}

void oi_p2_buffers_destroy(BufferPool* pool) {
  for (auto& slot : pool->slots) free_slot(&slot);
  if (pool->arena) clReleaseMemObject(pool->arena);
  if (pool->desc_ring) clReleaseMemObject(pool->desc_ring);
  *pool = BufferPool{};
}

}  // namespace oi_p2
