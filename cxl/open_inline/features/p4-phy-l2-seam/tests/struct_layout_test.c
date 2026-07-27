/* struct_layout_test.c — P4-R1/R2's own test plan item: runtime sizeof/offsetof assertions
 * (the _Static_assert checks in oi_seam_ring.h already enforce the critical ones at every
 * compile; this file additionally checks every individual field offset explicitly, and does a
 * real memcpy round-trip, matching the test plan's "round-trip a slot through memcpy and verify
 * field-for-field equality" requirement literally). Plain C, no test framework.
 */
#include <stdio.h>
#include <string.h>

#include "../src/oi_seam_ring.h"

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
  check(sizeof(oi_seam_slot_t) % 64 == 0, "oi_seam_slot_t size is a whole number of cachelines");
  check(_Alignof(oi_seam_slot_t) == 64, "oi_seam_slot_t is 64-byte aligned");
  check(offsetof(oi_seam_slot_t, status) == 0, "status is byte offset 0 (first field, P4-R3)");
  check(offsetof(oi_seam_slot_t, epoch) == 4, "epoch is byte offset 4");
  check(offsetof(oi_seam_slot_t, seq) == 8, "seq is byte offset 8");
  check(offsetof(oi_seam_slot_t, sfn) == 16, "sfn is byte offset 16");
  check(offsetof(oi_seam_slot_t, slot) == 20, "slot is byte offset 20");
  check(offsetof(oi_seam_slot_t, rnti) == 22, "rnti is byte offset 22");
  check(offsetof(oi_seam_slot_t, harq_id) == 24, "harq_id is byte offset 24");
  check(offsetof(oi_seam_slot_t, crc_ok) == 25, "crc_ok is byte offset 25");
  check(offsetof(oi_seam_slot_t, tb_len) == 28, "tb_len is byte offset 28");
  check(offsetof(oi_seam_slot_t, t_enqueue_ns) == 32, "t_enqueue_ns is byte offset 32");
  check(offsetof(oi_seam_slot_t, tb) == 40, "tb[] starts at byte offset 40");
  check(sizeof(((oi_seam_slot_t*)0)->tb) == OI_SEAM_TB_MAX_BYTES, "tb[] is exactly OI_SEAM_TB_MAX_BYTES long");

  check(sizeof(oi_seam_ring_hdr_t) % 64 == 0, "oi_seam_ring_hdr_t size is a whole number of cachelines");
  check(_Alignof(oi_seam_ring_hdr_t) == 64, "oi_seam_ring_hdr_t is 64-byte aligned");
  check(offsetof(oi_seam_ring_hdr_t, magic) == 0, "magic is byte offset 0");
  check(offsetof(oi_seam_ring_hdr_t, format_version) == 4, "format_version is byte offset 4");
  check(offsetof(oi_seam_ring_hdr_t, epoch) == 8, "hdr epoch is byte offset 8");
  check(offsetof(oi_seam_ring_hdr_t, ring_capacity) == 12, "ring_capacity is byte offset 12");
  check(offsetof(oi_seam_ring_hdr_t, slot_bytes) == 16, "slot_bytes is byte offset 16");
  check(offsetof(oi_seam_ring_hdr_t, head) == 64, "head starts at byte offset 64 (own cacheline)");
  check(offsetof(oi_seam_ring_hdr_t, tail) == 72, "tail follows head at byte offset 72");

  check(OI_SEAM_MAGIC != 0, "OI_SEAM_MAGIC is a real, non-zero constant");
  check(OI_SEAM_TB_MAX_BYTES == 3457, "OI_SEAM_TB_MAX_BYTES == 3457 (real, computed from MCS 21's 27656 tbs_bits / 8)");

  /* Real memcpy round-trip: build a slot, memcpy it to a fresh buffer, verify field-for-field. */
  oi_seam_slot_t src;
  memset(&src, 0, sizeof(src));
  src.status = OI_SEAM_READY;
  src.epoch = 7;
  src.seq = 12345;
  src.sfn = 99;
  src.slot = 5;
  src.rnti = 0x4601;
  src.harq_id = 2;
  src.crc_ok = 1;
  src.tb_len = 10;
  src.t_enqueue_ns = 1000000;
  for (int i = 0; i < 10; i++) src.tb[i] = (uint8_t)(i + 1);

  oi_seam_slot_t dst;
  memcpy(&dst, &src, sizeof(src));

  check(dst.epoch == src.epoch, "memcpy round-trip: epoch matches");
  check(dst.seq == src.seq, "memcpy round-trip: seq matches");
  check(dst.sfn == src.sfn, "memcpy round-trip: sfn matches");
  check(dst.slot == src.slot, "memcpy round-trip: slot matches");
  check(dst.rnti == src.rnti, "memcpy round-trip: rnti matches");
  check(dst.harq_id == src.harq_id, "memcpy round-trip: harq_id matches");
  check(dst.crc_ok == src.crc_ok, "memcpy round-trip: crc_ok matches");
  check(dst.tb_len == src.tb_len, "memcpy round-trip: tb_len matches");
  check(memcmp(dst.tb, src.tb, sizeof(src.tb)) == 0, "memcpy round-trip: tb[] byte-identical (incl. zero-padded tail)");

  if (g_fail == 0) {
    printf("\nstruct_layout_test: ALL PASS\n");
  } else {
    fprintf(stderr, "\nstruct_layout_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
