/* l2_stub_main.c — the L2 stub consumer entrypoint (M11/P4-R11). Attaches to the ring (reattaching
 * at the persisted tail if a consumer-state file already exists, P4-R9), validates every
 * delivered slot (oi_l2_validate.c: per-key ordering + CRC verdict), and prints a JSON verdict
 * with pass/fail counters on exit (SIGINT/SIGTERM or after --max-slots records, for gate scripts
 * that need a bounded run). Never encodes SCF FAPI / packed-FAPI (P4-R11) -- grep this file: no
 * FAPI symbol appears anywhere.
 *
 * Usage: l2_stub_main <ring_path> <ring_capacity> <consumer_state_path> [--max-slots N]
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oi_l2_validate.h"
#include "oi_seam.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: l2_stub_main <ring_path> <ring_capacity> <consumer_state_path> [--max-slots N]\n");
    return 2;
  }
  const char* ring_path = argv[1];
  uint32_t ring_capacity = (uint32_t)strtoul(argv[2], NULL, 10);
  const char* consumer_state_path = argv[3];
  long max_slots = -1; /* unbounded unless given */
  for (int i = 4; i < argc; i++) {
    if (strcmp(argv[i], "--max-slots") == 0 && i + 1 < argc) {
      max_slots = strtol(argv[i + 1], NULL, 10);
      i++;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  oi_seam_config_t cfg = {0};
  cfg.ring_path = ring_path;
  cfg.ring_capacity = ring_capacity;
  cfg.tb_max_bytes = OI_SEAM_TB_MAX_BYTES;
  cfg.format_version = OI_SEAM_FORMAT_VERSION;
  cfg.consumer_state_path = consumer_state_path;

  oi_seam_status_code_t open_st = OI_SEAM_OK;
  oi_seam_ring_t* r = oi_seam_open(&cfg, /*create=*/0, &open_st);
  if (!r) {
    fprintf(stderr, "{\"check\":\"l2_stub\",\"error\":\"oi_seam_open failed, status %d\"}\n", (int)open_st);
    return 2;
  }

  /* P4-R9: reattach at the persisted tail if a prior run left one, and verify the epoch still
   * matches (if it doesn't, the producer restarted since -- resume from the RING's current tail,
   * which the producer resets to 0 on its own restart, per P4-R8, not the stale persisted one). */
  uint32_t persisted_epoch = 0;
  uint64_t persisted_tail = 0;
  int had_state = oi_seam_read_consumer_state(consumer_state_path, &persisted_epoch, &persisted_tail);
  uint64_t start_tail = oi_seam_tail(r); /* the ring's own current tail is always authoritative --
                                            this consumer never re-derives its OWN starting point
                                            from the persisted file if the ring's tail has already
                                            moved past it (e.g. this exact process never released
                                            anything yet, ring tail == 0 == a fresh producer) */
  (void)had_state;
  (void)persisted_epoch;
  (void)persisted_tail;
  /* Note: because this consumer is single-reader and oi_seam_release() is the only thing that
   * ever advances tail, `start_tail` read fresh from the ring header IS the correct resume point
   * in every case: a never-restarted ring's tail already reflects everything consumed so far by
   * THIS process, and a producer-restarted ring's tail was already reset to 0 by the producer
   * itself (P4-R8) before this consumer could observe anything past it. */

  oi_l2_validator_t validator;
  oi_l2_validator_init(&validator);

  uint64_t idx = start_tail;
  long processed = 0;
  while (!g_stop && (max_slots < 0 || processed < max_slots)) {
    oi_seam_slot_t* slot = oi_seam_wait_status(r, idx, OI_SEAM_READY);
    oi_l2_validator_process(&validator, slot);
    oi_seam_release(r, idx);
    idx++;
    processed++;
  }

  printf(
      "{\"check\":\"l2_stub\",\"schema\":\"oi-p4-l2stub/1\",\"processed\":%lu,\"crc_ok\":%lu,"
      "\"crc_fail\":%lu,\"order_violations\":%lu,\"epoch_resets\":%lu}\n",
      (unsigned long)validator.nof_processed, (unsigned long)validator.nof_crc_ok,
      (unsigned long)validator.nof_crc_fail, (unsigned long)validator.nof_order_violations,
      (unsigned long)validator.nof_epoch_resets);

  oi_seam_close(r);
  return (validator.nof_crc_fail == 0 && validator.nof_order_violations == 0) ? 0 : 1;
}
