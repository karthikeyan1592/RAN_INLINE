/* restart_test.c — P4-G3: (a) producer restart -> epoch bump + consumer resync (P4-R8); (b)
 * consumer restart -> reattach at persisted tail, no duplicate delivery, no loss (P4-R9). Real
 * ring file (persists across "restart" the same way it would across a real container restart --
 * this test just re-opens/re-attaches within the same process, which exercises the identical
 * code path oi_seam_open() takes for a genuine restart).
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

static void publish_one(oi_seam_ring_t* r, uint16_t slot_no) {
  uint64_t seq;
  oi_seam_slot_t* s = oi_seam_reserve(r, &seq);
  s->sfn = 0;
  s->slot = slot_no;
  s->rnti = 0x4601;
  s->harq_id = 0;
  s->crc_ok = 1;
  s->tb_len = 0;
  oi_seam_publish(s, OI_SEAM_READY);
}

int main(void) {
  const char* ring_path = "/tmp/oi_p4_restart_test.ring";
  const char* state_path = "/tmp/oi_p4_restart_test.state";
  unlink(ring_path);
  unlink(state_path);

  oi_seam_config_t producer_cfg = {.ring_path = ring_path, .ring_capacity = 8, .tb_max_bytes = OI_SEAM_TB_MAX_BYTES,
                                  .format_version = OI_SEAM_FORMAT_VERSION, .consumer_state_path = NULL};
  oi_seam_status_code_t st;

  /* --- (a) Producer restart: epoch bump + consumer resync --- */
  oi_seam_ring_t* p1 = oi_seam_open(&producer_cfg, /*create=*/1, &st);
  check(p1 != NULL, "first producer open (fresh ring) succeeds");
  check(oi_seam_epoch(p1) == 0, "fresh ring starts at epoch 0");
  publish_one(p1, 0);
  publish_one(p1, 1);
  oi_seam_close(p1); /* simulates a clean stop; the restart path below is identical either way */

  oi_seam_ring_t* p2 = oi_seam_open(&producer_cfg, /*create=*/1, &st); /* producer "restarts" */
  check(p2 != NULL, "producer re-open (restart) against the existing ring succeeds");
  check(oi_seam_epoch(p2) == 1, "P4-R8: epoch bumped from 0 to 1 on producer restart");
  check(oi_seam_head(p2) == 0 && oi_seam_tail(p2) == 0, "P4-R8: head/tail reset to 0 on producer restart");
  publish_one(p2, 0); /* new epoch's own slot 0 -- unrelated to the old epoch's slot 0/1 */

  /* Consumer observing across the restart must see the epoch change and reset its own state --
   * exercised directly via oi_l2_validate (same logic l2_stub_main.c uses). */
  oi_l2_validator_t v;
  oi_l2_validator_init(&v);
  oi_seam_config_t consumer_cfg = producer_cfg;
  consumer_cfg.consumer_state_path = NULL;
  oi_seam_ring_t* c1 = oi_seam_open(&consumer_cfg, /*create=*/0, &st);
  check(c1 != NULL, "consumer attaches to the post-restart ring");
  oi_seam_slot_t* s0 = oi_seam_wait_status(c1, 0, OI_SEAM_READY);
  oi_l2_validator_process(&v, s0); /* this is epoch 1's slot 0 -- first record this validator ever saw */
  check(v.nof_epoch_resets == 1, "P4-R8: consumer's validator detects the epoch and resets (nof_epoch_resets==1)");
  oi_seam_release(c1, 0);
  oi_seam_close(c1);
  oi_seam_close(p2);
  unlink(ring_path);

  /* --- (b) Consumer restart: reattach at persisted tail, no dup, no loss --- */
  unlink(ring_path);
  unlink(state_path);
  oi_seam_ring_t* prod = oi_seam_open(&producer_cfg, /*create=*/1, &st);
  check(prod != NULL, "fresh ring for the consumer-restart scenario");
  for (int i = 0; i < 5; i++) publish_one(prod, (uint16_t)i);

  oi_seam_config_t cons_cfg = producer_cfg;
  cons_cfg.consumer_state_path = state_path;

  /* First consumer "life": processes slots 0,1,2 then "crashes" (just stops, no clean release of
   * anything further -- oi_seam_close doesn't erase the persisted state file). */
  oi_seam_ring_t* cons1 = oi_seam_open(&cons_cfg, /*create=*/0, &st);
  check(cons1 != NULL, "first consumer life attaches");
  for (uint64_t idx = 0; idx < 3; idx++) {
    oi_seam_wait_status(cons1, idx, OI_SEAM_READY);
    oi_seam_release(cons1, idx);
  }
  check(oi_seam_tail(cons1) == 3, "first consumer life released exactly 3 slots (tail==3)");
  oi_seam_close(cons1);

  uint32_t persisted_epoch;
  uint64_t persisted_tail;
  int had = oi_seam_read_consumer_state(state_path, &persisted_epoch, &persisted_tail);
  check(had == 1, "consumer state file was persisted after release() calls");
  check(persisted_tail == 3, "persisted tail == 3, matching what was actually released");

  /* Second consumer "life": re-attaches, resumes from the ring's own current tail (==3, matching
   * the persisted value exactly since the producer never restarted in this scenario) -- must
   * process slots 3 and 4 exactly once each, no re-delivery of 0/1/2, no skip. */
  oi_l2_validator_t v2;
  oi_l2_validator_init(&v2);
  oi_seam_ring_t* cons2 = oi_seam_open(&cons_cfg, /*create=*/0, &st);
  check(cons2 != NULL, "second consumer life (restart) re-attaches to the same ring");
  uint64_t resume_from = oi_seam_tail(cons2);
  check(resume_from == 3, "P4-R9: second consumer life resumes exactly at the persisted tail (3), not 0 (dup) or later (loss)");
  int delivered_slots[8] = {0};
  for (uint64_t idx = resume_from; idx < 5; idx++) {
    oi_seam_slot_t* s = oi_seam_wait_status(cons2, idx, OI_SEAM_READY);
    oi_l2_validator_process(&v2, s);
    delivered_slots[s->slot]++;
    oi_seam_release(cons2, idx);
  }
  check(v2.nof_processed == 2, "second consumer life processes exactly the 2 remaining slots (3,4)");
  check(delivered_slots[3] == 1 && delivered_slots[4] == 1, "slots 3 and 4 each delivered exactly once (no duplicate, no loss)");
  check(delivered_slots[0] == 0 && delivered_slots[1] == 0 && delivered_slots[2] == 0,
        "slots 0,1,2 (already released before the restart) are NOT re-delivered");

  oi_seam_close(cons2);
  oi_seam_close(prod);
  unlink(ring_path);
  unlink(state_path);

  if (g_fail == 0) {
    printf("\nrestart_test: ALL PASS\n");
  } else {
    fprintf(stderr, "\nrestart_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
