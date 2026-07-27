/* gpu_phy_seam_bridge.c -- resolves LLD Q1 ("which process/thread owns the drain-and-publish
 * call site"): gpu-phy's own persistent event loop directly calls oi_p2_drain and then this
 * feature's producer, in the same process, no separate adapter (LLD Q1's option A; "either shape
 * satisfies this spec... the ring format does not care" -- chosen for the simpler process
 * topology, one fewer container/IPC hop than a separate adapter process would need).
 *
 * Structure is deliberately modeled on p3-live-tap-ul-inject/tools/bit_exact_harness.cpp's own
 * continuous multi-slot loop (poll ingest -> detect slot boundary via oi_ingest_last_slot_id ->
 * launch_slot -> drain), with the oracle-comparison tail replaced by this feature's own
 * reserve -> producer_fill_slot -> publish sequence (oi_seam.h / oi_seam_producer.h, both this
 * feature's own real, already-tested API -- no new ring/producer logic here, only the call-site
 * wiring itself, which is exactly and only what LLD Q1 leaves open).
 *
 * Written as plain C11 (not C++, unlike bit_exact_harness.cpp/pipeline_runner.cpp) specifically
 * because oi_seam_ring.h uses C11 `_Atomic` type-specifier syntax, which g++ does not accept in a
 * .cpp translation unit even via a __cplusplus-guarded header (real compile error hit and fixed
 * during this session: "'_Atomic' does not name a type" once oi_seam.h was #include-d from a
 * .cpp file) -- oi_p2_host.h/oi_ingest_af_packet.h are both __cplusplus-guarded and equally valid
 * to include from C, so plain C avoids the clash entirely, matching l2_stub_main.c's own
 * language choice on the consumer side.
 *
 * Usage: gpu_phy_seam_bridge <config_yaml> <iface> <mcs_index> <ring_path> <ring_capacity>
 *                            <rnti_hex> <harq_id> <slots_per_frame> <ru_mac xx:xx:xx:xx:xx:xx>
 *                            <udcomphdr_bytes 0|2> [--max-slots N]
 *   <ru_mac>: REQUIRED as of the 2026-07-26 src-MAC BPF filter fix (see
 *   oi_ingest_af_packet.h/.cpp) -- read from the rig's own config (ru_mac_addr), never hardcoded.
 *   <udcomphdr_bytes>: 0 or 2 -- REQUIRED as of the 2026-07-26 udCompHdr offset fix (see
 *   oi_oran_wire_layout.h); the real ru_emulator binary always uses 2, never sniffed/assumed here.
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CL/cl.h>

#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"
#include "../src/oi_seam.h"
#include "../src/oi_seam_producer.h"
#include "oi_ingest_af_packet.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* Same real bug + fix as bit_exact_harness.cpp (2026-07-26, found live on GCP by falsification --
 * see that file's own comment for the full account): oi_ingest_poll() issues >=1 recvmsg() plus 2
 * getsockopt(PACKET_STATISTICS) syscalls per call regardless of whether new data arrived: an
 * unthrottled busy-loop calling it dominates wall-clock time on syscall overhead alone,
 * independent of real frame volume. Safe to back off 50us whenever a poll cycle finds nothing new
 * -- oi_ingest_poll()'s own inner loop already fully drains the socket before returning. */
static void poll_backoff(void) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 50000; /* 50us */
  nanosleep(&ts, NULL);
}

/* Duplicated (not shared) across gpu_phy_seam_bridge.c / bit_exact_harness.cpp / the ingest
 * test's own literal constants -- same "genuinely separate binaries" precedent this project
 * already established for oi_osg_schedule.h, not an oversight. */
