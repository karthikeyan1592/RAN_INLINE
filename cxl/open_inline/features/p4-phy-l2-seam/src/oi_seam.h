/* oi_seam.h — IF-P4-API: reserve/publish/wait_status/release/epoch, byte-for-byte matching the
 * LLD's own public API listing. Naming intentionally parallels the CXL PoC's own
 * desc_ring_try_push/try_pop (real precedent, cxl_ran_poc/phase5_cxl/desc_ring.h) adapted to this
 * LLD's per-slot-status design (HLD D1/D2's stated Phase-2-port-is-a-rename intent).
 */
#ifndef OI_SEAM_H
#define OI_SEAM_H

#include <stdint.h>

#include "oi_seam_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oi_seam_ring_s oi_seam_ring_t; /* opaque: wraps the mapped hdr + slots + fd/path state */

typedef struct {
  const char* ring_path;         /* shared-memory-backed regular file, P4-R10 */
  uint32_t ring_capacity;        /* power of two */
  uint32_t tb_max_bytes;         /* must equal OI_SEAM_TB_MAX_BYTES at MVP (P4-R14 cross-check) */
  uint32_t format_version;       /* must equal OI_SEAM_FORMAT_VERSION */
  const char* consumer_state_path; /* l2-stub only; NULL for the producer */
} oi_seam_config_t;

typedef enum {
  OI_SEAM_OK = 0,
  OI_SEAM_ERR_OPEN = 1,           /* couldn't open/create/mmap the backing file */
  OI_SEAM_ERR_ABSENT = 2,         /* create=0 and no valid ring exists yet (P4-R14 error table) */
  OI_SEAM_ERR_HEADER_MISMATCH = 3, /* capacity/slot_bytes/format_version disagree (P4-R14) */
  OI_SEAM_ERR_TB_TOO_LARGE = 4,   /* tb_len > OI_SEAM_TB_MAX_BYTES at publish time */
} oi_seam_status_code_t;

/* Create-or-attach the named ring segment (P4-R10: backed by a regular file in a named,
 * volume-persisted mount -- NOT tmpfs/`ipc: container:X`; the file itself, mmap'd MAP_SHARED, IS
 * the persistence mechanism). `create` mirrors the CXL PoC's cxl_region_open() signature
 * deliberately (HLD D2): the Phase-2 port is expected to add an mbind() call here and change
 * nothing else about this function's contract. On `create=1`, if the file doesn't exist it is
 * created+sized+zeroed+epoch=0 header written; if it already exists (a real, not first, restart)
 * epoch is bumped (P4-R8) and head=tail=0 are reset, but pre-existing SLOT memory is not
 * separately zeroed (every slot's status will read as whatever epoch tag was left -- the epoch
 * bump alone is sufficient for consumers to discard stale state per P4-R8, no need to also
 * memset the (possibly large) slot array on every producer restart).
 * On `create=0`, an absent or malformed segment is OI_SEAM_ERR_ABSENT/HEADER_MISMATCH, never a
 * partial/best-effort attach. Returns NULL and sets *out_status on failure. */
oi_seam_ring_t* oi_seam_open(const oi_seam_config_t* cfg, int create, oi_seam_status_code_t* out_status);
void oi_seam_close(oi_seam_ring_t* r);

/* Producer: block (bounded wait: spin -> sched_yield -> nanosleep backoff, no deadline -- SIM
 * tier, P4-R7) until a slot is free; returns a pointer to it (status left EMPTY, NOT yet
 * transitioned) with *out_seq assigned. Caller writes every other field, THEN calls
 * oi_seam_publish. Never overwrites a slot whose status isn't EMPTY/DONE-and-already-passed-tail. */
oi_seam_slot_t* oi_seam_reserve(oi_seam_ring_t* r, uint64_t* out_seq);

/* Producer: publish a status transition with release ordering -- every other slot field MUST
 * already be fully written before this call (P4-R3). */
void oi_seam_publish(oi_seam_slot_t* s, oi_seam_status_t st);

/* Consumer: spin/poll (same bounded backoff as reserve) until slots[idx].status == want; acquire
 * ordering on return, so every field written before the producer's matching release-publish is
 * visible here. `idx` is a physical ring index (seq % ring_capacity), computed by the caller. */
oi_seam_slot_t* oi_seam_wait_status(oi_seam_ring_t* r, uint64_t idx, oi_seam_status_t want);

/* Consumer: mark DONE, then advance tail (frees the slot for the producer). Persists the new
 * tail (+ current epoch) to consumer_state_path so a consumer restart can resume correctly
 * (P4-R9). */
void oi_seam_release(oi_seam_ring_t* r, uint64_t idx);

/* Consumer/producer: read the current epoch (acquire ordering, same header field the ring uses
 * internally) -- a change vs. the caller's own last-observed value means the ring was
 * reinitialized; caller MUST discard any per-key ordering state (P4-R8). */
uint32_t oi_seam_epoch(const oi_seam_ring_t* r);

/* Accessors the consumer/tests need without reaching into the opaque struct's real layout. */
uint64_t oi_seam_head(const oi_seam_ring_t* r);
uint64_t oi_seam_tail(const oi_seam_ring_t* r);
oi_seam_slot_t* oi_seam_slot_at(oi_seam_ring_t* r, uint64_t idx);

/* Reads the persisted consumer state file (epoch, tail) written by oi_seam_release. Returns 0 and
 * leaves out_epoch / out_tail both at 0 if the file doesn't exist yet (fresh consumer, never
 * released anything) -- not an error, matches P4-R9's "resume from the persisted tail" for a
 * never-before-run consumer (tail=0 is correct in that case). */
int oi_seam_read_consumer_state(const char* path, uint32_t* out_epoch, uint64_t* out_tail);

#ifdef __cplusplus
}
#endif

#endif /* OI_SEAM_H */
