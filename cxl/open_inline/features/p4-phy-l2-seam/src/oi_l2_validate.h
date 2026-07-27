/* oi_l2_validate.h — the L2 stub's (M11) validation core: per-key (rnti,harq_id) ordering
 * (P4-R6) + CRC verdict counting, factored out of the consumer's main loop so both
 * `l2_stub_main.c` and this feature's unit tests (P4-G1/G2/G3) can exercise it directly without
 * spawning a real process pair. Does NOT implement SCF FAPI / packed-FAPI encoding (P4-R11,
 * out of scope) -- grep this file and l2_stub_main.c for any FAPI symbol: none exist.
 */
#ifndef OI_L2_VALIDATE_H
#define OI_L2_VALIDATE_H

#include <stdint.h>

#include "oi_seam_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OI_L2_MAX_TRACKED_KEYS 64 /* generous bound for synthetic multi-key tests; MVP uses 1 */

typedef struct {
  uint16_t rnti;
  uint8_t harq_id;
  uint8_t in_use;
  uint32_t last_sfn;
  uint32_t last_slot;
  int has_last;
} oi_l2_key_state_t;

typedef struct {
  oi_l2_key_state_t keys[OI_L2_MAX_TRACKED_KEYS];
  uint32_t last_epoch;
  int have_epoch;

  uint64_t nof_processed;
  uint64_t nof_crc_ok;
  uint64_t nof_crc_fail;
  uint64_t nof_order_violations;
  uint64_t nof_epoch_resets;
} oi_l2_validator_t;

void oi_l2_validator_init(oi_l2_validator_t* v);

/* Call once per slot delivered to the consumer, AFTER oi_seam_wait_status(READY) and BEFORE
 * oi_seam_release. Detects an epoch change first (P4-R8: discards ALL per-key state, does not
 * count the transitioning record itself as an order violation) then checks per-key (sfn,slot)
 * monotonicity (P4-R6) and tallies the CRC verdict. Never mutates the slot itself (read-only). */
void oi_l2_validator_process(oi_l2_validator_t* v, const oi_seam_slot_t* slot);

#ifdef __cplusplus
}
#endif

#endif /* OI_L2_VALIDATE_H */