static int parse_mac(const char* s, uint8_t out[6]) {
  unsigned b[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return 0;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 11) {
    fprintf(stderr,
           "usage: gpu_phy_seam_bridge <config_yaml> <iface> <mcs_index> <ring_path> "
           "<ring_capacity> <rnti_hex> <harq_id> <slots_per_frame> <ru_mac xx:xx:xx:xx:xx:xx> "
           "<udcomphdr_bytes 0|2> [--max-slots N]\n");
    return 2;
  }
  const char* config_yaml = argv[1];
  const char* iface = argv[2];
  uint32_t mcs_index = (uint32_t)atoi(argv[3]);
  const char* ring_path = argv[4];
  uint32_t ring_capacity = (uint32_t)strtoul(argv[5], NULL, 10);
  uint16_t rnti = (uint16_t)strtoul(argv[6], NULL, 0);
  uint8_t harq_id = (uint8_t)strtoul(argv[7], NULL, 10);
  uint32_t slots_per_frame = (uint32_t)atoi(argv[8]);
  uint8_t ru_mac[6];
  if (!parse_mac(argv[9], ru_mac)) {
    fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"invalid ru_mac '%s', expected xx:xx:xx:xx:xx:xx\"}\n",
           argv[9]);
    return 2;
  }
  int udcomphdr_bytes_arg = atoi(argv[10]);
  if (udcomphdr_bytes_arg != (int)OI_WIRE_UDCOMPHDR_BYTES_ABSENT && udcomphdr_bytes_arg != (int)OI_WIRE_UDCOMPHDR_BYTES_PRESENT) {
    fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"invalid udcomphdr_bytes '%s', expected 0 or 2\"}\n",
           argv[10]);
    return 2;
  }
  uint8_t udcomphdr_bytes = (uint8_t)udcomphdr_bytes_arg;
  long max_slots = -1; /* unbounded unless given (matches l2_stub_main's own --max-slots) */
  for (int i = 11; i < argc; i++) {
    if (strcmp(argv[i], "--max-slots") == 0 && i + 1 < argc) {
      max_slots = strtol(argv[i + 1], NULL, 10);
      i++;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  /* --- Real OpenCL + real p2 pipeline (same bootstrap as pipeline_runner.cpp / bit_exact_harness.cpp) --- */
  cl_platform_id platform;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  cl_device_id device;
  cl_uint nof_devices = 0;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &nof_devices);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  oi_p2_pipeline* pipeline = NULL;
  oi_p2_status pst = oi_p2_setup(config_yaml, ctx, queue, &pipeline);
  if (pst != OI_P2_OK) {
    fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"oi_p2_setup failed, status %d\"}\n",
           (int)pst);
    return 1;
  }

  /* --- This feature's own real ring: producer side, create-or-attach (gpu-phy owns ring
   * creation/init, LLD's own oi_seam_open doc comment). --- */
  oi_seam_config_t seam_cfg;
  memset(&seam_cfg, 0, sizeof(seam_cfg));
  seam_cfg.ring_path = ring_path;
  seam_cfg.ring_capacity = ring_capacity;
  seam_cfg.tb_max_bytes = OI_SEAM_TB_MAX_BYTES;
  seam_cfg.format_version = OI_SEAM_FORMAT_VERSION;
  seam_cfg.consumer_state_path = NULL; /* producer side: must be NULL, oi_seam.h's own doc comment */

  oi_seam_status_code_t seam_open_st = OI_SEAM_OK;
  oi_seam_ring_t* ring = oi_seam_open(&seam_cfg, /*create=*/1, &seam_open_st);
  if (!ring) {
    fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"oi_seam_open failed, status %d\"}\n",
           (int)seam_open_st);
    oi_p2_teardown(pipeline);
    return 1;
  }

  const uint64_t kArenaBytes = 16ull * 1024 * 1024;
  oi_ingest_handle ingest = oi_ingest_open_af_packet(iface, pipeline, kArenaBytes, ru_mac, udcomphdr_bytes);
  if (!ingest) {
    fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"oi_ingest_open_af_packet failed on '%s'\"}\n",
           iface);
    oi_seam_close(ring);
    oi_p2_teardown(pipeline);
    return 1;
  }

  oi_seam_producer_config_t producer_cfg;
  producer_cfg.rnti = rnti;
  producer_cfg.harq_id = harq_id;
  producer_cfg.slots_per_frame = slots_per_frame;

  int have_prev_slot = 0;
  uint32_t prev_slot_id = 0;
  uint64_t nof_published = 0, nof_oversize_refused = 0, nof_launch_errors = 0, nof_drain_errors = 0;

  /* Same continuous multi-slot loop shape as bit_exact_harness.cpp: poll ingest; a slot_id
   * advance means the PREVIOUS slot's 14 symbols are all in -- launch+drain it, then (this
   * feature's own addition, the actual LLD Q1 wiring) reserve+fill+publish it onto the seam ring. */
  while (!g_stop && (max_slots < 0 || (long)nof_published < max_slots)) {
    oi_ingest_poll(ingest);
    if (!oi_ingest_has_last_slot_id(ingest)) {
      poll_backoff();
      continue;
    }
    uint32_t observed = oi_ingest_last_slot_id(ingest);

    if (!have_prev_slot) {
      prev_slot_id = observed;
      have_prev_slot = 1;
      continue;
    }
    if (observed == prev_slot_id) {
      poll_backoff();
      continue;
    }

    oi_p2_status lst = oi_p2_launch_slot(pipeline, prev_slot_id, mcs_index);
    if (lst != OI_P2_OK) {
      fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"launch_slot failed for slot_id=%u, status %d\"}\n",
             prev_slot_id, (int)lst);
      nof_launch_errors++;
      prev_slot_id = observed;
      continue;
    }
    oi_p2_tb_record_c record;
    memset(&record, 0, sizeof(record));
    oi_p2_status dst = oi_p2_drain(pipeline, prev_slot_id, &record);
    if (dst != OI_P2_OK) {
      fprintf(stderr, "{\"check\":\"gpu_phy_seam_bridge\",\"error\":\"drain failed for slot_id=%u, status %d\"}\n",
             prev_slot_id, (int)dst);
      nof_drain_errors++;
      prev_slot_id = observed;
      continue;
    }

    /* The actual LLD Q1 wiring: p2's drained record -> this feature's own reserve/fill/publish. */
    oi_seam_p2_record_view_t view;
    view.slot_id = record.slot_id;
    view.tb_size_bytes = record.tb_size_bytes;
    view.crc24a_ok = record.crc24a_ok;
    view.tb_data = record.tb_data;

    uint64_t seq = 0;
    oi_seam_slot_t* slot = oi_seam_reserve(ring, &seq); /* blocks (bounded backoff) if ring full */
    int fill_rc = oi_seam_producer_fill_slot(slot, &view, &producer_cfg, /*t_enqueue_ns=*/0);
    if (fill_rc != 0) {
      /* P4-R14: refuse to publish an oversize TB rather than truncate. The reserved slot is left
       * EMPTY (never published READY) -- a real, disclosed limitation of this MVP wiring (no
       * oversize TB is expected at the pinned MVP MCS points per p4's own OI_SEAM_TB_MAX_BYTES
       * derivation, so this path is defensive, not expected to fire). */
      nof_oversize_refused++;
      prev_slot_id = observed;
      continue;
    }
    oi_seam_publish(slot, OI_SEAM_READY);
    nof_published++;

    prev_slot_id = observed;
  }

  oi_ingest_counters counters = oi_ingest_poll(ingest);

  printf(
      "{\"check\":\"gpu_phy_seam_bridge\",\"schema\":\"oi-p4-bridge/1\",\"published\":%lu,"
      "\"oversize_refused\":%lu,\"launch_errors\":%lu,\"drain_errors\":%lu,\"socket_drops\":%lu,"
      "\"feed_backpressure\":%lu,\"parse_failed\":%lu}\n",
      (unsigned long)nof_published, (unsigned long)nof_oversize_refused,
      (unsigned long)nof_launch_errors, (unsigned long)nof_drain_errors,
      (unsigned long)counters.socket_drops, (unsigned long)counters.feed_backpressure,
      (unsigned long)counters.parse_failed);

  oi_ingest_close(ingest);
  oi_seam_close(ring);
  oi_p2_teardown(pipeline);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  int ok = (nof_published > 0) && (nof_launch_errors == 0) && (nof_drain_errors == 0) &&
          (counters.socket_drops == 0);
  return ok ? 0 : 1;
}
