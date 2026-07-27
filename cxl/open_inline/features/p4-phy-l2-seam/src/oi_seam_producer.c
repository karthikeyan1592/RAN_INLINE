/* oi_seam_producer.c — see oi_seam_producer.h. */
#include "oi_seam_producer.h"

#include <string.h>

int oi_seam_producer_fill_slot(oi_seam_slot_t* out, const oi_seam_p2_record_view_t* rec,
                               const oi_seam_producer_config_t* cfg, uint64_t t_enqueue_ns) {
  if (rec->tb_size_bytes > OI_SEAM_TB_MAX_BYTES) {
    return -1; /* P4-R14: refuse to publish, never truncate */
  }
  /* sfn = slot_id / slots_per_frame, slot = slot_id % slots_per_frame -- P4-R12's exact derivation
   * from p2's own slot_id + the pinned numerology constant, not read off p2's record (it has no
   * sfn/slot fields). */
  out->sfn = rec->slot_id / cfg->slots_per_frame;
  out->slot = (uint16_t)(rec->slot_id % cfg->slots_per_frame);
  out->rnti = cfg->rnti;       /* pinned config constant, not per-record (P4-R12) */
  out->harq_id = cfg->harq_id; /* pinned config constant, not per-record (P4-R12) */
  out->crc_ok = rec->crc24a_ok ? 1u : 0u;
  out->tb_len = rec->tb_size_bytes;
  out->t_enqueue_ns = t_enqueue_ns; /* observational only, never a gate operand (P4-R13) */
  memset(out->tb, 0, sizeof(out->tb));
  if (rec->tb_size_bytes > 0) {
    memcpy(out->tb, rec->tb_data, rec->tb_size_bytes);
  }
  return 0;
}
