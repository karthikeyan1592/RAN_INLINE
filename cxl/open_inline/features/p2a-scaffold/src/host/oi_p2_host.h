/* oi_p2_host.h — setup/feed/drain API (P2-R17), stable per p2-phy-kernels LLD §2.
 *
 * C-linkage (OpenCL-host-callable from C or C++; the SYCL variant wraps the same shape, HLD §6
 * "two thin enqueue adapters"). All buffers referenced by handle, not raw pointer, so the
 * SIM/PHYSICAL ingest_backend/handoff_backend swap (I1, I7) is invisible here.
 *
 * p2a-scaffold implementation note: this slice builds the orchestration (queue/event chain, arena/
 * descriptor-ring plumbing) against a placeholder kernel (_p2a_stub_stage.cl) standing in for
 * K1..K6+LDPC. Real kernels replace the stub stage-by-stage as p2b..p2f land; this header's
 * signatures do not change when that happens (that's the whole point of P2-R17).
 */
#ifndef OI_P2_HOST_H
#define OI_P2_HOST_H

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <stddef.h>
#include <stdint.h>

#include "oi_frame_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oi_p2_pipeline oi_p2_pipeline;  // opaque

typedef enum {
  OI_P2_OK = 0,
  OI_P2_ERR_CONFIG_REJECTED = 1,  // P2-R11: config != MVP
  OI_P2_ERR_CL_BUILD_FAILED = 2,
  OI_P2_ERR_ARENA_OVERFLOW = 3,   // I1 descriptor ring full
  OI_P2_ERR_SLOT_INCOMPLETE = 4,  // slot never launched / not yet drained
  OI_P2_ERR_ORACLE_MISMATCH = 5,  // test/CI builds only
  OI_P2_ERR_DEVICE = 6,           // clGetDeviceInfo / event error propagated from ICD
} oi_p2_status;

/// Debug scaffolding stage IDs used by p2a's stub kernel chain (oi_p2_tap's stage_id argument).
/// Real kernels (p2b..p2f) use the same numeric slots once they replace the stub; the *meaning*
/// of "stage 2" doesn't change (it's always "whatever produces I3"), only its implementation.
enum {
  OI_P2_STAGE_I2_RE_GRID = 2,
  OI_P2_STAGE_I3_CHAN_EST = 3,
  OI_P2_STAGE_I4_EQ_OUT = 4,
  OI_P2_STAGE_I5_LLR = 5,
  OI_P2_STAGE_I6_CB_LLR = 6,
  OI_P2_STAGE_I7_DECODED_BITS = 7,
};

/// C view of a TB record (I8, LLD §4.7 fixed header; tb_data/crc24a are variable-length tails
/// read via pointers valid until the next oi_p2_drain call on this pipeline).
typedef struct {
  uint32_t schema;
  uint32_t slot_id;
  uint32_t tb_size_bytes;
  uint8_t nof_cb;
  uint8_t base_graph;
  uint8_t crc24a_ok;
  uint8_t mcs_index;
  const uint8_t* tb_data;   // header.tb_size_bytes bytes
  const uint8_t* crc24a;    // 3 bytes
} oi_p2_tb_record_c;

/// Parses+validates a YAML config against the fixed MVP shape and, on success, builds the kernel
/// program(s) + allocates all pipeline-lifetime buffers (HLD §5: zero device allocation after
/// this call). Returns a ready, empty pipeline.
oi_p2_status oi_p2_setup(const char* yaml_path, cl_context ctx, cl_command_queue q,
                          oi_p2_pipeline** out_pipeline);

/// Appends one already-parsed frame descriptor to the packet-arena ring (I1). The frame's raw
/// bytes must already be in the arena at desc->arena_offset (written by the ingest_backend); desc
/// must be fully populated by the CALLER -- feed() does not parse header bytes and does not
/// derive slot_id (reconciled 2026-07-22: parsing is the ingest_backend's job, via the shared
/// oi_oran_preparse() helper, because PHYSICAL's dmabuf path cannot cheaply parse from inside the
/// pipeline, and because slot_id derivation requires the symbol-wrap state that only a stateful
/// per-stream parser can maintain -- feed() has no way to derive it honestly). Non-blocking;
/// returns after the descriptor is appended to the ring. desc is copied; the caller may reuse or
/// free it immediately after this call returns.
oi_p2_status oi_p2_feed(oi_p2_pipeline* p, const oi_frame_desc* desc);

/// Writes raw frame bytes into the pipeline's packet arena at the given offset (2026-07-23
/// addition, p2f-integration -- see VERIFICATION.md "arena write gap"). Fills a real hole in the
/// original API surface: oi_p2_feed's own doc comment requires frame bytes to already be in the
/// arena at desc->arena_offset before feed() is called, but until now nothing in this header let
/// a caller (ingest_backend, or a test harness standing in for one) actually place them there --
/// the arena cl_mem is private to oi_p2_pipeline. Purely additive (new function, no existing
/// signature touched), same P2-R17 reasoning already applied to the mcs_index addition: nothing
/// outside this project's own tests calls this API yet, so this is the cheapest point to close
/// the gap. `len` must not exceed the pipeline's fixed arena_bytes budget (oi_p2_setup); offset+len
/// must not overflow it either. Returns OI_P2_ERR_ARENA_OVERFLOW if it would.
oi_p2_status oi_p2_write_arena(oi_p2_pipeline* p, uint64_t offset, const void* data, size_t len);

/// Signals slot slot_id is complete and enqueues the kernel chain for that slot on the
/// pipeline's in-order queue. Non-blocking; the resulting cl_event is stored internally and
/// surfaced via oi_p2_drain.
///
/// `mcs_index` (2026-07-23 addition, p2f-integration -- see VERIFICATION.md): must be one of the
/// MVP's fixed set {4, 13, 21} (P2-R11; oi_p2_config's mcs_set validates this same set at setup,
/// but that only bounds what the *pipeline* accepts across its lifetime -- MCS is a real,
/// per-transmission MAC-scheduler decision, not a property derivable from PHY wire bytes alone or
/// fixed once for the whole pipeline). K4's Qm and K6/LDPC's base_graph/lifting_size/rm_length are
/// all derived from this one value. Returns OI_P2_ERR_CONFIG_REJECTED if mcs_index isn't in the
/// configured mcs_set.
oi_p2_status oi_p2_launch_slot(oi_p2_pipeline* p, uint32_t slot_id, uint32_t mcs_index);

/// Blocks until slot slot_id's host CRC tail has run and writes the TB+CRC record into
/// out_record. Returns OI_P2_ERR_SLOT_INCOMPLETE if the slot was never launched.
oi_p2_status oi_p2_drain(oi_p2_pipeline* p, uint32_t slot_id, oi_p2_tb_record_c* out_record);

/// Optional debug tap (I10): reads back any of I2-I6 for the given slot into a caller buffer.
oi_p2_status oi_p2_tap(oi_p2_pipeline* p, uint32_t slot_id, int stage_id, void* out_buf,
                       size_t out_buf_bytes);

void oi_p2_teardown(oi_p2_pipeline* p);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_P2_HOST_H */
