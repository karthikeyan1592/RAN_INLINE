/* oi_l2_validate.c — see oi_l2_validate.h. */
#include "oi_l2_validate.h"

#include <string.h>

void oi_l2_validator_init(oi_l2_validator_t* v) {
  memset(v, 0, sizeof(*v));
}

static oi_l2_key_state_t* find_or_alloc_key(oi_l2_validator_t* v, uint16_t rnti, uint8_t harq_id) {
  int free_idx = -1;
  for (int i = 0; i < OI_L2_MAX_TRACKED_KEYS; i++) {
    if (v->keys[i].in_use && v->keys[i].rnti == rnti && v->keys[i].harq_id == harq_id) {
      return &v->keys[i];
    }
    if (!v->keys[i].in_use && free_idx < 0) free_idx = i;
  }
  if (free_idx < 0) return NULL; /* exhausted OI_L2_MAX_TRACKED_KEYS -- not expected at MVP/test scale */
  v->keys[free_idx].in_use = 1;
  v->keys[free_idx].rnti = rnti;
  v->keys[free_idx].harq_id = harq_id;
  v->keys[free_idx].has_last = 0;
  return &v->keys[free_idx];
}

void oi_l2_validator_process(oi_l2_validator_t* v, const oi_seam_slot_t* slot) {
  /* P4-R8: epoch change -> discard ALL per-key state, do not treat this record as an order
   * violation (it's the first record of a fresh stream from the consumer's point of view). */
  if (!v->have_epoch || slot->epoch != v->last_epoch) {
    memset(v->keys, 0, sizeof(v->keys));
    v->last_epoch = slot->epoch;
    v->have_epoch = 1;
    v->nof_epoch_resets++;
  }

  oi_l2_key_state_t* key = find_or_alloc_key(v, slot->rnti, slot->harq_id);
  if (key) {
    if (key->has_last) {
      /* Monotonic (sfn,slot) as a single comparable value: sfn is the more-significant field. */
      int regressed = (slot->sfn < key->last_sfn) || (slot->sfn == key->last_sfn && slot->slot <= key->last_slot);
      if (regressed) v->nof_order_violations++;
    }
    key->last_sfn = slot->sfn;
    key->last_slot = slot->slot;
    key->has_last = 1;
  }

  if (slot->crc_ok) {
    v->nof_crc_ok++;
  } else {
    v->nof_crc_fail++;
  }
  v->nof_processed++;
}
