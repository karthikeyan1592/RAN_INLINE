/* ordering_test.c — P4-G1 (P4-R6): real ring (real mmap'd file, real reserve/publish/
 * wait_status/release), synthetic producer feeding 2 (rnti,harq_id) keys with deliberately
 * out-of-order CROSS-KEY completion (key B's slot arrives before key A's earlier slot) --
 * per-key (sfn,slot) monotonicity must still hold (0 violations), since P4-R6 only requires
 * monotonicity WITHIN a key, not a single global order across different keys. Also verifies the
 * negative case: a genuine WITHIN-key regression is caught (>=1 violation).
 *
 * Honesty note (SPEC.md's own "out-of-order-completion gate is synthetic"): this harness
 * fabricates the interleaving itself; the real MVP pipeline never organically produces it
 * (single in-flight slot). That's stated here, not hidden.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/oi_l2_validate.h"
#include "../src/oi_seam.h"

static int g_fail = 0;
static void check(int cond, const char* what) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", what);
    g_fail++;
  } else {
    printf("PASS: %s\n", what);
  }
}

static void fill_and_publish(oi_seam_ring_t* r, uint16_t rnti, uint8_t harq_id, uint32_t sfn, uint16_t slot_no,
                             uint8_t crc_ok) {
  uint64_t seq;
  oi_seam_slot_t* s = oi_seam_reserve(r, &seq);
  s->sfn = sfn;
  s->slot = slot_no;
  s->rnti = rnti;
  s->harq_id = harq_id;
  s->crc_ok = crc_ok;
  s->tb_len = 0;
  s->t_enqueue_ns = 0;
  oi_seam_publish(s, OI_SEAM_READY);
}

int main(void) {
  const char* path = "/tmp/oi_p4_ordering_test.ring";
  unlink(path);

  oi_seam_config_t cfg = {.ring_path = path, .ring_capacity = 16, .tb_max_bytes = OI_SEAM_TB_MAX_BYTES,
                         .format_version = OI_SEAM_FORMAT_VERSION, .consumer_state_path = NULL};
  oi_seam_status_code_t st;
  oi_seam_ring_t* r = oi_seam_open(&cfg, /*create=*/1, &st);
  check(r != NULL, "real ring created for the ordering test");

  const uint16_t RNTI_A = 0x4601, RNTI_B = 0x4602;

  /* Deliberately scrambled cross-key completion order: B/slot20, A/slot5, B/slot21, A/slot6,
   * A/slot7, B/slot22 -- each KEY's own sub-sequence is still increasing. */
  fill_and_publish(r, RNTI_B, 0, 0, 20, 1);
  fill_and_publish(r, RNTI_A, 0, 0, 5, 1);
  fill_and_publish(r, RNTI_B, 0, 0, 21, 1);
  fill_and_publish(r, RNTI_A, 0, 0, 6, 1);
  fill_and_publish(r, RNTI_A, 0, 0, 7, 1);
  fill_and_publish(r, RNTI_B, 0, 0, 22, 1);

  oi_l2_validator_t v;
  oi_l2_validator_init(&v);
  for (uint64_t idx = 0; idx < 6; idx++) {
    oi_seam_slot_t* s = oi_seam_wait_status(r, idx, OI_SEAM_READY);
    oi_l2_validator_process(&v, s);
    oi_seam_release(r, idx);
  }
  check(v.nof_processed == 6, "all 6 synthetic records processed");
  check(v.nof_order_violations == 0,
        "P4-R6: 0 order violations despite cross-key interleaving (B's completion order never affects A's own monotonicity)");

  /* Negative case: now inject a genuine WITHIN-key regression for RNTI_A (goes backwards: slot 3
   * after slot 7) and confirm the validator actually catches it -- proves the check is real, not
   * vacuously always passing. */
  fill_and_publish(r, RNTI_A, 0, 0, 3, 1); /* regression: A was last at slot 7 */
  oi_seam_slot_t* s = oi_seam_wait_status(r, 6, OI_SEAM_READY);
  oi_l2_validator_process(&v, s);
  oi_seam_release(r, 6);
  check(v.nof_order_violations == 1, "genuine within-key regression IS detected (negative-case proof)");

  oi_seam_close(r);
  unlink(path);

  if (g_fail == 0) {
    printf("\nordering_test: ALL PASS\n");
  } else {
    fprintf(stderr, "\nordering_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
