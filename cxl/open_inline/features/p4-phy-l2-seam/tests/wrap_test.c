/* wrap_test.c — P4-G2 (P4-R7): drives the ring past ring_capacity with a paused consumer; asserts
 * the producer genuinely BLOCKS (not error, not silent drop, not overwrite) until the consumer
 * resumes, and that per-slot correctness holds afterward. Uses a real second thread for the
 * producer (oi_seam_reserve's own bounded spin/yield/nanosleep backoff is REAL blocking
 * behavior, so this needs true concurrency, not a single-threaded simulation, to prove it).
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

#define CAPACITY 4
#define OVER_CAPACITY_ATTEMPTS (CAPACITY + 3) /* ring_capacity + k, per the LLD's own test wording */

static oi_seam_ring_t* g_ring;
static _Atomic int g_producer_done = 0;
static _Atomic uint64_t g_reserved_count = 0;

static void* producer_thread(void* arg) {
  (void)arg;
  for (int i = 0; i < OVER_CAPACITY_ATTEMPTS; i++) {
    uint64_t seq;
    oi_seam_slot_t* s = oi_seam_reserve(g_ring, &seq);
    s->sfn = 0;
    s->slot = (uint16_t)i;
    s->rnti = 0x4601;
    s->harq_id = 0;
    s->crc_ok = 1;
    s->tb_len = 0;
    oi_seam_publish(s, OI_SEAM_READY);
    atomic_fetch_add_explicit(&g_reserved_count, 1, memory_order_relaxed);
  }
  atomic_store_explicit(&g_producer_done, 1, memory_order_release);
  return NULL;
}

int main(void) {
  const char* path = "/tmp/oi_p4_wrap_test.ring";
  unlink(path);

  oi_seam_config_t cfg = {.ring_path = path, .ring_capacity = CAPACITY, .tb_max_bytes = OI_SEAM_TB_MAX_BYTES,
                         .format_version = OI_SEAM_FORMAT_VERSION, .consumer_state_path = NULL};
  oi_seam_status_code_t st;
  g_ring = oi_seam_open(&cfg, /*create=*/1, &st);
  check(g_ring != NULL, "real ring created for the wrap test (capacity=4)");

  pthread_t producer;
  pthread_create(&producer, NULL, producer_thread, NULL);

  /* Give the producer a real chance to fill the ring and start blocking on attempt CAPACITY+1. */
  struct timespec pause = {0, 200000000}; /* 200ms -- SIM-tier, not a quotable performance number,
                                             just a test-harness settle time */
  nanosleep(&pause, NULL);

  uint64_t reserved_before_drain = atomic_load_explicit(&g_reserved_count, memory_order_relaxed);
  check(reserved_before_drain <= CAPACITY,
        "P4-R7: producer genuinely blocked at/before ring_capacity reservations while the consumer was paused "
        "(no overwrite, no silent drop of the backpressure)");
  check(atomic_load_explicit(&g_producer_done, memory_order_acquire) == 0,
        "producer thread has NOT finished yet (still blocked on the full ring, not silently proceeding)");

  /* Now drain: consumer catches up, releasing slots so the producer can keep going. */
  for (uint64_t idx = 0; idx < OVER_CAPACITY_ATTEMPTS; idx++) {
    oi_seam_slot_t* s = oi_seam_wait_status(g_ring, idx, OI_SEAM_READY);
    check((uint16_t)idx == s->slot, "drained slot's own `slot` field matches its expected sequence position (no corruption from the blocked-then-resumed reserve)");
    oi_seam_release(g_ring, idx);
  }

  pthread_join(producer, NULL);
  check(atomic_load_explicit(&g_producer_done, memory_order_acquire) == 1,
        "producer thread completed all ring_capacity+k reservations once the consumer resumed");
  check(oi_seam_tail(g_ring) == OVER_CAPACITY_ATTEMPTS, "final tail == total reservations (every slot consumed exactly once)");
  check(oi_seam_head(g_ring) == OVER_CAPACITY_ATTEMPTS, "final head == total reservations (every slot produced exactly once)");

  oi_seam_close(g_ring);
  unlink(path);

  if (g_fail == 0) {
    printf("\nwrap_test: ALL PASS\n");
  } else {
    fprintf(stderr, "\nwrap_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
