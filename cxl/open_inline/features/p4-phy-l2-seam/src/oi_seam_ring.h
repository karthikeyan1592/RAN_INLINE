/* oi_seam_ring.h — byte-precise ring wire format (P4-R1/R2), LLD §Data structures verbatim.
 *
 * Precedent note (real finding, not hidden): the LLD's own P4-R3 text cites "the same discipline
 * as the CXL PoC's e2e_slot_t status field" as prior art -- grepping the actual CXL PoC tree
 * (cxl_ran_poc/) finds NO `e2e_slot_t` anywhere; the closest REAL precedent that exists is
 * cxl_ran_poc/phase5_cxl/desc_ring.h's `desc_ring_t` (head/tail release/acquire SPSC ring). This
 * file follows that real release/acquire discipline, applied to LLD's own per-SLOT status field
 * design (not desc_ring_t's whole-ring head/tail-only design) -- the LLD's byte layout is
 * authoritative regardless of which precedent named it correctly.
 *
 * OI_SEAM_TB_MAX_BYTES: real, computed value (LLD Q3), not guessed -- max TBS across the p2 MVP's
 * pinned MCS set {4,13,21} is MCS 21 (see p2f-integration/src/host/oi_oracle_pack.h's kMcsTable:
 * {21, 6, 27656, 0.6016f}). tbs_bits=27656 -> exactly 3457 bytes (27656/8, no remainder).
 */
#ifndef OI_SEAM_RING_H
#define OI_SEAM_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define OI_SEAM_MAGIC 0x4F49534Du /* 'OISM' little-endian bytes 'M','S','I','O' read as LE u32 */
#define OI_SEAM_FORMAT_VERSION 1u

/* Real, computed (not guessed) per LLD Q3 -- see file header. */
#define OI_SEAM_TB_MAX_BYTES 3457u

typedef enum {
  OI_SEAM_EMPTY = 0,    /* free, producer may reserve */
  OI_SEAM_RESERVED = 1, /* producer owns it, mid-write; consumer MUST NOT read */
  OI_SEAM_READY = 2,    /* payload complete, release-published; consumer may read */
  OI_SEAM_DONE = 3      /* consumer finished; producer may reuse once tail passes it */
} oi_seam_status_t;

typedef struct __attribute__((aligned(64))) {
  _Atomic uint32_t status; /* oi_seam_status_t; release/acquire discipline (P4-R3) */
  uint32_t epoch;          /* ring generation this slot belongs to (P4-R8) */
  uint64_t seq;            /* global monotonic insertion sequence (wrap detection) */
  uint32_t sfn;
  uint16_t slot;
  uint16_t rnti;
  uint8_t harq_id;
  uint8_t crc_ok; /* 1 = pass, 0 = fail */
  uint8_t _pad[2];
  uint32_t tb_len;       /* bytes actually valid in tb[] */
  uint64_t t_enqueue_ns; /* CLOCK_MONOTONIC_RAW, producer-side; OBSERVATIONAL ONLY -- never a gate
                           operand (P4-R13) */
  uint8_t tb[OI_SEAM_TB_MAX_BYTES]; /* zero-padded tail beyond tb_len */
} oi_seam_slot_t;

typedef struct __attribute__((aligned(64))) {
  uint32_t magic;          /* fixed OI_SEAM_MAGIC */
  uint32_t format_version; /* this document = version 1 */
  uint32_t epoch;          /* bumped by producer on every (re)init (P4-R8) */
  uint32_t ring_capacity;  /* N slots, power of two (wrap arithmetic) */
  uint32_t slot_bytes;     /* sizeof(oi_seam_slot_t); cross-build safety check */
  uint8_t _pad0[44];       /* pad header to a cacheline boundary before the counters */
  _Atomic uint64_t head;   /* producer reserve counter, monotonic, reset only on epoch bump */
  _Atomic uint64_t tail;   /* consumer free counter; persisted for reattachment (P4-R9) */
  uint8_t _pad1[48];       /* isolate head/tail from slots[] on separate cachelines */
  /* oi_seam_slot_t slots[ring_capacity] follow immediately in the mapped segment */
} oi_seam_ring_hdr_t;

/* Static, compile-time proof of P4-R1/R2's byte-precise layout (test plan's own "sizeof/offsetof
 * assertions match this document's byte layout" requirement) -- checked at EVERY compile, not
 * just in a test binary, so any toolchain/platform that would silently break the wire format
 * fails the build immediately. */
_Static_assert(sizeof(oi_seam_slot_t) % 64 == 0, "oi_seam_slot_t must be a whole number of cachelines");
_Static_assert(_Alignof(oi_seam_slot_t) == 64, "oi_seam_slot_t must be 64-byte aligned");
_Static_assert(offsetof(oi_seam_slot_t, status) == 0, "status must be the first field (P4-R3 ordering)");
_Static_assert(sizeof(oi_seam_ring_hdr_t) % 64 == 0, "oi_seam_ring_hdr_t must be a whole number of cachelines");
_Static_assert(_Alignof(oi_seam_ring_hdr_t) == 64, "oi_seam_ring_hdr_t must be 64-byte aligned");
_Static_assert(offsetof(oi_seam_ring_hdr_t, head) != offsetof(oi_seam_ring_hdr_t, tail),
              "head and tail must be distinct fields");

#endif /* OI_SEAM_RING_H */
