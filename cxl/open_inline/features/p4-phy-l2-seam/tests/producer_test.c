/* producer_test.c — P4-R12: the field-mapping producer helper against synthetic p2 record views.
 * Verifies the 1:1 field mapping (no renaming/reinterpretation) and the sfn/slot derivation from
 * slot_id + the pinned slots_per_frame, plus the tb_size_bytes > OI_SEAM_TB_MAX_BYTES refusal
 * (P4-R14).
 */
#include <stdio.h>
#include <string.h>

#include "../src/oi_seam_producer.h"

static int g_fail = 0;
static void check(int cond, const char* what) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", what);
    g_fail++;
  } else {
    printf("PASS: %s\n", what);
  }
}

int main(void) {
  oi_seam_producer_config_t cfg = {.rnti = 0x4601, .harq_id = 0, .slots_per_frame = 20};

  /* slot_id = 103 -> sfn = 103/20 = 5, slot = 103%20 = 3 */
  uint8_t tb_bytes[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
  oi_seam_p2_record_view_t rec = {.slot_id = 103, .tb_size_bytes = 5, .crc24a_ok = 1, .tb_data = tb_bytes};

  oi_seam_slot_t slot;
  memset(&slot, 0xFF, sizeof(slot)); /* poison, so any un-set field is obviously wrong */
  int rc = oi_seam_producer_fill_slot(&slot, &rec, &cfg, 123456789ull);
  check(rc == 0, "oi_seam_producer_fill_slot succeeds for a normal record");
  check(slot.sfn == 5, "P4-R12: sfn == slot_id / slots_per_frame (103/20=5)");
  check(slot.slot == 3, "P4-R12: slot == slot_id %% slots_per_frame (103%%20=3)");
  check(slot.rnti == 0x4601, "P4-R12: rnti comes from the pinned config, not the record");
  check(slot.harq_id == 0, "P4-R12: harq_id comes from the pinned config, not the record");
  check(slot.crc_ok == 1, "crc_ok maps 1:1 from crc24a_ok");
  check(slot.tb_len == 5, "tb_len maps 1:1 from tb_size_bytes");
  check(memcmp(slot.tb, tb_bytes, 5) == 0, "tb[] bytes copied byte-for-byte from tb_data");
  check(slot.t_enqueue_ns == 123456789ull, "t_enqueue_ns set to the given timestamp (observational only, P4-R13)");
  int all_zero_tail = 1;
  for (size_t i = 5; i < sizeof(slot.tb); i++) {
    if (slot.tb[i] != 0) all_zero_tail = 0;
  }
  check(all_zero_tail, "tb[] tail beyond tb_len is zero-padded, not left as poison/garbage");

  /* crc_ok = 0 case */
  oi_seam_p2_record_view_t rec_fail = rec;
  rec_fail.crc24a_ok = 0;
  oi_seam_slot_t slot2;
  oi_seam_producer_fill_slot(&slot2, &rec_fail, &cfg, 0);
  check(slot2.crc_ok == 0, "crc_ok correctly maps a real CRC failure verdict, not just the pass case");

  /* P4-R14: tb_size_bytes > OI_SEAM_TB_MAX_BYTES must refuse, not truncate */
  oi_seam_p2_record_view_t rec_too_big = rec;
  rec_too_big.tb_size_bytes = OI_SEAM_TB_MAX_BYTES + 1;
  oi_seam_slot_t slot3;
  int rc2 = oi_seam_producer_fill_slot(&slot3, &rec_too_big, &cfg, 0);
  check(rc2 == -1, "P4-R14: tb_size_bytes > OI_SEAM_TB_MAX_BYTES is refused (-1), never silently truncated");

  /* Boundary: exactly OI_SEAM_TB_MAX_BYTES must succeed (off-by-one check on the refusal) */
  static uint8_t max_tb[OI_SEAM_TB_MAX_BYTES];
  oi_seam_p2_record_view_t rec_exact = {.slot_id = 0, .tb_size_bytes = OI_SEAM_TB_MAX_BYTES, .crc24a_ok = 1,
                                        .tb_data = max_tb};
  oi_seam_slot_t slot4;
  int rc3 = oi_seam_producer_fill_slot(&slot4, &rec_exact, &cfg, 0);
  check(rc3 == 0, "exactly OI_SEAM_TB_MAX_BYTES succeeds (boundary, not off-by-one rejected)");

  if (g_fail == 0) {
    printf("\nproducer_test: ALL PASS\n");
  } else {
    fprintf(stderr, "\nproducer_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
