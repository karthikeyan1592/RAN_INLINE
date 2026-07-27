/* oi_seam_producer.h — P4-R12's field-mapping producer helper: p2's opaque oi_p2_tb_record_c ->
 * this feature's own oi_seam_slot_t, 1:1, no renaming/reinterpretation of p2's real fields, and
 * no new fields invented beyond what P4-R2/R12 explicitly call for.
 *
 * P4-R12 note (verbatim from SPEC.md, since this is the crux of this file): p2's real record
 * (p2-phy-kernels LLD §4.7, `oi_p2_tb_record_c`: slot_id, tb_size_bytes, nof_cb, base_graph,
 * crc24a_ok, mcs_index, tb_data, crc24a) carries NO sfn/rnti/harq_id -- the MVP pins RNTI and
 * single-shot HARQ as config constants for the whole run, and sfn is derived from slot_id via
 * the pinned slots_per_frame, not read off the record. This file does exactly that derivation,
 * nothing more -- it never invents a field p2's record doesn't have.
 */
#ifndef OI_SEAM_PRODUCER_H
#define OI_SEAM_PRODUCER_H

#include <stdint.h>

#include "oi_seam_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MVP-pinned constants this producer derives (sfn, slot, rnti, harq_id) from -- matches the same
 * pinned values p2f-integration/tools/oracle_tx_gen.cpp and p3-live-tap-ul-inject/tools/osg_gen.cpp
 * already use (rnti=0x4601), and p3's own ORACLE_INJECTION_SLOTS_PER_FRAME=20 (mu=1, 30kHz SCS). */
typedef struct {
  uint16_t rnti;            /* pinned C-RNTI, e.g. 0x4601 */
  uint8_t harq_id;           /* pinned single-shot HARQ process id, e.g. 0 */
  uint32_t slots_per_frame;  /* pinned numerology-derived constant, e.g. 20 */
} oi_seam_producer_config_t;

/* Minimal, opaque-respecting view of p2's real oi_p2_tb_record_c fields this producer needs --
 * declared here (not by #include-ing p2a-scaffold/src/host/oi_p2_host.h, to keep this feature
 * from creating a hard header dependency on p2's exact struct definition; the field NAMES/TYPES
 * below are copied verbatim from p2-phy-kernels LLD §4.7 / oi_p2_host.h, not invented) --
 * callers construct this from a real oi_p2_tb_record_c field-by-field (P4-R12's own "1:1 field
 * mapping, reviewed against p2's LLD" requirement is satisfied by this struct's fields matching
 * p2's exactly, one name for one name).
 */
typedef struct {
  uint32_t slot_id;
  uint32_t tb_size_bytes;
  uint8_t crc24a_ok;
  const uint8_t* tb_data; /* tb_size_bytes bytes */
} oi_seam_p2_record_view_t;

/* Populates every field of *out except status (left OI_SEAM_RESERVED by the caller's own
 * oi_seam_reserve, per the API's own contract -- this function never touches status, matching
 * "producer writes every OTHER field, then publishes" from oi_seam.h's own doc comment).
 * Returns 0 on success, -1 if tb_size_bytes > OI_SEAM_TB_MAX_BYTES (P4-R14/error table: producer
 * refuses to publish rather than truncate).
 */
int oi_seam_producer_fill_slot(oi_seam_slot_t* out, const oi_seam_p2_record_view_t* rec,
                               const oi_seam_producer_config_t* cfg, uint64_t t_enqueue_ns);

#ifdef __cplusplus
}
#endif

#endif /* OI_SEAM_PRODUCER_H */
